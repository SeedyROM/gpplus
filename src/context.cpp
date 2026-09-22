// The portable half: device bookkeeping, mapping, deadzones, edges and
// events. Nothing here knows what an operating system is.

#include <gpplus/gamepad.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#include "backend.hpp"
#include "mapping.hpp"

namespace gpp {
namespace {

using detail::Backend;
using detail::Mapping;
using detail::MappingTable;
using detail::RawInputs;

constexpr const char *kBackendIds[] = {
#define GPPLUS_BACKEND(id) #id,
#include "gpplus_backends.inc"
#undef GPPLUS_BACKEND
    nullptr,
};

std::vector<Backend *> make_backends() {
  std::vector<Backend *> v;
#define GPPLUS_BACKEND(id) v.push_back(detail::make_backend_##id());
#include "gpplus_backends.inc"
#undef GPPLUS_BACKEND
  return v;
}

float clamp01(float v) noexcept {
  return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}
float clamp_unit(float v) noexcept {
  return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
}

/// Applies a deadzone to a value already known to be one-dimensional --
/// triggers, and sticks under PerAxis. Rescales so that the first live
/// value is zero rather than the deadzone, because otherwise every stick
/// jumps to 15% the moment it is touched.
float apply_1d_deadzone(float v, float dz) noexcept {
  if (dz <= 0.0f) {
    return v;
  }
  const float mag = std::fabs(v);
  if (mag <= dz) {
    return 0.0f;
  }
  const float capped = mag > 1.0f ? 1.0f : mag;
  const float scaled = (capped - dz) / (1.0f - dz);
  return v < 0.0f ? -scaled : scaled;
}

Stick stick_of(Axis a) noexcept {
  return (a == Axis::LeftX || a == Axis::LeftY) ? Stick::Left : Stick::Right;
}

struct StickAxes {
  Axis x;
  Axis y;
};

StickAxes axes_of(Stick s) noexcept {
  return s == Stick::Left ? StickAxes{Axis::LeftX, Axis::LeftY}
                          : StickAxes{Axis::RightX, Axis::RightY};
}

std::string read_file(const std::string &path, bool &ok) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    ok = false;
    return {};
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  ok = true;
  return ss.str();
}

struct Device {
  bool live = false;
  DeviceId id = kInvalidDevice;
  Backend *backend = nullptr;
  void *handle = nullptr;
  bool backend_mapped = false;
  std::string unique_key;
  DeviceInfo info;

  std::vector<std::uint8_t> raw_buttons;
  std::vector<float> raw_axes;
  std::vector<std::uint8_t> raw_hats;

  std::array<std::uint8_t, kButtonCount> buttons{};
  std::array<std::uint8_t, kButtonCount> prev_buttons{};
  /// Mapped but untreated: no deadzone, no Y flip. Everything the public
  /// API reports is derived from these, so that changing the deadzone
  /// changes what the next query answers rather than the next frame's.
  std::array<float, kAxisCount> axes{};
  std::array<float, kAxisCount> prev_processed{};

  const Mapping *mapping = nullptr;

  // The last rumble()/rumble_triggers() actually sent to the backend, so a
  // request identical to it is not sent again -- see the dedup in
  // Context::rumble()/rumble_triggers() for why. Reset to "nothing sent
  // yet" by add_device()'s `d = Device{}`, so a device that reconnects (or
  // a slot a different device now occupies) always gets its first rumble
  // through regardless of what the previous occupant last had set.
  bool has_rumble = false;
  float rumble_low = 0.0f;
  float rumble_high = 0.0f;
  std::uint32_t rumble_duration_ms = 0;

  bool has_trigger_rumble = false;
  float trigger_rumble_left = 0.0f;
  float trigger_rumble_right = 0.0f;
  std::uint32_t trigger_rumble_duration_ms = 0;
};

} // namespace

// ---------------------------------------------------------------------------
// Guid
// ---------------------------------------------------------------------------

bool Guid::is_zero() const noexcept {
  for (std::uint8_t b : bytes) {
    if (b != 0) {
      return false;
    }
  }
  return true;
}

std::string Guid::str() const {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string s(32, '0');
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    s[i * 2] = kHex[(bytes[i] >> 4) & 0xF];
    s[i * 2 + 1] = kHex[bytes[i] & 0xF];
  }
  return s;
}

