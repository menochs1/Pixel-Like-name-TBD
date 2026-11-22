// Interactive sandbox: explore a generated world as a little character, paint
// materials and watch them simulate.
//
// Controls
//   A / D, arrows  walk
//   W / Up / Space jump (hold for a higher jump; in water: swim up)
//   S / Down       swim down
//   Tab            free camera on/off: WASD / arrows then move the camera
//                  (Shift = faster) and the character waits where it is
//   Middle mouse   drag to move the camera (free camera)
//   Home           back to the starting point
//   Left mouse     use the current tool (paint, heat or cool)
//   Right mouse    erase
//   Mouse wheel    brush size
//   1 sand   2 water   3 stone   4 oil   5 wood
//   6 fire   7 lava    8 ice     9 steam 0 smoke
//   Q / E          previous / next material (all of them, incl. dirt, coal...)
//                  (choosing a material switches back to painting)
//   H              heat brush: melts rock, boils water, lights fuel
//   J              cool brush: freezes water, hardens lava, puts out fire
//   I              toggle the pixel inspector (what is under the cursor)
//   [ / ]          nudge the wind left / right (it also changes by itself)
//
// Weather: steam (9) rises and cools into cloud, which gathers in a layer
// above the ground and rains where it is thick. Wind pushes clouds, smoke,
// steam, flames and falling rain.
//   T              toggle heat view (temperature map)
//   P              pause / resume
//   . (period)     single step while paused
//   F5             save (to saves/quicksave)
//   F9             load (from saves/quicksave)
//   R              regenerate the world (same seed, changes discarded)
//   N              new world (new seed)
//   C              clear the world
//   F              toggle the active-region (dirty rect) overlay
//   Esc            quit
//
// The generated world wraps around horizontally: keep going right and you come
// back to where you started. Only the chunks around the camera are in memory;
// the rest live on disk (changed ones) or are regenerated from the seed when
// the camera comes back (unchanged ones).
//
// Command line:
//   --seed N    world seed (default 0xC0FFEE)
//   --demo      the small hand-built demo scene instead of a generated world
//   --load DIR  start from a save
//   --frames N  quit after N frames (for smoke tests)

#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/material.hpp"
#include "core/rng.hpp"
#include "core/simulator.hpp"
#include "core/world.hpp"
#include "entities/player.hpp"
#include "render/pixel_renderer.hpp"
#include "render/sprites.hpp"
#include "render/text.hpp"
#include "sandbox/tools.hpp"
#include "save/chunk_store.hpp"
#include "save/streamer.hpp"
#include "save/world_save.hpp"
#include "world/demo_scene.hpp"
#include "world/world_gen.hpp"

