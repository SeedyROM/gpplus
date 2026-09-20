// End-to-end test of the evdev backend against a real kernel device.
//
// Every other test in this suite stops at the library's own edge: the
// virtual backend fakes a device, and the parser is fed strings. This one
// goes the whole way. It asks the kernel for a gamepad through
// /dev/uinput, and from that moment the device is indistinguishable from
// hardware -- udev creates a node, the evdev backend finds it by scanning
// /dev/input, opens it, enumerates its capabilities with the same ioctls
// it would use on a real pad, and reads real input events off a real file
// descriptor.
//
// That makes it the only test that can catch the things the type checker
// cannot: a mis-numbered button, an ioctl with the wrong argument, an
// absinfo range read from the wrong field. It is also the only test that
// proves the numbering matches SDL's, because the mapping below says
// `a:b0` and the kernel decides what button 0 is.
//
// Compiled only where <linux/uinput.h> exists. Exits 77 (ctest's "skip")
// when the uinput module is absent or not writable, because a developer
// machine without it is not a failure -- CI loads the module explicitly.

#include <gpplus/gamepad.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <unistd.h>

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

void emit(int fd, std::uint16_t type, std::uint16_t code, std::int32_t value) {
  struct input_event ev {};
  ev.type = type;
  ev.code = code;
  ev.value = value;
  if (write(fd, &ev, sizeof(ev)) != static_cast<ssize_t>(sizeof(ev))) {
    std::printf("note: write to uinput failed: %s\n", std::strerror(errno));
  }
}

/// A packet, the way a real device sends one: the changes, then a SYN to
/// say the frame is complete.
void sync_report(int fd) { emit(fd, EV_SYN, SYN_REPORT, 0); }

/// The kernel creates the device node asynchronously and the events we
/// write take a moment to come back around, so every assertion here is
/// "eventually" rather than "now". A fixed sleep would be either flaky or
/// slow; this is neither.
template <typename Predicate>
bool wait_until(gpp::Context &ctx, Predicate pred, int timeout_ms = 3000) {
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

/// Buttons are declared in ascending code order, which is the order the
/// backend numbers them in, which is the order the mapping below assumes.
constexpr std::uint16_t kButtons[] = {
    BTN_A, BTN_B, BTN_X, BTN_Y, BTN_TL, BTN_TR, BTN_SELECT, BTN_START,
};

constexpr std::uint16_t kAxes[] = {
    ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ,
};

int create_device() {
  int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
  if (fd < 0) {
    return -1;
  }

  if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0) {
    close(fd);
    return -1;
  }
  for (std::uint16_t code : kButtons) {
    ioctl(fd, UI_SET_KEYBIT, code);
  }
  ioctl(fd, UI_SET_EVBIT, EV_ABS);
  for (std::uint16_t code : kAxes) {
    ioctl(fd, UI_SET_ABSBIT, code);
  }
  ioctl(fd, UI_SET_ABSBIT, ABS_HAT0X);
  ioctl(fd, UI_SET_ABSBIT, ABS_HAT0Y);

  struct uinput_abs_setup abs {};
  for (std::uint16_t code : kAxes) {
    abs.code = code;
    abs.absinfo = {};
    abs.absinfo.minimum = -32768;
    abs.absinfo.maximum = 32767;
    ioctl(fd, UI_ABS_SETUP, &abs);
  }
  for (std::uint16_t code : {ABS_HAT0X, ABS_HAT0Y}) {
    abs.code = code;
    abs.absinfo = {};
    abs.absinfo.minimum = -1;
    abs.absinfo.maximum = 1;
    ioctl(fd, UI_ABS_SETUP, &abs);
  }

  struct uinput_setup setup {};
  setup.id.bustype = BUS_USB;
  setup.id.vendor = 0x1234;
  setup.id.product = 0x5678;
  setup.id.version = 0x0111;
  std::snprintf(setup.name, sizeof(setup.name), "gpplus uinput test pad");
  if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

/// Written against the numbering the kernel will produce for the
/// capabilities declared above: buttons in ascending code order, axes in
/// ascending code order with the hat pair held out. If the backend ever
/// numbers them differently, every assertion below fails -- which is the
/// point of spelling the mapping out rather than deriving it.
std::string mapping_for(const gpp::Guid &guid) {
  return guid.str() + ",gpplus uinput test pad,"
                      "a:b0,b:b1,x:b2,y:b3,leftshoulder:b4,rightshoulder:b5,"
                      "back:b6,start:b7,"
                      "leftx:a0,lefty:a1,lefttrigger:a2,rightx:a3,righty:a4,"
                      "righttrigger:a5,"
                      "dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,"
                      "platform:Linux,";
}

} // namespace

