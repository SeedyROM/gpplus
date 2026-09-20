#include "mapping.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gpp::detail {
namespace {

struct NameEntry {
  const char *name;
  std::uint8_t index;
};

// Order follows the enums; the strings are the database's, not ours.
constexpr NameEntry kButtonNames[] = {
    {"a", 0},
    {"b", 1},
    {"x", 2},
    {"y", 3},
    {"back", 4},
    {"guide", 5},
    {"start", 6},
    {"leftstick", 7},
    {"rightstick", 8},
    {"leftshoulder", 9},
    {"rightshoulder", 10},
    {"dpup", 11},
    {"dpdown", 12},
    {"dpleft", 13},
    {"dpright", 14},
    {"misc1", 15},
    {"paddle1", 16},
    {"paddle2", 17},
    {"paddle3", 18},
    {"paddle4", 19},
    {"touchpad", 20},
};

constexpr NameEntry kAxisNames[] = {
    {"leftx", 0},  {"lefty", 1},       {"rightx", 2},
    {"righty", 3}, {"lefttrigger", 4}, {"righttrigger", 5},
};

std::string trim(const std::string &s) {
  std::size_t b = 0;
  std::size_t e = s.size();
  while (b < e && (std::isspace(static_cast<unsigned char>(s[b])) != 0)) {
    ++b;
  }
  while (e > b && (std::isspace(static_cast<unsigned char>(s[e - 1])) != 0)) {
    --e;
  }
  return s.substr(b, e - b);
}

/// Parses the right-hand side of a binding: `b3`, `a1~`, `+a2`, `h0.4`.
bool parse_source(const std::string &spec, Binding &b) {
  std::size_t i = 0;
  if (i < spec.size() && (spec[i] == '+' || spec[i] == '-')) {
    b.src_half = spec[i] == '+' ? 1 : -1;
    ++i;
  }
  if (i >= spec.size()) {
    return false;
  }
  const char kind = spec[i++];
  std::size_t digits_begin = i;
  while (i < spec.size() &&
         (std::isdigit(static_cast<unsigned char>(spec[i])) != 0)) {
    ++i;
  }
  if (i == digits_begin) {
    return false;
  }
  const int index =
      std::atoi(spec.substr(digits_begin, i - digits_begin).c_str());

  switch (kind) {
  case 'b':
    b.src = SrcType::Button;
    b.src_index = static_cast<std::int16_t>(index);
    break;
  case 'a':
    b.src = SrcType::Axis;
    b.src_index = static_cast<std::int16_t>(index);
    break;
  case 'h': {
    // `h0.4`: hat number, then the bitmask of the direction wanted. The
    // mask is a mask and not a direction: `h0.6` (right|down) is how
    // diagonal-only bindings are written, and both bits must be set.
    if (i >= spec.size() || spec[i] != '.') {
      return false;
    }
    ++i;
    digits_begin = i;
    while (i < spec.size() &&
           (std::isdigit(static_cast<unsigned char>(spec[i])) != 0)) {
      ++i;
    }
    if (i == digits_begin) {
      return false;
    }
    b.src = SrcType::Hat;
    b.src_index = static_cast<std::int16_t>(index);
    b.hat_mask = static_cast<std::uint8_t>(
        std::atoi(spec.substr(digits_begin, i - digits_begin).c_str()));
    break;
  }
  default:
    return false;
  }

  if (i < spec.size() && spec[i] == '~') {
    b.src_invert = true;
    ++i;
  }
  return true;
}

bool parse_binding(const std::string &key_in, const std::string &value,
                   Binding &out) {
  Binding b;
  std::string key = key_in;
  if (!key.empty() && (key[0] == '+' || key[0] == '-')) {
    b.dst_half = key[0] == '+' ? 1 : -1;
    key.erase(0, 1);
  }

  Button button{};
  Axis axis{};
  if (db_button_from_name(key, button)) {
    b.dst = DstType::Button;
    b.dst_index = static_cast<std::uint8_t>(button);
  } else if (db_axis_from_name(key, axis)) {
    b.dst = DstType::Axis;
    b.dst_index = static_cast<std::uint8_t>(axis);
  } else {
    return false;
  }

  if (!parse_source(value, b)) {
    return false;
  }
  out = b;
  return true;
}

float clamp_unit(float v) noexcept {
  return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
}

} // namespace

bool db_button_from_name(const std::string &name, Button &out) noexcept {
  for (const NameEntry &e : kButtonNames) {
    if (name == e.name) {
      out = static_cast<Button>(e.index);
      return true;
    }
  }
  return false;
}