namespace {

constexpr int kWorldChunksX = 768;  // 49152 x 4608 cells, wrapping around in x (streamed)
constexpr int kWorldChunksY = 72;
constexpr float kWorldScale = 3.0f;  // terrain features to match: 3x the 16384 x 1536 world
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
// Streaming, in chunks beyond the simulated region: loaded right away (so
// everything that simulates only ever meets loaded chunks), prefetched a few
// per frame, and kept before unloading (so edge chunks do not thrash).
constexpr int kRequiredMargin = 1;
constexpr int kPrefetchMargin = 2;
constexpr int kKeepMargin = 3;
constexpr int kPrefetchPerFrame = 6;
const char* const kQuickSave = "saves/quicksave";
// Weather: the cloud layer sits this far above the mean ground level, and the
// wind wanders up to this fast (cells/step; 0.3 = 18 cells/s) over about a minute.
constexpr int kCloudHeight = 200;
constexpr float kMaxWind = 0.3f;
constexpr int kWindPeriod = 3600;

/// Clouds a fixed height above the mean ground, wind calm below ground level.
px::Weather weather_for(int ground_y) {
  px::Weather sky;
  sky.clouds = true;
  sky.cloud_top = std::max(0, ground_y - kCloudHeight);
  sky.cloud_wobble = 16;
  sky.max_wind = kMaxWind;
  sky.wind_period = kWindPeriod;
  sky.ground_y = ground_y;
  return sky;
}

/// One world being played, and what keeps it streamed. Members are destroyed in
/// reverse order: the streamer before what it refers to.
struct Session {
  std::unique_ptr<px::World> world;
  std::unique_ptr<px::ChunkStore> store;          // null: everything in memory (demo)
  std::optional<px::WorldGenerator> generator;    // recreates untouched chunks
  std::optional<px::WorldGenParams> gen_params;   // recorded in saves
  std::unique_ptr<px::ChunkStreamer> streamer;
  double home_x = 0.0, home_y = 0.0;              // camera start
  px::Player player;
  bool player_placed = false;                     // placed once its chunks are loaded
  float spawn_x = 0.0f;                           // start: left edge of the player...
  int spawn_from_y = 0;                           // ...dropped onto the ground below this row
  bool restore_player = false;                    // from a save: put it exactly here instead
  float saved_x = 0.0f, saved_y = 0.0f;
};

/// A scratch directory for the chunks this session unloads.
std::filesystem::path session_dir() {
  static int counter = 0;
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("pixel_sandbox_" + std::to_string(stamp % 1000000007) + "_" + std::to_string(counter++));
}

void attach_streaming(Session& s) {
  s.store = std::make_unique<px::ChunkStore>(session_dir());
  s.streamer = std::make_unique<px::ChunkStreamer>(*s.world, *s.store, s.generator ? &*s.generator : nullptr);
}

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

void draw_brush_outline(std::uint32_t* pixels, int w, int h, int cx, int cy, int r, std::uint32_t color) {
  const int segments = std::max(16, r * 6);
  for (int i = 0; i < segments; ++i) {
    const double a = 2.0 * 3.14159265358979 * i / segments;
    const int x = cx + static_cast<int>(std::lround(std::cos(a) * r));
    const int y = cy + static_cast<int>(std::lround(std::sin(a) * r));
    if (x >= 0 && y >= 0 && x < w && y < h) pixels[y * w + x] = color;
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
  std::string load_dir;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) max_frames = std::atol(argv[++i]);
    else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 0);
    else if (std::strcmp(argv[i], "--demo") == 0) demo = true;
    else if (std::strcmp(argv[i], "--load") == 0 && i + 1 < argc) load_dir = argv[++i];
  }

  px::MaterialRegistry materials;
  const px::BaseMaterials ids = px::register_base_materials(materials);
  px::PixelRenderer renderer(materials);

  const int w = demo ? kDemoChunksX * px::kChunkSize : kViewW;  // view size in cells
  const int h = demo ? kDemoChunksY * px::kChunkSize : kViewH;

  // The World's seed drives both generation and the simulation's randomness,
  // so a new seed means a new World (and session).
  auto new_session = [&](std::uint64_t s) {
    auto session = std::make_unique<Session>();
    if (demo) {
      session->world = std::make_unique<px::World>(kDemoChunksX, kDemoChunksY, materials, s);
      px::build_demo_scene(*session->world, ids);
      session->spawn_x = 12.0f;  // on the ground left of the water basin
      px::Weather sky = weather_for(session->world->height() - 40);
      sky.cloud_top = 40;  // a small world: clouds near the top
      session->world->set_weather(sky);
      return session;
    }
    session->world = std::make_unique<px::World>(kWorldChunksX, kWorldChunksY, materials, s, px::EdgeX::Wrap,
                                                 px::ChunkStorage::Streamed);
    session->gen_params = px::WorldGenParams{};
    session->gen_params->scale = kWorldScale;
    session->generator.emplace(ids, s, session->world->width(), session->world->height(), *session->gen_params);
    attach_streaming(*session);
    session->world->set_weather(
        weather_for(static_cast<int>(session->gen_params->surface * static_cast<float>(session->world->height()))));
    session->home_x = session->world->width() / 2.0 - w / 2.0;
    session->home_y =
        std::clamp(session->generator->surface_y(session->world->width() / 2) - h / 3, 0, session->world->height() - h);
    session->spawn_x = static_cast<float>(session->world->width() / 2 - 8);
    session->spawn_from_y =
        session->generator->surface_y(session->world->width() / 2) - static_cast<int>(80 * kWorldScale);  // above the trees
    return session;
  };
  // Saves always load streamed, whatever they came from.
  auto load_session = [&](const std::string& dir, std::string& error) -> std::unique_ptr<Session> {
    std::optional<px::OpenedSave> save = px::open_save(dir, materials, &error);
    if (!save) return nullptr;
    auto session = std::make_unique<Session>();
    session->world = std::move(save->world);
    session->gen_params = save->info.generator;
    if (session->gen_params) {
      session->generator.emplace(ids, session->world->seed(), session->world->width(), session->world->height(),
                                 *session->gen_params);
    }
    attach_streaming(*session);
    session->store->set_base(save->chunk_dir, save->remap);
    const float surface = session->gen_params ? session->gen_params->surface : 0.8f;  // no generator: low ground
    session->world->set_weather(weather_for(static_cast<int>(surface * static_cast<float>(session->world->height()))));
    session->home_x = save->info.view_x;
    session->home_y = save->info.view_y;
    session->spawn_x = static_cast<float>(save->info.view_x + w / 2 - 8);
    session->spawn_from_y = save->info.view_y;
    session->restore_player = save->info.has_player;
    session->saved_x = save->info.player_x;
    session->saved_y = save->info.player_y;
    return session;
  };

  std::unique_ptr<Session> session;
  std::string toast;          // a short message shown on screen (saved, loaded, errors)
  double toast_time = 0.0;
  if (!load_dir.empty()) {
    std::string error;
    session = load_session(load_dir, error);
    if (session) {
      seed = session->world->seed();
      toast = "loaded " + load_dir;
    } else {
      toast = "cannot load: " + error;
    }
    toast_time = 4.0;
  }
  if (!session) session = new_session(seed);

  // Camera: top-left of the view in world cells. x is never wrapped here (the
  // renderer and World wrap it), so a paint stroke stays continuous while the
  // camera crosses the seam.
  double cam_x = session->home_x, cam_y = session->home_y;

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
  enum class Tool { Paint, Heat, Cool };
  Tool tool = Tool::Paint;
  bool show_inspector = true;
  bool free_camera = false;
  auto tool_name = [&]() -> std::string {
    switch (tool) {
      case Tool::Heat: return "heat brush";
      case Tool::Cool: return "cool brush";
      case Tool::Paint: break;
    }
    return "paint " + materials.def(brush.material).name;
  };
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
    px::World& world = *session->world;
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
    const int max_y = session->world->height() - h;
    cam_y = max_y > 0 ? std::clamp(cam_y, 0.0, static_cast<double>(max_y)) : 0.0;  // a world shorter than the view
  };
  auto cycle_material = [&](int dir) {
    tool = Tool::Paint;
    const int n = static_cast<int>(materials.size());
    int m = brush.material;
    do m = (m + dir + n) % n;
    while (m == px::kEmpty || materials.def(static_cast<px::MaterialId>(m)).internal);
    brush.material = static_cast<px::MaterialId>(m);
  };

  // Actions that replace the session run between frames: `world` below must
  // stay valid for the whole frame.
  enum class Pending { None, NewWorld, Regenerate, Load };
  Pending pending = Pending::None;
  auto show = [&](std::string message) {
    toast = std::move(message);
    toast_time = 3.0;
  };
  for (long frame = 0; running && (max_frames < 0 || frame < max_frames); ++frame) {
    if (pending != Pending::None) {
      if (pending == Pending::NewWorld) {
        seed = px::hash32(static_cast<std::uint32_t>(seed) + 0x9E3779B9U) |
               (static_cast<std::uint64_t>(px::hash32(static_cast<std::uint32_t>(seed >> 32) + 1U)) << 32);
      }
      std::unique_ptr<Session> next;
      if (pending == Pending::Load) {
        std::string error;
        next = load_session(kQuickSave, error);
        if (next) {
          seed = next->world->seed();
          show("loaded " + std::string(kQuickSave));
        } else {
          show("cannot load: " + error);
        }
      } else {
        next = new_session(seed);
      }
      if (next) {
        session = std::move(next);
        cam_x = session->home_x;
        cam_y = session->home_y;
        clamp_camera();
        stroke_active = false;
      }
      pending = Pending::None;
    }
    px::World& world = *session->world;
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
            case SDLK_p: paused = !paused; break;
            case SDLK_TAB:
              free_camera = !free_camera;
              show(free_camera ? "free camera: WASD moves the view (Tab: back to the character)"
                               : "following the character");
              break;
            case SDLK_PERIOD: single_step = true; break;
            case SDLK_1: brush.material = ids.sand; tool = Tool::Paint; break;
            case SDLK_2: brush.material = ids.water; tool = Tool::Paint; break;
            case SDLK_3: brush.material = ids.stone; tool = Tool::Paint; break;
            case SDLK_4: brush.material = ids.oil; tool = Tool::Paint; break;
            case SDLK_5: brush.material = ids.wood; tool = Tool::Paint; break;
            case SDLK_6: brush.material = ids.fire; tool = Tool::Paint; break;
            case SDLK_7: brush.material = ids.lava; tool = Tool::Paint; break;
            case SDLK_8: brush.material = ids.ice; tool = Tool::Paint; break;
            case SDLK_9: brush.material = ids.steam; tool = Tool::Paint; break;
            case SDLK_0: brush.material = ids.smoke; tool = Tool::Paint; break;
            case SDLK_h: tool = Tool::Heat; break;
            case SDLK_j: tool = Tool::Cool; break;
            case SDLK_i: show_inspector = !show_inspector; break;
            case SDLK_q: cycle_material(-1); break;
            case SDLK_e: cycle_material(1); break;
            case SDLK_t: show_heat = !show_heat; break;
            case SDLK_r: pending = Pending::Regenerate; break;
            case SDLK_n: pending = Pending::NewWorld; break;
            case SDLK_F9: pending = Pending::Load; break;
            case SDLK_F5: {
              px::SaveInfo info;
              info.generator = session->gen_params;
              info.view_x = cam_ix();
              info.view_y = cam_iy();
              info.has_player = session->player_placed;
              info.player_x = session->player.x();
              info.player_y = session->player.y();
              std::string error;
              if (px::save_world(world, info, kQuickSave, session->store.get(), &error)) {
                if (session->store) px::mark_saved(world, *session->store, kQuickSave);
                show("saved to " + std::string(kQuickSave));
              } else {
                show("save failed: " + error);
              }
              break;
            }
            case SDLK_c:
              // Everything empty: forget stored chunks and stop regenerating, so
              // chunks loaded later come back empty too.
              if (session->store) {
                session->store->clear();
                session->streamer->set_generator(nullptr);
                session->gen_params.reset();
              }
              world.clear();
              break;
            case SDLK_f: show_dirty = !show_dirty; break;
            case SDLK_LEFTBRACKET:
            case SDLK_RIGHTBRACKET: {
              px::Weather sky = world.weather();
              sky.wind_bias += e.key.keysym.sym == SDLK_RIGHTBRACKET ? 0.05f : -0.05f;
              world.set_weather(sky);
              char msg[64];
              std::snprintf(msg, sizeof(msg), "wind nudged %+.0f cells/s", static_cast<double>(sky.wind_bias) * 60.0);
              show(msg);
              break;
            }
            case SDLK_HOME:
              cam_x = session->home_x;
              cam_y = session->home_y;
              session->player_placed = false;  // back to the start, once its chunks are loaded
              session->restore_player = false;
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
          // Painting and erasing; the thermal brushes act once per simulation step instead.
          if ((e.button.button == SDL_BUTTON_LEFT && tool == Tool::Paint) || e.button.button == SDL_BUTTON_RIGHT) {
            // SDL already reports mouse events in logical (world) coordinates
            // because the renderer has a logical size set.
            stroke_active = false;
            stroke_to(e.button.x, e.button.y, e.button.button == SDL_BUTTON_RIGHT);
          }
          break;
        case SDL_MOUSEMOTION:
          if (panning && !demo && free_camera) {
            cam_x -= e.motion.x - pan_x;
            cam_y -= e.motion.y - pan_y;
            pan_x = e.motion.x;
            pan_y = e.motion.y;
            clamp_camera();
          }
          if (e.motion.state & SDL_BUTTON_RMASK) {
            stroke_to(e.motion.x, e.motion.y, true);
          } else if ((e.motion.state & SDL_BUTTON_LMASK) && tool == Tool::Paint) {
            stroke_to(e.motion.x, e.motion.y, false);
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

    // Keyboard: the character, or the camera in free-camera mode.
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    px::PlayerInput input;
    if (free_camera && !demo) {
      const double speed = kCameraSpeed * std::min(dt, 0.1) * (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT] ? 3.0 : 1.0);
      if (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT]) cam_x -= speed;
      if (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]) cam_x += speed;
      if (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP]) cam_y -= speed;
      if (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN]) cam_y += speed;
      clamp_camera();
    } else if (!free_camera) {
      input.left = keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT];
      input.right = keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT];
      input.jump = keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP] || keys[SDL_SCANCODE_SPACE];
      input.down = keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN];
    }
    // The camera follows the character, easing toward it.
    px::Player& player = session->player;
    if (!free_camera && !demo && session->player_placed) {
      const double tx = player.x() + player.params().width / 2.0 - w / 2.0;
      const double ty = player.y() + player.params().height / 2.0 - h * 0.55;
      const double k = std::min(1.0, dt * 8.0);
      cam_x += (tx - cam_x) * k;
      cam_y += (ty - cam_y) * k;
      clamp_camera();
    }

    // The simulated region: the view plus a margin, chunks beyond it pause.
    px::ChunkRegion region;
    if (demo) {
      region = {0, 0, world.width_chunks() - 1, world.height_chunks() - 1};
    } else {
      region.cx0 = (cam_ix() >> px::kChunkShift) - kSimMarginChunks;
      region.cy0 = (cam_iy() >> px::kChunkShift) - kSimMarginChunks;
      region.cx1 = ((cam_ix() + w - 1) >> px::kChunkShift) + kSimMarginChunks;
      region.cy1 = ((cam_iy() + h - 1) >> px::kChunkShift) + kSimMarginChunks;
    }
    // Stream chunks in around it and out far from it.
    if (session->streamer) {
      auto grow = [&](int m) { return px::ChunkRegion{region.cx0 - m, region.cy0 - m, region.cx1 + m, region.cy1 + m}; };
      session->streamer->update(grow(kRequiredMargin), grow(kPrefetchMargin), grow(kKeepMargin), kPrefetchPerFrame);
      if (!session->streamer->last_error().empty() && toast_time <= 0.0) show(session->streamer->last_error());
    }
    // Put the character in the world once the ground under it is loaded.
    if (!session->player_placed && world.in_bounds(static_cast<int>(session->spawn_x), std::max(session->spawn_from_y, 0))) {
      if (session->restore_player) player.place(session->saved_x, session->saved_y);
      else player.place_on_ground(world, session->spawn_x, session->spawn_from_y);
      session->player_placed = true;
    }
    // It only moves while the chunks around it are loaded (in free-camera
    // mode it can be left far behind; it waits there).
    auto player_loaded = [&] {
      const int l = player.left(), t = player.top();
      const int r = l + player.params().width - 1, b = t + player.params().height - 1;
      return world.in_bounds(l, t) && world.in_bounds(r, t) && world.in_bounds(l, b) && world.in_bounds(r, b);
    };

    // Holding the button still keeps pouring at the cursor.
    int wx = 0, wy = 0;
    const Uint32 buttons = SDL_GetMouseState(&wx, &wy);
    int mx = 0, my = 0;
    to_view(wx, wy, mx, my);
    const bool erasing = (buttons & SDL_BUTTON_RMASK) != 0;
    const bool using_tool = (buttons & SDL_BUTTON_LMASK) != 0 && !erasing;
    if (stroke_active && !painted_this_frame && (erasing || (using_tool && tool == Tool::Paint))) {
      stroke_to(mx, my, erasing);
    }

    // Fixed-timestep simulation, limited to the chunks around the camera.
    accumulator = std::min(accumulator + dt, kStepSeconds * kMaxStepsPerFrame);
    if (paused) accumulator = 0.0;
    int steps = paused ? (single_step ? 1 : 0) : static_cast<int>(accumulator / kStepSeconds);
    single_step = false;
    double frame_sim_ms = 0.0;
    for (int s = 0; s < steps; ++s) {
      if (using_tool && tool != Tool::Paint) {  // once per step: independent of frame rate
        px::apply_thermal_brush(world, mx + cam_ix(), my + cam_iy(), brush.radius,
                                tool == Tool::Heat ? px::kHeatBrush : px::kCoolBrush);
      }
      const bool move_player = session->player_placed && player_loaded();
      if (move_player) {
        player.update(world, input);
        player.occupy(world);  // solid to the simulation for this step
      }
      const auto t0 = clock::now();
      last_stats = px::step(world, region);
      if (move_player) player.release(world);
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
    if (session->player_placed) {
      int sx = player.left() - cam_ix();
      if (world.wraps_x()) {  // the copy of it nearest the view
        const int ww = world.width();
        sx = ((sx + ww / 2) % ww + ww) % ww - ww / 2;
      }
      px::draw_player(pixels.data(), w, w, h, sx, player.top() - cam_iy(),
                      px::player_pose(player.on_ground() || player.in_liquid(), player.walk_steps()),
                      player.facing_left());
    }
    const std::uint32_t outline = tool == Tool::Heat ? 0xFFFF7040U : (tool == Tool::Cool ? 0xFF60C0FFU : 0xFFE8E8F0U);
    draw_brush_outline(pixels.data(), w, h, mx, my, brush.radius, outline);
    if (show_inspector) {
      std::vector<std::string> lines = px::describe(world, px::inspect(world, mx + cam_ix(), my + cam_iy()));
      // Wind where the camera is: arrows show direction, number is cells/s.
      const float wind = world.wind_at(cam_iy() + h / 2) * 60.0f;
      const int arrows = std::min(3, static_cast<int>(std::fabs(wind) / 8.0f) + (std::fabs(wind) >= 1.0f ? 1 : 0));
      std::string wind_text = "wind ";
      if (arrows == 0) wind_text += "calm";
      else wind_text += std::string(static_cast<std::size_t>(arrows), wind > 0 ? '>' : '<') + " " +
                        std::to_string(static_cast<int>(std::lround(std::fabs(wind))));
      lines.insert(lines.begin(), tool_name() + "  r" + std::to_string(brush.radius) + "  " + wind_text +
                                      (paused ? "  paused" : ""));
      int pw = 0;
      for (const std::string& line : lines) pw = std::max(pw, px::text_width(line) + 6);
      // Top-left, unless the cursor is there: then top-right.
      const int px0 = (mx < pw + 8 && my < static_cast<int>(lines.size()) * px::kLineHeight + 12) ? w - pw - 2 : 2;
      px::draw_text_panel(pixels.data(), w, w, h, px0, 2, lines);
    }
    if (toast_time > 0.0) {
      toast_time -= dt;
      const int tw = px::text_width(toast) + 6;
      px::draw_text_panel(pixels.data(), w, w, h, (w - tw) / 2, h - 16, {toast}, 0xFFFFE080U);
    }
    SDL_UpdateTexture(texture, nullptr, pixels.data(), w * static_cast<int>(sizeof(std::uint32_t)));
    SDL_RenderClear(sdl_renderer);
    SDL_RenderCopy(sdl_renderer, texture, nullptr, nullptr);
    SDL_RenderPresent(sdl_renderer);

    ++frames;
    title_timer += dt;
    if (title_timer >= 0.5) {
      char where[160] = "";
      if (!demo) {
        const int resident = world.loaded_chunk_count();
        std::snprintf(where, sizeof(where), " | seed %llu | x %d/%d depth %d | %d chunks in memory (%.1f MiB)",
                      static_cast<unsigned long long>(seed), world.wrap_x(cam_ix() + w / 2), world.width(),
                      cam_iy() + h / 2, resident, resident * static_cast<double>(sizeof(px::Chunk)) / (1024.0 * 1024.0));
      }
      char title[320];
      std::snprintf(title, sizeof(title),
                    "Pixel Sandbox | %s | brush %d | %.0f fps | sim %.2f ms | active chunks %d/%d%s%s%s",
                    tool_name().c_str(), brush.radius, frames / title_timer,
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