int main() {
  const int fd = create_device();
  if (fd < 0) {
    std::printf("skipping: /dev/uinput is not available (%s).\n"
                "  CI loads it with: sudo modprobe uinput && "
                "sudo chmod 0666 /dev/uinput\n",
                std::strerror(errno));
    return kSkipExitCode;
  }

  gpp::Config cfg;
  // Only what this test declares, so a real pad plugged into the machine
  // running the suite cannot satisfy an assertion by accident.
  cfg.load_builtin_db = false;
  cfg.db_env_var.clear();
  cfg.hotplug_interval = 0.0f;
  cfg.deadzone_mode = gpp::DeadzoneMode::None;
  gpp::Context ctx(cfg);

  gpp::DeviceId id = gpp::kInvalidDevice;
  const bool appeared = wait_until(ctx, [&] {
    for (gpp::DeviceId candidate : ctx.devices()) {
      if (ctx.info(candidate).name == "gpplus uinput test pad") {
        id = candidate;
        return true;
      }
    }
    return false;
  });
  CHECK(appeared);

  if (!appeared) {
    std::printf("the device never appeared; is /dev/input readable?\n");
    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
    return 1;
  }

  const gpp::DeviceInfo info = ctx.info(id);
  std::printf("found '%s' on backend '%s', guid %s\n", info.name.c_str(),
              info.backend, info.guid.str().c_str());
  CHECK(std::string(info.backend) == "evdev");
  CHECK(info.vendor == 0x1234);
  CHECK(info.product == 0x5678);
  CHECK(info.version == 0x0111);
  // Eight buttons, six axes, one hat -- exactly what was declared, which
  // means the capability ioctls were read correctly.
  CHECK(info.raw_button_count == 8);
  CHECK(info.raw_axis_count == 6);
  CHECK(info.raw_hat_count == 1);
  // No database was loaded, so nothing can know the layout yet.
  CHECK(!info.mapped);
  // A uinput device publishes no power supply, and the backend reports
  // that as wired rather than unknown -- the same answer a real wired pad
  // gets, which is what makes the distinction worth having.
  CHECK(info.power == gpp::PowerLevel::Wired);

  CHECK(ctx.add_mapping(mapping_for(info.guid)));
  CHECK(ctx.info(id).mapped);

  // --- buttons ---------------------------------------------------------
  emit(fd, EV_KEY, BTN_A, 1);
  sync_report(fd);
  CHECK(wait_until(ctx, [&] { return ctx.down(id, gpp::Button::South); }));
  CHECK(!ctx.down(id, gpp::Button::East));

  emit(fd, EV_KEY, BTN_A, 0);
  emit(fd, EV_KEY, BTN_START, 1);
  sync_report(fd);
  CHECK(wait_until(ctx, [&] { return ctx.down(id, gpp::Button::Start); }));
  CHECK(!ctx.down(id, gpp::Button::South));
  emit(fd, EV_KEY, BTN_START, 0);
  sync_report(fd);

  // --- axes ------------------------------------------------------------
  emit(fd, EV_ABS, ABS_X, 32767);
  sync_report(fd);
  CHECK(wait_until(ctx, [&] { return ctx.axis(id, gpp::Axis::LeftX) > 0.9f; }));

  emit(fd, EV_ABS, ABS_X, -32768);
  sync_report(fd);
  CHECK(
      wait_until(ctx, [&] { return ctx.axis(id, gpp::Axis::LeftX) < -0.9f; }));

  // A trigger on a full-range axis rests at the minimum, and the core
  // rescales it to 0..1 -- the conversion that is wrong in most
  // hand-rolled input layers.
  emit(fd, EV_ABS, ABS_Z, -32768);
  sync_report(fd);
  CHECK(wait_until(
      ctx, [&] { return ctx.axis(id, gpp::Axis::LeftTrigger) < 0.01f; }));
  emit(fd, EV_ABS, ABS_Z, 32767);
  sync_report(fd);
  CHECK(wait_until(
      ctx, [&] { return ctx.axis(id, gpp::Axis::LeftTrigger) > 0.99f; }));

  // --- hat -------------------------------------------------------------
  emit(fd, EV_ABS, ABS_HAT0Y, -1);
  sync_report(fd);
  CHECK(wait_until(ctx, [&] { return ctx.down(id, gpp::Button::DpadUp); }));
  CHECK(!ctx.down(id, gpp::Button::DpadDown));

  emit(fd, EV_ABS, ABS_HAT0Y, 0);
  emit(fd, EV_ABS, ABS_HAT0X, 1);
  sync_report(fd);
  CHECK(wait_until(ctx, [&] { return ctx.down(id, gpp::Button::DpadRight); }));
  CHECK(!ctx.down(id, gpp::Button::DpadUp));

  // --- unplug ----------------------------------------------------------
  ioctl(fd, UI_DEV_DESTROY);
  close(fd);
  CHECK(wait_until(ctx, [&] { return !ctx.connected(id); }));
  CHECK(ctx.device_count() == 0);

  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
