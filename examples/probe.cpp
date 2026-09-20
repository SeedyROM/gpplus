// gpplus_probe - what the library sees, live in a terminal.
//
// This is the program to run first, and the one to ask a player to run
// when their pad "does not work": it prints which backend found the
// device, whether a mapping matched, and every input as it moves --
// including the raw numbered inputs, which is what a new database line
// gets written from.

#include <gpplus/gamepad.hpp>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace {

volatile std::sig_atomic_t g_quit = 0;
void on_signal(int) { g_quit = 1; }

const char *power_str(gpp::PowerLevel p) {
  switch (p) {
  case gpp::PowerLevel::Wired:
    return "wired";
  case gpp::PowerLevel::Empty:
    return "empty";
  case gpp::PowerLevel::Low:
    return "low";
  case gpp::PowerLevel::Medium:
    return "medium";
  case gpp::PowerLevel::Full:
    return "full";
  case gpp::PowerLevel::Charging:
    return "charging";
  default:
    return "unknown";
  }
}

void print_device(const gpp::Context &ctx, gpp::DeviceId id) {
  const gpp::DeviceInfo info = ctx.info(id);
  std::printf("\n[%u] %s\n", info.id, info.name.c_str());
  std::printf("     guid     %s\n", info.guid.str().c_str());
  std::printf("     backend  %s\n", info.backend);
  std::printf("     mapped   %s\n",
              info.mapped ? "yes" : "NO - no database line matches this guid");
  std::printf("     raw      %d buttons, %d axes, %d hats\n",
              info.raw_button_count, info.raw_axis_count, info.raw_hat_count);
  std::printf("     rumble   %s%s\n", info.caps.rumble ? "yes" : "no",
              info.caps.trigger_rumble ? " (+triggers)" : "");
  std::printf("     power    %s\n", power_str(info.power));
  const std::string mapping = ctx.mapping_for(id);
  if (!mapping.empty()) {
    std::printf("     mapping  %s\n", mapping.c_str());
  }
}

} // namespace

int main(int argc, char **argv) {
  std::signal(SIGINT, on_signal);

  gpp::Config cfg;
  if (argc > 1) {
    cfg.db_path = argv[1];
  }
  gpp::Context ctx(cfg);

  std::printf("gpplus %s on %s\n", GPPLUS_VERSION_STRING, gpp::platform_name());
  std::printf("backends:");
  for (const char *const *b = gpp::compiled_backends(); *b != nullptr; ++b) {
    std::printf(" %s", *b);
  }
  std::printf("\nmappings: %zu\n", ctx.mapping_count());
  std::printf("waiting for a controller; ctrl-c to quit\n");

  bool rumbled = false;
  while (g_quit == 0) {
    ctx.update();

    for (const gpp::Event &e : ctx.events()) {
      switch (e.type) {
      case gpp::Event::Type::DeviceAdded:
        print_device(ctx, e.device);
        break;
      case gpp::Event::Type::DeviceRemoved:
        std::printf("\n[%u] disconnected\n", e.device);
        break;
      case gpp::Event::Type::ButtonDown:
        std::printf("[%u] %-14s down\n", e.device, gpp::to_string(e.button));
        break;
      case gpp::Event::Type::ButtonUp:
        std::printf("[%u] %-14s up\n", e.device, gpp::to_string(e.button));
        break;
      case gpp::Event::Type::AxisMotion:
        // Axis events are continuous, so only the interesting ones: a
        // stick returning to centre prints once and then goes quiet.
        if (std::abs(e.value) > 0.25f) {
          std::printf("[%u] %-14s %+.2f\n", e.device, gpp::to_string(e.axis),
                      static_cast<double>(e.value));
        }
        break;
      }
    }

    // Anything unmapped is worth showing raw, because those numbers are
    // what a new database line is written from.
    for (gpp::DeviceId id : ctx.devices()) {
      if (ctx.info(id).mapped) {
        continue;
      }
      for (int i = 0; i < ctx.info(id).raw_button_count; ++i) {
        if (ctx.raw_button(id, i)) {
          std::printf("[%u] raw button %d down\n", id, i);
        }
      }
    }

    // South once, to prove the motors work, and only once.
    const gpp::DeviceId id = ctx.first_device();
    if (id != gpp::kInvalidDevice && ctx.pressed(id, gpp::Button::South) &&
        !rumbled) {
      rumbled = ctx.rumble(id, 0.6f, 0.6f, 300);
      std::printf("[%u] rumble %s\n", id, rumbled ? "sent" : "unavailable");
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }

  std::printf("\nbye\n");
  return 0;
}
