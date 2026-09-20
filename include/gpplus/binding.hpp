// Turning "the player just pressed something" into a database line.
//
// The raw accessors on Context exist so a game can build a rebinding
// screen, and mapping_for() hands a finished line back out, but the step
// between them is left for every game to write again. It is also the step
// with all the traps: a stick that rests at 0.13 is not being pushed, a
// trigger that rests at -1 is not being pulled, a thumb brushing a stick
// on the way to a button must not win the binding, and an axis that
// reports backwards has to be marked so rather than silently inverting
// the game.
//
// The flow this is built for:
//
//   MappingRecorder rec(ctx, pad);
//   rec.begin();                       // "press the button for Jump"
//   ...
//   const RawInput in = rec.poll();    // once a frame, after ctx.update()
//   if (in.valid()) { rec.bind(Button::South, in); next_prompt(); }
//   ...
//   rec.apply();                       // live immediately, no reconnect
//   save(rec.mapping_line());          // and to a file for next time

#ifndef GPPLUS_BINDING_HPP
#define GPPLUS_BINDING_HPP

#include <string>
#include <vector>

#include <gpplus/gamepad.hpp>

namespace gpp {

/// One physical input, as the recorder saw it move.
struct RawInput {
  enum class Kind : std::uint8_t { None, Button, Axis, Hat };

  Kind kind = Kind::None;
  int index = 0;
  /// Which way an axis went from its resting place: +1 or -1. The sign
  /// matters because the prompt asked for a direction -- "push right" --
  /// and hardware that answers with a falling value has to be written
  /// down as inverted, not quietly accepted.
  int direction = 0;
  /// For a hat, the direction bits that came on: 1 up, 2 right, 4 down,
  /// 8 left.
  std::uint8_t hat_mask = 0;
  /**
   * @brief Whether this axis rests at one end of its travel.
   *
   * True for a trigger on its own full-range axis, which sits at -1 when
   * released. It changes how the binding is written -- `a2` rather than
   * `+a2` -- and getting it wrong is the classic gamepad bug: half
   * throttle from a trigger nobody is touching.
   */
  bool rests_at_extreme = false;

  [[nodiscard]] bool valid() const noexcept { return kind != Kind::None; }
  friend bool operator==(const RawInput &a, const RawInput &b) noexcept {
    return a.kind == b.kind && a.index == b.index &&
           a.direction == b.direction && a.hat_mask == b.hat_mask;
  }
  friend bool operator!=(const RawInput &a, const RawInput &b) noexcept {
    return !(a == b);
  }
};

/**
 * @brief Watches one device and writes down what it sees.
 *
 * Holds a reference to the Context, so it must not outlive it. Reading
 * only: it never calls update(), because the game's loop owns when that
 * happens.
 */
class MappingRecorder {
public:
  MappingRecorder(Context &context, DeviceId device);

  /**
   * @brief Takes the baseline: whatever the pad is doing now is "at rest".
   *
   * Call it when the rebinding screen opens, and again whenever the pad
   * has been left alone for a moment. Without it a held trigger or an
   * off-centre stick reads as a press the instant the screen appears.
   */
  void begin();

  /**
   * @brief Once a frame, after Context::update(). Returns what moved.
   *
   * An invalid RawInput means "nothing yet", which is what most frames
   * answer. A detection is reported once and then not again until that
   * input returns to rest, so holding a button does not rebind everything
   * in sequence.
   */
  RawInput poll();

  /// How many consecutive polls an input must stay moved before it counts.
  /// Rejects contact bounce and the momentary spike a stick gives when a
  /// thumb crosses it. Default 3.
  void set_stability(int frames) noexcept;
  /// How far an axis must travel from its baseline to count. Default 0.5 --
  /// deliberately high, because the whole point is to pick the input the
  /// player meant out of the several that are moving.
  void set_axis_threshold(float threshold) noexcept;

  void bind(Button button, const RawInput &input);
  void bind(Axis axis, const RawInput &input);
  /// Binding the same output again replaces it, so a player who picks the
  /// wrong button can simply do it again.
  void clear(Button button);
  void clear(Axis axis);
  void clear_all();

  [[nodiscard]] bool bound(Button button) const noexcept;
  [[nodiscard]] bool bound(Axis axis) const noexcept;
  [[nodiscard]] std::size_t binding_count() const noexcept;

  /**
   * @brief The finished line, in the database's format.
   *
   * Empty if nothing is bound or the device has gone. Carries this
   * platform's `platform:` field, so the line can be appended to a
   * database file that holds every platform's.
   */
  [[nodiscard]] std::string mapping_line() const;

  /// Installs it in the Context it was recording from; takes effect on the
  /// next query, with no reconnect. False if there is nothing to install.
  bool apply();

private:
  struct Entry {
    bool is_axis = false;
    std::uint8_t output = 0;
    RawInput input;
  };

  [[nodiscard]] RawInput detect() const;
  void note(const Entry &entry);

  Context *context_;
  DeviceId device_;
  std::vector<std::uint8_t> baseline_buttons_;
  std::vector<float> baseline_axes_;
  std::vector<std::uint8_t> baseline_hats_;
  std::vector<Entry> entries_;
  RawInput candidate_;
  RawInput latched_;
  int streak_ = 0;
  int stability_ = 3;
  float axis_threshold_ = 0.5f;
};

} // namespace gpp

#endif // GPPLUS_BINDING_HPP
