#include <gpplus/binding.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace gpp {
namespace {

/// An axis whose baseline sits this close to either end is a trigger, not
/// a stick: it travels one way only, and the database writes it as a
/// full-range axis so that the core's -1..1 to 0..1 rescale applies.
constexpr float kExtremeBaseline = 0.7f;

/// How the input is written depends on what it drives, and the three
/// cases are genuinely different:
///
///   - a button output takes a *half* axis, because "pushed far enough
///     one way" is what a button means;
///   - a stick-axis output takes the *whole* axis, or the game gets a
///     stick that only reports one side;
///   - a trigger output takes whichever matches the hardware. A trigger
///     on its own full-range axis rests at -1 and is written whole, so the
///     core's (v+1)/2 rescale applies; one that rests at 0 is written as a
///     half, because rescaling it would report half throttle at rest.
std::string source_text(const RawInput &in, bool axis_output,
                        bool trigger_output) {
  char buffer[32] = {};
  switch (in.kind) {
  case RawInput::Kind::Button:
    std::snprintf(buffer, sizeof(buffer), "b%d", in.index);
    break;
  case RawInput::Kind::Hat:
    std::snprintf(buffer, sizeof(buffer), "h%d.%u", in.index,
                  static_cast<unsigned>(in.hat_mask));
    break;
  case RawInput::Kind::Axis: {
    const bool whole = axis_output && (!trigger_output || in.rests_at_extreme);
    if (whole) {
      // `~` when the hardware answered the prompt by moving the other
      // way. Writing it down beats silently inverting the game.
      std::snprintf(buffer, sizeof(buffer), "a%d%s", in.index,
                    in.direction < 0 ? "~" : "");
    } else {
      std::snprintf(buffer, sizeof(buffer), "%ca%d",
                    in.direction < 0 ? '-' : '+', in.index);
    }
    break;
  }
  case RawInput::Kind::None:
    break;
  }
  return buffer;
}

} // namespace

MappingRecorder::MappingRecorder(Context &context, DeviceId device)
    : context_(&context), device_(device) {
  begin();
}

void MappingRecorder::begin() {
  const DeviceInfo info = context_->info(device_);
  baseline_buttons_.assign(
      static_cast<std::size_t>(std::max(0, info.raw_button_count)), 0);
  baseline_axes_.assign(
      static_cast<std::size_t>(std::max(0, info.raw_axis_count)), 0.0f);
  baseline_hats_.assign(
      static_cast<std::size_t>(std::max(0, info.raw_hat_count)), 0);

  for (std::size_t i = 0; i < baseline_buttons_.size(); ++i) {
    baseline_buttons_[i] =
        context_->raw_button(device_, static_cast<int>(i)) ? 1 : 0;
  }
  for (std::size_t i = 0; i < baseline_axes_.size(); ++i) {
    baseline_axes_[i] = context_->raw_axis(device_, static_cast<int>(i));
  }
  for (std::size_t i = 0; i < baseline_hats_.size(); ++i) {
    baseline_hats_[i] = context_->raw_hat(device_, static_cast<int>(i));
  }
  candidate_ = RawInput{};
  latched_ = RawInput{};
  streak_ = 0;
}

void MappingRecorder::set_stability(int frames) noexcept {
  stability_ = frames < 1 ? 1 : frames;
}

void MappingRecorder::set_axis_threshold(float threshold) noexcept {
  axis_threshold_ = threshold;
}

RawInput MappingRecorder::detect() const {
  // Buttons first, then hats, then axes. A thumb crossing a stick on the
  // way to a button moves both, and the button is what the player meant.
  for (std::size_t i = 0; i < baseline_buttons_.size(); ++i) {
    const bool now = context_->raw_button(device_, static_cast<int>(i));
    if (now && baseline_buttons_[i] == 0) {
      RawInput in;
      in.kind = RawInput::Kind::Button;
      in.index = static_cast<int>(i);
      return in;
    }
  }
  for (std::size_t i = 0; i < baseline_hats_.size(); ++i) {
    const std::uint8_t now = context_->raw_hat(device_, static_cast<int>(i));
    const std::uint8_t added =
        static_cast<std::uint8_t>(now & ~baseline_hats_[i]);
    if (added != 0) {
      RawInput in;
      in.kind = RawInput::Kind::Hat;
      in.index = static_cast<int>(i);
      in.hat_mask = added;
      return in;
    }
  }

  // The furthest-moved axis wins, not the first: a pad whose triggers rest
  // slightly off centre would otherwise always answer with a trigger.
  RawInput best;
  float best_travel = axis_threshold_;
  for (std::size_t i = 0; i < baseline_axes_.size(); ++i) {
    const float now = context_->raw_axis(device_, static_cast<int>(i));
    const float travel = now - baseline_axes_[i];
    if (std::fabs(travel) <= best_travel) {
      continue;
    }
    best_travel = std::fabs(travel);
    best.kind = RawInput::Kind::Axis;
    best.index = static_cast<int>(i);
    best.direction = travel < 0.0f ? -1 : 1;
    best.rests_at_extreme = std::fabs(baseline_axes_[i]) >= kExtremeBaseline;
  }
  return best;
}

