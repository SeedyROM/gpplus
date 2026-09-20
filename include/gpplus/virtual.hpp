// Synthetic controllers, for tests and for playing a game without one.
//
// A gamepad library is awkward to test and awkward to develop against,
// because the interesting cases are hardware you do not have: the pad that
// disconnects mid-frame, the stick that rests at 0.13, the eight-way hat,
// the device with no mapping. This backend makes those cases ordinary --
// a test constructs the pad it needs, and a CI machine with no USB ports
// exercises every path except the twenty lines that touch the OS.
//
// Compiled only when GPPLUS_BACKEND_VIRTUAL is on; it defaults to on
// wherever tests are built and off in a release build, so shipping code
// cannot accidentally depend on it.

#ifndef GPPLUS_VIRTUAL_HPP
#define GPPLUS_VIRTUAL_HPP

#include <string>

#include <gpplus/gamepad.hpp>

namespace gpp::virtualpad {

/// Names a synthetic device before it exists. Distinct from DeviceId,
/// which the Context assigns once the device is actually admitted.
using VirtualId = std::uint32_t;
inline constexpr VirtualId kInvalidVirtual = 0;

struct Spec {
  std::string name = "Virtual Gamepad";
  /// Left zero to be derived from vendor/product/name, so that a test can
  /// simply state which real product it is pretending to be.
  Guid guid;
  std::uint16_t vendor = 0;
  std::uint16_t product = 0;
  std::uint16_t version = 0;
  /// Raw devices go through the mapping database like an evdev pad; a
  /// backend-mapped one reports Buttons and Axes directly, like
  /// GameController. Both paths are worth testing and they are not the
  /// same code.
  bool backend_mapped = false;
  int raw_button_count = 16;
  int raw_axis_count = 6;
  int raw_hat_count = 1;
  bool rumble = false;
};

/// Appears on the next Context::update(), the way a real plug-in does.
VirtualId add(const Spec &spec);
void remove(VirtualId id);
void remove_all();

/// The id the Context gave it, or kInvalidDevice before the update() that
/// admits it.
[[nodiscard]] DeviceId device_id(VirtualId id);

void set_raw_button(VirtualId id, int index, bool down);
void set_raw_axis(VirtualId id, int index, float value);
void set_raw_hat(VirtualId id, int index, std::uint8_t mask);

/// For a `backend_mapped` device only.
void set_button(VirtualId id, Button b, bool down);
void set_axis(VirtualId id, Axis a, float value);

void set_power(VirtualId id, PowerLevel level);

/// What the last rumble() asked for, so a test can assert that a game
/// actually shook the pad.
struct RumbleState {
  float low = 0.0f;
  float high = 0.0f;
  std::uint32_t duration_ms = 0;
  int call_count = 0;
};
[[nodiscard]] RumbleState rumble_state(VirtualId id);

} // namespace gpp::virtualpad

#endif // GPPLUS_VIRTUAL_HPP
