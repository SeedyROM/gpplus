# Changelog

## 0.1.0-alpha — 2026-09-19

First release. Gamepad input for C++17 with no platform layer attached: the
controller half of SDL's answer, reading the same community mapping
database, depending on nothing.

### Backends

- `gamecontroller` — macOS and iOS, pre-mapped, with CoreHaptics rumble on
  both handles and the triggers, LED colour and battery state.
- `iokit` — macOS raw HID, for the pads GameController refuses. Skips
  anything GameController already claims, so nothing is reported twice.
- `evdev` — Linux, with FF_RUMBLE force feedback, battery state from sysfs,
  and input numbering identical to SDL's so the database's Linux lines
  apply.
- `xinput` — Windows, pre-mapped, with rumble and battery state (resolved
  at runtime, because the redistributable stub does not export it).
- `dinput` — Windows DirectInput 8, for the hardware XInput cannot see:
  arcade sticks, flight gear, wheels, older pads. Skips XInput devices so
  an Xbox pad is not reported twice.
- `virtual` — synthetic devices for tests and for developing without
  hardware.

Which backends exist in a build is decided by `try_compile` probes, not by
preprocessor guards: a backend whose API is not present is never compiled.

### Library

- Stable device ids that are never reused, hotplug on every backend, events
  or polling.
- Radial rescaled deadzones with a separate trigger deadzone, a central
  Y-axis convention, and trigger rescaling that does not report half
  throttle at rest.
- The SDL mapping database, fetched at configure time, filtered to the
  target platform and compiled in; overridable at runtime from a file, an
  environment variable or a string.
- Contributed mappings: every `data/contrib/*.txt` is compiled in after the
  database, filtered to the target platform, and wins over an upstream line
  for the same pad. See `data/contrib/README.md`.
- `gpplus/binding.hpp` — a recorder that turns what the player pressed into
  a database mapping line.
- `gpplus/virtual.hpp` — synthetic devices for tests.

### Examples

- `gpplus_basic` — the smallest game loop: a `Context`, an `update()` per
  frame, stick, button edge and rumble. Runs with no pad and picks one up
  when it is plugged in.
- `gpplus_probe` — what the library sees, live: backend, guid, mapping and
  every input, including the raw numbered ones.
- `gpplus_rebind` — walks through a pad and prints a database line. Given a
  directory, as in `gpplus_rebind data/contrib`, it also writes
  `<guid>.txt` there, ready to send in.

### Testing

- Unit tests for the mapping parser, the guid layout and the CRC.
- Context tests covering hotplug, edge detection, deadzone shaping and
  device lifetime, driven by the synthetic backend.
- Binding tests covering the recorder's judgement calls.
- `tests/test_uinput.cpp` — an end-to-end test on Linux that asks the
  kernel for a real gamepad through `/dev/uinput` and drives the evdev
  backend through real device nodes.
- `tests/test_vigem.cpp` — the Windows counterpart, behind
  `GPPLUS_TEST_VIGEM`. Asks the ViGEmBus driver for an emulated DualShock 4
  and checks the DirectInput backend reports it once, with every button,
  the hat and all six axes, mapped by the real database line. It also drives
  an emulated Xbox 360 pad through XInput, including rumble; that phase
  needs a Windows client machine, because a hosted Server image has no Xbox
  driver for the pad to bind to, and it skips itself there.
- `tools/check-foreign-backends.sh` — type-checks the backends and tests the
  current machine cannot build, against the real Linux, mingw-w64 and
  ViGEmClient headers.

### Known limitations

What 0.1.0-alpha has and has not been shown to do, so nobody has to find out the
hard way.

- **No backend has met a physical controller.** Everything above was
  verified against emulated or synthetic devices: the virtual backend, a
  kernel uinput device, and an emulated DualShock 4. Treat real-hardware
  behaviour as untested until it is not. That includes `gpplus_rebind`: the
  recorder is tested through the synthetic backend, but nobody has yet
  walked a physical pad through the walkthrough.
- **XInput has never run.** Hosted Windows CI has no Xbox 360 driver for an
  emulated pad to bind to, so the XInput end-to-end phase is skipped there.
  It builds and links; its rumble path and the DirectInput filter that stops
  an Xbox pad being reported twice have never executed.
- **macOS has no end-to-end test.** There is no supported way to create a
  virtual HID device or a GameController pad from a test, so the macOS
  backends are covered by compile checks and unit tests only.
- **The Windows end-to-end test depends on ViGEmBus,** which is retired
  upstream. It runs in CI pinned to its last release and may stop working
  with a future Windows update; it does not gate the build.
- The mapping database is fetched from upstream's `master` by default, so
  mappings can change between builds. See the README for pinning it.
