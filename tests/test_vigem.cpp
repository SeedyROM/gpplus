// End-to-end test of the XInput and DirectInput backends against emulated
// hardware.
//
// The Windows counterpart of test_uinput.cpp, and for the same reason: every
// other test stops at the library's own edge, and the two Windows backends
// have only ever been type-checked. This one asks ViGEmBus -- a signed
// kernel driver that emulates game controllers -- for real devices, so that
// from the moment they are plugged in the operating system cannot tell them
// from hardware. XInput and DirectInput then enumerate and read them
// exactly as they would a pad on a USB port.
//
// Two phases, one per backend, each against a device only that backend
// should claim:
//
//   Xbox 360 pad -> XInput. Reported exactly once, which is the check on
//   the DirectInput backend's `ig_` filter: every XInput pad is also a
//   DirectInput device, and without the filter it would arrive twice. Every
//   button, both sticks, both triggers, and rumble coming back out of the
//   driver.
//
//   DualShock 4 -> DirectInput. Mapped by the *real* database line for
//   this pad, not one written for the test. That is deliberate: the
//   database's Windows lines were generated against DirectInput's
//   enumeration order, so `a:b1` meaning Cross is only true if this backend
//   numbers buttons and axes the way SDL does. A wrong order is not an
//   obvious failure in the field, it is every Windows mapping quietly
//   rebinding, and this is the one place it can be seen.
//
// Compiled only when GPPLUS_TEST_VIGEM is on. Exits 77 (ctest's "skip")
// when the driver is not installed, because a developer machine without it
// is not a failure -- CI installs it explicitly.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <ViGEm/Client.h>

#include <gpplus/gamepad.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace {

constexpr int kSkipExitCode = 77;

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char *what, int line) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::printf("FAIL %s:%d: %s\n", __FILE__, line, what);
  }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