Guid Guid::parse(const std::string &hex) noexcept {
  Guid g;
  if (hex.size() < 32) {
    return g;
  }
  const auto digit = [](char c) -> int {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
      return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
      return c - 'A' + 10;
    }
    return -1;
  };
  for (std::size_t i = 0; i < 16; ++i) {
    const int hi = digit(hex[i * 2]);
    const int lo = digit(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) {
      return Guid{};
    }
    g.bytes[i] = static_cast<std::uint8_t>((hi << 4) | lo);
  }
  return g;
}

// ---------------------------------------------------------------------------
// Context
// ---------------------------------------------------------------------------

struct Context::Impl final : detail::Host {
  Config config;
  MappingTable mappings;
  std::vector<Backend *> backends;
  std::vector<Device> slots;
  std::vector<DeviceId> live_ids;
  std::vector<Event> events;
  DeviceId next_id = 1;
  /// Which backend is being driven right now, so that a device added from
  /// inside its poll() is attributed without every backend having to say
  /// so on every call.
  Backend *current_backend = nullptr;
  std::chrono::steady_clock::time_point last_scan{};
  bool first_update = true;

  explicit Impl(const Config &cfg) : config(cfg) {}

  ~Impl() override {
    // Motors first, teardown second. On several platforms the effect is
    // owned by a system service and survives the process, so a game that
    // exits mid-explosion leaves the pad buzzing; the order here is the
    // difference.
    for (Device &d : slots) {
      if (d.live && d.backend != nullptr && d.info.caps.rumble) {
        d.backend->rumble(d.handle, 0.0f, 0.0f, 0);
      }
    }
    for (Backend *b : backends) {
      b->shutdown();
      delete b;
    }
  }

  Device *find(DeviceId id) {
    for (Device &d : slots) {
      if (d.live && d.id == id) {
        return &d;
      }
    }
    return nullptr;
  }

  const Device *find(DeviceId id) const {
    return const_cast<Impl *>(this)->find(id);
  }

  void resolve_mapping(Device &d) {
    if (d.backend_mapped) {
      d.mapping = nullptr;
      d.info.mapped = true;
      return;
    }
    d.mapping = mappings.find(d.info.guid);
    d.info.mapped = d.mapping != nullptr;
  }

  void refresh_all_mappings() {
    for (Device &d : slots) {
      if (d.live) {
        const bool was_mapped = d.info.mapped;
        resolve_mapping(d);
        // A pad that was unreadable and is now understood counts as
        // arriving: a game that only listens for DeviceAdded would
        // otherwise never notice the pad it just gained a mapping for.
        if (!was_mapped && d.info.mapped) {
          events.push_back(Event{Event::Type::DeviceAdded, d.id, Button::South,
                                 Axis::LeftX, 0.0f});
        }
      }
    }
  }

  // --- Host -----------------------------------------------------------

  int add_device(const detail::DeviceDesc &desc) override {
    int slot = -1;
    for (std::size_t i = 0; i < slots.size(); ++i) {
      if (!slots[i].live) {
        slot = static_cast<int>(i);
        break;
      }
    }
    if (slot < 0) {
      slot = static_cast<int>(slots.size());
      slots.emplace_back();
    }

    Device &d = slots[static_cast<std::size_t>(slot)];
    d = Device{};
    d.live = true;
    d.id = next_id++;
    d.backend = current_backend;
    d.handle = desc.handle;
    d.backend_mapped = desc.backend_mapped;
    d.unique_key = desc.unique_key;
    d.info.id = d.id;
    d.info.name = desc.name;
    d.info.guid = desc.guid;
    d.info.vendor = desc.vendor;
    d.info.product = desc.product;
    d.info.version = desc.version;
    d.info.caps = desc.caps;
    d.info.raw_button_count = desc.raw_button_count;
    d.info.raw_axis_count = desc.raw_axis_count;
    d.info.raw_hat_count = desc.raw_hat_count;
    d.info.backend = current_backend != nullptr ? current_backend->name() : "";
    d.raw_buttons.assign(
        static_cast<std::size_t>(std::max(0, desc.raw_button_count)), 0);
    d.raw_axes.assign(
        static_cast<std::size_t>(std::max(0, desc.raw_axis_count)), 0.0f);
    d.raw_hats.assign(static_cast<std::size_t>(std::max(0, desc.raw_hat_count)),
                      0);
    resolve_mapping(d);

    if (!d.info.mapped && !config.report_unmapped) {
      d.live = false;
      return -1;
    }

    live_ids.push_back(d.id);
    events.push_back(Event{Event::Type::DeviceAdded, d.id, Button::South,
                           Axis::LeftX, 0.0f});
    return slot;
  }

