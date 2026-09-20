// Tests for the parts that can be wrong without hardware: the guid, the
// mapping grammar, and how a binding turns raw numbers into a controller.
// No test framework, because a library with no dependencies should not
// grow one to be tested.

#include "mapping.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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
  check(std::fabs(a - b) < 0.001f, what, line);
  if (std::fabs(a - b) >= 0.001f) {
    std::printf("     %.4f != %.4f\n", static_cast<double>(a),
                static_cast<double>(b));
  }
}

#define CHECK(expr) check((expr), #expr, __LINE__)
#define CHECK_NEAR(a, b) check_near((a), (b), #a " ~= " #b, __LINE__)

using namespace gpp;
using namespace gpp::detail;

struct Raw {
  std::vector<std::uint8_t> buttons;
  std::vector<float> axes;
  std::vector<std::uint8_t> hats;

  RawInputs view() const {
    RawInputs r;
    r.buttons = buttons.data();
    r.axes = axes.data();
    r.hats = hats.data();
    r.button_count = static_cast<int>(buttons.size());
    r.axis_count = static_cast<int>(axes.size());
    r.hat_count = static_cast<int>(hats.size());
    return r;
  }
};

struct Mapped {
  std::uint8_t buttons[kButtonCount]{};
  float axes[kAxisCount]{};

  bool down(Button b) const {
    return buttons[static_cast<std::size_t>(b)] != 0;
  }
  float axis(Axis a) const { return axes[static_cast<std::size_t>(a)]; }
};

Mapped run(const Mapping &m, const Raw &raw) {
  Mapped out;
  apply_mapping(m, raw.view(), out.buttons, out.axes);
  return out;
}

void test_guid_roundtrip() {
  const std::string hex = "030000005e0400008e02000014010000";
  const Guid g = Guid::parse(hex);
  CHECK(!g.is_zero());
  CHECK(g.str() == hex);
  // Byte order is little-endian per field, which is what makes a guid
  // built here match a line written by SDL on another machine.
  CHECK(g.bytes[0] == 0x03);
  CHECK(g.bytes[4] == 0x5e && g.bytes[5] == 0x04); // vendor 0x045e, Microsoft

  CHECK(Guid::parse("nonsense").is_zero());
  CHECK(Guid::parse("").is_zero());
  CHECK(Guid::parse("03000000zz0400008e02000014010000").is_zero());
}

void test_guid_layout() {
  const Guid g = make_guid(0x0003, 0x045e, 0x028e, 0x0114, "");
  CHECK(g.str() == "030000005e0400008e02000014010000");

  // A name changes the guid, because the CRC of it lives in bytes 2-3.
  // Two products that differ only by name are otherwise identical, and
  // several vendors ship exactly that.
  const Guid named = make_guid(0x0003, 0x045e, 0x028e, 0x0114, "Xbox Pad");
  CHECK(named != g);
  CHECK(named.bytes[4] == g.bytes[4]);
  CHECK(std::memcmp(named.bytes.data() + 4, g.bytes.data() + 4, 12) == 0);
}

void test_crc16_is_sdls() {
  // The value SDL's crc16 produces for this input; if this changes, every
  // database line carrying a CRC stops matching.
  const std::string s = "A";
  CHECK(crc16(reinterpret_cast<const std::uint8_t *>(s.data()), s.size()) ==
        0x30C0);
}

void test_parse_rejects_junk() {
  Mapping m;
  CHECK(!parse_mapping("", m));
  CHECK(!parse_mapping("# a comment", m));
  CHECK(!parse_mapping("not,a,mapping", m));
  // A guid and a name but no bindings is not useful and not accepted.
  CHECK(!parse_mapping("030000005e0400008e02000014010000,Pad,", m));
}