RawInput MappingRecorder::poll() {
  const RawInput found = detect();

  if (!found.valid()) {
    // Everything back at rest: whatever was latched can be offered again.
    candidate_ = RawInput{};
    latched_ = RawInput{};
    streak_ = 0;
    return RawInput{};
  }
  if (found == latched_) {
    return RawInput{}; // still held from the detection already reported
  }
  if (found == candidate_) {
    ++streak_;
  } else {
    candidate_ = found;
    streak_ = 1;
  }
  if (streak_ < stability_) {
    return RawInput{};
  }
  latched_ = found;
  candidate_ = RawInput{};
  streak_ = 0;
  return found;
}

void MappingRecorder::note(const Entry &entry) {
  const auto same = [&entry](const Entry &e) {
    return e.is_axis == entry.is_axis && e.output == entry.output;
  };
  const auto it = std::find_if(entries_.begin(), entries_.end(), same);
  if (it != entries_.end()) {
    *it = entry;
    return;
  }
  entries_.push_back(entry);
}

void MappingRecorder::bind(Button button, const RawInput &input) {
  if (!input.valid()) {
    return;
  }
  Entry entry;
  entry.is_axis = false;
  entry.output = static_cast<std::uint8_t>(button);
  entry.input = input;
  note(entry);
}

void MappingRecorder::bind(Axis axis, const RawInput &input) {
  if (!input.valid()) {
    return;
  }
  Entry entry;
  entry.is_axis = true;
  entry.output = static_cast<std::uint8_t>(axis);
  entry.input = input;
  note(entry);
}

void MappingRecorder::clear(Button button) {
  const auto out = static_cast<std::uint8_t>(button);
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [out](const Entry &e) {
                                  return !e.is_axis && e.output == out;
                                }),
                 entries_.end());
}

void MappingRecorder::clear(Axis axis) {
  const auto out = static_cast<std::uint8_t>(axis);
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [out](const Entry &e) {
                                  return e.is_axis && e.output == out;
                                }),
                 entries_.end());
}

void MappingRecorder::clear_all() { entries_.clear(); }

bool MappingRecorder::bound(Button button) const noexcept {
  const auto out = static_cast<std::uint8_t>(button);
  return std::any_of(entries_.begin(), entries_.end(), [out](const Entry &e) {
    return !e.is_axis && e.output == out;
  });
}

bool MappingRecorder::bound(Axis axis) const noexcept {
  const auto out = static_cast<std::uint8_t>(axis);
  return std::any_of(entries_.begin(), entries_.end(), [out](const Entry &e) {
    return e.is_axis && e.output == out;
  });
}

std::size_t MappingRecorder::binding_count() const noexcept {
  return entries_.size();
}

std::string MappingRecorder::mapping_line() const {
  if (entries_.empty()) {
    return {};
  }
  const DeviceInfo info = context_->info(device_);
  if (info.id == kInvalidDevice) {
    return {};
  }

  std::string line = info.guid.str();
  line += ',';
  // A comma in the name would split the line into fields that are not
  // fields. Rare, entirely possible, and dropped rather than substituted
  // so that "Bad, Pad" reads as "Bad Pad" and not "Bad  Pad".
  std::string name = info.name.empty() ? std::string("Gamepad") : info.name;
  name.erase(std::remove(name.begin(), name.end(), ','), name.end());
  line += name;

  for (const Entry &e : entries_) {
    const char *output = e.is_axis ? db_name(static_cast<Axis>(e.output))
                                   : db_name(static_cast<Button>(e.output));
    if (output == nullptr || output[0] == '\0') {
      continue;
    }
    line += ',';
    line += output;
    line += ':';
    const bool trigger_output =
        e.is_axis && (static_cast<Axis>(e.output) == Axis::LeftTrigger ||
                      static_cast<Axis>(e.output) == Axis::RightTrigger);
    line += source_text(e.input, e.is_axis, trigger_output);
  }
  line += ",platform:";
  line += platform_name();
  line += ',';
  return line;
}

bool MappingRecorder::apply() {
  const std::string line = mapping_line();
  return !line.empty() && context_->add_mapping(line);
}

} // namespace gpp
