// Linux: evdev, straight from /dev/input/event*.
//
// evdev rather than the older joydev (/dev/input/js*) because joydev is a
// compatibility shim with no force feedback, no device identity beyond a
// name, and a button ordering that does not match what the mapping
// database was generated against. Everything modern -- SDL included --
// reads evdev, and the numbering below is deliberately identical to SDL's,
// because a database line is nothing but a statement about which number
// means which button.
//
// Compiled only where CMake's probe built against <linux/input.h>; there
// is no #ifdef in this file.

#include "backend.hpp"
#include "mapping.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace gpp::detail {
namespace {

constexpr int kBitsPerLong = sizeof(unsigned long) * 8;

constexpr std::size_t bit_words(std::size_t bits) {
  return (bits + kBitsPerLong - 1) / kBitsPerLong;
}

bool test_bit(int bit, const unsigned long *array) {
  return (array[bit / kBitsPerLong] >> (bit % kBitsPerLong)) & 1UL;
}

struct HatState {
  int x = 0;
  int y = 0;
};

struct AxisInfo {
  int minimum = 0;
  int maximum = 0;
};

struct Entry {
  int fd = -1;
  int slot = -1;
  std::string path;
  /// `/sys/class/power_supply/<name>` for this pad, or empty when the
  /// kernel does not publish one -- which is the normal case for a wired
  /// device and for plenty of wireless ones whose driver never reports it.
  std::string power_supply;
  bool writable = false;
  /// The kernel's effect id, or -1 for "nothing uploaded yet". Re-uploaded
  /// under the same id when the intensity changes, because an effect slot
  /// is a scarce resource and leaking one per rumble exhausts the device
  /// after a few dozen explosions.
  int effect_id = -1;
  /// Kernel key code -> our button index, and likewise for axes. -1 for
  /// codes this device does not have.
  std::vector<std::int16_t> key_map;
  std::vector<std::int16_t> abs_map;
  std::vector<AxisInfo> axis_info;
  std::vector<HatState> hats;
};

/// SDL's button ordering, reproduced exactly: the gamepad and joystick
/// codes first, in numeric order, then everything below BTN_JOYSTICK --
/// the mouse and misc buttons some pads also claim. Reasonable-looking
/// alternatives (sorting, or a single pass) all renumber the buttons and
/// silently break every mapping in the database.
void build_key_map(Entry &e, const unsigned long *keybit) {
  e.key_map.assign(KEY_MAX + 1, -1);
  std::int16_t index = 0;
  for (int code = BTN_JOYSTICK; code < KEY_MAX; ++code) {
    if (test_bit(code, keybit)) {
      e.key_map[static_cast<std::size_t>(code)] = index++;
    }
  }
  for (int code = 0; code < BTN_JOYSTICK; ++code) {
    if (test_bit(code, keybit)) {
      e.key_map[static_cast<std::size_t>(code)] = index++;
    }
  }
}

void build_abs_map(Entry &e, const unsigned long *absbit, int fd) {
  e.abs_map.assign(ABS_MAX + 1, -1);
  std::int16_t axis_index = 0;
  int hat_count = 0;
  for (int code = 0; code < ABS_MAX; ++code) {
    if (code >= ABS_HAT0X && code <= ABS_HAT3Y) {
      if (test_bit(code, absbit)) {
        const int hat = (code - ABS_HAT0X) / 2;
        hat_count = std::max(hat_count, hat + 1);
      }
      continue;
    }
    if (!test_bit(code, absbit)) {
      continue;
    }
    struct input_absinfo info {};
    if (ioctl(fd, EVIOCGABS(code), &info) < 0) {
      continue;
    }
    e.abs_map[static_cast<std::size_t>(code)] = axis_index++;
    e.axis_info.push_back(AxisInfo{info.minimum, info.maximum});
  }
  e.hats.assign(static_cast<std::size_t>(hat_count), HatState{});
}

/// Finds the power supply the kernel associates with this input device,
/// by walking from the event node to its device directory in sysfs. Pads
/// that report a battery hang one off `device/power_supply/<name>`.
std::string find_power_supply(const std::string &event_path) {
  const std::size_t slash = event_path.rfind('/');
  if (slash == std::string::npos) {
    return {};
  }
  const std::string node = event_path.substr(slash + 1);
  const std::string dir = "/sys/class/input/" + node + "/device/power_supply";
  DIR *handle = opendir(dir.c_str());
  if (handle == nullptr) {
    return {};
  }
  std::string found;
  while (struct dirent *entry = readdir(handle)) {
    if (entry->d_name[0] == '.') {
      continue;
    }
    found = dir + "/" + entry->d_name;
    break;
  }
  closedir(handle);
  return found;
}

/// One small sysfs file, trimmed. Empty on anything unreadable, which
/// includes the ordinary race of a device unplugged between the scan and
/// the read.
std::string read_sysfs(const std::string &path) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return {};
  }
  char buffer[64] = {};
  const ssize_t n = read(fd, buffer, sizeof(buffer) - 1);
  close(fd);
  if (n <= 0) {
    return {};
  }
  std::string value(buffer, static_cast<std::size_t>(n));
  while (!value.empty() && (value.back() == '\n' || value.back() == ' ')) {
    value.pop_back();
  }
  return value;
}

