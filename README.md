# gpplus

Gamepad input for C++17, with no platform layer attached.

A game that already has a window and a renderer — sokol, GLFW, raylib, its
own — still has no gamepads. The options today are:

- **SDL**, which has the best controller support in the business, and
  brings a second video, audio, event and threading layer beside the one
  the game already has.
- **GLFW's** gamepad API, which needs a GLFW window and a GLFW event loop.
- **libgamepad** and friends, which have no macOS backend.
- Writing it yourself, which is a week per platform and then a decade of
  other people's hardware.

gpplus is the controller half of SDL's answer on its own: a backend per
platform, and the same community mapping database, in a library that links
in one line and depends on nothing.

```cmake
include(FetchContent)
FetchContent_Declare(gpplus
  GIT_REPOSITORY https://github.com/SeedyROM/gpplus.git
  GIT_TAG        v0.1.0-alpha)
FetchContent_MakeAvailable(gpplus)
target_link_libraries(mygame PRIVATE gpplus::gpplus)
```

**Status: alpha.** Every backend builds and passes CI, but none has met a
physical controller: they are verified against emulated and synthetic
devices only, and XInput has never run. See "Known limitations" in
[CHANGELOG.md](CHANGELOG.md).

```cpp
#include <gpplus/gamepad.hpp>

gpp::Context pads;                       // once

pads.update();                           // once a frame
const gpp::DeviceId pad = pads.first_device();
const gpp::Vec2 move = pads.stick(pad, gpp::Stick::Left);
if (pads.pressed(pad, gpp::Button::South)) {
  jump();
  pads.rumble(pad, 0.4f, 0.2f, 120);
}
```

That is the whole integration. No init, no shutdown, no callbacks, no
thread, no window.

## What it does

- **Hotplug**, on every backend, with no notification API to subscribe to
  and nothing to tear down. Plug a pad in mid-game and it is there on the
  next `update()`.
- **The SDL mapping database**, so a pad nobody involved here has ever
  seen still has a South button. ~2300 community mappings, filtered at
  build time to the ones for your platform.
- **Stable device ids** that are never reused, so a stored id cannot
  silently come to mean a different player's pad after a reconnect.
- **Deadzones that are right by default**: radial, rescaled, clamped
  before rescaling, with a separate smaller one for triggers.
- **Rumble**, including on macOS, where it means CoreHaptics and an engine
  per motor.
- **Events or polling**, whichever the game is written around.
- **Raw access** to numbered buttons and axes, plus a recorder that turns
  what the player pressed into a database line — the rebinding screen,
  without writing the fiddly part again.
- **Battery reporting** on every backend that can answer: GameController,
  evdev via sysfs, XInput where the runtime exports it.
- **A synthetic backend** for tests and for a CI machine with no USB ports.

## What it does not do

- Keyboards, mice, touch, or motion. Gamepads and joysticks only.
- Gyro, touchpads, adaptive triggers, LED patterns beyond a single colour.
- Force feedback on the macOS HID backend (that is ForceFeedback.framework
  and a second protocol; the pads that have it are the pads
  GameController already handles).

Which of these are planned, and what finishing each one involves, is in
[docs/ROADMAP.md](docs/ROADMAP.md) — written as a checklist, with the
invariants a new backend has to respect.

## Platforms

| Platform | Backend | Kind | Rumble |
|---|---|---|---|
| macOS, iOS | `gamecontroller` — GameController.framework | pre-mapped | yes (CoreHaptics, both handles + triggers) |
| macOS | `iokit` — raw HID, for pads GameController refuses | database-mapped | no |
| Linux, FreeBSD | `evdev` — `/dev/input/event*` | database-mapped | yes (FF_RUMBLE) |
| Windows | `xinput` | pre-mapped | yes |
| Windows | `dinput` — DirectInput 8, for everything XInput cannot see | database-mapped | no |
| anywhere | `virtual` — synthetic devices for tests | either | reported to the test |

The split between **pre-mapped** and **database-mapped** backends is the
one structural idea in the library. GameController and XInput hand over a
controller: *this* is South, *this* is the left trigger. evdev and raw HID
hand over a numbered pile of switches and potentiometers, and the database
is what turns one into the other. Both kinds implement the same internal
interface; only the core knows the difference.

## Detection, not `#ifdef`

Every backend is compiled **only when a `try_compile` probe using its own
API actually built**. Not "we are on Linux, therefore evdev" — a container
without kernel headers is Linux too. `cmake/GpplusBackends.cmake` probes,
and the result decides whether the source file is ever handed to the
compiler. There is no `#ifdef __linux__` in any backend source, because a
file that cannot build on this platform is simply not part of the build.

The list of what survived is generated into a one-line-per-backend include
that the core reads twice — once to declare the factories, once to call
them. Adding a platform is a new file and a new detection block, and no
change to anything that already exists.

Each backend is a tri-state:

```bash
cmake -B build -DGPPLUS_BACKEND_IOKIT=OFF     # never build it
cmake -B build -DGPPLUS_BACKEND_EVDEV=ON      # build it or fail configure
cmake -B build -DGPPLUS_BACKEND_XINPUT=AUTO   # default: build it if it probes
```

