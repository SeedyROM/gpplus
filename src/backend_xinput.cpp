// Windows: XInput.
//
// A *mapped* backend: XInput's whole premise is that every device it
// reports is an Xbox-layout controller, so the database is not consulted
// for these. That premise is also its limit -- four slots, no device
// identity, and nothing that is not an Xbox pad or pretending to be one.
// Everything else on Windows (DirectInput and Raw HID devices: arcade
// sticks, flight gear, older pads) is not covered here; that is a second
// backend, and the raw-device path the database feeds already exists for
// it.
//
// Compiled only where CMake found and probed an XInput import library.

#include "backend.hpp"
#include "mapping.hpp"

#include <array>
#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <xinput.h>

namespace gpp::detail {
namespace {

constexpr int kMaxSlots = XUSER_MAX_COUNT; // four, and always has been

struct Pad {
  int slot = -1;
  bool connected = false;
  DWORD last_packet = 0;
  /// Which XInput user index this is, so rumble reaches the right pad.
  DWORD user = 0;
  ULONGLONG rumble_until = 0;
  bool rumble_timed = false;
};

/**
 * @brief XInputGetBatteryInformation, if this machine has it.
 *
 * Resolved at runtime rather than linked. xinput9_1_0 -- the
 * redistributable stub that is present on everything back to Vista, and
 * what CMake falls back to -- does not export it, so linking the symbol
 * would trade battery reporting for a build that does not link at all on
 * the configuration most likely to need the fallback.
 */
using BatteryFn = DWORD(WINAPI *)(DWORD, BYTE, XINPUT_BATTERY_INFORMATION *);

BatteryFn battery_function() {
  static BatteryFn resolved = [] {
    for (const wchar_t *dll : {L"xinput1_4.dll", L"xinput1_3.dll"}) {
      HMODULE module = GetModuleHandleW(dll);
      if (module == nullptr) {
        module = LoadLibraryW(dll);
      }
      if (module == nullptr) {
        continue;
      }
      if (auto *fn = reinterpret_cast<BatteryFn>(
              GetProcAddress(module, "XInputGetBatteryInformation"))) {
        return fn;
      }
    }
    return static_cast<BatteryFn>(nullptr);
  }();
  return resolved;
}

PowerLevel power_of(DWORD user) {
  BatteryFn fn = battery_function();
  if (fn == nullptr) {
    return PowerLevel::Unknown;
  }
  XINPUT_BATTERY_INFORMATION info = {};
  if (fn(user, BATTERY_DEVTYPE_GAMEPAD, &info) != ERROR_SUCCESS) {
    return PowerLevel::Unknown;
  }
  if (info.BatteryType == BATTERY_TYPE_WIRED) {
    return PowerLevel::Wired;
  }
  if (info.BatteryType == BATTERY_TYPE_DISCONNECTED ||
      info.BatteryType == BATTERY_TYPE_UNKNOWN) {
    return PowerLevel::Unknown;
  }
  switch (info.BatteryLevel) {
  case BATTERY_LEVEL_EMPTY:
    return PowerLevel::Empty;
  case BATTERY_LEVEL_LOW:
    return PowerLevel::Low;
  case BATTERY_LEVEL_MEDIUM:
    return PowerLevel::Medium;
  case BATTERY_LEVEL_FULL:
    return PowerLevel::Full;
  default:
    return PowerLevel::Unknown;
  }
}

float trigger(BYTE v) { return static_cast<float>(v) / 255.0f; }

/// XInput's sticks are -32768..32767 and up-positive; the database, and
/// everything else here, is down-positive. The negation happens once, in
/// this function, for the same reason it happens once in every other
/// backend: a flip applied anywhere else inverts exactly one device.
float stick(SHORT v) {
  const float f = static_cast<float>(v) / (v < 0 ? 32768.0f : 32767.0f);
  return f < -1.0f ? -1.0f : (f > 1.0f ? 1.0f : f);
}

class XInputBackend final : public Backend {
public:
  const char *name() const noexcept override { return "xinput"; }

  bool init(Host *host) override {
    host_ = host;
    for (DWORD i = 0; i < static_cast<DWORD>(kMaxSlots); ++i) {
      pads_[i].user = i;
    }
    return true;
  }

  void shutdown() override {
    for (Pad &p : pads_) {
      if (p.connected) {
        XINPUT_VIBRATION off = {0, 0};
        XInputSetState(p.user, &off);
      }
    }
    host_ = nullptr;
  }