/// The same thresholds the GameController backend uses, so that a game
/// showing a battery icon does not have to know which backend it is on.
PowerLevel level_from_capacity(int capacity) {
  if (capacity > 70) {
    return PowerLevel::Full;
  }
  if (capacity > 40) {
    return PowerLevel::Medium;
  }
  if (capacity > 10) {
    return PowerLevel::Low;
  }
  return PowerLevel::Empty;
}

float normalize(const AxisInfo &info, int value) {
  if (info.maximum <= info.minimum) {
    return 0.0f;
  }
  const double span = static_cast<double>(info.maximum) - info.minimum;
  const double t = (static_cast<double>(value) - info.minimum) / span;
  return static_cast<float>(t * 2.0 - 1.0);
}

std::uint8_t hat_mask(const HatState &h) {
  std::uint8_t mask = 0;
  if (h.y < 0) {
    mask |= 0x01;
  }
  if (h.x > 0) {
    mask |= 0x02;
  }
  if (h.y > 0) {
    mask |= 0x04;
  }
  if (h.x < 0) {
    mask |= 0x08;
  }
  return mask;
}

class EvdevBackend final : public Backend {
public:
  const char *name() const noexcept override { return "evdev"; }

  bool init(Host *host) override {
    host_ = host;
    return true;
  }

  void shutdown() override {
    for (auto &e : entries_) {
      close_entry(*e);
    }
    entries_.clear();
    host_ = nullptr;
  }

  void poll(bool rescan) override {
    if (rescan) {
      scan();
    }
    for (std::size_t i = entries_.size(); i-- > 0;) {
      if (rescan) {
        // Once a second, not once a frame: these are file reads, and a
        // battery does not move at 60 Hz.
        update_power(*entries_[i]);
      }
      if (!read_events(*entries_[i])) {
        Entry &e = *entries_[i];
        close_entry(e);
        host_->remove_device(e.slot);
        entries_.erase(entries_.begin() + static_cast<long>(i));
      }
    }
  }

  bool rumble(void *handle, float low, float high,
              std::uint32_t duration_ms) override {
    auto *e = static_cast<Entry *>(handle);
    if (e == nullptr || !e->writable) {
      return false;
    }
    if (low <= 0.0f && high <= 0.0f) {
      stop(*e);
      return true;
    }

    struct ff_effect effect {};
    effect.type = FF_RUMBLE;
    effect.id = static_cast<std::int16_t>(e->effect_id);
    effect.replay.length =
        duration_ms == 0 ? 0xFFFFu
                         : static_cast<std::uint16_t>(
                               std::min<std::uint32_t>(duration_ms, 0xFFFFu));
    effect.replay.delay = 0;
    effect.u.rumble.strong_magnitude =
        static_cast<std::uint16_t>(low * 65535.0f);
    effect.u.rumble.weak_magnitude =
        static_cast<std::uint16_t>(high * 65535.0f);

    if (ioctl(e->fd, EVIOCSFF, &effect) < 0) {
      // A stale id -- after a suspend, or a driver that dropped the
      // effect -- is refused. Forget it and let the next call upload a
      // fresh one rather than rumbling never again.
      e->effect_id = -1;
      return false;
    }
    e->effect_id = effect.id;

    struct input_event play {};
    play.type = EV_FF;
    play.code = static_cast<std::uint16_t>(effect.id);
    play.value = 1;
    return write(e->fd, &play, sizeof(play)) ==
           static_cast<ssize_t>(sizeof(play));
  }

private:
  static void stop(Entry &e) {
    if (e.effect_id < 0) {
      return;
    }
    struct input_event play {};
    play.type = EV_FF;
    play.code = static_cast<std::uint16_t>(e.effect_id);
    play.value = 0;
    (void)!write(e.fd, &play, sizeof(play));
  }

  void scan() {
    DIR *dir = opendir("/dev/input");
    if (dir == nullptr) {
      return;
    }
    while (struct dirent *entry = readdir(dir)) {
      if (std::strncmp(entry->d_name, "event", 5) != 0) {
        continue;
      }
      std::string path = std::string("/dev/input/") + entry->d_name;
      if (find(path) != nullptr) {
        continue;
      }
      add(path);
    }
    closedir(dir);
  }

