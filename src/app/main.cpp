// Interactive sandbox: explore a generated world, paint materials and watch
// them simulate.
//
// Controls
//   WASD / arrows  move the camera (hold Shift to move faster)
//   Middle mouse   drag to move the camera
//   Home           back to the starting point
//   Left mouse     paint the selected material
//   Right mouse    erase
//   Mouse wheel    brush size
//   1 sand   2 water   3 stone   4 oil   5 wood
//   6 fire   7 lava    8 ice     9 steam 0 smoke
//   Q / E          previous / next material (all of them, incl. dirt, coal...)
//   T              toggle heat view (temperature map)
//   Space          pause / resume
//   . (period)     single step while paused
//   R              regenerate the world (same seed)
//   N              new world (new seed)
//   C              clear the world
//   F              toggle the active-region (dirty rect) overlay
//   Esc            quit
//
// The generated world wraps around horizontally: keep going right and you come
// back to where you started.
//
// Command line:
//   --seed N    world seed (default 0xC0FFEE)
//   --demo      the small hand-built demo scene instead of a generated world
//   --frames N  quit after N frames (for smoke tests)

#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "core/material.hpp"
#include "core/rng.hpp"
#include "core/simulator.hpp"
#include "core/world.hpp"
#include "render/pixel_renderer.hpp"
#include "world/demo_scene.hpp"
#include "world/world_gen.hpp"