  void remove_device(int slot) override {
    if (slot < 0 || static_cast<std::size_t>(slot) >= slots.size()) {
      return;
    }
    Device &d = slots[static_cast<std::size_t>(slot)];
    if (!d.live) {
      return;
    }
    const DeviceId id = d.id;
    d.live = false;
    d.backend = nullptr;
    live_ids.erase(std::remove(live_ids.begin(), live_ids.end(), id),
                   live_ids.end());
    events.push_back(Event{Event::Type::DeviceRemoved, id, Button::South,
                           Axis::LeftX, 0.0f});
  }

  Device *slot_device(int slot) {
    if (slot < 0 || static_cast<std::size_t>(slot) >= slots.size()) {
      return nullptr;
    }
    Device &d = slots[static_cast<std::size_t>(slot)];
    return d.live ? &d : nullptr;
  }

  void set_raw_button(int slot, int index, bool down) override {
    Device *d = slot_device(slot);
    if (d == nullptr || index < 0 ||
        static_cast<std::size_t>(index) >= d->raw_buttons.size()) {
      return;
    }
    d->raw_buttons[static_cast<std::size_t>(index)] = down ? 1 : 0;
  }

  void set_raw_axis(int slot, int index, float value) override {
    Device *d = slot_device(slot);
    if (d == nullptr || index < 0 ||
        static_cast<std::size_t>(index) >= d->raw_axes.size()) {
      return;
    }
    d->raw_axes[static_cast<std::size_t>(index)] = clamp_unit(value);
  }

  void set_raw_hat(int slot, int index, std::uint8_t mask) override {
    Device *d = slot_device(slot);
    if (d == nullptr || index < 0 ||
        static_cast<std::size_t>(index) >= d->raw_hats.size()) {
      return;
    }
    d->raw_hats[static_cast<std::size_t>(index)] = mask;
  }

  void set_button(int slot, Button b, bool down) override {
    Device *d = slot_device(slot);
    if (d == nullptr) {
      return;
    }
    d->buttons[static_cast<std::size_t>(b)] = down ? 1 : 0;
  }

  void set_axis(int slot, Axis a, float value) override {
    Device *d = slot_device(slot);
    if (d == nullptr) {
      return;
    }
    d->axes[static_cast<std::size_t>(a)] =
        (a == Axis::LeftTrigger || a == Axis::RightTrigger) ? clamp01(value)
                                                            : clamp_unit(value);
  }

  void set_power(int slot, PowerLevel level) override {
    if (Device *d = slot_device(slot)) {
      d->info.power = level;
    }
  }

  DeviceId device_id_for_slot(int slot) const override {
    if (slot < 0 || static_cast<std::size_t>(slot) >= slots.size()) {
      return kInvalidDevice;
    }
    const Device &d = slots[static_cast<std::size_t>(slot)];
    return d.live ? d.id : kInvalidDevice;
  }

  bool hardware_claimed(std::uint16_t vendor, std::uint16_t product,
                        const std::string &unique_key) const override {
    // Counted rather than matched, because two identical pads are common
    // and neither the mapped nor the raw backend can be relied on to
    // report the same unique key for the same physical device. If the
    // mapped backends already account for as many of this product as the
    // raw backends have taken, the next one is a duplicate.
    int mapped = 0;
    int raw = 0;
    for (const Device &d : slots) {
      if (!d.live || d.info.vendor != vendor || d.info.product != product) {
        continue;
      }
      if (!d.unique_key.empty() && d.unique_key == unique_key) {
        return true;
      }
      (d.backend_mapped ? mapped : raw) += 1;
    }
    return mapped > raw;
  }

  // --- deadzone ---------------------------------------------------------