  Entry *find(const std::string &path) {
    for (auto &e : entries_) {
      if (e->path == path) {
        return e.get();
      }
    }
    return nullptr;
  }

  void add(const std::string &path) {
    // Read-write first because force feedback needs it, read-only second
    // because a machine whose udev rules only grant reads should still get
    // a working pad, just a silent one.
    bool writable = true;
    int fd = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
      writable = false;
      fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    }
    if (fd < 0) {
      return;
    }

    unsigned long evbit[bit_words(EV_MAX + 1)] = {};
    unsigned long keybit[bit_words(KEY_MAX + 1)] = {};
    unsigned long absbit[bit_words(ABS_MAX + 1)] = {};
    unsigned long ffbit[bit_words(FF_MAX + 1)] = {};
    if (ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), evbit) < 0 ||
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit) < 0 ||
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbit)), absbit) < 0) {
      close(fd);
      return;
    }
    if (!is_gamepad(evbit, keybit, absbit)) {
      close(fd);
      return;
    }

    struct input_id id {};
    if (ioctl(fd, EVIOCGID, &id) < 0) {
      close(fd);
      return;
    }
    char name_buf[256] = {};
    if (ioctl(fd, EVIOCGNAME(sizeof(name_buf) - 1), name_buf) < 0) {
      std::snprintf(name_buf, sizeof(name_buf), "Unknown Gamepad");
    }

    auto entry = std::make_unique<Entry>();
    entry->fd = fd;
    entry->path = path;
    entry->writable = writable;
    build_key_map(*entry, keybit);
    build_abs_map(*entry, absbit, fd);

    DeviceDesc desc;
    desc.name = name_buf;
    desc.vendor = id.vendor;
    desc.product = id.product;
    desc.version = id.version;
    // The bus type goes in as the kernel reports it -- USB and Bluetooth
    // give the same pad two different guids, and the database has separate
    // lines for both, because a pad really does renumber its buttons
    // depending on how it is connected.
    desc.guid =
        make_guid(id.bustype, id.vendor, id.product, id.version, desc.name);
    desc.raw_button_count = count_mapped(entry->key_map);
    desc.raw_axis_count = static_cast<int>(entry->axis_info.size());
    desc.raw_hat_count = static_cast<int>(entry->hats.size());
    desc.caps.rumble = writable &&
                       ioctl(fd, EVIOCGBIT(EV_FF, sizeof(ffbit)), ffbit) >= 0 &&
                       test_bit(FF_RUMBLE, ffbit);
    desc.handle = entry.get();
    desc.unique_key = path;

    if (host_->hardware_claimed(desc.vendor, desc.product, desc.unique_key)) {
      close(fd);
      return;
    }
    const int slot = host_->add_device(desc);
    if (slot < 0) {
      close(fd);
      return;
    }
    entry->slot = slot;
    entry->power_supply = find_power_supply(path);
    seed_state(*entry);
    update_power(*entry);
    entries_.push_back(std::move(entry));
  }

  static bool is_gamepad(const unsigned long *evbit,
                         const unsigned long *keybit,
                         const unsigned long *absbit) {
    if (!test_bit(EV_ABS, evbit) || !test_bit(EV_KEY, evbit)) {
      return false;
    }
    if (!test_bit(ABS_X, absbit) || !test_bit(ABS_Y, absbit)) {
      return false;
    }
    // Two sticks and no buttons is a touchpad or an accelerometer. A
    // gamepad claims at least one button from the ranges the kernel
    // reserves for them.
    for (int code = BTN_JOYSTICK; code <= BTN_THUMBR; ++code) {
      if (test_bit(code, keybit)) {
        return true;
      }
    }
    return test_bit(BTN_TRIGGER, keybit) || test_bit(BTN_GAMEPAD, keybit);
  }

  static int count_mapped(const std::vector<std::int16_t> &map) {
    int count = 0;
    for (std::int16_t v : map) {
      if (v >= 0) {
        ++count;
      }
    }
    return count;
  }

  /// The kernel remembers the current state; read it once at open so that
  /// a trigger already held, or a stick already pushed, is true on the
  /// first frame rather than after the player moves it.
  void seed_state(Entry &e) {
    unsigned long keystate[bit_words(KEY_MAX + 1)] = {};
    if (ioctl(e.fd, EVIOCGKEY(sizeof(keystate)), keystate) >= 0) {
      for (int code = 0; code <= KEY_MAX; ++code) {
        const std::int16_t index = e.key_map[static_cast<std::size_t>(code)];
        if (index >= 0) {
          host_->set_raw_button(e.slot, index, test_bit(code, keystate));
        }
      }
    }
    for (int code = 0; code <= ABS_MAX; ++code) {
      struct input_absinfo info {};
      if (code >= ABS_HAT0X && code <= ABS_HAT3Y) {
        if (static_cast<std::size_t>((code - ABS_HAT0X) / 2) < e.hats.size() &&
            ioctl(e.fd, EVIOCGABS(code), &info) >= 0) {
          apply_hat(e, code, info.value);
        }
        continue;
      }
      const std::int16_t index = e.abs_map[static_cast<std::size_t>(code)];
      if (index >= 0 && ioctl(e.fd, EVIOCGABS(code), &info) >= 0) {
        host_->set_raw_axis(
            e.slot, index,
            normalize(e.axis_info[static_cast<std::size_t>(index)],
                      info.value));
      }
    }
  }

  void apply_hat(Entry &e, int code, int value) {
    const std::size_t hat = static_cast<std::size_t>((code - ABS_HAT0X) / 2);
    if (hat >= e.hats.size()) {
      return;
    }
    if (((code - ABS_HAT0X) % 2) == 0) {
      e.hats[hat].x = value;
    } else {
      e.hats[hat].y = value;
    }
    host_->set_raw_hat(e.slot, static_cast<int>(hat), hat_mask(e.hats[hat]));
  }

  /// False means the device is gone and the caller should drop it.
  bool read_events(Entry &e) {
    struct input_event events[32];
    for (;;) {
      const ssize_t n = read(e.fd, events, sizeof(events));
      if (n < 0) {
        // ENODEV is the unplug; EAGAIN is simply nothing new this frame.
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
      }
      if (n == 0) {
        return true;
      }
      const std::size_t count = static_cast<std::size_t>(n) / sizeof(events[0]);
      for (std::size_t i = 0; i < count; ++i) {
        const struct input_event &ev = events[i];
        if (ev.type == EV_KEY) {
          if (ev.code > KEY_MAX) {
            continue;
          }
          const std::int16_t index =
              e.key_map[static_cast<std::size_t>(ev.code)];
          if (index >= 0) {
            // value 2 is autorepeat, which a gamepad should not see and a
            // keyboard-shaped device certainly will. Held is held.
            host_->set_raw_button(e.slot, index, ev.value != 0);
          }
        } else if (ev.type == EV_ABS) {
          if (ev.code >= ABS_HAT0X && ev.code <= ABS_HAT3Y) {
            apply_hat(e, ev.code, ev.value);
          } else if (ev.code <= ABS_MAX) {
            const std::int16_t index =
                e.abs_map[static_cast<std::size_t>(ev.code)];
            if (index >= 0) {
              host_->set_raw_axis(
                  e.slot, index,
                  normalize(e.axis_info[static_cast<std::size_t>(index)],
                            ev.value));
            }
          }
        }
      }
      if (count < 1) {
        return true;
      }
    }
  }

  void update_power(Entry &e) {
    if (e.power_supply.empty()) {
      // No battery node at all. Wired is the honest answer rather than
      // Unknown: the kernel publishes one for anything with a cell in it.
      host_->set_power(e.slot, PowerLevel::Wired);
      return;
    }
    const std::string status = read_sysfs(e.power_supply + "/status");
    if (status == "Charging") {
      host_->set_power(e.slot, PowerLevel::Charging);
      return;
    }
    if (status == "Full") {
      host_->set_power(e.slot, PowerLevel::Full);
      return;
    }
    const std::string capacity = read_sysfs(e.power_supply + "/capacity");
    if (capacity.empty()) {
      // Some drivers report a coarse `capacity_level` string instead.
      const std::string level = read_sysfs(e.power_supply + "/capacity_level");
      if (level == "Full") {
        host_->set_power(e.slot, PowerLevel::Full);
      } else if (level == "Normal" || level == "High") {
        host_->set_power(e.slot, PowerLevel::Medium);
      } else if (level == "Low") {
        host_->set_power(e.slot, PowerLevel::Low);
      } else if (level == "Critical") {
        host_->set_power(e.slot, PowerLevel::Empty);
      } else {
        host_->set_power(e.slot, PowerLevel::Unknown);
      }
      return;
    }
    host_->set_power(e.slot, level_from_capacity(std::atoi(capacity.c_str())));
  }

  static void close_entry(Entry &e) {
    if (e.fd >= 0) {
      stop(e);
      close(e.fd);
      e.fd = -1;
    }
  }

  Host *host_ = nullptr;
  std::vector<std::unique_ptr<Entry>> entries_;
};

} // namespace

Backend *make_backend_evdev() { return new EvdevBackend(); }

} // namespace gpp::detail
