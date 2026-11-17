#include "core/simulator.hpp"

#include <algorithm>
#include <utility>

#include "core/rng.hpp"

namespace px {
namespace {

constexpr int kMaxFallSpeed = 8;  // velocity units; fall distance per step is 1 + v / 3
constexpr float kHeatRate = 0.125f;  // fraction of a temperature gap closed per neighbour pair per step
constexpr int kMinTemp = -273;
constexpr int kMaxTemp = 30000;

struct StepContext {
  World& world;
  const MaterialRegistry& materials;
  std::uint32_t tick;
  std::uint8_t clock;  // 1..255, never 0, so fresh cells (clock 0) never look "already updated"
  StepStats& stats;
};

/// Per-cell random stream seeded from (seed, x, y, tick): order-independent.
/// The seed hash is computed lazily: most cell updates (settled sand, still
/// water) never draw a random number, and hashing every visited cell costs ~25%.
struct Rng {
  std::uint64_t seed;
  int x, y;
  std::uint32_t tick;
  std::uint32_t state = 0;
  bool seeded = false;
  std::uint32_t next() {
    if (!seeded) {
      state = cell_hash(seed, x, y, tick);
      seeded = true;
    }
    return state = hash32(state + 0x9E3779B9U);
  }
  bool chance(std::uint16_t p) { return p == 0xFFFF || (next() & 0xFFFF) < p; }
  int direction() { return (next() & 1U) ? 1 : -1; }
};

bool is_gas(const MaterialRegistry& m, MaterialId id) { return id != kOutOfBounds && m.phase(id) == Phase::Gas; }

/// Free space for falling: empty or any gas.
bool is_open(const MaterialRegistry& m, MaterialId id) { return id == kEmpty || is_gas(m, id); }

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
  std::swap(a.temperature[ia], b.temperature[ib]);
  std::swap(a.life[ia], b.life[ib]);
  std::swap(a.velocity_y[ia], b.velocity_y[ib]);
  std::swap(a.velocity_x[ia], b.velocity_x[ib]);
  std::swap(a.shade[ia], b.shade[ib]);
  a.clock[ia] = ctx.clock;
  b.clock[ib] = ctx.clock;
  w.wake(x0, y0);
  w.wake(x1, y1);
  ++ctx.stats.cells_moved;
}

/// Turns the cell at (x, y) into `to` in place (burning, boiling, reactions,
/// decay). Keeps its temperature unless `to` has a spawn temperature.
/// `life` overrides the new material's own lifetime when non-zero.
void transform(StepContext& ctx, int x, int y, MaterialId to, std::uint16_t life, Rng& rng) {
  Chunk& c = ctx.world.chunk_for(x, y);
  const int i = World::local_index(x, y);
  const MaterialProps& p = ctx.materials.props(to);
  c.material[i] = to;
  if (p.has_spawn_temp) c.temperature[i] = p.spawn_temp;
  if (life == 0 && p.lifetime_max > 0) {
    life = static_cast<std::uint16_t>(p.lifetime_min + rng.next() % (p.lifetime_max - p.lifetime_min + 1U));
  }
  c.life[i] = p.lifetime_max > 0 || life > 0 ? life : 0;
  c.velocity_y[i] = 0;
  c.velocity_x[i] = 0;
  c.clock[i] = ctx.clock;
  ctx.world.wake(x, y);
}

// ------------------------------------------------------------------ heat

/// Exchanges heat with the 4 neighbours, conserving the total. Each pair closes
/// a fraction of its temperature gap scaled by the two conductivities.
/// Whenever a temperature changes, the 3x3 around that cell is woken for this
/// and the next step: a sleeping neighbour may now have a gap large enough to
/// exchange heat. Waking only the cell itself lets chunked simulation drift
/// from updating every cell. Returns true if heat flowed.
bool exchange_heat(StepContext& ctx, int x, int y, float conductivity) {
  World& w = ctx.world;
  Chunk& c = w.chunk_for(x, y);
  const int i = World::local_index(x, y);
  int t = c.temperature[i];
  bool flowed = false;
  static constexpr int kDx[4] = {1, -1, 0, 0};
  static constexpr int kDy[4] = {0, 0, 1, -1};
  for (int k = 0; k < 4; ++k) {
    const int nx = x + kDx[k], ny = y + kDy[k];
    if (!w.in_bounds(nx, ny)) continue;
    Chunk& nc = w.chunk_for(nx, ny);
    const int ni = World::local_index(nx, ny);
    const int tn = nc.temperature[ni];
    if (tn == t) continue;
    const float kc = (conductivity + ctx.materials.props(nc.material[ni]).conductivity) * 0.5f * kHeatRate;
    int delta = static_cast<int>(static_cast<float>(tn - t) * kc);  // truncates toward zero
    // Let small gaps still close by a degree, otherwise cells stall a few
    // degrees off their neighbours forever. A gap of 1 is left alone (moving
    // it would just swap the two values back and forth).
    if (delta == 0 && (tn - t >= 2 || tn - t <= -2)) delta = tn > t ? 1 : -1;
    if (delta == 0) continue;
    t += delta;
    nc.temperature[ni] = static_cast<Temperature>(std::clamp(tn - delta, kMinTemp, kMaxTemp));
    w.wake(nx, ny);
    flowed = true;
  }
  c.temperature[i] = static_cast<Temperature>(std::clamp(t, kMinTemp, kMaxTemp));
  if (flowed) w.wake(x, y);
  return flowed;
}

/// Air that is hotter or colder than ambient: shares heat and drifts back to ambient.
void update_air(StepContext& ctx, int x, int y) {
  Chunk& c = ctx.world.chunk_for(x, y);
  const int i = World::local_index(x, y);
  exchange_heat(ctx, x, y, ctx.materials.props(kEmpty).conductivity);
  const int t = c.temperature[i];
  int relax = (kAmbient - t) / 8;
  if (relax == 0) relax = (t > kAmbient) ? -1 : (t < kAmbient ? 1 : 0);
  c.temperature[i] = static_cast<Temperature>(t + relax);
  if (relax != 0) ctx.world.wake(x, y);
}

// ------------------------------------------------------------------ movement

void set_velocity(World& w, int x, int y, int v) {
  w.chunk_for(x, y).velocity_y[World::local_index(x, y)] = static_cast<std::int8_t>(v);
}

void set_flow(World& w, int x, int y, int d) {
  w.chunk_for(x, y).velocity_x[World::local_index(x, y)] = static_cast<std::int8_t>(d);
}

/// Falls straight down with gravity acceleration. Returns the final y.
/// Free fall through empty space or gas is a single swap to the landing cell;
/// sinking into a lighter liquid moves only one cell per step.
int fall(StepContext& ctx, int x, int y, MaterialId m, int velocity) {
  int v = std::min(velocity + 1, kMaxFallSpeed);
  const int max_cells = 1 + v / 3;
  int dist = 0;
  MaterialId below = ctx.world.get(x, y + 1);
  while (dist < max_cells && is_open(ctx.materials, below) && can_displace(ctx.materials, m, below)) {
    ++dist;
    if (dist < max_cells) below = ctx.world.get(x, y + dist + 1);
  }
  if (dist == 0 && can_displace(ctx.materials, m, below)) {
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

/// Tries down-left / down-right, `first` direction first. Returns the direction
/// taken, or 0 if neither was free.
int slide_diagonal(StepContext& ctx, int x, int y, MaterialId m, int first) {
  for (int d : {first, -first}) {
    if (can_displace(ctx.materials, m, ctx.world.get(x + d, y + 1))) {
      swap_cells(ctx, x, y, x + d, y + 1);
      return d;
    }
  }
  return 0;
}

bool update_powder(StepContext& ctx, int x, int y, MaterialId m, int velocity, Rng& rng) {
  if (fall(ctx, x, y, m, velocity) != y) return true;
  return slide_diagonal(ctx, x, y, m, rng.direction()) != 0;
}

/// Liquids remember the horizontal direction they last flowed in (velocity_x)
/// and keep going that way until blocked, like a current. Choosing a random
/// side every step instead turns flow into a random walk: water creeps and
/// jitters rather than rushing out and levelling.
bool update_liquid(StepContext& ctx, int x, int y, MaterialId m, int velocity, Rng& rng) {
  const int prev = ctx.world.chunk_for(x, y).velocity_x[World::local_index(x, y)];
  const int first = prev != 0 ? prev : rng.direction();

  if (fall(ctx, x, y, m, velocity) != y) return true;  // velocity_x travelled with the cell
  if (const int d = slide_diagonal(ctx, x, y, m, first); d != 0) {
    set_flow(ctx.world, x + d, y + 1, d);
    return true;
  }

  // Flow sideways up to `dispersion` cells, stopping early over a drop so the
  // liquid pours into holes instead of skipping across them.
  const int dispersion = ctx.materials.dispersion(m);
  for (int d : {first, -first}) {
    int reach = 0;
    for (int k = 1; k <= dispersion; ++k) {
      const int nx = x + d * k;
      const MaterialId target = ctx.world.get(nx, y);
      if (!can_displace(ctx.materials, m, target)) break;
      reach = k;
      if (ctx.materials.phase(target) == Phase::Liquid) break;
      if (can_displace(ctx.materials, m, ctx.world.get(nx, y + 1))) break;
    }
    if (reach > 0) {
      swap_cells(ctx, x, y, x + d * reach, y);
      set_flow(ctx.world, x + d * reach, y, d);
      return true;
    }
  }
  set_flow(ctx.world, x, y, 0);  // boxed in: forget the direction
  return false;
}

/// Gases rise (with their rise chance), bubble up through liquids, sort by
/// density against other gases, and drift sideways.
bool update_gas(StepContext& ctx, int x, int y, MaterialId m, const MaterialProps& p, Rng& rng) {
  const MaterialRegistry& mats = ctx.materials;
  if (rng.chance(p.rise_chance)) {
    const MaterialId up = ctx.world.get(x, y - 1);
    if (up == kEmpty) {
      swap_cells(ctx, x, y, x, y - 1);
      return true;
    }
    if (up != kOutOfBounds) {
      const Phase up_phase = mats.phase(up);
      const bool bubble = up_phase == Phase::Liquid && (rng.next() & 1U);
      const bool lighter = up_phase == Phase::Gas && mats.density(up) > p.density;
      if (bubble || lighter) {
        swap_cells(ctx, x, y, x, y - 1);
        return true;
      }
    }
    const int first = rng.direction();
    for (int d : {first, -first}) {
      if (ctx.world.get(x + d, y - 1) == kEmpty) {
        swap_cells(ctx, x, y, x + d, y - 1);
        return true;
      }
    }
  }
  if (p.dispersion > 0 && (rng.next() & 1U)) {
    const int d = rng.direction();
    int reach = 0;
    for (int k = 1; k <= p.dispersion && ctx.world.get(x + d * k, y) == kEmpty; ++k) reach = k;
    if (reach > 0) {
      swap_cells(ctx, x, y, x + d * reach, y);
      return true;
    }
  }
  (void)m;
  return false;
}

// ------------------------------------------------------------------ cell update

/// Tries pair reactions with the 4 neighbours (random order). Returns
/// 1 if a reaction happened, 0 if a partner exists but did not react this
/// step, -1 if there is no partner.
int react(StepContext& ctx, int x, int y, MaterialId m, Rng& rng) {
  static constexpr int kDx[4] = {1, 0, -1, 0};
  static constexpr int kDy[4] = {0, 1, 0, -1};
  const int start = static_cast<int>(rng.next() & 3U);
  int result = -1;
  for (int k = 0; k < 4; ++k) {
    const int nx = x + kDx[(start + k) & 3], ny = y + kDy[(start + k) & 3];
    const MaterialId n = ctx.world.get(nx, ny);
    if (n == kOutOfBounds) continue;
    const Reaction* r = ctx.materials.reaction(m, n);
    if (!r) continue;
    result = 0;
    if (rng.chance(r->chance)) {
      transform(ctx, x, y, r->a_to, 0, rng);
      transform(ctx, nx, ny, r->b_to, 0, rng);
      return 1;
    }
  }
  return result;
}

enum class StateResult { Unchanged, KeepAwake, Transformed };

/// Heat, reactions, temperature transitions, lifetime, emission and flicker.
/// Kept out of line so the common path in update_cell (plain sand or water at
/// ambient temperature) stays small: inlining this roughly doubled its cost.
[[gnu::noinline]] StateResult update_state(StepContext& ctx, int x, int y, MaterialId m, const MaterialProps& p,
                                          Rng& rng) {
  Chunk& c = ctx.world.chunk_for(x, y);
  const int i = World::local_index(x, y);
  bool keep_awake = false;

  // Heat: sources hold their temperature; anything off ambient shares heat.
  if (p.emits_heat) {
    if (c.temperature[i] != p.spawn_temp) {
      c.temperature[i] = p.spawn_temp;
      ctx.world.wake(x, y);
    }
    keep_awake = true;
  }
  if (c.temperature[i] != kAmbient && exchange_heat(ctx, x, y, p.conductivity)) keep_awake = true;

  // Contact reactions.
  if (p.has_reactions) {
    const int r = react(ctx, x, y, m, rng);
    if (r == 1) return StateResult::Transformed;
    if (r == 0) keep_awake = true;
  }

  // Temperature transitions (burning, boiling, condensing, melting, freezing).
  const Temperature t = c.temperature[i];
  if (p.has_high && t >= p.high_temp) {
    if (rng.chance(p.high_chance)) {
      std::uint16_t life = 0;
      if (p.burn_time > 0) life = static_cast<std::uint16_t>(p.burn_time * 3 / 4 + rng.next() % (p.burn_time / 2 + 1U));
      transform(ctx, x, y, p.high_to, life, rng);
      return StateResult::Transformed;
    }
    keep_awake = true;
  }
  if (p.has_low && t <= p.low_temp) {
    if (rng.chance(p.low_chance)) {
      transform(ctx, x, y, p.low_to, 0, rng);
      return StateResult::Transformed;
    }
    keep_awake = true;
  }

  // Limited lifetime.
  if (c.life[i] > 0) {
    if (--c.life[i] == 0) {
      transform(ctx, x, y, rng.chance(p.decay_chance) ? p.decay_to : kEmpty, 0, rng);
      return StateResult::Transformed;
    }
    keep_awake = true;
  }

  // Emission into an empty neighbour: mostly upward, sometimes sideways
  // (fire throws flames that lick at nearby fuel).
  if (p.emit_chance > 0) {
    keep_awake = true;
    if (rng.chance(p.emit_chance)) {
      const std::uint32_t r = rng.next() & 3U;
      const int ex = r == 2 ? x - 1 : (r == 3 ? x + 1 : x);
      const int ey = r < 2 ? y - 1 : y;
      if (ctx.world.get(ex, ey) == kEmpty) {
        transform(ctx, ex, ey, p.emits, 0, rng);
        ctx.world.chunk_for(ex, ey).clock[World::local_index(ex, ey)] = 0;  // may move this step
      }
    }
  }

  if (p.flicker) {
    c.shade[i] = static_cast<std::uint8_t>(rng.next() & 0x0F);
    keep_awake = true;
  }
  return keep_awake ? StateResult::KeepAwake : StateResult::Unchanged;
}

void update_cell(StepContext& ctx, int x, int y) {
  Chunk& c = ctx.world.chunk_for(x, y);
  const int i = World::local_index(x, y);
  if (c.clock[i] == ctx.clock) return;
  const MaterialId m = c.material[i];
  if (m == kEmpty) {
    if (c.temperature[i] != kAmbient) {
      c.clock[i] = ctx.clock;
      update_air(ctx, x, y);
    }
    return;
  }
  c.clock[i] = ctx.clock;

  const MaterialProps& p = ctx.materials.props(m);
  Rng rng{ctx.world.seed(), x, y, ctx.tick};
  bool keep_awake = false;
  if (p.active_at_ambient || c.temperature[i] != kAmbient || c.life[i] != 0) {
    const StateResult r = update_state(ctx, x, y, m, p, rng);
    if (r == StateResult::Transformed) return;
    keep_awake = r == StateResult::KeepAwake;
  }

  bool moved = false;
  switch (p.phase) {
    case Phase::Powder:
      moved = update_powder(ctx, x, y, m, c.velocity_y[i], rng);
      break;
    case Phase::Liquid:
      moved = update_liquid(ctx, x, y, m, c.velocity_y[i], rng);
      break;
    case Phase::Gas:
      moved = update_gas(ctx, x, y, m, p, rng);
      if (!moved) keep_awake = true;  // gases keep drifting
      break;
    case Phase::Empty:
    case Phase::Solid:
      break;
  }
  if (!moved && keep_awake) ctx.world.keep_awake(x, y);
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

  // Scan whole world rows, bottom to top, visiting only the dirty span of each
  // chunk in that row. Scanning chunk-by-chunk instead (all rows of one chunk,
  // then the next) makes material behave differently at chunk edges: liquids
  // pile up into walls along the 64-pixel grid.
  const bool left_to_right = (tick & 1U) != 0;
  for (int cy = hc - 1; cy >= 0; --cy) {
    for (int cx = 0; cx < wc; ++cx) {
      const DirtyRect& rect = world.chunk(cx, cy).dirty;
      if (rect.empty()) continue;
      ++stats.active_chunks;
      stats.cells_scanned += rect.area();
    }
    const int base_y = cy << kChunkShift;
    for (int ly = kChunkMask; ly >= 0; --ly) {
      for (int i = 0; i < wc; ++i) {
        const int cx = left_to_right ? i : wc - 1 - i;
        const DirtyRect& rect = world.chunk(cx, cy).dirty;
        if (ly < rect.min_y || ly > rect.max_y) continue;  // also skips empty rects
        const int base_x = cx << kChunkShift;
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
