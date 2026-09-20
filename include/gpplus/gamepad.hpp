// gpplus - gamepad input for C++, without a platform layer attached.
//
// The problem this exists for: a game that already has a window and a
// renderer (sokol, GLFW, raylib, its own) still has no gamepads. SDL has
// the best answer in the business -- a backend per platform plus the
// community mapping database -- but taking it means taking a second video,
// audio, event and threading layer beside the one already there. GLFW's
// gamepad support is tied to a GLFW window. libgamepad has no macOS
// backend. So: the controller half of SDL's answer, on its own, reading
// the same mapping database, with no dependency of any kind.
//
// C++17. No exceptions thrown, no RTTI required, no allocation after a
// device is opened.

#ifndef GPPLUS_GAMEPAD_HPP
#define GPPLUS_GAMEPAD_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gpplus/config.hpp>

namespace gpp {

/**
 * @brief Buttons, named by where they sit rather than what they say.
 *
 * South/East/West/North rather than A/B/X/Y because the letters move: a
 * Nintendo pad's A is where an Xbox pad's B is. Position is what is
 * actually stable across hardware, so code that wants "the bottom face
 * button" can ask for it and be right everywhere. The database's `a:` is
 * this South, `b:` is East, and so on -- the letters are the format's, and
 * they stop at the parser.
 */
enum class Button : std::uint8_t {
  South,
  East,
  West,
  North,
  Back,
  Guide,
  Start,
  LeftStick,
  RightStick,
  LeftShoulder,
  RightShoulder,
  DpadUp,
  DpadDown,
  DpadLeft,
  DpadRight,
  /// The share/capture/microphone button, whatever this pad calls it.
  Misc1,
  Paddle1,
  Paddle2,
  Paddle3,
  Paddle4,
  /// The touchpad pressed as a button (DualShock/DualSense).
  Touchpad,
  Count,
};

enum class Axis : std::uint8_t {
  LeftX,
  LeftY,
  RightX,
  RightY,
  LeftTrigger,
  RightTrigger,
  Count,
};

enum class Stick : std::uint8_t { Left, Right };

inline constexpr std::size_t kButtonCount =
    static_cast<std::size_t>(Button::Count);
inline constexpr std::size_t kAxisCount = static_cast<std::size_t>(Axis::Count);

/// Stable within one run of the program: ids are handed out increasing and
/// never reused, so a stored id cannot silently come to mean a different
/// pad after a reconnect. `kInvalidDevice` is what every query falls back
/// to, and every query on it is safe and answers "nothing".
using DeviceId = std::uint32_t;
inline constexpr DeviceId kInvalidDevice = 0;

/**
 * @brief The 16-byte device identity the mapping database is keyed by.
 *
 * Laid out exactly as SDL's: bus type, a CRC of the product name, vendor,
 * product and version, all little-endian. The layout matters because the
 * database is a flat file of these in hex and nothing else joins a line to
 * a device.
 */
struct Guid {
  std::array<std::uint8_t, 16> bytes{};

  [[nodiscard]] bool is_zero() const noexcept;
  /// 32 lowercase hex characters, the form used in the database.
  [[nodiscard]] std::string str() const;
  /// Parses that form. Returns a zero guid on anything malformed.
  static Guid parse(const std::string &hex) noexcept;

