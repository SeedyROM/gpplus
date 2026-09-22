// Tests for the parts that need a controller -- run against synthetic ones.
//
// Everything here would otherwise be untestable without a drawer of
// hardware and a person to press the buttons: hotplug, edge detection,
// deadzone shaping, the difference between a raw device and a pre-mapped
// one, and what a query does with an id whose device has gone.

#include <gpplus/gamepad.hpp>
#include <gpplus/virtual.hpp>

#include <cmath>
#include <cstdio>
#include <string>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char *what, int line) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::printf("FAIL %s:%d: %s\n", __FILE__, line, what);
  }
}

void check_near(float a, float b, const char *what, int line) {
  ++g_checks;
  if (std::fabs(a - b) >= 0.002f) {
    ++g_failures;
    std::printf("FAIL %s:%d: %s (%.4f != %.4f)\n", __FILE__, line, what,
                static_cast<double>(a), static_cast<double>(b));
  }
}

#define CHECK(expr) check((expr), #expr, __LINE__)
#define CHECK_NEAR(a, b) check_near((a), (b), #a " ~= " #b, __LINE__)

using namespace gpp;

/// A pad with the layout an evdev-style device would have, plus the
/// database line that describes it.
const char *kTestMapping = "0300a96badde0000efbe000000000000,Test Pad,"
                           "a:b0,b:b1,x:b2,y:b3,start:b7,back:b6,"
                           "dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,"
                           "leftx:a0,lefty:a1,rightx:a2,righty:a3,"
                           "lefttrigger:a4,righttrigger:a5,";

virtualpad::Spec test_spec() {
  virtualpad::Spec spec;
  spec.name = "Test Pad";
  spec.vendor = 0xDEAD;
  spec.product = 0xBEEF;
  spec.raw_button_count = 16;
  spec.raw_axis_count = 6;
  spec.raw_hat_count = 1;
  spec.rumble = true;
  return spec;
}

int count_events(const Context &ctx, Event::Type type) {
  int n = 0;
  for (const Event &e : ctx.events()) {
    if (e.type == type) {
      ++n;
    }
  }
  return n;
}

void test_hotplug_and_identity() {
  virtualpad::remove_all();
  Context ctx;
  CHECK(ctx.add_mapping(kTestMapping));
  CHECK(ctx.device_count() == 0);

  const auto vid = virtualpad::add(test_spec());
  // Nothing exists until the frame that looks: a device appearing between
  // updates must not change what a half-finished frame sees.
  CHECK(ctx.device_count() == 0);

  ctx.update();
  CHECK(ctx.device_count() == 1);
  CHECK(count_events(ctx, Event::Type::DeviceAdded) == 1);

  const DeviceId id = virtualpad::device_id(vid);
  CHECK(id != kInvalidDevice);
  CHECK(ctx.connected(id));
  const DeviceInfo info = ctx.info(id);
  CHECK(info.name == "Test Pad");
  CHECK(info.vendor == 0xDEAD && info.product == 0xBEEF);
  CHECK(info.mapped);
  CHECK(std::string(info.backend) == "virtual");
  CHECK(info.raw_button_count == 16);
  CHECK(ctx.mapping_for(id).find("Test Pad") != std::string::npos);

  virtualpad::remove(vid);
  ctx.update();
  CHECK(ctx.device_count() == 0);
  CHECK(count_events(ctx, Event::Type::DeviceRemoved) == 1);

  // Every query on a departed id is safe and answers nothing. A game that
  // stored the id last frame is the normal case, not the careless one.
  CHECK(!ctx.connected(id));
  CHECK(!ctx.down(id, Button::South));
  CHECK_NEAR(ctx.axis(id, Axis::LeftX), 0.0f);
  CHECK(ctx.info(id).id == kInvalidDevice);
  CHECK(ctx.mapping_for(id).empty());
  CHECK(!ctx.rumble(id, 1.0f, 1.0f, 100));
}

