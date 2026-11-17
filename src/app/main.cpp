// Interactive sandbox: paint materials and watch them simulate.
//
// Controls
//   Left mouse     paint the selected material
//   Right mouse    erase
//   Mouse wheel    brush size
//   1 / 2 / 3      select sand / water / stone
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
  return (p == px::Phase::Powder || p == px::Phase::Liquid) ? 0.55f : 1.0f;
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
    std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }
  const int w = world.width();
  const int h = world.height();
  SDL_Window* window = SDL_CreateWindow("Pixel Sandbox", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                        w * kWindowScale, h * kWindowScale, SDL_WINDOW_RESIZABLE);
  if (!window) {
    std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");  // nearest-neighbour: crisp pixels
  SDL_Renderer* sdl_renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!sdl_renderer) sdl_renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
  if (!sdl_renderer) {
    std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
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
  bool painting = false;
  int last_mx = 0, last_my = 0;

  using clock = std::chrono::steady_clock;
  auto previous = clock::now();
  double accumulator = 0.0;
  double title_timer = 0.0;
  double sim_ms_sum = 0.0;
  int frames = 0, sim_steps = 0;
  px::StepStats last_stats;

  for (long frame = 0; running && (max_frames < 0 || frame < max_frames); ++frame) {
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
            case SDLK_r: px::build_demo_scene(world, ids); break;
            case SDLK_c: world.clear(); break;
            case SDLK_d: show_dirty = !show_dirty; break;
            default: break;
          }
          break;
        case SDL_MOUSEWHEEL:
          brush.radius = std::clamp(brush.radius + (e.wheel.y > 0 ? 1 : -1), 1, 40);
          break;
        default:
          break;
      }
    }

    // Mouse position in world cells.
    int wx = 0, wy = 0;
    const Uint32 buttons = SDL_GetMouseState(&wx, &wy);
    float lx = 0, ly = 0;
    SDL_RenderWindowToLogical(sdl_renderer, wx, wy, &lx, &ly);
    const int mx = static_cast<int>(lx);
    const int my = static_cast<int>(ly);
    const bool left = (buttons & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;
    const bool right = (buttons & SDL_BUTTON(SDL_BUTTON_RIGHT)) != 0;
    if (left || right) {
      const px::MaterialId m = right ? px::kEmpty : brush.material;
      const float coverage = right ? 1.0f : coverage_for(materials, m);
      paint_line(world, painting ? last_mx : mx, painting ? last_my : my, mx, my, brush.radius, m, coverage);
    }
    painting = left || right;
    last_mx = mx;
    last_my = my;

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

    renderer.render(world, pixels.data(), w);
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
                    "Pixel Sandbox | %s | brush %d | %.0f fps | sim %.2f ms | active chunks %d/%d%s",
                    materials.def(brush.material).name.c_str(), brush.radius, frames / title_timer,
                    sim_steps ? sim_ms_sum / sim_steps : 0.0, last_stats.active_chunks,
                    world.width_chunks() * world.height_chunks(), paused ? " | PAUSED" : "");
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
