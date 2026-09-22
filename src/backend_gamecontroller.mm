// macOS/iOS: GameController.framework. A *mapped* backend -- the framework
// already knows which button is which, for every pad Apple supports, so
// the mapping database is not consulted for these devices at all.
//
// Polled rather than driven by GCControllerDidConnectNotification, and the
// polling is what makes hotplug free: [GCController controllers] is live,
// so a pad plugged in mid-game turns up on the next frame with nothing
// registered and nothing to tear down. It also keeps every read on the
// calling thread, where valueChangedHandler callbacks would arrive on an
// arbitrary queue and need marshalling back.
//
// Compiled only where CMake found GameController.framework; there is no
// #ifdef in this file.

#include "backend.hpp"
#include "mapping.hpp"

#include <chrono>
#include <cmath>
#include <memory>
#include <vector>

#import <CoreHaptics/CoreHaptics.h>
#import <GameController/GameController.h>

namespace gpp::detail {
namespace {

using Clock = std::chrono::steady_clock;

/// One continuous haptic pattern per motor, kept running between frames
/// with its intensity steered by a dynamic parameter. Starting an engine
/// per rumble is expensive and audibly gappy, and a transient event cannot
/// express "shake harder for a while", which is what every camera-trauma
/// system actually wants.
struct Motor {
  CHHapticEngine *engine = nil;
  id<CHHapticPatternPlayer> player = nil;
  float intensity = -1.0f;
  bool failed = false;
  // Set the moment intensity settles at ~0, cleared the moment it's asked
  // for a non-zero value again. What lets poll() notice "quiet for a
  // while" and release the engine instead of leaving it running forever
  // at zero intensity -- see kMotorIdleTimeout.
  bool idle = false;
  Clock::time_point idle_since{};
};

struct Entry {
  GCController *controller = nil;
  int slot = -1;
  Motor low;  // heavy motor: left handle
  Motor high; // light motor: right handle
  Motor trigger_left;
  Motor trigger_right;
  Clock::time_point rumble_until{};
  bool rumble_timed = false;
};

/// Stops one motor. With `pending`, the engine's stop is tracked by that
/// group so the caller can wait for the haptic service to acknowledge it.
void stop_motor(Motor &m, dispatch_group_t pending = nil) {
  if (m.engine != nil) {
    // Detach the handlers before anything else. They capture a pointer to
    // this Motor, and releasing or stopping the engine below makes the
    // system call them -- on some other queue, later, by which time the
    // Entry that owns the Motor may have been freed. Empty blocks rather
    // than nil, because the properties are declared non-null.
    m.engine.stoppedHandler = ^(CHHapticEngineStoppedReason) {
    };
    m.engine.resetHandler = ^{
    };
  }
  if (m.player != nil) {
    // Zero the intensity before stopping rather than only stopping.
    // Stopping is asynchronous and the process may be gone before it
    // completes; a parameter takes effect now, so the motor is already
    // quiet whatever happens to the teardown.
    CHHapticDynamicParameter *off = [[CHHapticDynamicParameter alloc]
        initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl
                      value:0.0f
               relativeTime:0];
    [m.player sendParameters:@[ off ] atTime:0 error:nil];
    [m.player stopAtTime:0 error:nil];
  }
  if (m.engine != nil) {
    if (pending != nil) {
      dispatch_group_enter(pending);
      [m.engine stopWithCompletionHandler:^(NSError *) {
        dispatch_group_leave(pending);
      }];
    } else {
      [m.engine stopWithCompletionHandler:nil];
    }
  }
  m.player = nil;
  m.engine = nil;
  m.intensity = -1.0f;
  m.idle = false;
  m.idle_since = Clock::time_point{};
}

/**
 * @brief Releases a motor's engine once it has sat at ~zero intensity for
 * kMotorIdleTimeout.
 *
 * A haptic engine costs real, ongoing CPU in the haptics service --
 * visible as elevated gamecontrollerd usage -- for as long as it stays
 * started, which with autoShutdownEnabled = NO and an infinite-duration
 * continuous player is otherwise "for the rest of the session", even at
 * zero intensity, even after a single one-off rumble (a hit-reaction
 * camera shake five minutes into a level, say). The timeout is long
 * enough that a firefight's rapid hits do not each pay start_motor's
 * engine-creation cost, and short enough that a rumble nobody has felt
 * for a while stops being a tax on everything after it.
 */
constexpr Clock::duration kMotorIdleTimeout = std::chrono::seconds(3);

void stop_idle_motor(Motor &m, Clock::time_point now) {
  if (m.player == nil || !m.idle) {
    return;
  }
  if (now - m.idle_since >= kMotorIdleTimeout) {
    stop_motor(m);
  }
}

bool start_motor(Motor &m, GCController *controller, NSString *locality) {
  if (m.failed) {
    return false;
  }
  if (m.player != nil) {
    return true;
  }
  GCDeviceHaptics *haptics = controller.haptics;
  if (haptics == nil ||
      ![haptics.supportedLocalities containsObject:locality]) {
    m.failed = true;
    return false;
  }
  CHHapticEngine *engine = [haptics createEngineWithLocality:locality];
  if (engine == nil) {
    m.failed = true;
    return false;
  }
  // The engine stops on its own -- idle, app deactivation, a media server
  // reset -- and a stopped engine plays nothing and says nothing. Dropping
  // the handles means the next rumble rebuilds it, which is the difference
  // between rumble that dies after the first alt-tab and rumble that does
  // not.
  engine.autoShutdownEnabled = NO;
  __block Motor *self_motor = &m;
  engine.stoppedHandler = ^(CHHapticEngineStoppedReason reason) {
    (void)reason;
    self_motor->engine = nil;
    self_motor->player = nil;
    self_motor->intensity = -1.0f;
  };
  engine.resetHandler = ^{
    self_motor->engine = nil;
    self_motor->player = nil;
    self_motor->intensity = -1.0f;
  };

  NSError *err = nil;
  [engine startAndReturnError:&err];
  if (err != nil) {
    m.failed = true;
    return false;
  }
  CHHapticEventParameter *p = [[CHHapticEventParameter alloc]
      initWithParameterID:CHHapticEventParameterIDHapticIntensity
                    value:1.0f];
  CHHapticEvent *event =
      [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous
                                    parameters:@[ p ]
                                  relativeTime:0
                                      duration:GCHapticDurationInfinite];
  CHHapticPattern *pattern = [[CHHapticPattern alloc] initWithEvents:@[ event ]
                                                          parameters:@[]
                                                               error:&err];
  if (pattern == nil || err != nil) {
    m.failed = true;
    return false;
  }
  // A simple player, not an advanced one: createAdvancedPlayerWithPattern
  // is refused on this path ("Couldn't communicate with a helper
  // application") and nothing here needs looping or seeking. Worth knowing
  // before someone upgrades it on principle.
  id<CHHapticPatternPlayer> player = [engine createPlayerWithPattern:pattern
                                                               error:&err];
  if (player == nil || err != nil) {
    m.failed = true;
    return false;
  }
  [player startAtTime:0 error:&err];
  if (err != nil) {
    m.failed = true;
    return false;
  }
  m.engine = engine;
  m.player = player;
  // Out of range on purpose so the first set always sends: the pattern's
  // own intensity is 1.0 and the parameter scales it, so until that first
  // send the motor would be at full.
  m.intensity = -1.0f;
  return true;
}

void set_motor(Motor &m, GCController *controller, NSString *locality,
               float intensity) {
  if (intensity < 0.01f && m.player == nil) {
    return; // nothing playing, nothing asked for: do not build an engine
  }
  if (!start_motor(m, controller, locality)) {
    return;
  }
  if (std::abs(intensity - m.intensity) < 0.01f) {
    return;
  }
  m.intensity = intensity;
  if (intensity < 0.01f) {
    if (!m.idle) {
      m.idle = true;
      m.idle_since = Clock::now();
    }
  } else {
    m.idle = false;
  }
  CHHapticDynamicParameter *dyn = [[CHHapticDynamicParameter alloc]
      initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl
                    value:intensity
             relativeTime:0];
  [m.player sendParameters:@[ dyn ] atTime:0 error:nil];
}

PowerLevel power_of(GCController *c) {
  if (@available(macOS 11.0, iOS 14.0, *)) {
    GCDeviceBattery *battery = c.battery;
    if (battery == nil) {
      return PowerLevel::Wired;
    }
    switch (battery.batteryState) {
    case GCDeviceBatteryStateCharging:
      return PowerLevel::Charging;
    case GCDeviceBatteryStateFull:
      return PowerLevel::Full;
    case GCDeviceBatteryStateDischarging: {
      const float level = battery.batteryLevel;
      if (level > 0.7f) {
        return PowerLevel::Full;
      }
      if (level > 0.4f) {
        return PowerLevel::Medium;
      }
      if (level > 0.1f) {
        return PowerLevel::Low;
      }
      return PowerLevel::Empty;
    }
    default:
      return PowerLevel::Unknown;
    }
  }
  return PowerLevel::Unknown;
}

class GameControllerBackend final : public Backend {
public:
  const char *name() const noexcept override { return "gamecontroller"; }

