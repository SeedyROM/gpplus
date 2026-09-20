# data/contrib/

Mappings for pads the database does not know, written by the people who own
them.

If a controller shows up as `mapped   NO` in `gpplus_probe`, gpplus can
still read it, but nothing knows which numbered button is South. Fixing that
takes about two minutes and produces a file that goes here.

## Mapping a pad

Build the examples, plug the pad in, and point `gpplus_rebind` at this
directory:

```bash
cmake -S . -B build && cmake --build build
./build/examples/gpplus_rebind data/contrib
```

It asks for each control in turn — press the one it names, and it moves on.
Ctrl-C stops early and keeps what was bound so far. At the end it writes
`data/contrib/<guid>.txt`:

```
# Some Pad Name
# gpplus 0.1.0, Linux, backend evdev
03000000...,Some Pad Name,a:b0,b:b1,leftx:a0,...,platform:Linux,
```

The `#` lines are comments and are ignored when the file is loaded, so the
file works as it is. Run it again for the same pad and the file is replaced,
not added to.

This is for the backends that read raw inputs: **evdev** (Linux, FreeBSD),
**iokit** (macOS) and **dinput** (Windows). `gamecontroller` and `xinput`
already report their own layout and never consult the database, so a line
recorded on them would be written and ignored. `rebind` says so when that is
the case.

## Trying it before you send it

The recorder applies the mapping to the running session, so the walkthrough
itself is a test. To check the saved file in a real program, without
rebuilding:

```bash
GPPLUS_GAMECONTROLLERDB=data/contrib/<guid>.txt ./build/examples/gpplus_probe
```

`gpplus_probe` should now say `mapped   yes`, and `South` should be the
bottom face button.

## What the build does with it

Every `*.txt` in this directory is compiled into the library, after the
database, at configure time. So a file that lands here works in the next
build, for everyone, without waiting on upstream:

- Adding a file or editing one triggers a reconfigure on its own; there is
  nothing to re-run by hand.
- Lines are filtered to the platform being built, exactly like the database
  itself, so a Linux line costs a macOS binary nothing.
- They are loaded last, and a later line for the same guid replaces an earlier
  one. A file here that disagrees with the upstream entry for its pad wins, so
  this is also the place for a *fix* to a line that is wrong.
- `GPPLUS_BUNDLE_CONTROLLER_DB=OFF` leaves them out along with everything else.
- Only `.txt` files are read, so this README is not mistaken for data.

## Sending it

Open a pull request with the one file. A few things make it easy to accept:

- Say what the pad is (model, and wired or wireless if it matters) and which
  OS you recorded it on. The comment header has the platform and backend.
- Check with `gpplus_probe` that the sticks push the way the prompts said and
  that a released trigger reads `0.00`.
- Mappings are per platform, because the same pad has a different guid, and
  different numbering, under evdev than under DirectInput. If you can record
  on a second OS, that is a second file.

**Upstream first, where you can.** The built-in database is
[SDL_GameControllerDB][db], and a line accepted there reaches every project
that uses it, this one included, on the next build. A file here helps players
of this library today; a line there helps everyone. The line in your file is
already in the format that project takes.

[db]: https://github.com/mdqinc/SDL_GameControllerDB