  void poll(bool rescan) override {
    const ULONGLONG now = GetTickCount64();
    for (Pad &p : pads_) {
      XINPUT_STATE state = {};
      // XInputGetState on an empty slot is famously expensive -- it walks
      // the USB tree and can cost milliseconds. Polling empty slots only
      // on the hotplug tick is the difference between a free backend and
      // a frame-rate bug that only appears with one controller plugged in.
      if (!p.connected && !rescan) {
        continue;
      }
      const DWORD result = XInputGetState(p.user, &state);
      if (result != ERROR_SUCCESS) {
        if (p.connected) {
          p.connected = false;
          host_->remove_device(p.slot);
          p.slot = -1;
        }
        continue;
      }
      if (!p.connected) {
        add(p);
        if (p.slot < 0) {
          continue;
        }
      }
      if (rescan) {
        // A system query, paced with the hotplug tick rather than run
        // every frame.
        host_->set_power(p.slot, power_of(p.user));
      }
      if (p.rumble_timed && now >= p.rumble_until) {
        p.rumble_timed = false;
        XINPUT_VIBRATION off = {0, 0};
        XInputSetState(p.user, &off);
      }
      // The packet number only changes when something moved, so an
      // untouched pad costs one comparison a frame.
      if (state.dwPacketNumber == p.last_packet) {
        continue;
      }
      p.last_packet = state.dwPacketNumber;
      read(p, state.Gamepad);
    }
  }

  bool rumble(void *handle, float low, float high,
              std::uint32_t duration_ms) override {
    Pad *p = static_cast<Pad *>(handle);
    if (p == nullptr || !p->connected) {
      return false;
    }
    XINPUT_VIBRATION v;
    v.wLeftMotorSpeed = static_cast<WORD>(low * 65535.0f);
    v.wRightMotorSpeed = static_cast<WORD>(high * 65535.0f);
    if (XInputSetState(p->user, &v) != ERROR_SUCCESS) {
      return false;
    }
    p->rumble_timed = duration_ms > 0;
    p->rumble_until = GetTickCount64() + duration_ms;
    return true;
  }

private:
  void add(Pad &p) {
    DeviceDesc desc;
    desc.name = "XInput Controller";
    desc.backend_mapped = true;
    // XInput exposes no vendor or product id at all, by design: every
    // device is "an Xbox controller". The guid is synthesised so that a
    // game writing a per-device settings file has something stable, and
    // the user index is in it so four pads are four keys.
    desc.guid = make_guid(0x03, 0x045e, 0x028e, 0, desc.name, 'x',
                          static_cast<std::uint8_t>(p.user));
    desc.caps.rumble = true;
    desc.handle = &p;
    desc.unique_key = std::string("xinput:") + std::to_string(p.user);

    const int slot = host_->add_device(desc);
    if (slot < 0) {
      return;
    }
    p.slot = slot;
    p.connected = true;
    p.last_packet = 0;
    host_->set_power(slot, power_of(p.user));
  }

  void read(Pad &p, const XINPUT_GAMEPAD &g) {
    const auto button = [&](Button b, WORD mask) {
      host_->set_button(p.slot, b, (g.wButtons & mask) != 0);
    };
    button(Button::South, XINPUT_GAMEPAD_A);
    button(Button::East, XINPUT_GAMEPAD_B);
    button(Button::West, XINPUT_GAMEPAD_X);
    button(Button::North, XINPUT_GAMEPAD_Y);
    button(Button::Back, XINPUT_GAMEPAD_BACK);
    button(Button::Start, XINPUT_GAMEPAD_START);
    button(Button::LeftShoulder, XINPUT_GAMEPAD_LEFT_SHOULDER);
    button(Button::RightShoulder, XINPUT_GAMEPAD_RIGHT_SHOULDER);
    button(Button::LeftStick, XINPUT_GAMEPAD_LEFT_THUMB);
    button(Button::RightStick, XINPUT_GAMEPAD_RIGHT_THUMB);
    button(Button::DpadUp, XINPUT_GAMEPAD_DPAD_UP);
    button(Button::DpadDown, XINPUT_GAMEPAD_DPAD_DOWN);
    button(Button::DpadLeft, XINPUT_GAMEPAD_DPAD_LEFT);
    button(Button::DpadRight, XINPUT_GAMEPAD_DPAD_RIGHT);
    // The Guide button is deliberately not in the public XInput API. The
    // undocumented ordinal 100 (XInputGetStateEx) reports it; reaching for
    // it means a hand-loaded DLL and an ordinal that Microsoft never
    // promised, so it stays unreported here.

    host_->set_axis(p.slot, Axis::LeftX, stick(g.sThumbLX));
    host_->set_axis(p.slot, Axis::LeftY, -stick(g.sThumbLY));
    host_->set_axis(p.slot, Axis::RightX, stick(g.sThumbRX));
    host_->set_axis(p.slot, Axis::RightY, -stick(g.sThumbRY));
    host_->set_axis(p.slot, Axis::LeftTrigger, trigger(g.bLeftTrigger));
    host_->set_axis(p.slot, Axis::RightTrigger, trigger(g.bRightTrigger));
  }

  Host *host_ = nullptr;
  std::array<Pad, kMaxSlots> pads_{};
};

} // namespace

Backend *make_backend_xinput() { return new XInputBackend(); }

} // namespace gpp::detail