/// Everything here is "eventually" rather than "now": a report written to
/// the driver takes a moment to come back out through XInput or
/// DirectInput, and a device takes longer still to enumerate. Polling to a
/// deadline is neither flaky nor slow, where a fixed sleep is one or the
/// other.
template <typename Predicate>
bool wait_until(gpp::Context &ctx, Predicate pred, int timeout_ms = 5000) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    ctx.update();
    if (pred()) {
      return true;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

/// Keeps updating for a fixed time and returns the most devices seen at any
/// moment. For "reported exactly once", which a poll-until-true cannot
/// express: the failure being looked for is the second copy arriving a
/// moment after the first.
std::size_t most_devices_over(gpp::Context &ctx, int duration_ms) {
  std::size_t most = 0;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(duration_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    ctx.update();
    most = std::max(most, ctx.device_count());
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return most;
}

gpp::Config test_config() {
  gpp::Config cfg;
  // The built-in database stays on: the DirectInput phase is only
  // meaningful because it is mapped by the real line. Everything else that
  // could bend a value is switched off so a stick pushed to the end reads
  // as the end.
  cfg.db_env_var.clear();
  cfg.hotplug_interval = 0.0f;
  cfg.deadzone_mode = gpp::DeadzoneMode::None;
  return cfg;
}

/// The first connected device on the named backend, or kInvalidDevice.
/// Matching on the backend is what stops a phase from being satisfied by
/// the wrong one -- an Xbox pad that showed up through DirectInput would
/// otherwise still be "a device".
gpp::DeviceId find_on(const gpp::Context &ctx, const char *backend) {
  for (gpp::DeviceId id : ctx.devices()) {
    if (std::strcmp(ctx.info(id).backend, backend) == 0) {
      return id;
    }
  }
  return gpp::kInvalidDevice;
}

/// One emulated device, unplugged and freed when it goes out of scope, so
/// a phase that fails early cannot leave a pad behind for the next one to
/// trip over.
class VirtualPad {
public:
  VirtualPad(PVIGEM_CLIENT client, PVIGEM_TARGET target)
      : client_(client), target_(target) {}
  VirtualPad(const VirtualPad &) = delete;
  VirtualPad &operator=(const VirtualPad &) = delete;
  ~VirtualPad() {
    unplug();
    vigem_target_free(target_);
  }

  /// Blocks until the driver reports the device fully operational.
  bool plug() {
    error_ = vigem_target_add(client_, target_);
    plugged_ = VIGEM_SUCCESS(error_);
    return plugged_;
  }

  /// Why plug() failed. A bare "refused" says nothing about whether the bus
  /// is out of slots, timed out, or has no driver for this device type --
  /// three different problems with three different fixes.
  VIGEM_ERROR error() const { return error_; }

  void unplug() {
    if (plugged_) {
      vigem_target_remove(client_, target_);
      plugged_ = false;
    }
  }

  PVIGEM_CLIENT client() const { return client_; }
  PVIGEM_TARGET target() const { return target_; }

private:
  PVIGEM_CLIENT client_;
  PVIGEM_TARGET target_;
  bool plugged_ = false;
  VIGEM_ERROR error_ = VIGEM_ERROR_NONE;
};

/// What the game side sent back: the motor speeds the driver was handed.
/// Written from the driver's notification thread, read from the test's.
struct RumbleSeen {
  std::atomic<int> heavy{-1};
  std::atomic<int> light{-1};
};

VOID CALLBACK on_x360_rumble(PVIGEM_CLIENT, PVIGEM_TARGET, UCHAR heavy_motor,
                             UCHAR light_motor, UCHAR, LPVOID user) {
  auto *seen = static_cast<RumbleSeen *>(user);
  seen->heavy = heavy_motor;
  seen->light = light_motor;
}

/// Press, confirm, release, confirm. The release matters as much as the
/// press: a bit read from the wrong place would show up as a button that
/// never lets go, or as a neighbour that lights up instead.
template <typename Send>
void check_button(gpp::Context &ctx, gpp::DeviceId id, gpp::Button button,
                  const char *label, Send send) {
  send(true);
  const bool down = wait_until(ctx, [&] { return ctx.down(id, button); });
  send(false);
  const bool up = wait_until(ctx, [&] { return !ctx.down(id, button); });
  ++g_checks;
  if (!down || !up) {
    ++g_failures;
    std::printf("FAIL %s: %s %s\n", label,
                down ? "pressed" : "never registered as down",
                up ? "" : "and never released");
  }
}

/// Runs `send`, then waits for the axis to land in [lo, hi]. A range rather
/// than a threshold because "released" is as much a claim as "pulled": a
/// trigger at rest that reads 0.5 is the bug this library exists to avoid,
/// and "at least zero" would pass it.
template <typename Send>
void check_axis(gpp::Context &ctx, gpp::DeviceId id, gpp::Axis axis,
                const char *label, float lo, float hi, Send send) {
  send();
  const bool ok = wait_until(ctx, [&] {
    const float v = ctx.axis(id, axis);
    return v >= lo && v <= hi;
  });
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::printf("FAIL %s: reads %.3f, wanted %.2f..%.2f\n", label,
                ctx.axis(id, axis), lo, hi);
  }
}

/// The WinMM joystick API, loaded by hand. It reads the same device by a
/// path that shares nothing with this library, so when a value here
/// disagrees with ours the disagreement is the finding. Declared locally and
/// resolved at runtime because one struct and one function are not worth a
/// header and a link dependency.
struct WinmmJoyInfo {
  DWORD dwSize, dwFlags, dwXpos, dwYpos, dwZpos, dwRpos, dwUpos, dwVpos,
      dwButtons, dwButtonNumber, dwPOV, dwReserved1, dwReserved2;
};
using JoyGetPosEx = UINT(WINAPI *)(UINT, WinmmJoyInfo *);
constexpr DWORD kJoyReturnAll = 0x000000FF;

/// Reads the first joystick WinMM knows about, or returns false.
bool read_winmm(WinmmJoyInfo &out) {
  static JoyGetPosEx fn = [] {
    HMODULE module = LoadLibraryW(L"winmm.dll");
    return module ? reinterpret_cast<JoyGetPosEx>(
                        GetProcAddress(module, "joyGetPosEx"))
                  : nullptr;
  }();
  if (fn == nullptr) {
    return false;
  }
  for (UINT joystick = 0; joystick < 16; ++joystick) {
    out = {};
    out.dwSize = sizeof(out);
    out.dwFlags = kJoyReturnAll;
    if (fn(joystick, &out) == 0) {
      return true;
    }
  }
  return false;
}

/// Sends a report through either of the client's two DualShock entry
/// points. The older one takes the nine-byte report; the newer takes the
/// full 63-byte one, which is what DS4Windows and friends use.
void send_ds4(PVIGEM_CLIENT client, PVIGEM_TARGET target, const DS4_REPORT &r,
              bool extended) {
  if (!extended) {
    vigem_target_ds4_update(client, target, r);
    return;
  }
  DS4_REPORT_EX ex{};
  ex.Report.bThumbLX = r.bThumbLX;
  ex.Report.bThumbLY = r.bThumbLY;
  ex.Report.bThumbRX = r.bThumbRX;
  ex.Report.bThumbRY = r.bThumbRY;
  ex.Report.wButtons = r.wButtons;
  ex.Report.bSpecial = r.bSpecial;
  ex.Report.bTriggerL = r.bTriggerL;
  ex.Report.bTriggerR = r.bTriggerR;
  vigem_target_ds4_update_ex(client, target, ex);
}

/// When a DualShock axis check fails, the number alone does not say whether
/// the emulated device ignored the report, DirectInput did not surface it,
/// or this library read it from the wrong place. So drive each report field
/// to its far end in turn, through both of the client's APIs, and print
/// every raw axis beside what WinMM reports for the same device. Whichever
/// column moves, and under which API, says where the value is lost.
void probe_ds4_axes(gpp::Context &ctx, gpp::DeviceId id, PVIGEM_CLIENT client,
                    PVIGEM_TARGET target) {
  const int count = ctx.info(id).raw_axis_count;
  const struct {
    BYTE DS4_REPORT::*field;
    const char *name;
  } fields[] = {
      {&DS4_REPORT::bThumbLX, "bThumbLX"},
      {&DS4_REPORT::bThumbLY, "bThumbLY"},
      {&DS4_REPORT::bThumbRX, "bThumbRX"},
      {&DS4_REPORT::bThumbRY, "bThumbRY"},
      {&DS4_REPORT::bTriggerL, "bTriggerL"},
      {&DS4_REPORT::bTriggerR, "bTriggerR"},
  };

  const auto show = [&](const char *label) {
    std::printf("    %-10s gpplus:", label);
    for (int i = 0; i < count; ++i) {
      std::printf(" a%d=%+.2f", i, ctx.raw_axis(id, i));
    }
    WinmmJoyInfo joy;
    if (read_winmm(joy)) {
      const auto f = [](DWORD v) { return static_cast<double>(v) / 65535.0; };
      std::printf("  | winmm: X=%.2f Y=%.2f Z=%.2f R=%.2f U=%.2f V=%.2f",
                  f(joy.dwXpos), f(joy.dwYpos), f(joy.dwZpos), f(joy.dwRpos),
                  f(joy.dwUpos), f(joy.dwVpos));
    } else {
      std::printf("  | winmm: unavailable");
    }
    std::printf("\n");
  };
  const auto settle = [&] {
    wait_until(
        ctx, [] { return false; }, 300);
  };

  DS4_REPORT rest;
  DS4_REPORT_INIT(&rest);
  for (const bool extended : {false, true}) {
    std::printf("  raw axes as each field is driven to 0xFF, via %s:\n",
                extended ? "vigem_target_ds4_update_ex"
                         : "vigem_target_ds4_update");
    send_ds4(client, target, rest, extended);
    settle();
    show("(rest)");
    for (const auto &f : fields) {
      DS4_REPORT r = rest;
      r.*(f.field) = 0xFF;
      send_ds4(client, target, r, extended);
      settle();
      show(f.name);
    }
  }
  send_ds4(client, target, rest, false);
}

// --- Xbox 360 pad, through XInput -------------------------------------------

bool xbox_phase(PVIGEM_CLIENT client) {
  std::printf("\n== Xbox 360 pad -> XInput ==\n");

  gpp::Context ctx(test_config());
  VirtualPad pad(client, vigem_target_x360_alloc());
  RumbleSeen rumble;

  if (!pad.plug()) {
    // Not a failure of this library, and not reported as one. The bus took
    // the request and the device node exists, but it never started: on a
    // Windows Server image there is no inbox Xbox 360 driver for it to bind
    // to (the node sits in CM_PROB_FAILED_INSTALL, which is how this was
    // diagnosed). The bus itself is fine -- the DualShock phase, which is a
    // plain HID device, has to pass for the run to count.
    std::printf("SKIPPED: the bus accepted an Xbox 360 pad but it never became "
                "ready (error 0x%08lx).\n"
                "  Usually Windows has no Xbox 360 driver for it, as on Server "
                "images. XInput is NOT covered by this run;\n"
                "  run this test on a Windows client machine to cover it.\n",
                static_cast<unsigned long>(pad.error()));
    return false;
  }
  CHECK(VIGEM_SUCCESS(vigem_target_x360_register_notification(
      client, pad.target(), on_x360_rumble, &rumble)));

  XUSB_REPORT report{};
  const auto send = [&] {
    vigem_target_x360_update(client, pad.target(), report);
  };
  send();

  gpp::DeviceId id = gpp::kInvalidDevice;
  const bool appeared = wait_until(
      ctx,
      [&] {
        id = find_on(ctx, "xinput");
        return id != gpp::kInvalidDevice;
      },
      10000);
  CHECK(appeared);
  if (!appeared) {
    std::printf("no XInput device appeared; the backends that did: ");
    for (gpp::DeviceId d : ctx.devices()) {
      std::printf("'%s' on %s  ", ctx.info(d).name.c_str(),
                  ctx.info(d).backend);
    }
    std::printf("\n");
    return true;
  }

  const gpp::DeviceInfo info = ctx.info(id);
  std::printf("found '%s' on backend '%s', guid %s\n", info.name.c_str(),
              info.backend, info.guid.str().c_str());
  CHECK(info.mapped);
  CHECK(info.caps.rumble);

  // The point of the phase. The device is enumerated by XInput *and* by
  // DirectInput, and only the `ig_` test in the DirectInput backend keeps
  // it from arriving twice. Watched over several hotplug scans, because
  // the duplicate would arrive on a later one, not the first.
  const std::size_t most = most_devices_over(ctx, 2000);
  std::printf("most devices seen at once: %zu\n", most);
  CHECK(most == 1);

  // --- buttons ---------------------------------------------------------
  const struct {
    USHORT mask;
    gpp::Button button;
    const char *label;
  } buttons[] = {
      {XUSB_GAMEPAD_A, gpp::Button::South, "A"},
      {XUSB_GAMEPAD_B, gpp::Button::East, "B"},
      {XUSB_GAMEPAD_X, gpp::Button::West, "X"},
      {XUSB_GAMEPAD_Y, gpp::Button::North, "Y"},
      {XUSB_GAMEPAD_BACK, gpp::Button::Back, "Back"},
      {XUSB_GAMEPAD_START, gpp::Button::Start, "Start"},
      {XUSB_GAMEPAD_LEFT_THUMB, gpp::Button::LeftStick, "L3"},
      {XUSB_GAMEPAD_RIGHT_THUMB, gpp::Button::RightStick, "R3"},
      {XUSB_GAMEPAD_LEFT_SHOULDER, gpp::Button::LeftShoulder, "LB"},
      {XUSB_GAMEPAD_RIGHT_SHOULDER, gpp::Button::RightShoulder, "RB"},
      {XUSB_GAMEPAD_DPAD_UP, gpp::Button::DpadUp, "Dpad up"},
      {XUSB_GAMEPAD_DPAD_DOWN, gpp::Button::DpadDown, "Dpad down"},
      {XUSB_GAMEPAD_DPAD_LEFT, gpp::Button::DpadLeft, "Dpad left"},
      {XUSB_GAMEPAD_DPAD_RIGHT, gpp::Button::DpadRight, "Dpad right"},
  };
  for (const auto &b : buttons) {
    check_button(ctx, id, b.button, b.label, [&](bool pressed) {
      report.wButtons = pressed ? b.mask : 0;
      send();
    });
  }

  // --- sticks ----------------------------------------------------------
  // Stick Y is down-positive everywhere in this library, XInput's is
  // up-positive, and the negation happens in exactly one place. Pushing up
  // has to read as negative.
  check_axis(ctx, id, gpp::Axis::LeftX, "left stick right", 0.9f, 1.1f, [&] {
    report.sThumbLX = 32767;
    send();
  });
  check_axis(ctx, id, gpp::Axis::LeftX, "left stick left", -1.1f, -0.9f, [&] {
    report.sThumbLX = -32768;
    send();
  });
  report.sThumbLX = 0;
  check_axis(ctx, id, gpp::Axis::LeftY, "left stick up", -1.1f, -0.9f, [&] {
    report.sThumbLY = 32767;
    send();
  });
  check_axis(ctx, id, gpp::Axis::LeftY, "left stick down", 0.9f, 1.1f, [&] {
    report.sThumbLY = -32768;
    send();
  });
  report.sThumbLY = 0;
  check_axis(ctx, id, gpp::Axis::RightX, "right stick right", 0.9f, 1.1f, [&] {
    report.sThumbRX = 32767;
    send();
  });
  report.sThumbRX = 0;
  check_axis(ctx, id, gpp::Axis::RightY, "right stick up", -1.1f, -0.9f, [&] {
    report.sThumbRY = 32767;
    send();
  });
  report.sThumbRY = 0;

  // --- triggers --------------------------------------------------------
  check_axis(ctx, id, gpp::Axis::LeftTrigger, "left trigger pulled", 0.95f,
             1.1f, [&] {
               report.bLeftTrigger = 255;
               send();
             });
  check_axis(ctx, id, gpp::Axis::LeftTrigger, "left trigger released", -0.1f,
             0.05f, [&] {
               report.bLeftTrigger = 0;
               send();
             });
  check_axis(ctx, id, gpp::Axis::RightTrigger, "right trigger pulled", 0.95f,
             1.1f, [&] {
               report.bRightTrigger = 255;
               send();
             });
  report.bRightTrigger = 0;
  send();

  // --- rumble ----------------------------------------------------------
  // The one direction that goes the other way: the library writes, the
  // driver hands the motor speeds to whoever plugged the pad in. Duration
  // zero means "until told otherwise".
  CHECK(ctx.rumble(id, 1.0f, 0.0f, 0));
  CHECK(wait_until(ctx, [&] { return rumble.heavy >= 250; }));
  CHECK(rumble.light == 0);

  CHECK(ctx.rumble(id, 0.0f, 1.0f, 0));
  CHECK(wait_until(ctx, [&] { return rumble.light >= 250; }));
  CHECK(rumble.heavy == 0);

  ctx.stop_rumble(id);
  CHECK(
      wait_until(ctx, [&] { return rumble.heavy == 0 && rumble.light == 0; }));

  // --- unplug ----------------------------------------------------------
  pad.unplug();
  CHECK(wait_until(ctx, [&] { return !ctx.connected(id); }));
  CHECK(ctx.device_count() == 0);
  return true;
}

// --- DualShock 4, through DirectInput ---------------------------------------

void ds4_phase(PVIGEM_CLIENT client) {
  std::printf("\n== DualShock 4 -> DirectInput ==\n");

  gpp::Context ctx(test_config());
  VirtualPad pad(client, vigem_target_ds4_alloc());

  if (!pad.plug()) {
    std::printf("vigem_target_add failed: error 0x%08lx\n",
                static_cast<unsigned long>(pad.error()));
    check(false, "the driver refused to plug in a DualShock 4", __LINE__);
    return;
  }

  DS4_REPORT report;
  DS4_REPORT_INIT(&report);
  const auto send = [&] {
    vigem_target_ds4_update(client, pad.target(), report);
  };
  send();

  gpp::DeviceId id = gpp::kInvalidDevice;
  const bool appeared = wait_until(
      ctx,
      [&] {
        id = find_on(ctx, "dinput");
        return id != gpp::kInvalidDevice;
      },
      10000);
  CHECK(appeared);
  if (!appeared) {
    std::printf("no DirectInput device appeared; the backends that did: ");
    for (gpp::DeviceId d : ctx.devices()) {
      std::printf("'%s' on %s  ", ctx.info(d).name.c_str(),
                  ctx.info(d).backend);
    }
    std::printf("\n");
    return;
  }

  const gpp::DeviceInfo info = ctx.info(id);
  std::printf("found '%s' on backend '%s', guid %s\n", info.name.c_str(),
              info.backend, info.guid.str().c_str());
  std::printf("%d buttons, %d axes, %d hats\n", info.raw_button_count,
              info.raw_axis_count, info.raw_hat_count);

  // The vendor and product ids come from DirectInput's own property query,
  // and they are what the database is looked up by.
  CHECK(info.vendor == 0x054c);
  CHECK(info.product == 0x05c4);
  // A DualShock 4's descriptor: a d-pad hat, six axes, and fourteen
  // buttons counting the PS button and the touchpad click. At least, not
  // exactly -- a driver revision that adds one is not this test's business.
  CHECK(info.raw_hat_count == 1);
  CHECK(info.raw_axis_count >= 6);
  CHECK(info.raw_button_count >= 14);

  const std::size_t most = most_devices_over(ctx, 1000);
  std::printf("most devices seen at once: %zu\n", most);
  CHECK(most == 1);

  if (!info.mapped) {
    // Nothing below can mean anything without a mapping, and saying why is
    // more use than fourteen failures that all have the same cause.
    std::printf("FAIL: no database line matched guid %s; the DirectInput "
                "guid layout or the Windows database has drifted\n",
                info.guid.str().c_str());
    ++g_failures;
    return;
  }

  // --- buttons ---------------------------------------------------------
  // The whole point. The database says `a:b1,b:b2,x:b0,y:b3` for this pad
  // on Windows, and those numbers are DirectInput's enumeration order. If
  // this backend numbers them any other way, Cross arrives as something
  // else.
  const struct {
    USHORT mask;
    gpp::Button button;
    const char *label;
  } buttons[] = {
      {DS4_BUTTON_CROSS, gpp::Button::South, "Cross"},
      {DS4_BUTTON_CIRCLE, gpp::Button::East, "Circle"},
      {DS4_BUTTON_SQUARE, gpp::Button::West, "Square"},
      {DS4_BUTTON_TRIANGLE, gpp::Button::North, "Triangle"},
      {DS4_BUTTON_SHOULDER_LEFT, gpp::Button::LeftShoulder, "L1"},
      {DS4_BUTTON_SHOULDER_RIGHT, gpp::Button::RightShoulder, "R1"},
      {DS4_BUTTON_SHARE, gpp::Button::Back, "Share"},
      {DS4_BUTTON_OPTIONS, gpp::Button::Start, "Options"},
      {DS4_BUTTON_THUMB_LEFT, gpp::Button::LeftStick, "L3"},
      {DS4_BUTTON_THUMB_RIGHT, gpp::Button::RightStick, "R3"},
  };
  for (const auto &b : buttons) {
    check_button(ctx, id, b.button, b.label, [&](bool pressed) {
      // The low nibble of wButtons is the d-pad, not buttons; keep it.
      report.wButtons = static_cast<USHORT>((report.wButtons & 0x000F) |
                                            (pressed ? b.mask : 0));
      send();
    });
  }
  check_button(ctx, id, gpp::Button::Guide, "PS", [&](bool pressed) {
    report.bSpecial = static_cast<BYTE>(pressed ? DS4_SPECIAL_BUTTON_PS : 0);
    send();
  });
  check_button(ctx, id, gpp::Button::Touchpad, "Touchpad", [&](bool pressed) {
    report.bSpecial =
        static_cast<BYTE>(pressed ? DS4_SPECIAL_BUTTON_TOUCHPAD : 0);
    send();
  });

  // --- d-pad -----------------------------------------------------------
  // A hat, not four buttons; the database says `dpup:h0.1` and so on.
  const struct {
    DS4_DPAD_DIRECTIONS direction;
    gpp::Button button;
    const char *label;
  } dpad[] = {
      {DS4_BUTTON_DPAD_NORTH, gpp::Button::DpadUp, "Dpad up"},
      {DS4_BUTTON_DPAD_SOUTH, gpp::Button::DpadDown, "Dpad down"},
      {DS4_BUTTON_DPAD_WEST, gpp::Button::DpadLeft, "Dpad left"},
      {DS4_BUTTON_DPAD_EAST, gpp::Button::DpadRight, "Dpad right"},
  };
  for (const auto &d : dpad) {
    check_button(ctx, id, d.button, d.label, [&](bool pressed) {
      DS4_SET_DPAD(&report, pressed ? d.direction : DS4_BUTTON_DPAD_NONE);
      send();
    });
  }

  // --- sticks ----------------------------------------------------------
  const int failures_before_axes = g_failures;
  // DualShock sticks are a byte, 0x80 at rest, and Y is already
  // down-positive on the wire, so up is 0x00.
  const auto set = [&](BYTE DS4_REPORT::*field, BYTE value) {
    report.*field = value;
    send();
  };
  check_axis(ctx, id, gpp::Axis::LeftX, "left stick right", 0.9f, 1.1f,
             [&] { set(&DS4_REPORT::bThumbLX, 0xFF); });
  check_axis(ctx, id, gpp::Axis::LeftX, "left stick left", -1.1f, -0.9f,
             [&] { set(&DS4_REPORT::bThumbLX, 0x00); });
  report.bThumbLX = 0x80;
  check_axis(ctx, id, gpp::Axis::LeftY, "left stick up", -1.1f, -0.9f,
             [&] { set(&DS4_REPORT::bThumbLY, 0x00); });
  check_axis(ctx, id, gpp::Axis::LeftY, "left stick down", 0.9f, 1.1f,
             [&] { set(&DS4_REPORT::bThumbLY, 0xFF); });
  report.bThumbLY = 0x80;
  check_axis(ctx, id, gpp::Axis::RightX, "right stick right", 0.9f, 1.1f,
             [&] { set(&DS4_REPORT::bThumbRX, 0xFF); });
  report.bThumbRX = 0x80;
  // Right stick Y is the axis the database numbers a5, out of order with
  // the others -- the one most likely to expose a wrong enumeration.
  check_axis(ctx, id, gpp::Axis::RightY, "right stick up", -1.1f, -0.9f,
             [&] { set(&DS4_REPORT::bThumbRY, 0x00); });
  report.bThumbRY = 0x80;

  // --- triggers --------------------------------------------------------
  // On the database's full-range axes, so the core's rescale applies: at
  // rest a trigger has to read zero rather than half.
  check_axis(ctx, id, gpp::Axis::LeftTrigger, "left trigger pulled", 0.95f,
             1.1f, [&] { set(&DS4_REPORT::bTriggerL, 0xFF); });
  check_axis(ctx, id, gpp::Axis::LeftTrigger, "left trigger released", -0.1f,
             0.05f, [&] { set(&DS4_REPORT::bTriggerL, 0x00); });
  check_axis(ctx, id, gpp::Axis::RightTrigger, "right trigger pulled", 0.95f,
             1.1f, [&] { set(&DS4_REPORT::bTriggerR, 0xFF); });
  check_axis(ctx, id, gpp::Axis::RightTrigger, "right trigger released", -0.1f,
             0.05f, [&] { set(&DS4_REPORT::bTriggerR, 0x00); });
  if (g_failures > failures_before_axes) {
    probe_ds4_axes(ctx, id, client, pad.target());
  }

  // --- unplug ----------------------------------------------------------
  pad.unplug();
  CHECK(wait_until(ctx, [&] { return !ctx.connected(id); }));
  CHECK(ctx.device_count() == 0);
}

} // namespace

int main() {
  PVIGEM_CLIENT client = vigem_alloc();
  if (client == nullptr) {
    std::printf("vigem_alloc failed\n");
    return 1;
  }
  const VIGEM_ERROR error = vigem_connect(client);
  if (!VIGEM_SUCCESS(error)) {
    std::printf("skipping: ViGEmBus is not available (error 0x%08lx).\n"
                "  CI installs it with: choco install vigembus\n",
                static_cast<unsigned long>(error));
    vigem_free(client);
    return kSkipExitCode;
  }

  const bool xbox_ran = xbox_phase(client);
  ds4_phase(client);
  if (!xbox_ran) {
    std::printf(
        "\nNOTE: the Xbox 360 / XInput phase was skipped; see above.\n");
  }

  vigem_disconnect(client);
  vigem_free(client);

  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