void test_parse_basic() {
  Mapping m;
  CHECK(parse_mapping(
      "030000005e0400008e02000014010000,Xbox 360 Controller,"
      "a:b0,b:b1,x:b2,y:b3,back:b6,start:b7,leftshoulder:b4,"
      "rightshoulder:b5,leftstick:b9,rightstick:b10,dpup:h0.1,dpdown:h0.4,"
      "dpleft:h0.8,dpright:h0.2,leftx:a0,lefty:a1,rightx:a3,righty:a4,"
      "lefttrigger:a2,righttrigger:a5,platform:Mac OS X,",
      m));
  CHECK(m.name == "Xbox 360 Controller");
  CHECK(m.platform == "Mac OS X");
  CHECK(m.bindings.size() == 20);

  Raw raw;
  raw.buttons.assign(16, 0);
  raw.axes.assign(6, 0.0f);
  raw.hats.assign(1, 0);

  raw.buttons[0] = 1;
  Mapped out = run(m, raw);
  CHECK(out.down(Button::South));
  CHECK(!out.down(Button::East));

  raw.buttons[0] = 0;
  raw.hats[0] = 0x04; // down
  out = run(m, raw);
  CHECK(out.down(Button::DpadDown));
  CHECK(!out.down(Button::DpadUp));

  // A diagonal sets both bits, and both single-direction bindings fire --
  // which is what a game reading dpleft||dpdown expects.
  raw.hats[0] = 0x04 | 0x08;
  out = run(m, raw);
  CHECK(out.down(Button::DpadDown));
  CHECK(out.down(Button::DpadLeft));

  raw.hats[0] = 0;
  raw.axes[0] = 0.5f;
  raw.axes[1] = -1.0f;
  out = run(m, raw);
  CHECK_NEAR(out.axis(Axis::LeftX), 0.5f);
  CHECK_NEAR(out.axis(Axis::LeftY), -1.0f);
}

void test_trigger_full_range_is_rescaled() {
  // The bug this prevents: a trigger on a full-range axis rests at -1, and
  // code that passes it straight through reports half throttle from a
  // trigger nobody is touching.
  Mapping m;
  CHECK(parse_mapping("030000005e0400008e02000014010000,Pad,"
                      "lefttrigger:a2,righttrigger:+a5,x:a4,",
                      m));
  Raw raw;
  raw.buttons.assign(1, 0);
  raw.axes.assign(6, 0.0f);
  raw.axes[2] = -1.0f; // at rest
  raw.axes[5] = 0.0f;
  Mapped out = run(m, raw);
  CHECK_NEAR(out.axis(Axis::LeftTrigger), 0.0f);
  CHECK_NEAR(out.axis(Axis::RightTrigger), 0.0f);

  raw.axes[2] = 1.0f; // fully pulled
  raw.axes[5] = 1.0f;
  out = run(m, raw);
  CHECK_NEAR(out.axis(Axis::LeftTrigger), 1.0f);
  // A half-axis source is already 0..1 and must not be rescaled again.
  CHECK_NEAR(out.axis(Axis::RightTrigger), 1.0f);

  // An axis driving a button crosses at half travel.
  raw.axes[4] = 0.4f;
  out = run(m, raw);
  CHECK(!out.down(Button::West));
  raw.axes[4] = 0.6f;
  out = run(m, raw);
  CHECK(out.down(Button::West));
}

void test_inverted_and_half_axes() {
  Mapping m;
  CHECK(parse_mapping("030000005e0400008e02000014010000,Pad,"
                      "lefty:a1~,-leftx:b0,+leftx:b1,",
                      m));
  Raw raw;
  raw.buttons.assign(2, 0);
  raw.axes.assign(2, 0.0f);

  raw.axes[1] = 0.5f;
  Mapped out = run(m, raw);
  CHECK_NEAR(out.axis(Axis::LeftY), -0.5f);

  // Two buttons standing in for one axis: the d-pad-as-stick case, which
  // only works if bindings to the same output accumulate instead of the
  // last one winning.
  raw.buttons[0] = 1;
  out = run(m, raw);
  CHECK_NEAR(out.axis(Axis::LeftX), -1.0f);
  raw.buttons[1] = 1;
  out = run(m, raw);
  CHECK_NEAR(out.axis(Axis::LeftX), 0.0f);
}