  [[nodiscard]] float processed_axis(const Device &d, Axis a) const {
    const float raw = d.axes[static_cast<std::size_t>(a)];
    if (a == Axis::LeftTrigger || a == Axis::RightTrigger) {
      return clamp01(apply_1d_deadzone(raw, config.trigger_deadzone));
    }
    const float signed_raw =
        (config.y_up && (a == Axis::LeftY || a == Axis::RightY)) ? -raw : raw;
    switch (config.deadzone_mode) {
    case DeadzoneMode::None:
      return signed_raw;
    case DeadzoneMode::PerAxis:
      return apply_1d_deadzone(signed_raw, config.stick_deadzone);
    case DeadzoneMode::RadialScaled:
      break;
    }
    const Vec2 v = processed_stick(d, stick_of(a));
    return (a == Axis::LeftX || a == Axis::RightX) ? v.x : v.y;
  }

  [[nodiscard]] Vec2 processed_stick(const Device &d, Stick s) const {
    const StickAxes ax = axes_of(s);
    float x = d.axes[static_cast<std::size_t>(ax.x)];
    float y = d.axes[static_cast<std::size_t>(ax.y)];
    if (config.y_up) {
      y = -y;
    }
    switch (config.deadzone_mode) {
    case DeadzoneMode::None:
      return Vec2{x, y};
    case DeadzoneMode::PerAxis:
      return Vec2{apply_1d_deadzone(x, config.stick_deadzone),
                  apply_1d_deadzone(y, config.stick_deadzone)};
    case DeadzoneMode::RadialScaled:
      break;
    }
    const float dz = config.stick_deadzone;
    const float len = std::sqrt(x * x + y * y);
    if (len <= dz || len <= 0.0f) {
      return Vec2{0.0f, 0.0f};
    }
    // Clamped before rescaling, not after. Hardware does report past 1 on
    // the diagonals, and without the clamp that arrives downstream as a
    // throttle above full.
    const float capped = len > 1.0f ? 1.0f : len;
    const float scaled = dz >= 1.0f ? 0.0f : (capped - dz) / (1.0f - dz);
    return Vec2{(x / len) * scaled, (y / len) * scaled};
  }

  // --- frame ------------------------------------------------------------

  void update() {
    events.clear();

    bool rescan = config.hotplug_interval <= 0.0f || first_update;
    const auto now = std::chrono::steady_clock::now();
    if (!rescan) {
      const auto elapsed =
          std::chrono::duration<float>(now - last_scan).count();
      rescan = elapsed >= config.hotplug_interval;
    }
    if (rescan) {
      last_scan = now;
    }
    first_update = false;

    for (Device &d : slots) {
      if (d.live) {
        d.prev_buttons = d.buttons;
        for (std::size_t i = 0; i < kAxisCount; ++i) {
          d.prev_processed[i] = processed_axis(d, static_cast<Axis>(i));
        }
      }
    }

    for (Backend *b : backends) {
      current_backend = b;
      b->poll(rescan);
    }
    current_backend = nullptr;

    for (Device &d : slots) {
      if (!d.live || d.backend_mapped) {
        continue;
      }
      if (d.mapping == nullptr) {
        // Readable but not understood. Its raw inputs are still being
        // updated, which is what a binding screen reads; the mapped view
        // stays empty rather than guessing, because a wrong guess is worse
        // than nothing for a player trying to hit "jump".
        d.buttons.fill(0);
        d.axes.fill(0.0f);
        continue;
      }
      RawInputs raw;
      raw.buttons = d.raw_buttons.data();
      raw.axes = d.raw_axes.data();
      raw.hats = d.raw_hats.data();
      raw.button_count = static_cast<int>(d.raw_buttons.size());
      raw.axis_count = static_cast<int>(d.raw_axes.size());
      raw.hat_count = static_cast<int>(d.raw_hats.size());
      detail::apply_mapping(*d.mapping, raw, d.buttons.data(), d.axes.data());
    }

    for (Device &d : slots) {
      if (!d.live) {
        continue;
      }
      for (std::size_t i = 0; i < kButtonCount; ++i) {
        if (d.buttons[i] == d.prev_buttons[i]) {
          continue;
        }
        events.push_back(Event{
            d.buttons[i] != 0 ? Event::Type::ButtonDown : Event::Type::ButtonUp,
            d.id, static_cast<Button>(i), Axis::LeftX, 0.0f});
      }
      for (std::size_t i = 0; i < kAxisCount; ++i) {
        const float v = processed_axis(d, static_cast<Axis>(i));
        // A stick sitting still still jitters in its last bit, and an
        // event per frame per axis for that is noise a game has to filter
        // out again.
        if (std::fabs(v - d.prev_processed[i]) < 0.0005f) {
          continue;
        }
        events.push_back(Event{Event::Type::AxisMotion, d.id, Button::South,
                               static_cast<Axis>(i), v});
      }
    }
  }