  bool init(Host *host) override {
    host_ = host;
    // GCController only publishes controllers once the framework has been
    // told to look, and on macOS that is this call. Without it the first
    // poll finds nothing and the pad appears a second later, which reads
    // as a flaky pad rather than a missing call. begin_discovery() bounds
    // how long that scan is allowed to run -- see its comment.
    begin_discovery();
    return true;
  }

  void shutdown() override {
    if (discovery_active_) {
      [GCController stopWirelessControllerDiscovery];
      discovery_active_ = false;
    }
    // Every motor's stop is issued first and waited for once, because
    // stopping is asynchronous and a process that exits straight after
    // asking can be gone before the haptic service has acted -- which is
    // the pad that keeps buzzing after the game has quit. Bounded, so a
    // service that never answers costs a quarter of a second, not a hang.
    // Only here: a disconnect mid-game must not stall a frame.
    dispatch_group_t pending = dispatch_group_create();
    for (auto &e : entries_) {
      stop_all(*e, pending);
    }
    dispatch_group_wait(pending,
                        dispatch_time(DISPATCH_TIME_NOW, 250 * NSEC_PER_MSEC));
    entries_.clear();
    host_ = nullptr;
  }

  void poll(bool rescan) override {
    update_discovery();

    // The controllers array is live, so scanning it *is* the hotplug
    // check; `rescan` only paces the battery query below.
    NSArray<GCController *> *controllers = [GCController controllers];

    // Gone first, so that a slot freed by a disconnect can be reused by a
    // reconnect in the same frame.
    for (std::size_t i = entries_.size(); i-- > 0;) {
      Entry &e = *entries_[i];
      if (![controllers containsObject:e.controller]) {
        stop_all(e);
        host_->remove_device(e.slot);
        entries_.erase(entries_.begin() + static_cast<long>(i));
      }
    }

    for (GCController *c in controllers) {
      if (c.extendedGamepad == nil) {
        // A remote, a racing wheel's non-gamepad profile, a keyboard.
        // Not an error and not a gamepad.
        continue;
      }
      if (find(c) == nullptr) {
        add(c);
      }
    }

    const auto now = Clock::now();
    for (auto &owned : entries_) {
      Entry &e = *owned;
      if (e.rumble_timed && now >= e.rumble_until) {
        e.rumble_timed = false;
        set_motor(e.low, e.controller, GCHapticsLocalityLeftHandle, 0.0f);
        set_motor(e.high, e.controller, GCHapticsLocalityRightHandle, 0.0f);
      }
      stop_idle_motor(e.low, now);
      stop_idle_motor(e.high, now);
      stop_idle_motor(e.trigger_left, now);
      stop_idle_motor(e.trigger_right, now);
      read(e);
      if (rescan) {
        // Battery state is a system query, not a register read; once a
        // second is plenty and every frame is not free.
        host_->set_power(e.slot, power_of(e.controller));
      }
    }
  }