bool db_axis_from_name(const std::string &name, Axis &out) noexcept {
  for (const NameEntry &e : kAxisNames) {
    if (name == e.name) {
      out = static_cast<Axis>(e.index);
      return true;
    }
  }
  return false;
}

namespace {

/// Up, then clockwise. Index is the direction, value is the database's
/// bitmask for it, with the diagonals setting both of their bits so that a
/// mapping written as two single-direction bindings fires both.
constexpr std::uint8_t kEightWay[8] = {
    0x01, 0x01 | 0x02, 0x02, 0x02 | 0x04, 0x04, 0x04 | 0x08, 0x08, 0x08 | 0x01,
};

} // namespace

std::uint8_t hat_mask_from_centidegrees(std::uint32_t centidegrees) noexcept {
  // 0xFFFF -- and the sign-extended forms of -1 -- mean centred.
  if ((centidegrees & 0xFFFFu) == 0xFFFFu || centidegrees > 36000u) {
    return 0;
  }
  // Half a sector is 2250, so adding it before dividing rounds to the
  // nearest direction rather than truncating towards up.
  return kEightWay[((centidegrees + 2250u) / 4500u) % 8u];
}

std::uint8_t hat_mask_from_position(int value, int minimum,
                                    int maximum) noexcept {
  const int range = maximum - minimum + 1;
  if (value < minimum || value > maximum || range <= 0) {
    return 0;
  }
  const int position = value - minimum;
  if (range == 4) {
    return kEightWay[(position % 4) * 2];
  }
  return kEightWay[position % 8];
}

std::uint16_t crc16(const std::uint8_t *data, std::size_t len,
                    std::uint16_t seed) noexcept {
  // CRC-16/ARC, reflected, polynomial 0xA001. Not chosen for its merits:
  // it is what SDL hashes names with, and a different one would fail to
  // match every database line that carries a CRC.
  std::uint16_t crc = seed;
  for (std::size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = static_cast<std::uint16_t>((crc >> 1) ^
                                       ((crc & 1) != 0 ? 0xA001u : 0u));
    }
  }
  return crc;
}

