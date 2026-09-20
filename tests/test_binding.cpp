// Tests for the rebinding recorder, driven by synthetic pads.
//
// The recorder's whole job is judgement calls about noisy hardware --
// which of several moving inputs the player meant, what counts as "at
// rest", whether an axis is a trigger -- and every one of them is
// testable without hardware as long as something can pretend to be a pad.

#include <gpplus/binding.hpp>
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

virtualpad::Spec spec() {
  virtualpad::Spec s;
  s.name = "Rebind Test Pad";
  s.vendor = 0x0F0F;
  s.product = 0xA1A1;
  s.raw_button_count = 12;
  s.raw_axis_count = 6;
  s.raw_hat_count = 1;
  return s;
}

/// The recorder reports an input only after it has been steady for a few
/// frames, so a test that presses something has to let those frames pass.
RawInput settle(Context &ctx, MappingRecorder &rec, int frames = 6) {
  RawInput found;
  for (int i = 0; i < frames; ++i) {
    ctx.update();
    const RawInput in = rec.poll();
    if (in.valid() && !found.valid()) {
      found = in;
    }
  }
  return found;
}

struct Rig {
  Context ctx;
  virtualpad::VirtualId vid;
  DeviceId id;

  Rig() : ctx(configured()) {
    virtualpad::remove_all();
    vid = virtualpad::add(spec());
    ctx.update();
    id = virtualpad::device_id(vid);
  }

  static Config configured() {
    Config cfg;
    // No database, so nothing can map this pad except what a test records.
    cfg.load_builtin_db = false;
    cfg.db_env_var.clear();
    cfg.deadzone_mode = DeadzoneMode::None;
    return cfg;
  }
};

void test_detects_a_button() {
  Rig rig;
  MappingRecorder rec(rig.ctx, rig.id);

  // Nothing moving, nothing reported, however long we wait.
  CHECK(!settle(rig.ctx, rec).valid());

  virtualpad::set_raw_button(rig.vid, 4, true);
  const RawInput in = settle(rig.ctx, rec);
  CHECK(in.valid());
  CHECK(in.kind == RawInput::Kind::Button);
  CHECK(in.index == 4);

  // Held, not re-reported: otherwise a player holding a button would
  // rebind every prompt in the list at once.
  CHECK(!settle(rig.ctx, rec).valid());
}

void test_stability_rejects_a_blip() {
  Rig rig;
  MappingRecorder rec(rig.ctx, rig.id);
  rec.set_stability(4);

  // Pressed for fewer frames than the stability window: contact bounce,
  // not an intentional press.
  virtualpad::set_raw_button(rig.vid, 2, true);
  RawInput seen;
  for (int i = 0; i < 2; ++i) {
    rig.ctx.update();
    const RawInput in = rec.poll();
    if (in.valid()) {
      seen = in;
    }
  }
  virtualpad::set_raw_button(rig.vid, 2, false);
  for (int i = 0; i < 4; ++i) {
    rig.ctx.update();
    const RawInput in = rec.poll();
    if (in.valid()) {
      seen = in;
    }
  }
  CHECK(!seen.valid());
}

void test_baseline_ignores_what_was_already_held() {
  Rig rig;
  // A trigger resting at -1 and a stick sitting off centre: the state a
  // rebinding screen usually opens in.
  virtualpad::set_raw_axis(rig.vid, 4, -1.0f);
  virtualpad::set_raw_axis(rig.vid, 0, 0.2f);
  virtualpad::set_raw_button(rig.vid, 7, true);
  rig.ctx.update();

  MappingRecorder rec(rig.ctx, rig.id);
  CHECK(!settle(rig.ctx, rec).valid());

  // Pulling the trigger from that resting place is movement.
  virtualpad::set_raw_axis(rig.vid, 4, 1.0f);
  const RawInput in = settle(rig.ctx, rec);
  CHECK(in.valid());
  CHECK(in.kind == RawInput::Kind::Axis);
  CHECK(in.index == 4);
  CHECK(in.direction == 1);
  CHECK(in.rests_at_extreme);
}

void test_button_beats_a_brushed_stick() {
  Rig rig;
  MappingRecorder rec(rig.ctx, rig.id);

  // A thumb crossing the stick on the way to the button moves both in the
  // same frame. The button is what the player meant.
  virtualpad::set_raw_axis(rig.vid, 0, 0.9f);
  virtualpad::set_raw_button(rig.vid, 1, true);
  const RawInput in = settle(rig.ctx, rec);
  CHECK(in.kind == RawInput::Kind::Button);
  CHECK(in.index == 1);
}