void test_out_of_range_sources_are_safe() {
  // A database line written for a pad with more inputs than this one must
  // not be able to read past the end of anything.
  Mapping m;
  CHECK(parse_mapping("030000005e0400008e02000014010000,Pad,"
                      "a:b30,leftx:a20,dpup:h9.1,",
                      m));
  Raw raw;
  raw.buttons.assign(2, 1);
  raw.axes.assign(2, 1.0f);
  raw.hats.assign(1, 0xF);
  const Mapped out = run(m, raw);
  CHECK(!out.down(Button::South));
  CHECK(!out.down(Button::DpadUp));
  CHECK_NEAR(out.axis(Axis::LeftX), 0.0f);
}

void test_unknown_fields_keep_the_line() {
  // The database gains fields faster than any one consumer of it. A line
  // using one we have never heard of must lose that binding and keep the
  // rest, or a database update breaks every pad at once.
  Mapping m;
  CHECK(parse_mapping("030000005e0400008e02000014010000,Pad,"
                      "a:b0,hyperbutton:b1,hint:SDL_SOMETHING:=1,b:b2,"
                      "crc:a1b2,type:controller,",
                      m));
  CHECK(m.bindings.size() == 2);
  Raw raw;
  raw.buttons.assign(3, 0);
  raw.axes.assign(1, 0.0f);
  raw.buttons[2] = 1;
  const Mapped out = run(m, raw);
  CHECK(out.down(Button::East));
}

void test_hat_angles() {
  // DirectInput's POV, in hundredths of a degree clockwise from up. The
  // diagonals set both bits, because a mapping binds each direction
  // separately and a hat pushed up-right has to satisfy both.
  CHECK(hat_mask_from_centidegrees(0) == 0x01);
  CHECK(hat_mask_from_centidegrees(4500) == (0x01 | 0x02));
  CHECK(hat_mask_from_centidegrees(9000) == 0x02);
  CHECK(hat_mask_from_centidegrees(13500) == (0x02 | 0x04));
  CHECK(hat_mask_from_centidegrees(18000) == 0x04);
  CHECK(hat_mask_from_centidegrees(22500) == (0x04 | 0x08));
  CHECK(hat_mask_from_centidegrees(27000) == 0x08);
  CHECK(hat_mask_from_centidegrees(31500) == (0x08 | 0x01));

  // Centred, in each of the forms -1 arrives as.
  CHECK(hat_mask_from_centidegrees(0xFFFF) == 0);
  CHECK(hat_mask_from_centidegrees(0xFFFFFFFF) == 0);

  // A continuous POV reports the angle it is actually at, and rounds to
  // the nearest of the eight directions rather than truncating towards up.
  CHECK(hat_mask_from_centidegrees(100) == 0x01);
  CHECK(hat_mask_from_centidegrees(8900) == 0x02);
  CHECK(hat_mask_from_centidegrees(35900) == 0x01);
  // Exactly on a boundary rounds away from up, consistently.
  CHECK(hat_mask_from_centidegrees(2250) == (0x01 | 0x02));
}

void test_hat_positions() {
  // HID hats report a position in a declared range. Eight-way:
  for (int i = 0; i < 8; ++i) {
    CHECK(hat_mask_from_position(i, 0, 7) ==
          hat_mask_from_centidegrees(static_cast<std::uint32_t>(i) * 4500));
  }
  // Four-way, where each position is a cardinal direction and nothing is
  // diagonal.
  CHECK(hat_mask_from_position(0, 0, 3) == 0x01);
  CHECK(hat_mask_from_position(1, 0, 3) == 0x02);
  CHECK(hat_mask_from_position(2, 0, 3) == 0x04);
  CHECK(hat_mask_from_position(3, 0, 3) == 0x08);

  // A range that does not start at zero, which plenty of devices declare.
  CHECK(hat_mask_from_position(1, 1, 8) == 0x01);
  CHECK(hat_mask_from_position(3, 1, 8) == 0x02);

  // Out of range is centred -- that is how a hat says "nothing pressed",
  // and reading it as a direction would leave a d-pad stuck.
  CHECK(hat_mask_from_position(15, 0, 7) == 0);
  CHECK(hat_mask_from_position(-1, 0, 7) == 0);
  CHECK(hat_mask_from_position(0, 0, 3) != 0);
}