void test_ids_are_not_reused() {
  virtualpad::remove_all();
  Context ctx;
  ctx.add_mapping(kTestMapping);

  const auto first = virtualpad::add(test_spec());
  ctx.update();
  const DeviceId first_id = virtualpad::device_id(first);
  virtualpad::remove(first);
  ctx.update();

  const auto second = virtualpad::add(test_spec());
  ctx.update();
  const DeviceId second_id = virtualpad::device_id(second);
  // The slot is reused; the id must not be, or a stale id silently starts
  // addressing a different player's pad.
  CHECK(second_id != first_id);
  CHECK(!ctx.connected(first_id));
  CHECK(ctx.connected(second_id));
}

void test_buttons_and_edges() {
  virtualpad::remove_all();
  Context ctx;
  ctx.add_mapping(kTestMapping);
  const auto vid = virtualpad::add(test_spec());
  ctx.update();
  const DeviceId id = virtualpad::device_id(vid);

  CHECK(!ctx.down(id, Button::South));
  virtualpad::set_raw_button(vid, 0, true);
  ctx.update();
  CHECK(ctx.down(id, Button::South));
  CHECK(ctx.pressed(id, Button::South));
  CHECK(!ctx.released(id, Button::South));
  CHECK(count_events(ctx, Event::Type::ButtonDown) == 1);

  // Held is not pressed: the edge is true for exactly one update, whatever
  // the frame does with it afterwards.
  ctx.update();
  CHECK(ctx.down(id, Button::South));
  CHECK(!ctx.pressed(id, Button::South));

  virtualpad::set_raw_button(vid, 0, false);
  ctx.update();
  CHECK(!ctx.down(id, Button::South));
  CHECK(ctx.released(id, Button::South));
  CHECK(count_events(ctx, Event::Type::ButtonUp) == 1);

  virtualpad::set_raw_hat(vid, 0, 0x04);
  ctx.update();
  CHECK(ctx.down(id, Button::DpadDown));
  CHECK(!ctx.down(id, Button::DpadUp));
}

void test_deadzone_shapes() {
  virtualpad::remove_all();
  Config cfg;
  cfg.stick_deadzone = 0.2f;
  Context ctx(cfg);
  ctx.add_mapping(kTestMapping);
  const auto vid = virtualpad::add(test_spec());
  ctx.update();
  const DeviceId id = virtualpad::device_id(vid);

  virtualpad::set_raw_axis(vid, 0, 0.1f);
  ctx.update();
  CHECK_NEAR(ctx.axis(id, Axis::LeftX), 0.0f);
  // Raw is raw: a binding screen and a calibration dialog both need the
  // number the hardware actually sent.
  CHECK_NEAR(ctx.axis_raw(id, Axis::LeftX), 0.1f);

  virtualpad::set_raw_axis(vid, 0, 1.0f);
  ctx.update();
  CHECK_NEAR(ctx.axis(id, Axis::LeftX), 1.0f);

  // Rescaled, not clipped: just past the deadzone reads just past zero.
  virtualpad::set_raw_axis(vid, 0, 0.2f + 0.8f * 0.5f);
  ctx.update();
  CHECK_NEAR(ctx.axis(id, Axis::LeftX), 0.5f);

  // Radial: a stick at the deadzone on both axes is further than the
  // deadzone overall, so it is live -- the square-hole bug the mode
  // exists to avoid.
  virtualpad::set_raw_axis(vid, 0, 0.18f);
  virtualpad::set_raw_axis(vid, 1, 0.18f);
  ctx.update();
  const Vec2 v = ctx.stick(id, Stick::Left);
  CHECK(std::sqrt(v.x * v.x + v.y * v.y) > 0.0f);

  // Diagonals past full are clamped before rescaling, so a worn stick
  // cannot report more than full throttle.
  virtualpad::set_raw_axis(vid, 0, 1.0f);
  virtualpad::set_raw_axis(vid, 1, 1.0f);
  ctx.update();
  const Vec2 corner = ctx.stick(id, Stick::Left);
  CHECK(std::sqrt(corner.x * corner.x + corner.y * corner.y) <= 1.0001f);
}