  friend bool operator==(const Guid &a, const Guid &b) noexcept {
    return a.bytes == b.bytes;
  }
  friend bool operator!=(const Guid &a, const Guid &b) noexcept {
    return !(a == b);
  }
};

enum class PowerLevel : std::uint8_t {
  Unknown,
  Wired,
  Empty,
  Low,
  Medium,
  Full,
  Charging,
};

/// What a backend can do with a device beyond reading it. Queried rather
/// than assumed: two pads on the same backend differ.
struct Capabilities {
  bool rumble = false;
  bool trigger_rumble = false;
  bool led = false;
};

struct DeviceInfo {
  DeviceId id = kInvalidDevice;
  std::string name;
  Guid guid;
  std::uint16_t vendor = 0;
  std::uint16_t product = 0;
  std::uint16_t version = 0;
  /**
   * @brief Whether this device's layout is known.
   *
   * False means the pad is connected and readable but nothing knows which
   * physical button is South, because no database line matches its guid and
   * the backend does not report a layout of its own. Its buttons and axes
   * still arrive through raw_button()/raw_axis(), which is what a "press
   * the button you want to bind" screen needs -- see the mapping helpers
   * below.
   */
  bool mapped = false;
  Capabilities caps;
  PowerLevel power = PowerLevel::Unknown;
  /// Which backend owns it: "gamecontroller", "iokit", "evdev", "xinput".
  /// Diagnostic only; nothing should branch on it.
  const char *backend = "";
  /// How many numbered inputs the hardware has, before any mapping.
  int raw_button_count = 0;
  int raw_axis_count = 0;
  int raw_hat_count = 0;
};

struct Event {
  enum class Type : std::uint8_t {
    DeviceAdded,
    DeviceRemoved,
    ButtonDown,
    ButtonUp,
    AxisMotion,
  };
  Type type = Type::DeviceAdded;
  DeviceId device = kInvalidDevice;
  Button button = Button::South;
  Axis axis = Axis::LeftX;
  /// For AxisMotion: the value after deadzone treatment.
  float value = 0.0f;
};

enum class DeadzoneMode : std::uint8_t {
  /// The stick as a whole is measured, and the live range is rescaled to
  /// start at zero. The default, and the only one that behaves: a per-axis
  /// deadzone leaves a square hole, so a stick pushed diagonally escapes it
  /// sooner than one pushed straight.
  RadialScaled,
  /// Each axis independently, no rescaling. For code that wants the raw
  /// feel and does its own shaping.
  PerAxis,
  None,
};

struct Config {
  /// Fraction of full deflection treated as centred.
  float stick_deadzone = 0.15f;
  /// Triggers rest slightly off zero on worn hardware and the resting value
  /// is not symmetrical, so this is separate and smaller.
  float trigger_deadzone = 0.06f;
  DeadzoneMode deadzone_mode = DeadzoneMode::RadialScaled;
  /**
   * @brief Which way stick Y points.
   *
   * Default false: Y is **down-positive**, matching the database, SDL, and
   * screen coordinates. Set true for a Y-up world, and the flip happens
   * once here instead of at every call site -- which is the bug this
   * exists to prevent, because a flip applied per device inverts exactly
   * one of them.
   */
  bool y_up = false;
  /// Load the mapping database compiled into the library, if there is one.
  /// See GPPLUS_BUNDLE_CONTROLLER_DB in the CMake options.
  bool load_builtin_db = true;
  /// Also load this file, if non-empty, over the top of the built-in one.
  /// Later mappings win, so this is how a game ships fixes without a
  /// rebuild.
  std::string db_path;
  /// And this environment variable, if set, the same way. Cleared to
  /// disable.
  std::string db_env_var = "GPPLUS_GAMECONTROLLERDB";
  /// Keep reporting devices with no known mapping. They answer nothing
  /// through down()/axis() but everything through raw_button()/raw_axis(),
  /// which is what a binding screen wants. Set false to hide them.
  bool report_unmapped = true;
  /// Seconds between scans for newly plugged-in hardware, on backends that
  /// have no hotplug notification of their own. Zero scans every update().
  float hotplug_interval = 1.0f;
};

struct Vec2 {
  float x = 0.0f;
  float y = 0.0f;
};

/**
 * @brief Everything the library knows, and the only thing that owns state.
 *
 * A Context is explicit rather than global so that a test can build one,
 * feed it a synthetic device and destroy it; shared() exists because most
 * games want exactly one and passing it around is noise. Not thread-safe:
 * call update() and every query from the same thread. Several backends
 * insist on the main thread anyway.
 */
class Context {
public:
  explicit Context(const Config &config = {});
  ~Context();
  Context(Context &&) noexcept;
  Context &operator=(Context &&) noexcept;
  Context(const Context &) = delete;
  Context &operator=(const Context &) = delete;

  /// The process-wide context, built on first use with default settings.
  /// Use configure() before the first call to give it a different Config.
  static Context &shared();
  /// Replaces shared()'s configuration. Only legal before the first
  /// shared() call; afterwards it returns false and changes nothing.
  static bool configure(const Config &config);

  /**
   * @brief Reads the hardware. Call once per frame, before any query.
   *
   * Everything happens here: hotplug, polling, deadzones, edge detection
   * and the event queue. Nothing in this library has a callback, a thread
   * or a timer, so a frame that does not call update() sees the previous
   * frame's world, which is the behaviour a paused game wants.
   */
  void update();

  /// Events since the previous update(), in order. Valid until the next
  /// update().
  [[nodiscard]] const std::vector<Event> &events() const noexcept;

  [[nodiscard]] std::size_t device_count() const noexcept;
  /// Connected devices, in the order they arrived.
  [[nodiscard]] const std::vector<DeviceId> &devices() const noexcept;
  /// The first connected device, or kInvalidDevice. For the common case of
  /// a single-player game that does not care which pad.
  [[nodiscard]] DeviceId first_device() const noexcept;
  [[nodiscard]] bool connected(DeviceId id) const noexcept;
  /// A default-constructed DeviceInfo for an id that is gone.
  [[nodiscard]] DeviceInfo info(DeviceId id) const;