  bool rumble(void *handle, float low, float high,
              std::uint32_t duration_ms) override {
    Entry *e = static_cast<Entry *>(handle);
    if (e == nullptr) {
      return false;
    }
    set_motor(e->low, e->controller, GCHapticsLocalityLeftHandle, low);
    set_motor(e->high, e->controller, GCHapticsLocalityRightHandle, high);
    e->rumble_timed = duration_ms > 0;
    e->rumble_until = Clock::now() + std::chrono::milliseconds(duration_ms);
    return !e->low.failed || !e->high.failed;
  }

  bool rumble_triggers(void *handle, float left, float right,
                       std::uint32_t duration_ms) override {
    Entry *e = static_cast<Entry *>(handle);
    if (e == nullptr) {
      return false;
    }
    (void)duration_ms;
    set_motor(e->trigger_left, e->controller, GCHapticsLocalityLeftTrigger,
              left);
    set_motor(e->trigger_right, e->controller, GCHapticsLocalityRightTrigger,
              right);
    return !e->trigger_left.failed || !e->trigger_right.failed;
  }

  bool set_led(void *handle, std::uint8_t r, std::uint8_t g,
               std::uint8_t b) override {
    Entry *e = static_cast<Entry *>(handle);
    if (e == nullptr) {
      return false;
    }
    if (@available(macOS 11.0, iOS 14.0, *)) {
      GCDeviceLight *light = e->controller.light;
      if (light == nil) {
        return false;
      }
      light.color = [[GCColor alloc] initWithRed:r / 255.0f
                                           green:g / 255.0f
                                            blue:b / 255.0f];
      return true;
    }
    return false;
  }

private:
  // How long one discovery burst runs before this stops it itself, rather
  // than trusting the framework's own undocumented timeout. Long enough to
  // catch a pad that's paired but was asleep or off when the app launched
  // -- the observed behaviour this call exists for is a pad appearing "a
  // second later" -- and short enough that this is not an active Bluetooth
  // scan for the rest of the session. Apple's own guidance frames
  // discovery as a user-initiated "pairing mode" (pause gameplay, show a
  // searching UI, let the player cancel), not something to start
  // unconditionally at launch and leave running; this bounds it to
  // approximate that without needing a UI of its own.
  static constexpr Clock::duration kDiscoveryBurstDuration =
      std::chrono::seconds(5);