void test_y_up_flips_once() {
  virtualpad::remove_all();
  Config cfg;
  cfg.y_up = true;
  cfg.deadzone_mode = DeadzoneMode::None;
  Context ctx(cfg);
  ctx.add_mapping(kTestMapping);
  const auto vid = virtualpad::add(test_spec());
  ctx.update();
  const DeviceId id = virtualpad::device_id(vid);

  virtualpad::set_raw_axis(vid, 1, 1.0f); // hardware says "down"
  ctx.update();
  CHECK_NEAR(ctx.axis(id, Axis::LeftY), -1.0f);
  CHECK_NEAR(ctx.stick(id, Stick::Left).y, -1.0f);
  // X is untouched by the flip, and the raw value still reports what the
  // hardware sent.
  CHECK_NEAR(ctx.axis_raw(id, Axis::LeftY), 1.0f);
}

void test_trigger_deadzone_is_separate() {
  virtualpad::remove_all();
  Config cfg;
  cfg.stick_deadzone = 0.5f;
  cfg.trigger_deadzone = 0.05f;
  Context ctx(cfg);
  ctx.add_mapping(kTestMapping);
  const auto vid = virtualpad::add(test_spec());
  ctx.update();
  const DeviceId id = virtualpad::device_id(vid);

  // a4 is a full-range axis bound to a trigger, so -1 is "released".
  virtualpad::set_raw_axis(vid, 4, -1.0f);
  ctx.update();
  CHECK_NEAR(ctx.axis(id, Axis::LeftTrigger), 0.0f);

  virtualpad::set_raw_axis(vid, 4, 0.0f); // half pulled
  ctx.update();
  CHECK(ctx.axis(id, Axis::LeftTrigger) > 0.4f);
  CHECK(ctx.axis(id, Axis::LeftTrigger) < 0.6f);
}

void test_unmapped_device_is_still_readable() {
  virtualpad::remove_all();
  Context ctx;
  virtualpad::Spec spec = test_spec();
  spec.name = "Nobody Has Ever Seen This";
  spec.vendor = 0x1234;
  spec.product = 0x5678;
  const auto vid = virtualpad::add(spec);
  ctx.update();
  const DeviceId id = virtualpad::device_id(vid);

  CHECK(id != kInvalidDevice);
  CHECK(!ctx.info(id).mapped);
  CHECK(ctx.mapping_for(id).empty());

  virtualpad::set_raw_button(vid, 3, true);
  ctx.update();
  // Nothing knows what button 3 means, so the mapped view stays empty --
  // a guess here is worse than nothing for a player trying to hit jump.
  CHECK(!ctx.down(id, Button::South));
  CHECK(ctx.raw_button(id, 3));
  // But "press anything to join" still has to work.
  CHECK(ctx.active(id));

  // And a mapping arriving later takes effect without a reconnect, which
  // is what makes an in-game binding screen possible.
  CHECK(ctx.add_mapping("03001f24341200007856000000000000,Late Mapping,"
                        "a:b3,platform:" +
                        std::string(platform_name()) + ","));
  CHECK(ctx.info(id).mapped);
  ctx.update();
  CHECK(ctx.down(id, Button::South));
}

void test_report_unmapped_off_hides_it() {
  virtualpad::remove_all();
  Config cfg;
  cfg.report_unmapped = false;
  Context ctx(cfg);
  virtualpad::Spec spec = test_spec();
  spec.vendor = 0x4321;
  spec.product = 0x8765;
  virtualpad::add(spec);
  ctx.update();
  CHECK(ctx.device_count() == 0);
}

