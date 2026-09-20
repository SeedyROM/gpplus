// gpplus_rebind - the "press the button you want" screen, in a terminal.
//
// Run it with a pad nobody has a mapping for and it walks through the
// controller, one prompt at a time, and prints a database line at the end.
// That line can be pasted into a file the game loads at startup, handed to
// Context::add_mapping, or sent upstream to SDL_GameControllerDB, which is
// where every line in the built-in database came from.
//
//   gpplus_rebind                  print the line
//   gpplus_rebind data/contrib     also write data/contrib/<guid>.txt
//
// The directory has to exist. One file per guid means two people mapping
// two pads never touch the same file, and mapping the same pad again
// replaces the old attempt instead of stacking a second line under it.

#include <gpplus/binding.hpp>
#include <gpplus/gamepad.hpp>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

volatile std::sig_atomic_t g_quit = 0;
void on_signal(int) { g_quit = 1; }

/// Writes `<dir>/<guid>.txt`: two comment lines saying where the mapping
/// came from, then the line itself. Comments are ignored by the database
/// parser, so the file loads as-is. Returns the path, or empty on failure.
std::string save_line(const std::string &dir, const gpp::DeviceInfo &info,
                      const std::string &line) {
  std::string path = dir;
  if (!path.empty() && path.back() != '/' && path.back() != '\\') {
    path += '/';
  }
  path += info.guid.str() + ".txt";

  std::FILE *f = std::fopen(path.c_str(), "w");
  if (f == nullptr) {
    return {};
  }
  std::fprintf(f, "# %s\n# gpplus %s, %s, backend %s\n%s\n", info.name.c_str(),
               GPPLUS_VERSION_STRING, gpp::platform_name(), info.backend,
               line.c_str());
  const bool ok = std::fclose(f) == 0;
  return ok ? path : std::string();
}

struct Prompt {
  const char *text;
  bool is_axis;
  gpp::Button button;
  gpp::Axis axis;
};

Prompt button_prompt(const char *text, gpp::Button b) {
  return Prompt{text, false, b, gpp::Axis::LeftX};
}
Prompt axis_prompt(const char *text, gpp::Axis a) {
  return Prompt{text, true, gpp::Button::South, a};
}

// Axis prompts name a *direction*, because that is the only way to find
// out whether the hardware reports it the way round the game expects.
const std::vector<Prompt> &prompts() {
  static const std::vector<Prompt> kPrompts = {
      button_prompt("the BOTTOM face button", gpp::Button::South),
      button_prompt("the RIGHT face button", gpp::Button::East),
      button_prompt("the LEFT face button", gpp::Button::West),
      button_prompt("the TOP face button", gpp::Button::North),
      button_prompt("the LEFT shoulder", gpp::Button::LeftShoulder),
      button_prompt("the RIGHT shoulder", gpp::Button::RightShoulder),
      button_prompt("BACK / SELECT", gpp::Button::Back),
      button_prompt("START", gpp::Button::Start),
      button_prompt("the LEFT stick, clicked in", gpp::Button::LeftStick),
      button_prompt("the RIGHT stick, clicked in", gpp::Button::RightStick),
      button_prompt("D-PAD UP", gpp::Button::DpadUp),
      button_prompt("D-PAD DOWN", gpp::Button::DpadDown),
      button_prompt("D-PAD LEFT", gpp::Button::DpadLeft),
      button_prompt("D-PAD RIGHT", gpp::Button::DpadRight),
      axis_prompt("the LEFT stick RIGHT", gpp::Axis::LeftX),
      axis_prompt("the LEFT stick DOWN", gpp::Axis::LeftY),
      axis_prompt("the RIGHT stick RIGHT", gpp::Axis::RightX),
      axis_prompt("the RIGHT stick DOWN", gpp::Axis::RightY),
      axis_prompt("the LEFT trigger", gpp::Axis::LeftTrigger),
      axis_prompt("the RIGHT trigger", gpp::Axis::RightTrigger),
  };
  return kPrompts;
}

} // namespace

int main(int argc, char **argv) {
  const std::string out_dir = argc > 1 ? argv[1] : "";
  std::signal(SIGINT, on_signal);

  gpp::Config cfg;
  // Start from nothing: the point is to describe this pad from scratch,
  // and a database line that already half-matches would answer the
  // prompts before the player does.
  cfg.load_builtin_db = false;
  gpp::Context ctx(cfg);

  std::printf("gpplus rebind\n\nlooking for a controller...\n");
  gpp::DeviceId id = gpp::kInvalidDevice;
  while (g_quit == 0 && id == gpp::kInvalidDevice) {
    ctx.update();
    id = ctx.first_device();
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
  if (id == gpp::kInvalidDevice) {
    return 0;
  }

  const gpp::DeviceInfo info = ctx.info(id);
  std::printf("\n%s\n  guid %s\n  %d buttons, %d axes, %d hats\n\n",
              info.name.c_str(), info.guid.str().c_str(), info.raw_button_count,
              info.raw_axis_count, info.raw_hat_count);
  if (info.backend == std::string("gamecontroller") ||
      info.backend == std::string("xinput")) {
    // Those backends report a layout of their own and never consult the
    // database, so a line recorded here would be written and ignored.
    std::printf(
        "note: '%s' already knows this pad's layout, so a recorded\n"
        "      mapping would not be used. This tool is for the\n"
        "      backends that read raw inputs: evdev, iokit, dinput.\n\n",
        info.backend);
  }
  std::printf("Press the control named, or SPACE-equivalent: hold any button\n"
              "you have already bound to skip. Ctrl-C to stop early.\n\n");

  gpp::MappingRecorder rec(ctx, id);
  std::size_t index = 0;
  bool prompted = false;

  while (g_quit == 0 && index < prompts().size()) {
    const Prompt &p = prompts()[index];
    if (!prompted) {
      std::printf("  press %-36s", p.text);
      std::fflush(stdout);
      prompted = true;
      // Re-baseline per prompt: whatever is being held now (a trigger at
      // rest, the button just released) is this prompt's zero.
      rec.begin();
    }

    ctx.update();
    if (!ctx.connected(id)) {
      std::printf("\n\ncontroller disconnected; nothing written.\n");
      return 1;
    }

    const gpp::RawInput in = rec.poll();
    if (in.valid()) {
      if (p.is_axis) {
        rec.bind(p.axis, in);
      } else {
        rec.bind(p.button, in);
      }
      std::printf("ok\n");
      ++index;
      prompted = false;
      // Wait for release, so one long press does not answer the next
      // prompt as well.
      while (g_quit == 0) {
        ctx.update();
        if (!rec.poll().valid() && !ctx.active(id)) {
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }

  const std::string line = rec.mapping_line();
  if (line.empty()) {
    std::printf("\nnothing recorded.\n");
    return 1;
  }

  std::printf("\n%zu of %zu controls bound.\n\n%s\n\n", rec.binding_count(),
              prompts().size(), line.c_str());
  if (rec.apply()) {
    std::printf("applied to this session.\n");
  }
  if (!out_dir.empty()) {
    const std::string path = save_line(out_dir, info, line);
    if (path.empty()) {
      std::printf("could not write into '%s' (does the directory exist?);\n"
                  "copy the line above by hand.\n",
                  out_dir.c_str());
      return 1;
    }
    std::printf("saved to %s\n", path.c_str());
  } else {
    std::printf("Save that line to a file and load it with Config::db_path,\n"
                "or set GPPLUS_GAMECONTROLLERDB to point at it. Run with a\n"
                "directory argument to have it written for you.\n");
  }
  return 0;
}
