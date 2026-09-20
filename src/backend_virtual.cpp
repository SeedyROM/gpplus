#include <gpplus/virtual.hpp>

#include <algorithm>
#include <array>
#include <memory>
#include <vector>

#include "backend.hpp"
#include "mapping.hpp"

namespace gpp {
namespace {

struct VirtualDevice {
  virtualpad::VirtualId vid = 0;
  virtualpad::Spec spec;
  bool present = true;
  /// Set once the backend has handed it to a Context.
  int slot = -1;
  DeviceId device_id = kInvalidDevice;
  bool announced = false;
  std::vector<std::uint8_t> buttons;
  std::vector<float> axes;
  std::vector<std::uint8_t> hats;
  std::array<std::uint8_t, kButtonCount> mapped_buttons{};
  std::array<float, kAxisCount> mapped_axes{};
  PowerLevel power = PowerLevel::Wired;
  bool power_dirty = true;
  virtualpad::RumbleState rumble;
};

/// Process-wide rather than owned by the backend: a test creates devices
/// before it creates the Context, and the Spec has to survive that.
struct Registry {
  std::vector<std::unique_ptr<VirtualDevice>> devices;
  virtualpad::VirtualId next_id = 1;

  VirtualDevice *find(virtualpad::VirtualId id) {
    for (auto &d : devices) {
      if (d->vid == id) {
        return d.get();
      }
    }
    return nullptr;
  }
};

Registry &registry() {
  static Registry r;
  return r;
}

} // namespace

namespace detail {
namespace {

class VirtualBackend final : public Backend {
public:
  const char *name() const noexcept override { return "virtual"; }

  bool init(Host *host) override {
    host_ = host;
    return true;
  }

  void shutdown() override { host_ = nullptr; }

  void poll(bool rescan) override {
    (void)rescan; // a synthetic device appears the moment it is asked for
    Registry &r = registry();

    for (auto &owned : r.devices) {
      VirtualDevice &d = *owned;
      if (!d.present && d.announced) {
        host_->remove_device(d.slot);
        d.announced = false;
        d.slot = -1;
        d.device_id = kInvalidDevice;
        continue;
      }
      if (d.present && !d.announced) {
        announce(d);
      }
      if (!d.announced) {
        continue;
      }
      if (d.power_dirty) {
        d.power_dirty = false;
        host_->set_power(d.slot, d.power);
      }
      if (d.spec.backend_mapped) {
        for (std::size_t i = 0; i < kButtonCount; ++i) {
          host_->set_button(d.slot, static_cast<Button>(i),
                            d.mapped_buttons[i] != 0);
        }
        for (std::size_t i = 0; i < kAxisCount; ++i) {
          host_->set_axis(d.slot, static_cast<Axis>(i), d.mapped_axes[i]);
        }
      } else {
        for (std::size_t i = 0; i < d.buttons.size(); ++i) {
          host_->set_raw_button(d.slot, static_cast<int>(i), d.buttons[i] != 0);
        }
        for (std::size_t i = 0; i < d.axes.size(); ++i) {
          host_->set_raw_axis(d.slot, static_cast<int>(i), d.axes[i]);
        }
        for (std::size_t i = 0; i < d.hats.size(); ++i) {
          host_->set_raw_hat(d.slot, static_cast<int>(i), d.hats[i]);
        }
      }
    }

    // Removed devices are only erased once the Context has been told, so
    // that remove() during a frame behaves like an unplug rather than a
    // dangling slot.
    for (std::size_t i = r.devices.size(); i-- > 0;) {
      if (!r.devices[i]->present && !r.devices[i]->announced) {
        r.devices.erase(r.devices.begin() + static_cast<long>(i));
      }
    }
  }