void test_backend_mapped_device_skips_the_database() {
  virtualpad::remove_all();
  Context ctx;
  virtualpad::Spec spec;
  spec.name = "Console Pad";
  spec.backend_mapped = true;
  const auto vid = virtualpad::add(spec);
  ctx.update();
  const DeviceId id = virtualpad::device_id(vid);

  // No database line exists for this guid, and it is mapped anyway: the
  // backend already said what the buttons mean.
  CHECK(ctx.info(id).mapped);
  CHECK(ctx.mapping_for(id).empty());

  virtualpad::set_button(vid, Button::North, true);
  virtualpad::set_axis(vid, Axis::RightX, -1.0f);
  ctx.update();
  CHECK(ctx.down(id, Button::North));
  CHECK(ctx.pressed(id, Button::North));
  CHECK_NEAR(ctx.axis(id, Axis::RightX), -1.0f);
}

void test_rumble_reaches_the_backend() {
  virtualpad::remove_all();
  Context ctx;
  ctx.add_mapping(kTestMapping);
  const auto vid = virtualpad::add(test_spec());
  ctx.update();
  const DeviceId id = virtualpad::device_id(vid);

  CHECK(ctx.rumble(id, 0.5f, 0.25f, 120));
  const auto state = virtualpad::rumble_state(vid);
  CHECK_NEAR(state.low, 0.5f);
  CHECK_NEAR(state.high, 0.25f);
  CHECK(state.duration_ms == 120);

  // Out-of-range intensities are clamped rather than passed to a driver
  // that may or may not check them.
  ctx.rumble(id, 5.0f, -3.0f, 0);
  CHECK_NEAR(virtualpad::rumble_state(vid).low, 1.0f);
  CHECK_NEAR(virtualpad::rumble_state(vid).high, 0.0f);

  ctx.stop_rumble(id);
  CHECK_NEAR(virtualpad::rumble_state(vid).low, 0.0f);
}

void test_repeated_rumble_is_not_resent() {
  // The shape a camera-shake system actually drives: rumble() called
  // every frame, whether or not the value it is asking for has moved.
  // What reaches the backend should track the value, not the call count --
  // a real device backend would otherwise pay a fresh packet, HID report
  // or ioctl every frame for nothing, and this is the one place that fix
  // covers every backend at once instead of each having to remember it.
  virtualpad::remove_all();
  Context ctx;
  ctx.add_mapping(kTestMapping);
  const auto vid = virtualpad::add(test_spec());
  ctx.update();
  const DeviceId id = virtualpad::device_id(vid);

  CHECK(ctx.rumble(id, 0.5f, 0.5f, 0));
  CHECK(virtualpad::rumble_state(vid).call_count == 1);

  // Same request, several times over: none of these should reach the
  // backend a second time.
  CHECK(ctx.rumble(id, 0.5f, 0.5f, 0));
  CHECK(ctx.rumble(id, 0.5f, 0.5f, 0));
  CHECK(ctx.rumble(id, 0.5f, 0.5f, 0));
  CHECK(virtualpad::rumble_state(vid).call_count == 1);

  // A value that actually changes reaches the backend, and starts a new
  // "unchanged" baseline of its own.
  CHECK(ctx.rumble(id, 0.5f, 0.6f, 0));
  CHECK(virtualpad::rumble_state(vid).call_count == 2);
  CHECK(ctx.rumble(id, 0.5f, 0.6f, 0));
  CHECK(virtualpad::rumble_state(vid).call_count == 2);

  // Same intensities, but a different duration is not a no-op: it changes
  // when the rumble will next stop on its own, which is real state a
  // backend has to be told about even though low/high did not move.
  CHECK(ctx.rumble(id, 0.5f, 0.6f, 250));
  CHECK(virtualpad::rumble_state(vid).call_count == 3);

  // stop_rumble() goes through the same path: repeating it while already
  // stopped costs nothing past the first call.
  ctx.stop_rumble(id);
  CHECK(virtualpad::rumble_state(vid).call_count == 4);
  ctx.stop_rumble(id);
  ctx.stop_rumble(id);
  CHECK(virtualpad::rumble_state(vid).call_count == 4);

  // A device that reconnects starts from "nothing sent yet" rather than
  // inheriting whatever the slot last held, even though low/high/duration
  // here are identical to what was last (de-duplicated) at that slot.
  virtualpad::remove(vid);
  ctx.update();
  const auto vid2 = virtualpad::add(test_spec());
  ctx.update();
  const DeviceId id2 = virtualpad::device_id(vid2);
  CHECK(ctx.rumble(id2, 0.0f, 0.0f, 0));
  CHECK(virtualpad::rumble_state(vid2).call_count == 1);
}

