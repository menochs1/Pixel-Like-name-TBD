#include "core/simulator.hpp"

#include <algorithm>
#include <utility>

#include "core/rng.hpp"

namespace px {
namespace {

constexpr int kMaxFallSpeed = 8;  // velocity units; fall distance per step is 1 + v / 3

struct StepContext {
  World& world;
  const MaterialRegistry& materials;
  std::uint32_t tick;
  std::uint8_t clock;  // 1..255, never 0, so fresh cells (clock 0) never look "already updated"
  StepStats& stats;
};

/// Can `mover` move into a cell holding `target`? Empty cells always accept;
/// liquids and gases accept anything denser than themselves.
bool can_displace(const MaterialRegistry& materials, MaterialId mover, MaterialId target) {
  if (target == kEmpty) return true;
  if (target == kOutOfBounds) return false;
  const Phase p = materials.phase(target);
  if (p != Phase::Liquid && p != Phase::Gas) return false;
  return materials.density(target) < materials.density(mover);
}

/// Swaps two cells (all per-cell arrays) and wakes both neighbourhoods.
void swap_cells(StepContext& ctx, int x0, int y0, int x1, int y1) {
  World& w = ctx.world;
  Chunk& a = w.chunk_for(x0, y0);
  Chunk& b = w.chunk_for(x1, y1);
  const int ia = World::local_index(x0, y0);
  const int ib = World::local_index(x1, y1);
  std::swap(a.material[ia], b.material[ib]);
  std::swap(a.velocity_y[ia], b.velocity_y[ib]);
  std::swap(a.shade[ia], b.shade[ib]);
  a.clock[ia] = ctx.clock;
  b.clock[ib] = ctx.clock;
  w.wake(x0, y0);
  w.wake(x1, y1);
  ++ctx.stats.cells_moved;
}

void set_velocity(World& w, int x, int y, int v) {
  w.chunk_for(x, y).velocity_y[World::local_index(x, y)] = static_cast<std::int8_t>(v);
}

/// Falls straight down with gravity acceleration. Returns the final y.
/// Free fall through empty cells is done as a single swap to the landing cell;
/// sinking into a lighter fluid moves only one cell per step.
int fall(StepContext& ctx, int x, int y, MaterialId m, int velocity) {
  int v = std::min(velocity + 1, kMaxFallSpeed);
  const int max_cells = 1 + v / 3;
  int dist = 0;
  while (dist < max_cells && ctx.world.get(x, y + dist + 1) == kEmpty) ++dist;
  if (dist == 0 && can_displace(ctx.materials, m, ctx.world.get(x, y + 1))) {
    dist = 1;
    v = std::min(v, 2);
  }
  if (dist == 0) {
    set_velocity(ctx.world, x, y, 0);
    return y;
  }
  swap_cells(ctx, x, y, x, y + dist);
  set_velocity(ctx.world, x, y + dist, v);
  return y + dist;
}

/// Tries down-left / down-right in a random order.
bool slide_diagonal(StepContext& ctx, int x, int y, MaterialId m, std::uint32_t rnd) {
  const int first = (rnd & 1U) ? 1 : -1;
  for (int d : {first, -first}) {
    if (can_displace(ctx.materials, m, ctx.world.get(x + d, y + 1))) {
      swap_cells(ctx, x, y, x + d, y + 1);
      return true;
    }
  }
  return false;
}

void update_powder(StepContext& ctx, int x, int y, MaterialId m, int velocity) {
  if (fall(ctx, x, y, m, velocity) != y) return;
  slide_diagonal(ctx, x, y, m, cell_hash(ctx.world.seed(), x, y, ctx.tick));
}

void update_liquid(StepContext& ctx, int x, int y, MaterialId m, int velocity) {
  if (fall(ctx, x, y, m, velocity) != y) return;
  const std::uint32_t rnd = cell_hash(ctx.world.seed(), x, y, ctx.tick);
  if (slide_diagonal(ctx, x, y, m, rnd)) return;

  // Flow sideways up to `dispersion` cells, stopping early over a drop so the
  // liquid pours into holes instead of skipping across them.
  const int dispersion = ctx.materials.dispersion(m);
  const int first = (rnd & 2U) ? 1 : -1;
  for (int d : {first, -first}) {
    int reach = 0;
    for (int k = 1; k <= dispersion; ++k) {
      const int nx = x + d * k;
      const MaterialId target = ctx.world.get(nx, y);
      if (!can_displace(ctx.materials, m, target)) break;
      reach = k;
      if (target != kEmpty) break;
      if (can_displace(ctx.materials, m, ctx.world.get(nx, y + 1))) break;
    }
    if (reach > 0) {
      swap_cells(ctx, x, y, x + d * reach, y);
      return;
    }
  }
}

void update_cell(StepContext& ctx, int x, int y) {
  Chunk& c = ctx.world.chunk_for(x, y);
  const int i = World::local_index(x, y);
  const MaterialId m = c.material[i];
  if (m == kEmpty || c.clock[i] == ctx.clock) return;
  c.clock[i] = ctx.clock;

  switch (ctx.materials.phase(m)) {
    case Phase::Powder:
      update_powder(ctx, x, y, m, c.velocity_y[i]);
      break;
    case Phase::Liquid:
      update_liquid(ctx, x, y, m, c.velocity_y[i]);
      break;
    case Phase::Empty:
    case Phase::Solid:
    case Phase::Gas:  // gases arrive in roadmap step 2
      break;
  }
}

}  // namespace

StepStats step(World& world) {
  StepStats stats;
  world.advance_tick();
  const std::uint32_t tick = world.tick();
  StepContext ctx{world, world.materials(), tick, static_cast<std::uint8_t>(tick % 255U + 1U), stats};

  const int wc = world.width_chunks();
  const int hc = world.height_chunks();
  for (int cy = 0; cy < hc; ++cy) {
    for (int cx = 0; cx < wc; ++cx) {
      Chunk& c = world.chunk(cx, cy);
      c.dirty = c.next_dirty;
      c.next_dirty.reset();
    }
  }

  const bool left_to_right = (tick & 1U) != 0;
  for (int cy = hc - 1; cy >= 0; --cy) {
    for (int i = 0; i < wc; ++i) {
      const int cx = left_to_right ? i : wc - 1 - i;
      const DirtyRect rect = world.chunk(cx, cy).dirty;
      if (rect.empty()) continue;
      ++stats.active_chunks;
      stats.cells_scanned += rect.area();

      const int base_x = cx << kChunkShift;
      const int base_y = cy << kChunkShift;
      for (int ly = rect.max_y; ly >= rect.min_y; --ly) {
        if (left_to_right) {
          for (int lx = rect.min_x; lx <= rect.max_x; ++lx) update_cell(ctx, base_x + lx, base_y + ly);
        } else {
          for (int lx = rect.max_x; lx >= rect.min_x; --lx) update_cell(ctx, base_x + lx, base_y + ly);
        }
      }
    }
  }
  return stats;
}

}  // namespace px
