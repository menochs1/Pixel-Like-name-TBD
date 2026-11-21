#include "world/world_gen.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <stdexcept>
#include <thread>
#include <vector>

#include "core/rng.hpp"

namespace px {
namespace {

int mod(int a, int m) {
  const int r = a % m;
  return r < 0 ? r + m : r;
}

float smooth(float edge0, float edge1, float v) {
  const float t = std::clamp((v - edge0) / (edge1 - edge0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

std::uint32_t lattice_hash(std::uint32_t seed, int a, int b) {
  return hash32(hash32(seed ^ (static_cast<std::uint32_t>(a) * 0x9E3779B1U)) ^
                (static_cast<std::uint32_t>(b) * 0x85EBCA77U));
}

float unit(std::uint32_t bits) { return static_cast<float>(bits & 0xFFFF) / 65535.0f; }

/// 2D gradient (Perlin-style) noise, roughly in [-1, 1]. The lattice repeats
/// every `period` units in u, so noise sampled across a full period joins up.
float gradient_noise(std::uint32_t seed, float u, float v, int period) {
  static constexpr float kGx[8] = {1.0f, 0.7071f, 0.0f, -0.7071f, -1.0f, -0.7071f, 0.0f, 0.7071f};
  static constexpr float kGy[8] = {0.0f, 0.7071f, 1.0f, 0.7071f, 0.0f, -0.7071f, -1.0f, -0.7071f};
  const float fu0 = std::floor(u), fv0 = std::floor(v);
  const int iv = static_cast<int>(fv0);
  const float fu = u - fu0, fv = v - fv0;
  const int a0 = mod(static_cast<int>(fu0), period);
  const int a1 = a0 + 1 == period ? 0 : a0 + 1;
  auto corner = [&](int a, int b, float dx, float dy) {
    const std::uint32_t h = lattice_hash(seed, a, b) & 7U;
    return kGx[h] * dx + kGy[h] * dy;
  };
  const float n00 = corner(a0, iv, fu, fv);
  const float n10 = corner(a1, iv, fu - 1.0f, fv);
  const float n01 = corner(a0, iv + 1, fu, fv - 1.0f);
  const float n11 = corner(a1, iv + 1, fu - 1.0f, fv - 1.0f);
  auto fade = [](float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); };
  const float su = fade(fu), sv = fade(fv);
  const float a = n00 + (n10 - n00) * su;
  const float b = n01 + (n11 - n01) * su;
  return (a + (b - a) * sv) * 1.4f;
}

}  // namespace

// ------------------------------------------------------------------ setup

namespace {

/// Fractal noise in roughly [-1, 1] that repeats every `width` cells in x.
/// `feature` is the size of the largest features in cells; it is rounded so a
/// whole number of lattice cells fits around the world. `stretch_y` > 1 squashes
/// features vertically (wide, flat tunnels).
float fbm(std::uint32_t seed, int width, float x, float y, float feature, int octaves, float stretch_y = 1.0f) {
  const int base = std::max(1, static_cast<int>(std::lround(static_cast<float>(width) / feature)));
  float sum = 0.0f, amp = 1.0f, norm = 0.0f;
  for (int o = 0; o < octaves; ++o) {
    const int count = base << o;
    const float scale = static_cast<float>(count) / static_cast<float>(width);
    sum += amp * gradient_noise(seed + static_cast<std::uint32_t>(o) * 0x632BE5ABU, x * scale, y * scale * stretch_y,
                                count);
    norm += amp;
    amp *= 0.5f;
  }
  return sum / norm;
}

// Sub-seeds for each noise layer, so layers are independent of each other.
enum Layer : std::uint32_t {
  kBiome = 1, kBiomeJitter, kHills, kDunes, kPeaks, kHighlandSoil, kSoilDepth, kEntrance,
  kTunnelA = 10, kTunnelB, kCavern, kCaveRegion,
  kCoal = 20, kSandPocket, kDirtPocket,
  kPools = 30,
  kTrees = 40, kLeaves,
};

}  // namespace

WorldGenerator::WorldGenerator(const BaseMaterials& ids, std::uint64_t seed, int width, int height,
                               WorldGenParams params)
    : ids_(ids), seed_(seed), width_(width), height_(height), p_(params) {
  if (width <= 0 || height <= 0) throw std::invalid_argument("WorldGenerator: dimensions must be positive");
  sea_y_ = static_cast<int>(static_cast<float>(height_) * (p_.surface + p_.sea_level_offset));
  tree_count_ = std::max(1, width_ / 26);
  pool_cols_ = std::max(1, static_cast<int>(std::lround(static_cast<float>(width_) / 128.0f)));
  pool_rows_ = std::max(1, height_ / 96);
}

// ------------------------------------------------------------------ helpers

namespace {

std::uint32_t sub_seed(std::uint64_t seed, std::uint32_t layer) {
  return hash32(static_cast<std::uint32_t>(seed) ^ hash32(static_cast<std::uint32_t>(seed >> 32) + layer * 0x9E3779B9U));
}

}  // namespace

int WorldGenerator::wrap_dx(int dx) const {
  const int d = mod(dx, width_);
  return d >= width_ / 2 ? d - width_ : d;
}

float WorldGenerator::biome_value(int x) const {
  // Huge, slow features: a handful of biome regions around the world.
  return fbm(sub_seed(seed_, kBiome), width_, static_cast<float>(x), 137.0f, static_cast<float>(width_) / 3.0f, 2);
}

WorldGenerator::Column WorldGenerator::column(int x) const {
  x = mod(x, width_);
  const float fx = static_cast<float>(x);
  const float b = biome_value(x);
  const float jitter = 0.03f * fbm(sub_seed(seed_, kBiomeJitter), width_, fx, 11.0f, 90.0f, 1);

  Column col{};
  col.biome = b + jitter < -0.22f ? Biome::Desert : (b + jitter > 0.28f ? Biome::Highlands : Biome::Meadow);

  // Surface height: blend each biome's terrain by smooth weights, so there are
  // no cliffs where biomes meet.
  const float w_desert = smooth(-0.12f, -0.32f, b);
  const float w_high = smooth(0.18f, 0.38f, b);
  const float w_meadow = std::max(0.0f, 1.0f - w_desert - w_high);
  const float k = static_cast<float>(height_) / 1536.0f;  // terrain scales with world height
  const float hills = fbm(sub_seed(seed_, kHills), width_, fx, 50.0f, 260.0f, 4);
  const float dunes = fbm(sub_seed(seed_, kDunes), width_, fx, 50.0f, 70.0f, 2);
  const float ridge = 1.0f - std::fabs(fbm(sub_seed(seed_, kPeaks), width_, fx, 50.0f, 240.0f, 2));
  const float rise = hills * (30.0f * w_desert + 70.0f * w_meadow + 110.0f * w_high) + dunes * 9.0f * w_desert +
                     ridge * ridge * 130.0f * w_high;
  const float base = static_cast<float>(height_) * p_.surface;
  col.surface = std::clamp(static_cast<int>(base - rise * k), height_ / 16, height_ / 2);

  const float depth01 = 0.5f + 0.5f * fbm(sub_seed(seed_, kSoilDepth), width_, fx, 90.0f, 50.0f, 2);
  switch (col.biome) {
    case Biome::Desert:
      col.soil = 14 + static_cast<int>(16.0f * depth01);
      break;
    case Biome::Meadow:
      col.soil = 7 + static_cast<int>(9.0f * depth01);
      break;
    case Biome::Highlands: {
      // Mostly bare rock with patches of soil.
      const float n = fbm(sub_seed(seed_, kHighlandSoil), width_, fx, 80.0f, 40.0f, 2);
      col.soil = n > 0.1f ? 2 + static_cast<int>(10.0f * n) : 0;
      break;
    }
  }
  const bool dry = col.surface < sea_y_;
  col.grass = col.biome != Biome::Desert && col.soil > 0 && dry;
  col.entrance = col.biome != Biome::Desert && col.surface < sea_y_ - 2 &&
                 fbm(sub_seed(seed_, kEntrance), width_, fx, 30.0f, 90.0f, 1) > 0.5f;
  return col;
}

int WorldGenerator::surface_y(int x) const { return column(x).surface; }
Biome WorldGenerator::biome_at(int x) const { return column(x).biome; }

// ------------------------------------------------------------------ trees

// Trees sit in `tree_count_` equal slots around the ring, each jittered within
// its slot and only grown on dry meadow ground. A tree is a pure function of
// its slot index, so any chunk can find the trees that overlap it.
MaterialId WorldGenerator::tree_cell(int x, int y) const {
  const float spacing = static_cast<float>(width_) / static_cast<float>(tree_count_);
  const int slot = static_cast<int>(std::floor(static_cast<float>(x) / spacing));
  const std::uint32_t seed = sub_seed(seed_, kTrees);
  for (int s = slot - 1; s <= slot + 1; ++s) {
    const int k = mod(s, tree_count_);
    const std::uint32_t h = lattice_hash(seed, k, 0);
    if ((h & 0xFF) >= 150) continue;  // ~60% of slots have a tree
    const int tx = mod(static_cast<int>((static_cast<float>(k) + 0.15f + 0.7f * unit(h >> 8)) * spacing), width_);
    const int dx = wrap_dx(x - tx);
    if (dx < -14 || dx > 14) continue;
    const Column col = column(tx);
    if (col.biome != Biome::Meadow || !col.grass) continue;
    const int height = 14 + static_cast<int>((h >> 24) & 15U);
    const float radius = 6.0f + 0.7f * static_cast<float>((h >> 28) & 7U);
    const int top = col.surface - height;
    const int trunk_w = height > 24 ? 3 : 2;
    if (dx >= -1 && dx < trunk_w - 1 && y >= top + 2 && y < col.surface) return ids_.wood;
    // Crown: two overlapping, slightly ragged ellipses of leaves.
    const float jitter = unit(cell_hash(seed_ ^ kLeaves, mod(x, width_), y, 0));
    const float fdx = static_cast<float>(dx), fdy = static_cast<float>(y - top) * 1.3f;
    if (fdx * fdx + fdy * fdy < radius * radius * (0.72f + 0.28f * jitter)) return ids_.leaves;
    const float side = (h & 0x100U) ? 1.0f : -1.0f;
    const float bx = fdx - side * radius * 0.6f, by = (static_cast<float>(y - top) - radius * 0.45f) * 1.3f;
    const float r2 = radius * 0.7f;
    if (bx * bx + by * by < r2 * r2 * (0.72f + 0.28f * jitter)) return ids_.leaves;
  }
  return kEmpty;
}

// ------------------------------------------------------------------ pools

WorldGenerator::Bowl WorldGenerator::bowl_at(int x, int y) const {
  Bowl out;
  x = mod(x, width_);
  const int cell_w = width_ / pool_cols_;  // the last column absorbs any remainder
  const int cell_h = height_ / pool_rows_;
  const int gx = std::min(x / cell_w, pool_cols_ - 1);
  const int gy = std::min(y / cell_h, pool_rows_ - 1);
  const std::uint32_t h = lattice_hash(sub_seed(seed_, kPools), gx, gy);
  if ((h & 0xFF) >= 110) return out;  // ~43% of grid cells have a pool

  const int left = gx * cell_w, top = gy * cell_h;
  const int right = gx == pool_cols_ - 1 ? width_ : left + cell_w;
  const int bottom = gy == pool_rows_ - 1 ? height_ : top + cell_h;
  constexpr int kShell = 5;
  // Radii and centre, chosen so bowl plus shell stays inside the grid cell.
  const float rx = std::min(16.0f + 30.0f * unit(h >> 8), static_cast<float>(right - left) * 0.5f - kShell - 2);
  const float ry = std::min(9.0f + 16.0f * unit(h >> 16), static_cast<float>(bottom - top) * 0.5f - kShell - 2);
  if (rx < 6.0f || ry < 4.0f) return out;
  const std::uint32_t h2 = hash32(h);
  const float span_x = static_cast<float>(right - left) - 2.0f * (rx + kShell + 1);
  const float span_y = static_cast<float>(bottom - top) - 2.0f * (ry + kShell + 1);
  const float cx = static_cast<float>(left) + rx + kShell + 1 + span_x * unit(h2);
  const float cy = static_cast<float>(top) + ry + kShell + 1 + span_y * unit(h2 >> 16);

  // Keep pools well below the deepest possible ground and soil, and clear of the bottom.
  const float k = static_cast<float>(height_) / 1536.0f;
  if (cy - ry < static_cast<float>(height_) * p_.surface + 150.0f * k) return out;
  if (cy + ry + kShell >= static_cast<float>(height_ - p_.bedrock_rows)) return out;

  const float dx = static_cast<float>(x) - cx, dy = static_cast<float>(y) - cy;
  if (std::fabs(dx) > rx + kShell + 1 || std::fabs(dy) > ry + kShell + 1) return out;
  // Ragged outline: wobble the radius a little.
  const float wobble =
      1.0f + 0.12f * fbm(sub_seed(seed_, kPools + 1), width_, static_cast<float>(x), static_cast<float>(y), 9.0f, 1);
  const float ex = dx / (rx * wobble), ey = dy / (ry * wobble);
  const float d = std::sqrt(ex * ex + ey * ey);
  const float shell = 1.0f + static_cast<float>(kShell) / std::min(rx, ry);

  const float depth = cy / static_cast<float>(height_);
  if (depth >= p_.lava_depth) {
    // A sealed magma chamber: filled to the top and wrapped all round in
    // insulating basalt. Exposed lava would keep heating the air and rock
    // around it forever; sealed, the chamber sleeps until someone digs in.
    out.liquid = ids_.lava;
    if (d < 1.0f) out.kind = Bowl::Liquid;
    else if (d < shell) out.kind = Bowl::Shell;
    return out;
  }
  out.liquid = depth >= p_.oil_depth && ((h >> 24) & 0xFF) < 110 ? ids_.oil : ids_.water;
  const float level = cy + ry * (-0.35f + 0.6f * unit(h2 >> 8));  // liquid fills from here down
  if (d < 1.0f) {
    out.kind = static_cast<float>(y) >= level ? Bowl::Liquid : Bowl::Air;
  } else if (static_cast<float>(y) >= level - 1.0f && d < shell) {
    out.kind = Bowl::Shell;  // containment: caves may not breach the bowl below its liquid level
  }
  return out;
}

// ------------------------------------------------------------------ cells

MaterialId WorldGenerator::underground(int x, int y, int depth, const Column& col) const {
  const int bottom = height_ - p_.bedrock_rows;
  if (y >= bottom) return ids_.stone;
  const float fx = static_cast<float>(x), fy = static_cast<float>(y);
  const float t = fy / static_cast<float>(height_);
  const int rock_depth = depth - col.soil;  // rows into the stone

  // Pools: a coarse grid where some cells hold a "bowl", an oval cavity filled
  // with liquid up to a flat level. Below that level a stone shell overrides the
  // caves, so the liquid is always contained and starts out at rest. Caves may
  // still cut into the air above it, which is how the pools are reached.
  if (const Bowl bowl = bowl_at(x, y); bowl.kind != Bowl::None) {
    if (bowl.kind == Bowl::Shell) return bowl.liquid == ids_.lava ? ids_.basalt : ids_.stone;
    return bowl.kind == Bowl::Liquid ? bowl.liquid : kEmpty;
  }

  // Caves: two layers of winding tunnels (thin bands where noise crosses zero)
  // plus large caverns (blobs where noise is high). Both grow with depth and
  // close up approaching the bottom.
  const float to_bottom = std::min(1.0f, static_cast<float>(bottom - y) / 40.0f);
  // Large-scale cave density: some regions are riddled with tunnels, others
  // are nearly solid rock.
  const float density =
      0.2f + 0.8f * smooth(-0.35f, 0.3f, fbm(sub_seed(seed_, kCaveRegion), width_, fx, fy, 450.0f, 2));
  bool cave = false;
  const bool tunnels_here = rock_depth >= static_cast<int>(p_.cave_start) || col.entrance;
  if (tunnels_here) {
    float width = (0.022f + 0.03f * t) * to_bottom * density;
    if (rock_depth < static_cast<int>(p_.cave_start)) width *= 0.7f;  // entrance shafts are narrower
    if (width < 0.016f) width = 0.0f;  // tunnels end rather than thinning to hairlines
    const float a = fbm(sub_seed(seed_, kTunnelA), width_, fx, fy, 120.0f, 2, 1.7f);
    cave = std::fabs(a) < width;
    if (!cave && rock_depth >= static_cast<int>(p_.cave_start)) {
      const float b = fbm(sub_seed(seed_, kTunnelB), width_, fx, fy, 170.0f, 2, 2.2f);
      cave = std::fabs(b) < width * 0.8f;
    }
  }
  if (!cave && rock_depth >= static_cast<int>(p_.cave_start * 2.0f)) {
    const float threshold = 0.46f - 0.16f * t + (1.0f - to_bottom) * 0.5f + (1.0f - density) * 0.3f;
    cave = fbm(sub_seed(seed_, kCavern), width_, fx, fy, 200.0f, 3, 1.3f) > threshold;
  }
  if (cave) return kEmpty;

  // Deposits in solid rock.
  if (t > 0.3f && t < 0.75f && fbm(sub_seed(seed_, kCoal), width_, fx, fy, 46.0f, 2) > 0.48f) return ids_.coal;
  if (t < 0.6f && fbm(sub_seed(seed_, kSandPocket), width_, fx, fy, 70.0f, 2) > 0.52f) return ids_.sand;
  if (rock_depth < 140 && fbm(sub_seed(seed_, kDirtPocket), width_, fx, fy, 50.0f, 2) > 0.45f) return ids_.dirt;
  return ids_.stone;
}

MaterialId WorldGenerator::cell(int x, int y, const Column& col) const {
  if (y < col.surface) {
    if (y >= sea_y_) return ids_.water;  // a lake in a valley below sea level
    if (y < col.surface - 60) return kEmpty;  // well above any tree
    return tree_cell(x, y);
  }
  const int depth = y - col.surface;
  if (depth < col.soil) {
    if (col.entrance && depth >= 2) {
      // Let entrance shafts cut through the soil too.
      const MaterialId below = underground(x, y, depth, col);
      if (below == kEmpty) return kEmpty;
    }
    if (col.biome == Biome::Desert) return ids_.sand;
    if (col.grass && (depth == 0 || (depth == 1 && (cell_hash(seed_, x, y, 3) & 1U)))) return ids_.grass;
    return ids_.dirt;
  }
  return underground(x, y, depth, col);
}

MaterialId WorldGenerator::material_at(int x, int y) const {
  if (y < 0 || y >= height_) return kOutOfBounds;
  x = mod(x, width_);
  return cell(x, y, column(x));
}

// ------------------------------------------------------------------ chunks

void WorldGenerator::generate_chunk(World& world, int cx, int cy) const {
  const MaterialRegistry& reg = world.materials();
  Chunk& c = world.chunk(cx, cy);
  c = Chunk{};
  const int x0 = cx << kChunkShift, y0 = cy << kChunkShift;
  Column cols[kChunkSize];
  for (int lx = 0; lx < kChunkSize; ++lx) cols[lx] = column(x0 + lx);
  for (int ly = 0; ly < kChunkSize; ++ly) {
    const int y = y0 + ly;
    for (int lx = 0; lx < kChunkSize; ++lx) {
      const int x = x0 + lx;
      const MaterialId m = cell(x, y, cols[lx]);
      const MaterialProps& p = reg.props(m);
      const int i = Chunk::index(lx, ly);
      c.material[i] = m;
      c.set_temperature(i, p.spawn_temp);
      c.set_shade(i, cell_hash(world.seed(), x, y, 0) & 0x0F);  // as World::set
      // Wake only what can move or change. Settled liquid and sand go back to
      // sleep after one step; solid rock and air never cost anything.
      if (p.phase == Phase::Powder || p.phase == Phase::Liquid || p.phase == Phase::Gas || p.active_at_ambient ||
          p.spawn_temp != kAmbient) {
        c.next_dirty.include(lx, ly);
      }
    }
  }
}

void WorldGenerator::generate(World& world, int threads) const {
  if (world.width() != width_ || world.height() != height_) {
    throw std::invalid_argument("WorldGenerator::generate: world size does not match the generator");
  }
  const int total = world.width_chunks() * world.height_chunks();
  if (threads <= 0) threads = static_cast<int>(std::max(1U, std::thread::hardware_concurrency()));
  threads = std::min(threads, total);
  std::atomic<int> next{0};
  auto work = [&] {
    for (int i = next.fetch_add(1); i < total; i = next.fetch_add(1)) {
      generate_chunk(world, i % world.width_chunks(), i / world.width_chunks());
    }
  };
  std::vector<std::thread> pool;
  for (int t = 1; t < threads; ++t) pool.emplace_back(work);
  work();
  for (std::thread& t : pool) t.join();
}

void generate_world(World& world, const BaseMaterials& ids, WorldGenParams params) {
  WorldGenerator(ids, world.seed(), world.width(), world.height(), params).generate(world);
}

}  // namespace px