  // How often a burst is retried while no GameController-recognized pad is
  // connected, so one powered on mid-session is still found without this
  // having stayed in discovery mode continuously since launch.
  static constexpr Clock::duration kDiscoveryRetryInterval =
      std::chrono::seconds(30);

  void begin_discovery() {
    [GCController startWirelessControllerDiscoveryWithCompletionHandler:nil];
    discovery_active_ = true;
    discovery_deadline_ = Clock::now() + kDiscoveryBurstDuration;
  }

  /// Ends a burst once it has run its course, and starts another if
  /// nothing is connected and enough time has passed since the last one.
  void update_discovery() {
    const auto now = Clock::now();
    if (discovery_active_) {
      if (now >= discovery_deadline_) {
        [GCController stopWirelessControllerDiscovery];
        discovery_active_ = false;
        next_discovery_attempt_ = now + kDiscoveryRetryInterval;
      }
      return;
    }
    if (entries_.empty() && now >= next_discovery_attempt_) {
      begin_discovery();
    }
  }

  Entry *find(GCController *c) {
    for (auto &e : entries_) {
      if (e->controller == c) {
        return e.get();
      }
    }
    return nullptr;
  }

  static void stop_all(Entry &e, dispatch_group_t pending = nil) {
    stop_motor(e.low, pending);
    stop_motor(e.high, pending);
    stop_motor(e.trigger_left, pending);
    stop_motor(e.trigger_right, pending);
  }

