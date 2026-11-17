// Interactive sandbox: paint materials and watch them simulate.
//
// Controls
//   Left mouse     paint the selected material
//   Right mouse    erase
//   Mouse wheel    brush size
//   1 sand   2 water   3 stone   4 oil   5 wood
//   6 fire   7 lava    8 ice     9 steam 0 smoke
//   T              toggle heat view (temperature map)
//   Space          pause / resume
//   . (period)     single step while paused
//   R              rebuild the demo scene
//   C              clear the world
//   D              toggle the active-region (dirty rect) overlay
//   Esc            quit
//
// Command line: --frames N  quit after N frames (for smoke tests).

#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/material.hpp"
#include "core/simulator.hpp"
#include "core/world.hpp"
#include "render/pixel_renderer.hpp"
#include "world/demo_scene.hpp"

namespace {

constexpr int kWorldChunksX = 8;  // 512 x 320 cells
constexpr int kWorldChunksY = 5;
constexpr int kWindowScale = 2;
constexpr double kStepSeconds = 1.0 / 60.0;
constexpr int kMaxStepsPerFrame = 4;  // avoid a spiral of death if the sim falls behind

struct Brush {
  px::MaterialId material = px::kEmpty;
  int radius = 6;
};

float coverage_for(const px::MaterialRegistry& reg, px::MaterialId m) {
  const px::Phase p = reg.phase(m);
  if (p == px::Phase::Powder || p == px::Phase::Liquid) return 0.55f;
  if (p == px::Phase::Gas) return 0.4f;
  return 1.0f;
}

void paint_line(px::World& world, int x0, int y0, int x1, int y1, int radius, px::MaterialId m, float coverage) {
  const int dx = x1 - x0;
  const int dy = y1 - y0;
  const int steps = std::max(1, std::max(std::abs(dx), std::abs(dy)) / std::max(1, radius / 2));
  for (int i = 0; i <= steps; ++i) {
    const int x = x0 + dx * i / steps;
    const int y = y0 + dy * i / steps;
    world.fill_circle(x, y, radius, m, coverage);
  }
}

void draw_brush_outline(std::uint32_t* pixels, int w, int h, int cx, int cy, int r) {
  const int segments = std::max(16, r * 6);
  for (int i = 0; i < segments; ++i) {
    const double a = 2.0 * 3.14159265358979 * i / segments;
    const int x = cx + static_cast<int>(std::lround(std::cos(a) * r));
    const int y = cy + static_cast<int>(std::lround(std::sin(a) * r));
    if (x >= 0 && y >= 0 && x < w && y < h) pixels[y * w + x] = 0xFFE8E8F0U;
  }
}

void report_error(const char* what) {
  std::fprintf(stderr, "%s failed: %s\n", what, SDL_GetError());
  std::string msg = std::string(what) + " failed: " + SDL_GetError();
  SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Pixel Sandbox", msg.c_str(), nullptr);
}

}  // namespace