namespace {

constexpr int kWorldChunksX = 64;  // 4096 x 1536 cells, wrapping around in x
constexpr int kWorldChunksY = 24;
constexpr int kDemoChunksX = 8;  // 512 x 320 cells
constexpr int kDemoChunksY = 5;
constexpr int kViewW = 640;  // camera size in cells
constexpr int kViewH = 360;
constexpr double kCameraSpeed = 420.0;  // cells per second (x3 with Shift)
constexpr int kWindowScale = 2;
constexpr double kStepSeconds = 1.0 / 60.0;
constexpr int kMaxStepsPerFrame = 2;
// Simulation time allowed per frame before the rest of the backlog is dropped.
// On a slow machine the game then runs in slow motion at a steady frame rate,
// instead of trying to catch up, taking even longer, and stuttering.
constexpr double kSimBudgetMs = 10.0;
// Chunks beyond the view that keep simulating; everything further away is
// paused (and resumes when the camera comes back), like Noita.
constexpr int kSimMarginChunks = 2;

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
  bool demo = false;
  std::uint64_t seed = 0xC0FFEEULL;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) max_frames = std::atol(argv[++i]);
    else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 0);
    else if (std::strcmp(argv[i], "--demo") == 0) demo = true;
  }

  px::MaterialRegistry materials;
  const px::BaseMaterials ids = px::register_base_materials(materials);
  px::PixelRenderer renderer(materials);

  // The World's seed drives both generation and the simulation's randomness,
  // so a new seed means a new World object.
  auto make_world = [&](std::uint64_t s) {
    return demo ? std::make_unique<px::World>(kDemoChunksX, kDemoChunksY, materials, s)
                : std::make_unique<px::World>(kWorldChunksX, kWorldChunksY, materials, s, px::EdgeX::Wrap);
  };
  std::unique_ptr<px::World> world_ptr = make_world(seed);
  const int w = demo ? world_ptr->width() : kViewW;  // view size in cells
  const int h = demo ? world_ptr->height() : kViewH;

  // Camera: top-left of the view in world cells. x is never wrapped here (the
  // renderer and World wrap it), so a paint stroke stays continuous while the
  // camera crosses the seam.
  double cam_x = 0.0, cam_y = 0.0;
  double home_x = 0.0, home_y = 0.0;
  auto build = [&](px::World& wd) {
    if (demo) {
      px::build_demo_scene(wd, ids);
      return;
    }
    const px::WorldGenerator gen(ids, wd.seed(), wd.width(), wd.height());
    gen.generate(wd);
    home_x = wd.width() / 2.0 - w / 2.0;
    home_y = std::clamp(gen.surface_y(wd.width() / 2) - h / 3, 0, wd.height() - h);
    cam_x = home_x;
    cam_y = home_y;
  };
  build(*world_ptr);

  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    report_error("SDL_Init");
    return 1;
  }
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
  // SDL_GetMouseState returns window pixels; convert those to view cells.
  auto to_view = [&](int window_x, int window_y, int& x, int& y) {
    float lx = 0, ly = 0;
    SDL_RenderWindowToLogical(sdl_renderer, window_x, window_y, &lx, &ly);
    x = static_cast<int>(std::floor(lx));
    y = static_cast<int>(std::floor(ly));
  };
  // View cells to world cells (x unwrapped; World::set wraps it).
  auto cam_ix = [&] { return static_cast<int>(std::floor(cam_x)); };
  auto cam_iy = [&] { return static_cast<int>(std::floor(cam_y)); };
  auto stroke_to = [&](int view_x, int view_y, bool erase) {
    px::World& world = *world_ptr;
    const int x = view_x + cam_ix(), y = view_y + cam_iy();
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

  bool panning = false;
  int pan_x = 0, pan_y = 0;
  auto clamp_camera = [&] {
    if (demo) return;
    cam_y = std::clamp(cam_y, 0.0, static_cast<double>(world_ptr->height() - h));
  };
  auto cycle_material = [&](int dir) {
    const int n = static_cast<int>(materials.size());
    int m = brush.material;
    do m = (m + dir + n) % n;
    while (m == px::kEmpty);
    brush.material = static_cast<px::MaterialId>(m);
  };

  bool new_world = false;
  for (long frame = 0; running && (max_frames < 0 || frame < max_frames); ++frame) {
    if (new_world) {  // swapped between frames: `world` below must stay valid for the whole frame
      seed = px::hash32(static_cast<std::uint32_t>(seed) + 0x9E3779B9U) |
             (static_cast<std::uint64_t>(px::hash32(static_cast<std::uint32_t>(seed >> 32) + 1U)) << 32);
      world_ptr = make_world(seed);
      build(*world_ptr);
      stroke_active = false;
      new_world = false;
    }
    px::World& world = *world_ptr;
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
            case SDLK_q: cycle_material(-1); break;
            case SDLK_e: cycle_material(1); break;
            case SDLK_t: show_heat = !show_heat; break;
            case SDLK_r: build(world); break;
            case SDLK_n: new_world = true; break;
            case SDLK_c: world.clear(); break;
            case SDLK_f: show_dirty = !show_dirty; break;
            case SDLK_HOME:
              cam_x = home_x;
              cam_y = home_y;
              break;
            default: break;
          }
          break;
        case SDL_MOUSEBUTTONDOWN:
          if (e.button.button == SDL_BUTTON_MIDDLE) {
            panning = true;
            pan_x = e.button.x;
            pan_y = e.button.y;
          }
          if (e.button.button == SDL_BUTTON_LEFT || e.button.button == SDL_BUTTON_RIGHT) {
            // SDL already reports mouse events in logical (world) coordinates
            // because the renderer has a logical size set.
            stroke_active = false;
            stroke_to(e.button.x, e.button.y, e.button.button == SDL_BUTTON_RIGHT);
          }
          break;
        case SDL_MOUSEMOTION:
          if (panning && !demo) {
            cam_x -= e.motion.x - pan_x;
            cam_y -= e.motion.y - pan_y;
            pan_x = e.motion.x;
            pan_y = e.motion.y;
            clamp_camera();
          }
          if (e.motion.state & (SDL_BUTTON_LMASK | SDL_BUTTON_RMASK)) {
            stroke_to(e.motion.x, e.motion.y, (e.motion.state & SDL_BUTTON_LMASK) == 0);
          }
          break;
        case SDL_MOUSEBUTTONUP:
          if (e.button.button == SDL_BUTTON_MIDDLE) panning = false;
          if ((SDL_GetMouseState(nullptr, nullptr) & (SDL_BUTTON_LMASK | SDL_BUTTON_RMASK)) == 0) stroke_active = false;
          break;
        case SDL_MOUSEWHEEL:
          brush.radius = std::clamp(brush.radius + (e.wheel.y > 0 ? 1 : -1), 1, 40);
          break;
        default:
          break;
      }
    }

    const auto now = clock::now();
    const double dt = std::chrono::duration<double>(now - previous).count();
    previous = now;

    // Camera movement from the keyboard.
    if (!demo) {
      const Uint8* keys = SDL_GetKeyboardState(nullptr);
      const double speed = kCameraSpeed * std::min(dt, 0.1) * (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT] ? 3.0 : 1.0);
      if (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT]) cam_x -= speed;
      if (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]) cam_x += speed;
      if (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP]) cam_y -= speed;
      if (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN]) cam_y += speed;
      clamp_camera();
    }

    // Holding the button still keeps pouring at the cursor.
    int wx = 0, wy = 0;
    const Uint32 buttons = SDL_GetMouseState(&wx, &wy);
    int mx = 0, my = 0;
    to_view(wx, wy, mx, my);
    if (stroke_active && !painted_this_frame && (buttons & (SDL_BUTTON_LMASK | SDL_BUTTON_RMASK))) {
      stroke_to(mx, my, (buttons & SDL_BUTTON_LMASK) == 0);
    }

    // Fixed-timestep simulation, limited to the chunks around the camera.
    px::ChunkRegion region;
    if (demo) {
      region = {0, 0, world.width_chunks() - 1, world.height_chunks() - 1};
    } else {
      region.cx0 = (cam_ix() >> px::kChunkShift) - kSimMarginChunks;
      region.cy0 = (cam_iy() >> px::kChunkShift) - kSimMarginChunks;
      region.cx1 = ((cam_ix() + w - 1) >> px::kChunkShift) + kSimMarginChunks;
      region.cy1 = ((cam_iy() + h - 1) >> px::kChunkShift) + kSimMarginChunks;
    }
    accumulator = std::min(accumulator + dt, kStepSeconds * kMaxStepsPerFrame);
    if (paused) accumulator = 0.0;
    int steps = paused ? (single_step ? 1 : 0) : static_cast<int>(accumulator / kStepSeconds);
    single_step = false;
    double frame_sim_ms = 0.0;
    for (int s = 0; s < steps; ++s) {
      const auto t0 = clock::now();
      last_stats = px::step(world, region);
      const double step_ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
      sim_ms_sum += step_ms;
      frame_sim_ms += step_ms;
      ++sim_steps;
      if (!paused) accumulator -= kStepSeconds;
      if (frame_sim_ms > kSimBudgetMs) {
        accumulator = std::min(accumulator, kStepSeconds * 0.5);  // drop the backlog: slow motion
        break;
      }
    }

    const px::Viewport view{cam_ix(), cam_iy(), w, h};
    if (show_heat) renderer.render_temperature(world, pixels.data(), w, view);
    else renderer.render(world, pixels.data(), w, view);
    if (show_dirty) renderer.draw_dirty_rects(world, pixels.data(), w, view);
    draw_brush_outline(pixels.data(), w, h, mx, my, brush.radius);
    SDL_UpdateTexture(texture, nullptr, pixels.data(), w * static_cast<int>(sizeof(std::uint32_t)));
    SDL_RenderClear(sdl_renderer);
    SDL_RenderCopy(sdl_renderer, texture, nullptr, nullptr);
    SDL_RenderPresent(sdl_renderer);

    ++frames;
    title_timer += dt;
    if (title_timer >= 0.5) {
      char where[96] = "";
      if (!demo) {
        std::snprintf(where, sizeof(where), " | seed %llu | x %d/%d depth %d", static_cast<unsigned long long>(seed),
                      world.wrap_x(cam_ix() + w / 2), world.width(), cam_iy() + h / 2);
      }
      char title[320];
      std::snprintf(title, sizeof(title),
                    "Pixel Sandbox | %s | brush %d | %.0f fps | sim %.2f ms | active chunks %d/%d%s%s%s",
                    materials.def(brush.material).name.c_str(), brush.radius, frames / title_timer,
                    sim_steps ? sim_ms_sum / sim_steps : 0.0, last_stats.active_chunks,
                    world.width_chunks() * world.height_chunks(), where, show_heat ? " | HEAT VIEW" : "",
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