  void load_configured_databases() {
    if (config.load_builtin_db) {
      const char *db = builtin_controller_db();
      if (db != nullptr && db[0] != '\0') {
        mappings.add_many(db);
      }
    }
    if (!config.db_path.empty()) {
      bool ok = false;
      const std::string text = read_file(config.db_path, ok);
      if (ok) {
        mappings.add_many(text);
      }
    }
    if (!config.db_env_var.empty()) {
      if (const char *path = std::getenv(config.db_env_var.c_str())) {
        bool ok = false;
        const std::string text = read_file(path, ok);
        if (ok) {
          mappings.add_many(text);
        }
      }
    }
  }
};

Context::Context(const Config &config) : impl_(new Impl(config)) {
  impl_->load_configured_databases();
  impl_->backends = make_backends();
  // A backend that will not start is dropped, not fatal: no permission on
  // /dev/input is a machine's ordinary state, and the game still runs.
  std::vector<Backend *> live;
  for (Backend *b : impl_->backends) {
    impl_->current_backend = b;
    if (b->init(impl_.get())) {
      live.push_back(b);
    } else {
      b->shutdown();
      delete b;
    }
  }
  impl_->current_backend = nullptr;
  impl_->backends = std::move(live);
}

Context::~Context() = default;
Context::Context(Context &&) noexcept = default;
Context &Context::operator=(Context &&) noexcept = default;

namespace {
Config g_shared_config;
bool g_shared_built = false;
} // namespace

Context &Context::shared() {
  static Context ctx(g_shared_config);
  g_shared_built = true;
  return ctx;
}

bool Context::configure(const Config &config) {
  if (g_shared_built) {
    return false;
  }
  g_shared_config = config;
  return true;
}

void Context::update() { impl_->update(); }

const std::vector<Event> &Context::events() const noexcept {
  return impl_->events;
}

std::size_t Context::device_count() const noexcept {
  return impl_->live_ids.size();
}

const std::vector<DeviceId> &Context::devices() const noexcept {
  return impl_->live_ids;
}

DeviceId Context::first_device() const noexcept {
  return impl_->live_ids.empty() ? kInvalidDevice : impl_->live_ids.front();
}

bool Context::connected(DeviceId id) const noexcept {
  return impl_->find(id) != nullptr;
}

DeviceInfo Context::info(DeviceId id) const {
  const Device *d = impl_->find(id);
  return d != nullptr ? d->info : DeviceInfo{};
}

bool Context::down(DeviceId id, Button b) const noexcept {
  const Device *d = impl_->find(id);
  const auto i = static_cast<std::size_t>(b);
  return d != nullptr && i < kButtonCount && d->buttons[i] != 0;
}

bool Context::pressed(DeviceId id, Button b) const noexcept {
  const Device *d = impl_->find(id);
  const auto i = static_cast<std::size_t>(b);
  return d != nullptr && i < kButtonCount && d->buttons[i] != 0 &&
         d->prev_buttons[i] == 0;
}

bool Context::released(DeviceId id, Button b) const noexcept {
  const Device *d = impl_->find(id);
  const auto i = static_cast<std::size_t>(b);
  return d != nullptr && i < kButtonCount && d->buttons[i] == 0 &&
         d->prev_buttons[i] != 0;
}

float Context::axis(DeviceId id, Axis a) const noexcept {
  const Device *d = impl_->find(id);
  if (d == nullptr || static_cast<std::size_t>(a) >= kAxisCount) {
    return 0.0f;
  }
  return impl_->processed_axis(*d, a);
}

float Context::axis_raw(DeviceId id, Axis a) const noexcept {
  const Device *d = impl_->find(id);
  if (d == nullptr || static_cast<std::size_t>(a) >= kAxisCount) {
    return 0.0f;
  }
  return d->axes[static_cast<std::size_t>(a)];
}