int main(int argc, char** argv) {
  long max_frames = -1;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) max_frames = std::atol(argv[++i]);
  }

  px::MaterialRegistry materials;
  const px::BaseMaterials ids = px::register_base_materials(materials);
  px::World world(kWorldChunksX, kWorldChunksY, materials, 0xC0FFEEULL);
  px::build_demo_scene(world, ids);
  px::PixelRenderer renderer(materials);

  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    report_error("SDL_Init");
    return 1;
  }
  const int w = world.width();
  const int h = world.height();
  SDL_Window* window = SDL_CreateWindow("Pixel Sandbox", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                        w * kWindowScale, h * kWindowScale, SDL_WINDOW_RESIZABLE);
  if (!window) {
    report_error("SDL_CreateWindow");
    SDL_Quit();
    return 1;
  }
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");  // nearest-neighbour: crisp pixels
  SDL_Renderer* sdl_renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!sdl_renderer) sdl_renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
  if (!sdl_renderer) {
    report_error("SDL_CreateRenderer");
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }
  SDL_RenderSetLogicalSize(sdl_renderer, w, h);
  SDL_Texture* texture = SDL_CreateTexture(sdl_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
  std::vector<std::uint32_t> pixels(static_cast<std::size_t>(w) * static_cast<std::size_t>(h));

  Brush brush{ids.sand, 6};
  bool running = true;
  bool paused = false;
  bool single_step = false;
  bool show_dirty = false;
  bool show_heat = false;

  // Painting is driven by mouse events, not by polling the button once per
  // frame: a click shorter than a frame still paints, and a fast drag paints
  // every motion segment even if the frame rate drops.
  bool stroke_active = false;
  int stroke_x = 0, stroke_y = 0;
  bool painted_this_frame = false;
  // SDL_GetMouseState returns window pixels; convert those to world cells.
  auto to_world = [&](int window_x, int window_y, int& x, int& y) {
    float lx = 0, ly = 0;
    SDL_RenderWindowToLogical(sdl_renderer, window_x, window_y, &lx, &ly);
    x = static_cast<int>(std::floor(lx));
    y = static_cast<int>(std::floor(ly));
  };
  auto stroke_to = [&](int x, int y, bool erase) {
    const px::MaterialId m = erase ? px::kEmpty : brush.material;
    const float coverage = erase ? 1.0f : coverage_for(materials, m);
    if (stroke_active) {
      paint_line(world, stroke_x, stroke_y, x, y, brush.radius, m, coverage);
    } else {
      world.fill_circle(x, y, brush.radius, m, coverage);
    }
    stroke_x = x;
    stroke_y = y;
    stroke_active = true;
    painted_this_frame = true;
  };

  using clock = std::chrono::steady_clock;
  auto previous = clock::now();
  double accumulator = 0.0;
  double title_timer = 0.0;
  double sim_ms_sum = 0.0;
  int frames = 0, sim_steps = 0;
  px::StepStats last_stats;

  for (long frame = 0; running && (max_frames < 0 || frame < max_frames); ++frame) {
    painted_this_frame = false;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      switch (e.type) {
        case SDL_QUIT:
          running = false;
          break;
        case SDL_KEYDOWN:
          switch (e.key.keysym.sym) {
            case SDLK_ESCAPE: running = false; break;
            case SDLK_SPACE: paused = !paused; break;
            case SDLK_PERIOD: single_step = true; break;
            case SDLK_1: brush.material = ids.sand; break;
            case SDLK_2: brush.material = ids.water; break;
            case SDLK_3: brush.material = ids.stone; break;
            case SDLK_4: brush.material = ids.oil; break;
            case SDLK_5: brush.material = ids.wood; break;
            case SDLK_6: brush.material = ids.fire; break;
            case SDLK_7: brush.material = ids.lava; break;
            case SDLK_8: brush.material = ids.ice; break;
            case SDLK_9: brush.material = ids.steam; break;
            case SDLK_0: brush.material = ids.smoke; break;
            case SDLK_t: show_heat = !show_heat; break;
            case SDLK_r: px::build_demo_scene(world, ids); break;
            case SDLK_c: world.clear(); break;
            case SDLK_d: show_dirty = !show_dirty; break;
            default: break;
          }
          break;
        case SDL_MOUSEBUTTONDOWN:
          if (e.button.button == SDL_BUTTON_LEFT || e.button.button == SDL_BUTTON_RIGHT) {
            // SDL already reports mouse events in logical (world) coordinates
            // because the renderer has a logical size set.
            stroke_active = false;
            stroke_to(e.button.x, e.button.y, e.button.button == SDL_BUTTON_RIGHT);
          }
          break;
        case SDL_MOUSEMOTION:
          if (e.motion.state & (SDL_BUTTON_LMASK | SDL_BUTTON_RMASK)) {
            stroke_to(e.motion.x, e.motion.y, (e.motion.state & SDL_BUTTON_LMASK) == 0);
          }
          break;
        case SDL_MOUSEBUTTONUP:
          if ((SDL_GetMouseState(nullptr, nullptr) & (SDL_BUTTON_LMASK | SDL_BUTTON_RMASK)) == 0) stroke_active = false;
          break;
        case SDL_MOUSEWHEEL:
          brush.radius = std::clamp(brush.radius + (e.wheel.y > 0 ? 1 : -1), 1, 40);
          break;
        default:
          break;
      }
    }

    // Holding the button still keeps pouring at the cursor.
    int wx = 0, wy = 0;
    const Uint32 buttons = SDL_GetMouseState(&wx, &wy);
    int mx = 0, my = 0;
    to_world(wx, wy, mx, my);
    if (stroke_active && !painted_this_frame && (buttons & (SDL_BUTTON_LMASK | SDL_BUTTON_RMASK))) {
      stroke_to(mx, my, (buttons & SDL_BUTTON_LMASK) == 0);
    }

    // Fixed-timestep simulation.
    const auto now = clock::now();
    const double dt = std::chrono::duration<double>(now - previous).count();
    previous = now;
    accumulator = std::min(accumulator + dt, kStepSeconds * kMaxStepsPerFrame);
    if (paused) accumulator = 0.0;
    int steps = paused ? (single_step ? 1 : 0) : static_cast<int>(accumulator / kStepSeconds);
    single_step = false;
    for (int s = 0; s < steps; ++s) {
      const auto t0 = clock::now();
      last_stats = px::step(world);
      sim_ms_sum += std::chrono::duration<double, std::milli>(clock::now() - t0).count();
      ++sim_steps;
      if (!paused) accumulator -= kStepSeconds;
    }

    if (show_heat) renderer.render_temperature(world, pixels.data(), w);
    else renderer.render(world, pixels.data(), w);
    if (show_dirty) renderer.draw_dirty_rects(world, pixels.data(), w);
    draw_brush_outline(pixels.data(), w, h, mx, my, brush.radius);
    SDL_UpdateTexture(texture, nullptr, pixels.data(), w * static_cast<int>(sizeof(std::uint32_t)));
    SDL_RenderClear(sdl_renderer);
    SDL_RenderCopy(sdl_renderer, texture, nullptr, nullptr);
    SDL_RenderPresent(sdl_renderer);

    ++frames;
    title_timer += dt;
    if (title_timer >= 0.5) {
      char title[256];
      std::snprintf(title, sizeof(title),
                    "Pixel Sandbox | %s | brush %d | %.0f fps | sim %.2f ms | active chunks %d/%d%s%s",
                    materials.def(brush.material).name.c_str(), brush.radius, frames / title_timer,
                    sim_steps ? sim_ms_sum / sim_steps : 0.0, last_stats.active_chunks,
                    world.width_chunks() * world.height_chunks(), show_heat ? " | HEAT VIEW" : "",
                    paused ? " | PAUSED" : "");
      SDL_SetWindowTitle(window, title);
      title_timer = 0.0;
      sim_ms_sum = 0.0;
      frames = sim_steps = 0;
    }
  }

  SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(sdl_renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