Guid make_guid(std::uint16_t bus, std::uint16_t vendor, std::uint16_t product,
               std::uint16_t version, const std::string &name,
               std::uint8_t driver_signature,
               std::uint8_t driver_data) noexcept {
  Guid g;
  const std::uint16_t name_crc =
      name.empty() ? std::uint16_t{0}
                   : crc16(reinterpret_cast<const std::uint8_t *>(name.data()),
                           name.size());
  const auto put16 = [&g](std::size_t at, std::uint16_t v) {
    g.bytes[at] = static_cast<std::uint8_t>(v & 0xFF);
    g.bytes[at + 1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
  };
  put16(0, bus);
  put16(2, name_crc);
  put16(4, vendor);
  put16(6, 0);
  put16(8, product);
  put16(10, 0);
  put16(12, version);
  g.bytes[14] = driver_signature;
  g.bytes[15] = driver_data;
  return g;
}

bool parse_mapping(const std::string &line_in, Mapping &out) {
  const std::string line = trim(line_in);
  if (line.empty() || line[0] == '#') {
    return false;
  }

  std::size_t pos = line.find(',');
  if (pos == std::string::npos) {
    return false;
  }
  Mapping m;
  m.guid = Guid::parse(line.substr(0, pos));
  if (m.guid.is_zero()) {
    return false;
  }

  const std::size_t name_begin = pos + 1;
  pos = line.find(',', name_begin);
  if (pos == std::string::npos) {
    return false;
  }
  m.name = trim(line.substr(name_begin, pos - name_begin));

  std::size_t field_begin = pos + 1;
  while (field_begin <= line.size()) {
    std::size_t field_end = line.find(',', field_begin);
    if (field_end == std::string::npos) {
      field_end = line.size();
    }
    const std::string field =
        trim(line.substr(field_begin, field_end - field_begin));
    field_begin = field_end + 1;
    if (field.empty()) {
      continue;
    }
    const std::size_t colon = field.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    const std::string key = field.substr(0, colon);
    const std::string value = field.substr(colon + 1);

    if (key == "platform") {
      m.platform = value;
      continue;
    }
    // `crc:` pins a line to one product name, and `hint:`/`sdk:`/`type:`
    // steer SDL's own internals. Skipped rather than rejected: an
    // unrecognised field must never cost the line its bindings, because
    // the database gains fields faster than any one consumer of it.
    if (key == "crc" || key == "hint" || key == "sdk" || key == "type") {
      continue;
    }

    Binding b;
    if (parse_binding(key, value, b)) {
      m.bindings.push_back(b);
    }
  }

  if (m.bindings.empty()) {
    return false;
  }
  m.source = line;
  out = std::move(m);
  return true;
}

void apply_mapping(const Mapping &m, const RawInputs &raw,
                   std::uint8_t *buttons_out, float *axes_out) {
  std::memset(buttons_out, 0, kButtonCount);
  for (std::size_t i = 0; i < kAxisCount; ++i) {
    axes_out[i] = 0.0f;
  }

  for (const Binding &b : m.bindings) {
    float value = 0.0f;
    bool pressed = false;

    switch (b.src) {
    case SrcType::Button:
      pressed = raw.button(b.src_index);
      value = pressed ? 1.0f : 0.0f;
      break;
    case SrcType::Hat:
      // All the bits the mask asks for, not any of them: a `h0.3` binding
      // is up-and-right, and a hat pushed straight up must not fire it.
      pressed =
          (raw.hat(b.src_index) & b.hat_mask) == b.hat_mask && b.hat_mask != 0;
      value = pressed ? 1.0f : 0.0f;
      break;
    case SrcType::Axis: {
      float v = raw.axis(b.src_index);
      if (b.src_invert) {
        v = -v;
      }
      if (b.src_half > 0) {
        v = v > 0.0f ? v : 0.0f;
      } else if (b.src_half < 0) {
        v = v < 0.0f ? -v : 0.0f;
      }
      value = v;
      // Half the travel, because a stick bound to a button should not need
      // to be slammed, and a trigger resting at zero must not count.
      pressed = v > 0.5f;
      break;
    }
    case SrcType::None:
      continue;
    }

    if (b.dst == DstType::Button) {
      if (pressed) {
        buttons_out[b.dst_index] = 1;
      }
      continue;
    }

    const auto dst_axis = static_cast<Axis>(b.dst_index);
    const bool is_trigger =
        dst_axis == Axis::LeftTrigger || dst_axis == Axis::RightTrigger;

    float contribution = value;
    if (is_trigger && b.src == SrcType::Axis && b.src_half == 0) {
      // A trigger on its own full-range axis rests at -1 and ends at +1.
      // Without this the game sees half throttle from a trigger nobody is
      // touching, which is the single most common symptom of a hand-rolled
      // mapping layer.
      contribution = (contribution + 1.0f) * 0.5f;
    }
    if (b.dst_half < 0) {
      contribution = -contribution;
    }
    axes_out[b.dst_index] += contribution;
  }

  for (std::size_t i = 0; i < kAxisCount; ++i) {
    axes_out[i] = clamp_unit(axes_out[i]);
  }
  // Triggers are 0..1 by definition; a mapping that drives one negative is
  // wrong, and clamping is kinder than propagating it.
  for (Axis a : {Axis::LeftTrigger, Axis::RightTrigger}) {
    float &v = axes_out[static_cast<std::size_t>(a)];
    if (v < 0.0f) {
      v = 0.0f;
    }
  }
}

bool MappingTable::add(const std::string &line) {
  Mapping m;
  if (!parse_mapping(line, m)) {
    return false;
  }
  by_guid_[m.guid.str()] = std::move(m);
  return true;
}

int MappingTable::add_many(const std::string &text) {
  int added = 0;
  std::size_t begin = 0;
  while (begin <= text.size()) {
    std::size_t end = text.find('\n', begin);
    if (end == std::string::npos) {
      end = text.size();
    }
    std::string line = text.substr(begin, end - begin);
    begin = end + 1;
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (add(line)) {
      ++added;
    }
  }
  return added;
}

const Mapping *MappingTable::lookup_exact(const Guid &guid) const {
  const auto it = by_guid_.find(guid.str());
  if (it == by_guid_.end()) {
    return nullptr;
  }
  const Mapping &m = it->second;
  if (!m.platform.empty() && m.platform != platform_name()) {
    return nullptr;
  }
  return &m;
}

const Mapping *MappingTable::find(const Guid &guid) const {
  if (const Mapping *m = lookup_exact(guid)) {
    return m;
  }
  // Ladder down the parts of the identity that are allowed to drift. The
  // name CRC first: it is absent from most database lines, and present on
  // the device whenever the OS gave us a name. Then the firmware version,
  // because a vendor shipping an update must not unbind the pad.
  Guid relaxed = guid;
  relaxed.bytes[2] = 0;
  relaxed.bytes[3] = 0;
  if (const Mapping *m = lookup_exact(relaxed)) {
    return m;
  }
  relaxed.bytes[12] = 0;
  relaxed.bytes[13] = 0;
  if (const Mapping *m = lookup_exact(relaxed)) {
    return m;
  }
  Guid versionless = guid;
  versionless.bytes[12] = 0;
  versionless.bytes[13] = 0;
  return lookup_exact(versionless);
}

} // namespace gpp::detail