void test_furthest_axis_wins() {
  Rig rig;
  MappingRecorder rec(rig.ctx, rig.id);

  // Two axes moving past the threshold; the one the player pushed is the
  // one that moved further.
  virtualpad::set_raw_axis(rig.vid, 1, 0.6f);
  virtualpad::set_raw_axis(rig.vid, 3, -0.95f);
  const RawInput in = settle(rig.ctx, rec);
  CHECK(in.kind == RawInput::Kind::Axis);
  CHECK(in.index == 3);
  CHECK(in.direction == -1);
}

void test_hat_reports_only_the_new_direction() {
  Rig rig;
  MappingRecorder rec(rig.ctx, rig.id);
  virtualpad::set_raw_hat(rig.vid, 0, 0x02);
  const RawInput in = settle(rig.ctx, rec);
  CHECK(in.kind == RawInput::Kind::Hat);
  CHECK(in.index == 0);
  CHECK(in.hat_mask == 0x02);
}

/// Records one binding and returns the recorder, so the round-trip tests
/// below can stay about the line rather than the ceremony.
RawInput record(Rig &rig, MappingRecorder &rec, int button) {
  virtualpad::set_raw_button(rig.vid, button, true);
  const RawInput in = settle(rig.ctx, rec);
  virtualpad::set_raw_button(rig.vid, button, false);
  settle(rig.ctx, rec);
  return in;
}

void test_line_round_trips_through_the_parser() {
  Rig rig;
  MappingRecorder rec(rig.ctx, rig.id);
  CHECK(rec.binding_count() == 0);

  rec.bind(Button::South, record(rig, rec, 0));
  rec.bind(Button::East, record(rig, rec, 1));
  CHECK(rec.bound(Button::South));
  CHECK(!rec.bound(Button::North));
  CHECK(rec.binding_count() == 2);

  const std::string line = rec.mapping_line();
  CHECK(line.find(rig.ctx.info(rig.id).guid.str()) == 0);
  CHECK(line.find(",a:b0,") != std::string::npos);
  CHECK(line.find(",b:b1,") != std::string::npos);
  CHECK(line.find(std::string("platform:") + platform_name()) !=
        std::string::npos);

  // The real test: the Context accepts it, and the pad it was recorded
  // from now answers to it.
  CHECK(!rig.ctx.info(rig.id).mapped);
  CHECK(rec.apply());
  CHECK(rig.ctx.info(rig.id).mapped);

  virtualpad::set_raw_button(rig.vid, 1, true);
  rig.ctx.update();
  CHECK(rig.ctx.down(rig.id, Button::East));
  CHECK(!rig.ctx.down(rig.id, Button::South));
}

void test_rebinding_replaces() {
  Rig rig;
  MappingRecorder rec(rig.ctx, rig.id);
  rec.bind(Button::South, record(rig, rec, 0));
  rec.bind(Button::South, record(rig, rec, 5));
  CHECK(rec.binding_count() == 1);

  const std::string line = rec.mapping_line();
  CHECK(line.find(",a:b5,") != std::string::npos);
  CHECK(line.find(",a:b0,") == std::string::npos);

  rec.clear(Button::South);
  CHECK(!rec.bound(Button::South));
  CHECK(rec.mapping_line().empty());
}

void test_inverted_stick_is_written_down() {
  Rig rig;
  MappingRecorder rec(rig.ctx, rig.id);

  // The prompt said "push right"; this pad answered by going negative.
  virtualpad::set_raw_axis(rig.vid, 0, -1.0f);
  const RawInput in = settle(rig.ctx, rec);
  CHECK(in.direction == -1);
  rec.bind(Axis::LeftX, in);

  const std::string line = rec.mapping_line();
  CHECK(line.find("leftx:a0~") != std::string::npos);

  CHECK(rec.apply());
  rig.ctx.update();
  // Pushed the same way as during recording, the game now reads "right".
  CHECK(rig.ctx.axis(rig.id, Axis::LeftX) > 0.9f);
}

void test_trigger_at_rest_reads_zero() {
  Rig rig;
  // A trigger on a full-range axis: released is -1.
  virtualpad::set_raw_axis(rig.vid, 4, -1.0f);
  rig.ctx.update();
  MappingRecorder rec(rig.ctx, rig.id);

  virtualpad::set_raw_axis(rig.vid, 4, 1.0f);
  const RawInput in = settle(rig.ctx, rec);
  CHECK(in.rests_at_extreme);
  rec.bind(Axis::LeftTrigger, in);

  // Written whole, so the core's (v+1)/2 rescale applies.
  const std::string line = rec.mapping_line();
  CHECK(line.find("lefttrigger:a4,") != std::string::npos);
  CHECK(rec.apply());

  rig.ctx.update();
  CHECK_NEAR(rig.ctx.axis(rig.id, Axis::LeftTrigger), 1.0f);
  virtualpad::set_raw_axis(rig.vid, 4, -1.0f);
  rig.ctx.update();
  // The bug this exists to prevent: half throttle from a released trigger.
  CHECK_NEAR(rig.ctx.axis(rig.id, Axis::LeftTrigger), 0.0f);
}