Vec2 Context::stick(DeviceId id, Stick s) const noexcept {
  const Device *d = impl_->find(id);
  return d != nullptr ? impl_->processed_stick(*d, s) : Vec2{};
}

bool Context::active(DeviceId id) const noexcept {
  const Device *d = impl_->find(id);
  if (d == nullptr) {
    return false;
  }
  for (std::uint8_t b : d->buttons) {
    if (b != 0) {
      return true;
    }
  }
  for (Stick s : {Stick::Left, Stick::Right}) {
    const Vec2 v = impl_->processed_stick(*d, s);
    if (v.x != 0.0f || v.y != 0.0f) {
      return true;
    }
  }
  // An unmapped pad has no buttons as far as the mapped view is concerned,
  // so fall through to the raw one -- otherwise "press anything to join"
  // can never be satisfied on hardware nobody has a mapping for.
  if (!d->info.mapped) {
    for (std::uint8_t b : d->raw_buttons) {
      if (b != 0) {
        return true;
      }
    }
  }
  return false;
}

bool Context::raw_button(DeviceId id, int index) const noexcept {
  const Device *d = impl_->find(id);
  return d != nullptr && index >= 0 &&
         static_cast<std::size_t>(index) < d->raw_buttons.size() &&
         d->raw_buttons[static_cast<std::size_t>(index)] != 0;
}

float Context::raw_axis(DeviceId id, int index) const noexcept {
  const Device *d = impl_->find(id);
  if (d == nullptr || index < 0 ||
      static_cast<std::size_t>(index) >= d->raw_axes.size()) {
    return 0.0f;
  }
  return d->raw_axes[static_cast<std::size_t>(index)];
}

std::uint8_t Context::raw_hat(DeviceId id, int index) const noexcept {
  const Device *d = impl_->find(id);
  if (d == nullptr || index < 0 ||
      static_cast<std::size_t>(index) >= d->raw_hats.size()) {
    return 0;
  }
  return d->raw_hats[static_cast<std::size_t>(index)];
}

// Whether a newly requested (low, high, duration_ms) triple asks for
// anything different from what was last actually sent. duration_ms is
// compared exactly, not fuzzily: it is when the rumble will next change on
// its own (a timed rumble re-armed with the same intensities but a fresh
// duration is not a no-op, even though the intensities alone did not
// move), so a real difference there always has to reach the backend.
bool rumble_state_matches(float last_low, float last_high,
                          std::uint32_t last_duration_ms, float low, float high,
                          std::uint32_t duration_ms) noexcept {
  return last_duration_ms == duration_ms && std::abs(last_low - low) < 0.01f &&
         std::abs(last_high - high) < 0.01f;
}

bool Context::rumble(DeviceId id, float low, float high,
                     std::uint32_t duration_ms) {
  Device *d = impl_->find(id);
  if (d == nullptr || d->backend == nullptr || !d->info.caps.rumble) {
    return false;
  }
  const float lo = clamp01(low);
  const float hi = clamp01(high);
  // Skips the backend call entirely when nothing would change -- the
  // point being a caller that drives rumble continuously (a camera-shake
  // system calling this every frame, say) does not cost a fresh
  // packet/report/ioctl on every one of those frames, on every platform,
  // rather than each backend having to remember to do this itself.
  if (d->has_rumble &&
      rumble_state_matches(d->rumble_low, d->rumble_high, d->rumble_duration_ms,
                           lo, hi, duration_ms)) {
    return true;
  }
  if (!d->backend->rumble(d->handle, lo, hi, duration_ms)) {
    return false;
  }
  d->has_rumble = true;
  d->rumble_low = lo;
  d->rumble_high = hi;
  d->rumble_duration_ms = duration_ms;
  return true;
}

bool Context::rumble_triggers(DeviceId id, float left, float right,
                              std::uint32_t duration_ms) {
  Device *d = impl_->find(id);
  if (d == nullptr || d->backend == nullptr || !d->info.caps.trigger_rumble) {
    return false;
  }
  const float l = clamp01(left);
  const float r = clamp01(right);
  if (d->has_trigger_rumble &&
      rumble_state_matches(d->trigger_rumble_left, d->trigger_rumble_right,
                           d->trigger_rumble_duration_ms, l, r, duration_ms)) {
    return true;
  }
  if (!d->backend->rumble_triggers(d->handle, l, r, duration_ms)) {
    return false;
  }
  d->has_trigger_rumble = true;
  d->trigger_rumble_left = l;
  d->trigger_rumble_right = r;
  d->trigger_rumble_duration_ms = duration_ms;
  return true;
}