  bool rumble(void *handle, float low, float high,
              std::uint32_t duration_ms) override {
    auto *d = static_cast<VirtualDevice *>(handle);
    if (d == nullptr) {
      return false;
    }
    d->rumble.low = low;
    d->rumble.high = high;
    d->rumble.duration_ms = duration_ms;
    d->rumble.call_count += 1;
    return true;
  }

private:
  void announce(VirtualDevice &d) {
    DeviceDesc desc;
    desc.name = d.spec.name;
    desc.vendor = d.spec.vendor;
    desc.product = d.spec.product;
    desc.version = d.spec.version;
    desc.guid = d.spec.guid.is_zero()
                    ? make_guid(0x03, d.spec.vendor, d.spec.product,
                                d.spec.version, d.spec.name)
                    : d.spec.guid;
    desc.backend_mapped = d.spec.backend_mapped;
    desc.raw_button_count = d.spec.backend_mapped ? 0 : d.spec.raw_button_count;
    desc.raw_axis_count = d.spec.backend_mapped ? 0 : d.spec.raw_axis_count;
    desc.raw_hat_count = d.spec.backend_mapped ? 0 : d.spec.raw_hat_count;
    desc.caps.rumble = d.spec.rumble;
    desc.handle = &d;
    desc.unique_key = "virtual:" + std::to_string(d.vid);

    const int slot = host_->add_device(desc);
    if (slot < 0) {
      return;
    }
    d.slot = slot;
    d.device_id = host_->device_id_for_slot(slot);
    d.announced = true;
  }

  Host *host_ = nullptr;
};

} // namespace

Backend *make_backend_virtual() { return new VirtualBackend(); }

} // namespace detail

namespace virtualpad {

VirtualId add(const Spec &spec) {
  Registry &r = registry();
  auto d = std::make_unique<VirtualDevice>();
  d->vid = r.next_id++;
  d->spec = spec;
  d->buttons.assign(
      static_cast<std::size_t>(std::max(0, spec.raw_button_count)), 0);
  d->axes.assign(static_cast<std::size_t>(std::max(0, spec.raw_axis_count)),
                 0.0f);
  d->hats.assign(static_cast<std::size_t>(std::max(0, spec.raw_hat_count)), 0);
  const VirtualId id = d->vid;
  r.devices.push_back(std::move(d));
  return id;
}

void remove(VirtualId id) {
  if (VirtualDevice *d = registry().find(id)) {
    d->present = false;
  }
}

void remove_all() {
  for (auto &d : registry().devices) {
    d->present = false;
  }
}

DeviceId device_id(VirtualId id) {
  const VirtualDevice *d = registry().find(id);
  return d != nullptr ? d->device_id : kInvalidDevice;
}

void set_raw_button(VirtualId id, int index, bool down) {
  VirtualDevice *d = registry().find(id);
  if (d != nullptr && index >= 0 &&
      static_cast<std::size_t>(index) < d->buttons.size()) {
    d->buttons[static_cast<std::size_t>(index)] = down ? 1 : 0;
  }
}

void set_raw_axis(VirtualId id, int index, float value) {
  VirtualDevice *d = registry().find(id);
  if (d != nullptr && index >= 0 &&
      static_cast<std::size_t>(index) < d->axes.size()) {
    d->axes[static_cast<std::size_t>(index)] = value;
  }
}

void set_raw_hat(VirtualId id, int index, std::uint8_t mask) {
  VirtualDevice *d = registry().find(id);
  if (d != nullptr && index >= 0 &&
      static_cast<std::size_t>(index) < d->hats.size()) {
    d->hats[static_cast<std::size_t>(index)] = mask;
  }
}

void set_button(VirtualId id, Button b, bool down) {
  VirtualDevice *d = registry().find(id);
  if (d != nullptr && static_cast<std::size_t>(b) < kButtonCount) {
    d->mapped_buttons[static_cast<std::size_t>(b)] = down ? 1 : 0;
  }
}

void set_axis(VirtualId id, Axis a, float value) {
  VirtualDevice *d = registry().find(id);
  if (d != nullptr && static_cast<std::size_t>(a) < kAxisCount) {
    d->mapped_axes[static_cast<std::size_t>(a)] = value;
  }
}

void set_power(VirtualId id, PowerLevel level) {
  VirtualDevice *d = registry().find(id);
  if (d != nullptr) {
    d->power = level;
    d->power_dirty = true;
  }
}

RumbleState rumble_state(VirtualId id) {
  const VirtualDevice *d = registry().find(id);
  return d != nullptr ? d->rumble : RumbleState{};
}

} // namespace virtualpad
} // namespace gpp