void test_axis_events_are_not_spam() {
  virtualpad::remove_all();
  Config cfg;
  cfg.deadzone_mode = DeadzoneMode::None;
  Context ctx(cfg);
  ctx.add_mapping(kTestMapping);
  const auto vid = virtualpad::add(test_spec());
  ctx.update();

  virtualpad::set_raw_axis(vid, 0, 0.75f);
  ctx.update();
  CHECK(count_events(ctx, Event::Type::AxisMotion) == 1);

  // A stick held still emits nothing, however many frames pass.
  ctx.update();
  CHECK(count_events(ctx, Event::Type::AxisMotion) == 0);
}

void test_power_is_reported() {
  virtualpad::remove_all();
  Context ctx;
  ctx.add_mapping(kTestMapping);
  const auto vid = virtualpad::add(test_spec());
  ctx.update();
  const DeviceId id = virtualpad::device_id(vid);

  // Wired until a backend says otherwise: a pad with no battery to report
  // is the common case, and Unknown would have every game draw an empty
  // battery icon for it.
  CHECK(ctx.info(id).power == PowerLevel::Wired);

  virtualpad::set_power(vid, PowerLevel::Low);
  ctx.update();
  CHECK(ctx.info(id).power == PowerLevel::Low);

  virtualpad::set_power(vid, PowerLevel::Charging);
  ctx.update();
  CHECK(ctx.info(id).power == PowerLevel::Charging);
}

void test_multiple_devices_are_independent() {
  virtualpad::remove_all();
  Context ctx;
  ctx.add_mapping(kTestMapping);
  const auto a = virtualpad::add(test_spec());
  const auto b = virtualpad::add(test_spec());
  ctx.update();
  CHECK(ctx.device_count() == 2);

  const DeviceId id_a = virtualpad::device_id(a);
  const DeviceId id_b = virtualpad::device_id(b);
  CHECK(id_a != id_b);
  CHECK(ctx.first_device() == id_a);

  virtualpad::set_raw_button(a, 1, true);
  ctx.update();
  CHECK(ctx.down(id_a, Button::East));
  CHECK(!ctx.down(id_b, Button::East));

  // Unplugging the first leaves the second exactly where it was, which is
  // the bug an index-based API cannot avoid.
  virtualpad::remove(a);
  ctx.update();
  CHECK(ctx.device_count() == 1);
  CHECK(ctx.first_device() == id_b);
  CHECK(ctx.connected(id_b));
}

} // namespace

int main() {
  test_hotplug_and_identity();
  test_ids_are_not_reused();
  test_buttons_and_edges();
  test_deadzone_shapes();
  test_y_up_flips_once();
  test_trigger_deadzone_is_separate();
  test_unmapped_device_is_still_readable();
  test_report_unmapped_off_hides_it();
  test_backend_mapped_device_skips_the_database();
  test_rumble_reaches_the_backend();
  test_repeated_rumble_is_not_resent();
  test_axis_events_are_not_spam();
  test_power_is_reported();
  test_multiple_devices_are_independent();

  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