  void add(GCController *c) {
    auto entry = std::make_unique<Entry>();
    entry->controller = c;

    DeviceDesc desc;
    desc.name = c.vendorName != nil ? std::string(c.vendorName.UTF8String)
                                    : std::string("Gamepad");
    desc.backend_mapped = true;
    // Informational only for a mapped backend -- nothing looks this up --
    // but a game that logs it, or writes a binding file keyed by it, gets
    // something stable per product rather than nothing.
    desc.guid = make_guid(0x03, 0, 0, 0, desc.name, 'g', 0);
    desc.handle = entry.get();
    if (@available(macOS 11.0, iOS 14.0, *)) {
      GCDeviceHaptics *h = c.haptics;
      desc.caps.rumble =
          h != nil &&
          [h.supportedLocalities containsObject:GCHapticsLocalityLeftHandle];
      desc.caps.trigger_rumble =
          h != nil &&
          [h.supportedLocalities containsObject:GCHapticsLocalityLeftTrigger];
      desc.caps.led = c.light != nil;
    }
    // Two of the same pad are told apart by the framework's own ordering,
    // which is stable for as long as both stay connected.
    desc.unique_key = std::string("gc:") + std::to_string(next_key_++);

    const int slot = host_->add_device(desc);
    if (slot < 0) {
      return;
    }
    entry->slot = slot;
    host_->set_power(slot, power_of(c));
    entries_.push_back(std::move(entry));
  }

  void read(Entry &e) {
    GCController *c = e.controller;
    GCExtendedGamepad *pad = c.extendedGamepad;
    if (pad == nil) {
      return;
    }
    const int slot = e.slot;
    const auto button = [&](Button b, GCControllerButtonInput *in) {
      host_->set_button(slot, b, in != nil && in.isPressed);
    };

    button(Button::South, pad.buttonA);
    button(Button::East, pad.buttonB);
    button(Button::West, pad.buttonX);
    button(Button::North, pad.buttonY);
    button(Button::LeftShoulder, pad.leftShoulder);
    button(Button::RightShoulder, pad.rightShoulder);
    button(Button::DpadUp, pad.dpad.up);
    button(Button::DpadDown, pad.dpad.down);
    button(Button::DpadLeft, pad.dpad.left);
    button(Button::DpadRight, pad.dpad.right);

    // Gated per property rather than per OS version, because that is how
    // they arrived: thumbstick clicks in 10.14.1, menu and options in
    // 10.15, home in 11.0 -- and buttonOptions is nullable even where it
    // exists, since some pads have no Back button at all.
    if (@available(macOS 10.14.1, iOS 12.1, *)) {
      button(Button::LeftStick, pad.leftThumbstickButton);
      button(Button::RightStick, pad.rightThumbstickButton);
    }
    if (@available(macOS 10.15, iOS 13.0, *)) {
      button(Button::Start, pad.buttonMenu);
      button(Button::Back, pad.buttonOptions);
    }
    if (@available(macOS 11.0, iOS 14.0, *)) {
      button(Button::Guide, pad.buttonHome);
    }

    host_->set_axis(slot, Axis::LeftX, pad.leftThumbstick.xAxis.value);
    // Negated here and nowhere else. GameController reports up-positive and
    // the database, SDL and screen coordinates are all down-positive; one
    // convention has to win before the value reaches a game, and a flip
    // applied per backend instead of here is how exactly one device ends up
    // inverted.
    host_->set_axis(slot, Axis::LeftY, -pad.leftThumbstick.yAxis.value);
    host_->set_axis(slot, Axis::RightX, pad.rightThumbstick.xAxis.value);
    host_->set_axis(slot, Axis::RightY, -pad.rightThumbstick.yAxis.value);
    host_->set_axis(slot, Axis::LeftTrigger, pad.leftTrigger.value);
    host_->set_axis(slot, Axis::RightTrigger, pad.rightTrigger.value);
  }

  Host *host_ = nullptr;
  std::vector<std::unique_ptr<Entry>> entries_;
  unsigned next_key_ = 0;
  bool discovery_active_ = false;
  Clock::time_point discovery_deadline_{};
  Clock::time_point next_discovery_attempt_{};
};

} // namespace

Backend *make_backend_gamecontroller() { return new GameControllerBackend(); }

} // namespace gpp::detail
