// The mapping database: what turns "button 7 on this pad" into "Start".
//
// The format is SDL's, and deliberately so -- the thousands of lines in
// SDL_GameControllerDB are the single largest body of controller knowledge
// that exists, contributed over a decade by people with the hardware in
// their hands. It is data, not code, under the same permissive license as
// the rest of this, and reading it is the whole reason a game can support
// a pad nobody here has ever seen.
//
// A line is:
//   <32 hex guid>,<name>,a:b0,b:b1,dpup:h0.1,leftx:a0,lefttrigger:+a2,...
// with an optional trailing `platform:Mac OS X,`.

#ifndef GPPLUS_MAPPING_HPP
#define GPPLUS_MAPPING_HPP

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <gpplus/gamepad.hpp>

namespace gpp::detail {

enum class SrcType : std::uint8_t { None, Button, Axis, Hat };
enum class DstType : std::uint8_t { Button, Axis };

/// One `output:input` pair. Several may target the same output -- a pad
/// whose d-pad is bound to a stick needs `-lefty:b11,+lefty:b12` -- so
/// applying them accumulates rather than assigns.
struct Binding {
  SrcType src = SrcType::None;
  DstType dst = DstType::Button;
  std::int16_t src_index = 0;
  std::uint8_t hat_mask = 0;
  /// Trailing `~`: the hardware reports this axis backwards.
  bool src_invert = false;
  /// Leading `+`/`-` on the input: use only that half of the travel, and
  /// present it as 0..1.
  std::int8_t src_half = 0;
  /// Leading `+`/`-` on the output: this input only ever pushes the output
  /// that way.
  std::int8_t dst_half = 0;
  std::uint8_t dst_index = 0;
};

struct Mapping {
  Guid guid;
  std::string name;
  /// Empty means every platform. A line for another platform is kept but
  /// never matched, so that a config file can hold all of them.
  std::string platform;
  std::vector<Binding> bindings;
  /// The line as written, for handing back out of Context::mapping_for().
  std::string source;
};

/// Reads whatever the hardware currently says, by raw index. Bounds are
/// the implementation's problem: out-of-range reads answer zero, because a
/// database line written for a pad with more axes than this one must not
/// be able to crash the game.
struct RawInputs {
  const std::uint8_t *buttons = nullptr;
  const float *axes = nullptr;
  const std::uint8_t *hats = nullptr;
  int button_count = 0;
  int axis_count = 0;
  int hat_count = 0;

  [[nodiscard]] bool button(int i) const noexcept {
    return i >= 0 && i < button_count && buttons[i] != 0;
  }
  [[nodiscard]] float axis(int i) const noexcept {
    return i >= 0 && i < axis_count ? axes[i] : 0.0f;
  }
  [[nodiscard]] std::uint8_t hat(int i) const noexcept {
    return i >= 0 && i < hat_count ? hats[i] : std::uint8_t{0};
  }
};

/// Parses one line. Returns false, and leaves `out` untouched, on anything
/// that is not one -- including comments and blanks, which callers feeding
/// it a file will see a lot of.
bool parse_mapping(const std::string &line, Mapping &out);

/// Writes the mapped state. Both outputs are fully overwritten, so a
/// binding removed between frames cannot leave a button stuck down.
void apply_mapping(const Mapping &m, const RawInputs &raw,
                   std::uint8_t *buttons_out, float *axes_out);

/**
 * @brief A hat angle, in hundredths of a degree clockwise from up, as the
 * database's direction bitmask (1 up, 2 right, 4 down, 8 left).
 *
 * Lives here, portable and tested, rather than in the Windows backend that
 * needs it: it is the fiddliest pure function either raw backend has, and
 * a machine that cannot compile DirectInput can still prove this right.
 * Rounds to the nearest of the eight directions, because a continuous POV
 * -- some wheels and sticks have one -- reports whatever angle it is
 * actually at rather than a multiple of 45 degrees.
 */
std::uint8_t hat_mask_from_centidegrees(std::uint32_t centidegrees) noexcept;

/**
 * @brief A hat position as that same bitmask.
 *
 * HID hat switches report a position within a declared range instead of an
 * angle: eight positions is usual, four is not rare, and both start at up
 * and go clockwise. Out-of-range means centred, which is how a hat says
 * "nothing pressed".
 */
std::uint8_t hat_mask_from_position(int value, int minimum,
                                    int maximum) noexcept;

/// SDL's CRC-16/ARC over the product name, which is what the guid's middle
/// two bytes hold. Devices that differ only in name are otherwise
/// indistinguishable, and several vendors ship exactly that.
std::uint16_t crc16(const std::uint8_t *data, std::size_t len,
                    std::uint16_t seed = 0) noexcept;

/// Builds the identity the database is keyed by, in SDL's byte layout.
/// `bus` is the USB/Bluetooth bus type from the OS where there is one, or
/// one of the synthetic values SDL uses otherwise (3 = USB, 5 = Bluetooth).
Guid make_guid(std::uint16_t bus, std::uint16_t vendor, std::uint16_t product,
               std::uint16_t version, const std::string &name,
               std::uint8_t driver_signature = 0,
               std::uint8_t driver_data = 0) noexcept;

/**
 * @brief Every mapping known, and the lookup that tolerates near-misses.
 *
 * The near-misses are the point. A device's guid carries a name CRC and a
 * firmware version, and database lines predate firmware revisions; an exact
 * match would mean a pad stops working because its vendor shipped an
 * update. So a lookup falls back, in order, to ignoring the CRC and then
 * ignoring the version -- the same ladder SDL walks, for the same reason.
 */
class MappingTable {
public:
  /// Replaces any mapping with the same guid, so later sources win: the
  /// built-in database, then the user's file, then whatever the game adds.
  bool add(const std::string &line);
  int add_many(const std::string &text);

  [[nodiscard]] const Mapping *find(const Guid &guid) const;
  [[nodiscard]] std::size_t size() const noexcept { return by_guid_.size(); }

private:
  [[nodiscard]] const Mapping *lookup_exact(const Guid &guid) const;

  std::unordered_map<std::string, Mapping> by_guid_;
};

/// The database's spelling of an output, and its inverse. Unknown names
/// answer false, which is how forward-compatibility works here: a line
/// using an output this version has never heard of loses that one binding
/// and keeps the rest.
bool db_button_from_name(const std::string &name, Button &out) noexcept;
bool db_axis_from_name(const std::string &name, Axis &out) noexcept;

} // namespace gpp::detail

#endif // GPPLUS_MAPPING_HPP
