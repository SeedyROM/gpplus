// gpplus_basic - the smallest program that is a game loop.
//
// A marker on a one-line track. The left stick or d-pad moves it, South
// counts a "hit" and buzzes the pad, Start quits. Everything the library
// asks of a program is here: build a Context, call update() once per
// frame, then ask questions.
//
// There is nothing to set up for a controller arriving or leaving. Start it
// with no pad, plug one in, unplug it: the loop does not change.

#include <gpplus/gamepad.hpp>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>
#include <thread>

namespace {

volatile std::sig_atomic_t g_quit = 0;
void on_signal(int) { g_quit = 1; }

constexpr int kTrackWidth = 40;
constexpr float kCellsPerSecond = 24.0f;

} // namespace

int main() {
  std::signal(SIGINT, on_signal);

  gpp::Context pads; // once

  std::printf("gpplus basic\n"
              "  left stick / d-pad  move\n"
              "  south               hit (and rumble)\n"
              "  start               quit\n\n");

  float x = kTrackWidth / 2.0f;
  int hits = 0;
  auto last = std::chrono::steady_clock::now();

  while (g_quit == 0) {
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::chrono::duration<float>(now - last).count();
    last = now;

    pads.update(); // once a frame, before any question

    // Arrivals and departures are events; nothing else needs handling.
    for (const gpp::Event &e : pads.events()) {
      if (e.type == gpp::Event::Type::DeviceAdded) {
        const std::string line = "connected: " + pads.info(e.device).name;
        std::printf("\r%-72s\n", line.c_str());
      } else if (e.type == gpp::Event::Type::DeviceRemoved) {
        std::printf("\r%-72s\n", "disconnected");
      }
    }

    // Every query is safe on kInvalidDevice and answers "nothing", so a
    // frame with no pad plugged in needs no special case.
    const gpp::DeviceId pad = pads.first_device();

    float move = pads.stick(pad, gpp::Stick::Left).x;
    if (pads.down(pad, gpp::Button::DpadLeft)) {
      move -= 1.0f;
    }
    if (pads.down(pad, gpp::Button::DpadRight)) {
      move += 1.0f;
    }
    x += move * kCellsPerSecond * dt;
    if (x < 0.0f) {
      x = 0.0f;
    }
    if (x > kTrackWidth - 1) {
      x = kTrackWidth - 1;
    }

    if (pads.pressed(pad, gpp::Button::South)) { // true for one update only
      ++hits;
      pads.rumble(pad, 0.4f, 0.2f, 120);
    }
    if (pads.pressed(pad, gpp::Button::Start)) {
      break;
    }

    std::string track(kTrackWidth, '.');
    track[static_cast<std::size_t>(x)] = '@';
    std::printf("\r|%s|  hits %-3d  %s ", track.c_str(), hits,
                pad == gpp::kInvalidDevice ? "(no controller)" : "");
    std::fflush(stdout);

    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }

  std::printf("\n\nbye - %d hits\n", hits);
  return 0; // the Context stops any rumble on the way out
}