  [[nodiscard]] bool down(DeviceId id, Button b) const noexcept;
  /// Rose during the most recent update(). True for exactly one update,
  /// however many fixed simulation steps that frame runs.
  [[nodiscard]] bool pressed(DeviceId id, Button b) const noexcept;
  [[nodiscard]] bool released(DeviceId id, Button b) const noexcept;

  /// Deadzone applied, triggers 0..1, sticks -1..1.
  [[nodiscard]] float axis(DeviceId id, Axis a) const noexcept;
  /// Straight from the hardware, before deadzone or Y flip.
  [[nodiscard]] float axis_raw(DeviceId id, Axis a) const noexcept;
  /// Both axes of a stick, treated as one vector -- which is the only way
  /// a radial deadzone can be applied.
  [[nodiscard]] Vec2 stick(DeviceId id, Stick s) const noexcept;

  /// Whether the player is touching it at all: any button, or a stick out
  /// of its deadzone. Triggers are excluded, because a worn trigger resting
  /// off zero would otherwise claim the player forever.
  [[nodiscard]] bool active(DeviceId id) const noexcept;

  /// Numbered inputs, before mapping. The mapped queries above are what a
  /// game should use; these are for a rebinding screen, and for an
  /// unmapped pad they are all there is.
  [[nodiscard]] bool raw_button(DeviceId id, int index) const noexcept;
  [[nodiscard]] float raw_axis(DeviceId id, int index) const noexcept;
  /// Hat as the database's bitmask: 1 up, 2 right, 4 down, 8 left.
  [[nodiscard]] std::uint8_t raw_hat(DeviceId id, int index) const noexcept;

  /**
   * @brief Runs the motors until `duration_ms` elapses or the next call.
   *
   * Intensities are 0..1: `low` the heavy motor, `high` the light one. A
   * pad with one motor gets the larger of the two, and a pad with none
   * returns false. Returns false rather than reporting an error, because a
   * player without rumble is not a fault condition.
   *
   * The library stops the motors when the Context is destroyed. This
   * matters more than it looks: on several platforms a running effect is
   * owned by a system service and outlives the process, so a game that
   * quits mid-explosion leaves the pad buzzing until it is power-cycled.
   */
  bool rumble(DeviceId id, float low, float high, std::uint32_t duration_ms);
  /// The trigger motors on hardware that has them (Xbox One and later).
  bool rumble_triggers(DeviceId id, float left, float right,
                       std::uint32_t duration_ms);
  void stop_rumble(DeviceId id);

  /// The light bar or player indicator, where there is one.
  bool set_led(DeviceId id, std::uint8_t r, std::uint8_t g, std::uint8_t b);

  /**
   * @brief Adds one mapping line in the database's format.
   *
   * `03000000...,Pad Name,a:b1,b:b2,leftx:a0,...,platform:Mac OS X,`
   *
   * A line for a guid already known replaces it, so this is also how a
   * game overrides a database entry it disagrees with. Lines for another
   * platform are accepted and stored but never matched. Returns false if
   * the line does not parse.
   */
  bool add_mapping(const std::string &line);
  /// One line per mapping, `#` comments and blanks ignored. Returns how
  /// many were added, or -1 if the file could not be read.
  int add_mappings_from_string(const std::string &text);
  int add_mappings_from_file(const std::string &path);

  /// The mapping in use for a connected device, in the same format, or
  /// empty if it has none. What a binding screen writes back out.
  [[nodiscard]] std::string mapping_for(DeviceId id) const;
  [[nodiscard]] std::size_t mapping_count() const noexcept;

  [[nodiscard]] const Config &config() const noexcept;
  /// Deadzones and Y direction can change at any time; the rest of Config
  /// is read at construction and ignored here.
  void set_config(const Config &config);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Which platform string this build matches in the database's
/// `platform:` field: "Windows", "Mac OS X", "Linux", "Android", "iOS".
[[nodiscard]] const char *platform_name() noexcept;

/// Backends compiled into this build, in polling order, null-terminated.
/// Empty on a platform none of them supports -- which is a working build
/// that reports no devices, not a broken one.
[[nodiscard]] const char *const *compiled_backends() noexcept;

[[nodiscard]] const char *to_string(Button b) noexcept;
[[nodiscard]] const char *to_string(Axis a) noexcept;
/// The database's own spelling: "a", "b", "dpup", "leftx", "lefttrigger".
[[nodiscard]] const char *db_name(Button b) noexcept;
[[nodiscard]] const char *db_name(Axis a) noexcept;

/// The mapping database compiled in, or an empty string if the build has
/// none. Its license is SDL's; see LICENSE.
[[nodiscard]] const char *builtin_controller_db() noexcept;

} // namespace gpp

#endif // GPPLUS_GAMEPAD_HPP