void test_table_lookup_ladder() {
  MappingTable t;
  const std::string platform = platform_name();
  // Built for this platform so the lookup can match it at all: a line for
  // another platform is stored and never returned.
  CHECK(t.add("030000005e0400008e02000014010000,Pad,a:b0,platform:" + platform +
              ","));
  CHECK(t.add("030000005e0400008e02000000000000,Versionless,a:b1,platform:" +
              platform + ","));
  CHECK(t.add("03000000010000000100000000000000,Other "
              "Platform,a:b0,platform:Fictional,"));
  CHECK(t.size() == 3);

  CHECK(t.find(Guid::parse("030000005e0400008e02000014010000")) != nullptr);
  CHECK(t.find(Guid::parse("03000000010000000100000000000000")) == nullptr);

  // A device whose guid carries a name CRC still finds a line that does
  // not, which is most of them.
  Guid with_crc = Guid::parse("030000005e0400008e02000014010000");
  with_crc.bytes[2] = 0xAB;
  with_crc.bytes[3] = 0xCD;
  const Mapping *m = t.find(with_crc);
  CHECK(m != nullptr && m->name == "Pad");

  // A firmware update bumps the version; the pad must not stop working.
  Guid new_firmware = Guid::parse("030000005e0400008e02000099990000");
  m = t.find(new_firmware);
  CHECK(m != nullptr && m->name == "Versionless");

  CHECK(t.find(Guid::parse("ffffffffffffffffffffffffffffffff")) == nullptr);
}

void test_later_mappings_win() {
  MappingTable t;
  const std::string platform = platform_name();
  CHECK(t.add("030000005e0400008e02000014010000,First,a:b0,platform:" +
              platform + ","));
  CHECK(t.add("030000005e0400008e02000014010000,Second,a:b1,platform:" +
              platform + ","));
  CHECK(t.size() == 1);
  const Mapping *m = t.find(Guid::parse("030000005e0400008e02000014010000"));
  CHECK(m != nullptr && m->name == "Second");
}

void test_add_many_skips_comments() {
  MappingTable t;
  const std::string platform = platform_name();
  const std::string text =
      "# a comment\n"
      "\r\n"
      "030000005e0400008e02000014010000,One,a:b0,platform:" +
      platform +
      ",\n"
      "garbage line\n"
      "030000005e0400008e02000014010001,Two,a:b0,platform:" +
      platform + ",\r\n";
  CHECK(t.add_many(text) == 2);
}

void test_builtin_database_parses() {
  // Not a unit test of ours so much as a smoke test of the build: if the
  // database was compiled in, it has to survive our own parser.
  const char *db = builtin_controller_db();
  if (db == nullptr || db[0] == '\0') {
    std::printf("note: no built-in database in this build\n");
    return;
  }
  MappingTable t;
  const int added = t.add_many(db);
  std::printf("note: built-in database parsed %d mappings for %s\n", added,
              platform_name());
  CHECK(added > 0);
  // The filter in the CMake step should have left only this platform's
  // lines plus the platform-agnostic ones.
  CHECK(t.find(Guid::parse("030000005e0400008e02000014010000")) != nullptr ||
        added < 50);
}

} // namespace

int main() {
  test_guid_roundtrip();
  test_guid_layout();
  test_crc16_is_sdls();
  test_parse_rejects_junk();
  test_parse_basic();
  test_trigger_full_range_is_rescaled();
  test_inverted_and_half_axes();
  test_out_of_range_sources_are_safe();
  test_unknown_fields_keep_the_line();
  test_hat_angles();
  test_hat_positions();
  test_table_lookup_ladder();
  test_later_mappings_win();
  test_add_many_skips_comments();
  test_builtin_database_parses();

  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