void Context::stop_rumble(DeviceId id) {
  // Through rumble() itself rather than the backend directly, so this
  // gets the same dedup: a caller that calls stop_rumble() every frame
  // while nothing has been rumbling costs nothing past the first call.
  rumble(id, 0.0f, 0.0f, 0);
}

bool Context::set_led(DeviceId id, std::uint8_t r, std::uint8_t g,
                      std::uint8_t b) {
  Device *d = impl_->find(id);
  if (d == nullptr || d->backend == nullptr || !d->info.caps.led) {
    return false;
  }
  return d->backend->set_led(d->handle, r, g, b);
}

bool Context::add_mapping(const std::string &line) {
  if (!impl_->mappings.add(line)) {
    return false;
  }
  impl_->refresh_all_mappings();
  return true;
}

int Context::add_mappings_from_string(const std::string &text) {
  const int added = impl_->mappings.add_many(text);
  if (added > 0) {
    impl_->refresh_all_mappings();
  }
  return added;
}

int Context::add_mappings_from_file(const std::string &path) {
  bool ok = false;
  const std::string text = read_file(path, ok);
  if (!ok) {
    return -1;
  }
  return add_mappings_from_string(text);
}

std::string Context::mapping_for(DeviceId id) const {
  const Device *d = impl_->find(id);
  if (d == nullptr || d->mapping == nullptr) {
    return {};
  }
  return d->mapping->source;
}

std::size_t Context::mapping_count() const noexcept {
  return impl_->mappings.size();
}

const Config &Context::config() const noexcept { return impl_->config; }

void Context::set_config(const Config &config) {
  const Config old = impl_->config;
  impl_->config = config;
  // The parts that were consumed at construction stay as they were, so
  // that a caller who copies config(), edits a deadzone and hands it back
  // does not accidentally re-declare which databases were loaded.
  impl_->config.load_builtin_db = old.load_builtin_db;
  impl_->config.db_path = old.db_path;
  impl_->config.db_env_var = old.db_env_var;
}

// ---------------------------------------------------------------------------
// Free functions
// ---------------------------------------------------------------------------

const char *platform_name() noexcept { return GPPLUS_PLATFORM_NAME; }

const char *const *compiled_backends() noexcept { return kBackendIds; }

const char *to_string(Button b) noexcept {
  static const char *kNames[] = {
      "South",         "East",    "West",      "North",      "Back",
      "Guide",         "Start",   "LeftStick", "RightStick", "LeftShoulder",
      "RightShoulder", "DpadUp",  "DpadDown",  "DpadLeft",   "DpadRight",
      "Misc1",         "Paddle1", "Paddle2",   "Paddle3",    "Paddle4",
      "Touchpad"};
  const auto i = static_cast<std::size_t>(b);
  return i < kButtonCount ? kNames[i] : "?";
}

const char *to_string(Axis a) noexcept {
  static const char *kNames[] = {"LeftX",  "LeftY",       "RightX",
                                 "RightY", "LeftTrigger", "RightTrigger"};
  const auto i = static_cast<std::size_t>(a);
  return i < kAxisCount ? kNames[i] : "?";
}

const char *db_name(Button b) noexcept {
  static const char *kNames[] = {"a",
                                 "b",
                                 "x",
                                 "y",
                                 "back",
                                 "guide",
                                 "start",
                                 "leftstick",
                                 "rightstick",
                                 "leftshoulder",
                                 "rightshoulder",
                                 "dpup",
                                 "dpdown",
                                 "dpleft",
                                 "dpright",
                                 "misc1",
                                 "paddle1",
                                 "paddle2",
                                 "paddle3",
                                 "paddle4",
                                 "touchpad"};
  const auto i = static_cast<std::size_t>(b);
  return i < kButtonCount ? kNames[i] : "";
}

const char *db_name(Axis a) noexcept {
  static const char *kNames[] = {"leftx",  "lefty",       "rightx",
                                 "righty", "lefttrigger", "righttrigger"};
  const auto i = static_cast<std::size_t>(a);
  return i < kAxisCount ? kNames[i] : "";
}

} // namespace gpp