`AUTO` is what a game wants. `ON` is what CI wants: it turns "this backend
silently vanished from the build" into a configure error, which is the
failure mode detection-based builds otherwise have. The workflow in
`.github/workflows/ci.yml` uses `ON` on all three platforms for exactly
that reason.

At runtime:

```cpp
for (const char *const *b = gpp::compiled_backends(); *b; ++b) {
  std::printf("%s\n", *b);
}
```

A platform where nothing was detected is a working build that reports no
devices, not a failed configure. A game still runs on someone's BSD.

## The mapping database

`gamecontrollerdb.txt` is downloaded at configure time, filtered to the
target platform, and compiled in as a byte array. 600 KB of database
becomes about 90 KB of Mac mappings or 200 KB of Linux ones.

| Option | Default | Meaning |
|---|---|---|
| `GPPLUS_BUNDLE_CONTROLLER_DB` | `ON` | compile a database into the library |
| `GPPLUS_DOWNLOAD_CONTROLLER_DB` | `ON` | fetch it at configure time if it is not already there |
| `GPPLUS_CONTROLLER_DB_FILE` | *(empty)* | use this file instead of downloading |
| `GPPLUS_CONTROLLER_DB_URL` | upstream master | where to fetch from |
| `GPPLUS_CONTROLLER_DB_SHA256` | *(empty)* | pin the download for a reproducible build |

The download **fails soft**: an offline build, a hermetic CI, or a
corporate proxy produces a warning and a library with no built-in
mappings, not a broken configure. Pre-mapped backends are unaffected, and
raw devices report as unmapped rather than wrong. Drop the file in `data/`
to have a fallback that never touches the network.

**Reproducible builds.** The default URL follows upstream's `master`, so a
fresh build picks up new mappings as they land. That is deliberate: the worst
a stale or missing database does is leave a pad unmapped. If your build has
to be bit-for-bit reproducible, pin the URL *and* the hash together. A hash
on its own would break the build the moment upstream commits, because the
file behind `master` changes.

```bash
cmake -S . -B build \
  -DGPPLUS_CONTROLLER_DB_URL=https://raw.githubusercontent.com/mdqinc/SDL_GameControllerDB/<commit>/gamecontrollerdb.txt \
  -DGPPLUS_CONTROLLER_DB_SHA256=<sha256 of that file>
```

A download that fails for network reasons still falls back softly, as above.
A hash that does not match does not: the configure fails, since a build that
asked for exact bytes and got others should not go on quietly.

Every `data/contrib/*.txt` is compiled in on top of it, after the download,
so a mapping recorded by a contributor works in the next build without
waiting for upstream, and wins over an upstream line for the same pad. See
[data/contrib/README.md](data/contrib/README.md). Lines for other platforms
are filtered out of these too.

At runtime the built-in database is only the first source. Later ones win:

```cpp
gpp::Config cfg;
cfg.db_path = "assets/gamecontrollerdb.txt";   // ship fixes without a rebuild
gpp::Context pads(cfg);
pads.add_mapping(line_from_your_binding_screen);
```

`GPPLUS_GAMECONTROLLERDB` in the environment is read too, which is how a
player with an exotic pad fixes it themselves.

**Licensing.** The database is from [SDL_GameControllerDB][db], the format
is SDL's, and both are zlib-licensed — the same license as this library,
which is deliberate. No SDL code is included or linked; the mapping
grammar is a data format and gpplus has its own parser for it. The
attribution that travels with the data is in `LICENSE`, and it has to stay
there.

## Unmapped devices, and the rebinding screen

A pad with no matching database line is still reported, still readable,
and still says so:

```cpp
const gpp::DeviceInfo info = pads.info(id);
if (!info.mapped) {
  // Nothing knows which button is South, so down() answers nothing --
  // a wrong guess is worse than silence for a player trying to hit jump.
}
```

`gpplus/binding.hpp` is the way back from there. It watches the raw inputs
and hands you the one the player meant:

```cpp
gpp::MappingRecorder rec(pads, id);
rec.begin();                              // "press the button for Jump"

// once a frame, after pads.update()
const gpp::RawInput in = rec.poll();
if (in.valid()) {
  rec.bind(gpp::Button::South, in);
  next_prompt();
}

rec.apply();                              // live immediately, no reconnect
save_to_file(rec.mapping_line());         // and keep it for next time
```

The recorder is where the awkward parts live, and they are the reason this
is in the library rather than in every game: a stick resting at 0.13 is not
being pushed, a trigger resting at -1 is not being pulled, a thumb brushing
a stick on the way to a button must not win the binding, and an axis that
answers "push right" by falling has to be written down as inverted (`a0~`)
rather than quietly inverting the game. A trigger gets written whole or as
a half depending on where it rests, which is the difference between a
released trigger reading 0.0 and reading 0.5.

`rec.mapping_line()` is in the database's own format, so it can be appended
to a file the game loads at startup — or sent upstream to
SDL_GameControllerDB, which is where every line in the built-in database
came from.