void test_half_travel_trigger_is_written_as_a_half() {
  Rig rig;
  // This pad's trigger rests at 0 and only climbs -- rescaling it would
  // report half throttle at rest, so it has to be written as a half axis.
  MappingRecorder rec(rig.ctx, rig.id);
  virtualpad::set_raw_axis(rig.vid, 5, 1.0f);
  const RawInput in = settle(rig.ctx, rec);
  CHECK(!in.rests_at_extreme);
  rec.bind(Axis::RightTrigger, in);

  const std::string line = rec.mapping_line();
  CHECK(line.find("righttrigger:+a5") != std::string::npos);
  CHECK(rec.apply());

  virtualpad::set_raw_axis(rig.vid, 5, 0.0f);
  rig.ctx.update();
  CHECK_NEAR(rig.ctx.axis(rig.id, Axis::RightTrigger), 0.0f);
  virtualpad::set_raw_axis(rig.vid, 5, 1.0f);
  rig.ctx.update();
  CHECK_NEAR(rig.ctx.axis(rig.id, Axis::RightTrigger), 1.0f);
}

void test_axis_bound_to_a_button_is_a_half() {
  Rig rig;
  MappingRecorder rec(rig.ctx, rig.id);
  virtualpad::set_raw_axis(rig.vid, 2, -1.0f);
  const RawInput in = settle(rig.ctx, rec);
  rec.bind(Button::West, in);

  const std::string line = rec.mapping_line();
  CHECK(line.find("x:-a2") != std::string::npos);
  CHECK(rec.apply());
  rig.ctx.update();
  CHECK(rig.ctx.down(rig.id, Button::West));
}

void test_hat_binding_round_trips() {
  Rig rig;
  MappingRecorder rec(rig.ctx, rig.id);
  virtualpad::set_raw_hat(rig.vid, 0, 0x04);
  rec.bind(Button::DpadDown, settle(rig.ctx, rec));
  CHECK(rec.mapping_line().find("dpdown:h0.4") != std::string::npos);
  CHECK(rec.apply());
  rig.ctx.update();
  CHECK(rig.ctx.down(rig.id, Button::DpadDown));
}

void test_gone_device_yields_nothing() {
  Rig rig;
  MappingRecorder rec(rig.ctx, rig.id);
  rec.bind(Button::South, record(rig, rec, 0));
  virtualpad::remove(rig.vid);
  rig.ctx.update();
  // Unplugged mid-rebind: no line, no crash, no half-written mapping
  // installed against an id that no longer means anything.
  CHECK(rec.mapping_line().empty());
  CHECK(!rec.apply());
  CHECK(!rec.poll().valid());
}

void test_name_with_a_comma_cannot_break_the_line() {
  Rig rig;
  virtualpad::remove_all();
  virtualpad::Spec s = spec();
  s.name = "Bad, Pad";
  const auto vid = virtualpad::add(s);
  rig.ctx.update();
  const DeviceId id = virtualpad::device_id(vid);

  MappingRecorder rec(rig.ctx, id);
  virtualpad::set_raw_button(vid, 0, true);
  rec.bind(Button::South, settle(rig.ctx, rec));

  const std::string line = rec.mapping_line();
  CHECK(line.find("Bad  Pad") == std::string::npos);
  CHECK(line.find("Bad Pad") != std::string::npos);
  // The line still has to parse, which a stray comma would prevent by
  // turning the rest of the name into a binding field.
  CHECK(rig.ctx.add_mapping(line));
  CHECK(rig.ctx.info(id).mapped);
}

} // namespace

int main() {
  test_detects_a_button();
  test_stability_rejects_a_blip();
  test_baseline_ignores_what_was_already_held();
  test_button_beats_a_brushed_stick();
  test_furthest_axis_wins();
  test_hat_reports_only_the_new_direction();
  test_line_round_trips_through_the_parser();
  test_rebinding_replaces();
  test_inverted_stick_is_written_down();
  test_trigger_at_rest_reads_zero();
  test_half_travel_trigger_is_written_as_a_half();
  test_axis_bound_to_a_button_is_a_half();
  test_hat_binding_round_trips();
  test_gone_device_yields_nothing();
  test_name_with_a_comma_cannot_break_the_line();

  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