`examples/rebind.cpp` is the whole flow as a terminal walkthrough:

```bash
./build/examples/gpplus_rebind                # print the line
./build/examples/gpplus_rebind data/contrib   # and save it as <guid>.txt
```

Own a pad the database has never seen? Mapping it and sending the file in is
a two-minute job; [data/contrib/README.md](data/contrib/README.md) has the
steps.

## Conventions worth knowing

- **Buttons are named by position**: `South`, `East`, `West`, `North`, not
  A/B/X/Y. A Nintendo pad's A is where an Xbox pad's B is, so the letters
  are not a stable way to say "the bottom face button". The database's
  letters stop at the parser.
- **Stick Y is down-positive** by default, matching the database, SDL, and
  screen coordinates. `Config::y_up = true` flips it once, centrally —
  never per backend, which is how exactly one device ends up inverted.
- **Triggers are 0..1**, whatever the hardware's resting value is. A
  trigger on a full-range axis rests at -1, and passing that through is
  the single most common bug in hand-rolled gamepad layers: half throttle
  from a trigger nobody is touching.
- **`pressed()` is true for one `update()`**, however many fixed
  simulation steps that frame runs.
- **The Context stops the motors when it is destroyed.** This matters more
  than it looks: on several platforms a running effect belongs to a system
  service and outlives the process, so a game that quits mid-explosion
  leaves the pad buzzing until it is power-cycled.

## Testing without hardware

`GPPLUS_BACKEND_VIRTUAL` (on wherever tests are built, off in a release
build) compiles a synthetic backend:

```cpp
#include <gpplus/virtual.hpp>

gpp::virtualpad::Spec spec;
spec.name = "Test Pad";
spec.vendor = 0xDEAD;
spec.product = 0xBEEF;
const auto vid = gpp::virtualpad::add(spec);

pads.update();
const gpp::DeviceId id = gpp::virtualpad::device_id(vid);
gpp::virtualpad::set_raw_button(vid, 0, true);
pads.update();
assert(pads.down(id, gpp::Button::South));

gpp::virtualpad::remove(vid);   // unplug it mid-frame, and see what breaks
```

The interesting cases in a gamepad library are all hardware you do not
have: the pad that disconnects mid-frame, the stick that rests at 0.13,
the eight-way hat, the device nobody has a mapping for. `tests/` covers
them this way, and the CI runners — which have no USB ports — run every
one.

## Diagnosing a pad that "does not work"

```bash
cmake -S . -B build && cmake --build build
./build/examples/gpplus_probe
```

It prints which backend found the device, its guid, whether a mapping
matched, and every input live as it moves — including the raw numbered
ones. It is the right thing to ask a player to run, and its output is what
a new database line is written from.

## Examples

`examples/` is three terminal programs, built with the library and needing
nothing else:

| Program | What it shows |
|---|---|
| `gpplus_basic` | The whole integration: a `Context`, an `update()` per frame, and a loop that reads the stick, a button edge and rumble. Start it with no pad and plug one in. |
| `gpplus_probe` | What the library sees: backend, guid, mapping, and every input live. |
| `gpplus_rebind` | The recorder, as a walkthrough that prints a database line. |

```bash
./build/examples/gpplus_basic
```

## Checking the backends you cannot build

Two of the four platform backends can never be compiled by the person
writing them — whichever machine you are on, two of them are for another
one. CI catches that, but only after a push, and the mistakes involved are
typos and wrong struct field names, which a compiler answers in a second
if it is handed the right headers.

```bash
tools/check-foreign-backends.sh
```

fetches the real Linux UAPI, mingw-w64 Windows and ViGEmClient headers,
and runs `clang -fsyntax-only -Wall -Wextra` over the evdev, XInput and
DirectInput backends and the two end-to-end tests — full semantic analysis,
no toolchain required. It proves the code type-checks against the genuine
API: struct fields, ioctl names, constants, signatures. It does not prove it
links, runs, or that MSVC agrees about warnings; that is what CI is for.

CI goes one step further on Linux: `tests/test_uinput.cpp` asks the kernel
for a gamepad through `/dev/uinput`, and the evdev backend then finds it,
enumerates it and reads it exactly as it would real hardware — including
asserting that button numbering matches what the database expects. Windows
has an equivalent in ViGEmBus, `tests/test_vigem.cpp`, off by default and
enabled with `-DGPPLUS_TEST_VIGEM=ON`; macOS has none, because a virtual
HID device there needs a DriverKit extension and an approval click no
runner can give. See [docs/ROADMAP.md](docs/ROADMAP.md).

## Building

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Requires CMake 3.16 and a C++17 compiler. No submodules, no package
manager, no dependencies. `cmake --install build --prefix ...` installs a
`find_package(gpplus)` package for projects that prefer that to
FetchContent.

## License

zlib, matching SDL's, so that the mapping database and this library can
travel together under one set of terms. See `LICENSE`.

[db]: https://github.com/mdqinc/SDL_GameControllerDB
