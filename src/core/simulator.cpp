#include "core/simulator.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "core/rng.hpp"

namespace px {
namespace {

constexpr int kMaxFallSpeed = 8;  // velocity units; fall distance per step is 1 + v / 3
// A liquid whose sideways flow has reversed about 2 * kSettleCount times without
// it dropping is "settled": it stops wandering across a flat surface (see
// update_liquid). The count is stored as a negative fall speed, so it must fit
// in an int8; each reversal counts with probability 1/2 to double its reach.
constexpr int kSettleCount = 120;
// Fraction of a temperature gap closed per neighbour pair per exchange. Heat is
// exchanged on a checkerboard (see heat_turn), so each pair exchanges once per
// step; exchanging from both sides every step cost twice as much for the same
// diffusion.
constexpr float kHeatRate = 0.25f;
struct StepContext {
  World& world;
  MaterialRegistry& materials;  // reactions may add derived materials
  std::uint32_t tick;
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
  std::swap(a.thermal[ia], b.thermal[ib]);  // temperature and shade travel together
  std::swap(a.motion[ia], b.motion[ib]);    // fall speed / flow, or lifetime
  std::swap(a.health[ia], b.health[ib]);
  a.mark_updated(ia, ctx.tick);
  b.mark_updated(ib, ctx.tick);
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
  if (p.has_spawn_temp) c.set_temperature(i, p.spawn_temp);
  if (life == 0 && p.lifetime_max > 0) {
    life = static_cast<std::uint16_t>(p.lifetime_min + rng.next() % (p.lifetime_max - p.lifetime_min + 1U));
  }
  // At rest, or with its new lifetime (never both: see MaterialProps::uses_motion).
  c.motion[i] = p.uses_motion ? 0 : life;
  c.health[i] = Chunk::kFullHealth;  // a new material starts intact
  c.mark_updated(i, ctx.tick);
  ctx.world.wake(x, y);
}

// ------------------------------------------------------------------ heat

/// Exchanges heat with the 4 neighbours, conserving the total. Each pair closes
/// a fraction of its temperature gap scaled by the two conductivities.
/// Whenever a temperature changes, the 3x3 around that cell is woken for this
/// and the next step: a sleeping neighbour may now have a gap large enough to
/// exchange heat. Waking only the cell itself lets chunked simulation drift
/// from updating every cell. Returns true if heat flowed.
///
/// A material with conductivity 0 is a perfect insulator: no heat crosses into
/// or out of it (basalt sealing a buried lava chamber).
/// Cells exchange heat on alternating checkerboard squares: every neighbour
/// pair has one cell of each colour, so each pair exchanges exactly once per
/// step. World widths are whole chunks (even), so the pattern also holds
/// across the seam of a wrapping world.
bool heat_turn(const StepContext& ctx, int x, int y) {
  return ((static_cast<unsigned>(x) + static_cast<unsigned>(y) + ctx.tick) & 1U) == 0;
}

/// With Apply = false, only reports whether any heat would flow, changing nothing.
template <bool Apply = true>
bool exchange_heat(StepContext& ctx, int x, int y, float conductivity) {
  if (conductivity <= 0.0f) return false;
  World& w = ctx.world;
  Chunk& c = w.chunk_for(x, y);
  const int lx = x & kChunkMask, ly = y & kChunkMask;
  const int i = Chunk::index(lx, ly);
  int t = c.temperature(i);
  bool flowed = false;

  auto pair = [&](Chunk& nc, int ni) {
    const int tn = nc.temperature(ni);
    if (tn == t) return;
    const float nk = ctx.materials.props(nc.material[ni]).conductivity;
    if (nk <= 0.0f) return;  // insulator
    const float kc = (conductivity + nk) * 0.5f * kHeatRate;
    int delta = static_cast<int>(static_cast<float>(tn - t) * kc);  // truncates toward zero
    // Let small gaps still close by a degree, otherwise cells stall a few
    // degrees off their neighbours forever. A gap of 1 is left alone (moving
    // it would just swap the two values back and forth).
    if (delta == 0 && (tn - t >= 2 || tn - t <= -2)) delta = tn > t ? 1 : -1;
    if (delta == 0) return;
    flowed = true;
    if constexpr (!Apply) return;
    t += delta;
    nc.set_temperature(ni, tn - delta);  // clamps
  };

  // Neighbour order is fixed (+x, -x, +y, -y) so both paths give identical results.
  if (lx > 0 && lx < kChunkMask && ly > 0 && ly < kChunkMask) {
    // All four neighbours in this chunk: plain index offsets, no coordinate
    // wrapping or chunk lookups (~95% of cells).
    if constexpr (Apply) {
      pair(c, i + 1);
      pair(c, i - 1);
      pair(c, i + kChunkSize);
      pair(c, i - kChunkSize);
    } else {  // check only: stop at the first pair that would flow
      for (const int ni : {i + 1, i - 1, i + kChunkSize, i - kChunkSize}) {
        pair(c, ni);
        if (flowed) return true;
      }
    }
  } else {
    static constexpr int kDx[4] = {1, -1, 0, 0};
    static constexpr int kDy[4] = {0, 0, 1, -1};
    for (int k = 0; k < 4; ++k) {
      const int nx = x + kDx[k], ny = y + kDy[k];
      if (!w.in_bounds(nx, ny)) continue;
      pair(w.chunk_for(nx, ny), World::local_index(nx, ny));
      if constexpr (!Apply) {
        if (flowed) return true;
      }
    }
  }
  if constexpr (!Apply) return flowed;
  c.set_temperature(i, t);
  // One 5x5 wake covers the 3x3 around this cell and around every neighbour
  // whose temperature changed; waking each separately cost up to 5 wakes per
  // cell and was the largest single cost in hot scenes.
  if (flowed) w.wake_area(x, y, 2);
  return flowed;
}

/// Air that is hotter or colder than ambient: shares heat and drifts back to ambient.
void update_air(StepContext& ctx, int x, int y) {
  Chunk& c = ctx.world.chunk_for(x, y);
  const int i = World::local_index(x, y);
  const bool flowed = heat_turn(ctx, x, y) && exchange_heat(ctx, x, y, ctx.materials.props(kEmpty).conductivity);
  const int t = c.temperature(i);
  int relax = (kAmbient - t) / 8;
  if (relax == 0) relax = (t > kAmbient) ? -1 : (t < kAmbient ? 1 : 0);
  c.set_temperature(i, t + relax);
  if (relax != 0 && !flowed) ctx.world.wake(x, y);  // a heat flow already woke the 5x5
}

// ------------------------------------------------------------------ movement

void set_velocity(World& w, int x, int y, int v) { w.chunk_for(x, y).set_fall_speed(World::local_index(x, y), v); }

void set_flow(World& w, int x, int y, int d) { w.chunk_for(x, y).set_flow(World::local_index(x, y), d); }

/// Falls straight down with gravity acceleration. Returns the final y.
/// Free fall through empty space or gas is a single swap to the landing cell;
/// sinking into a lighter liquid moves only one cell per step.
int fall(StepContext& ctx, int x, int y, MaterialId m, int velocity) {
  int v = std::min(std::max(velocity, 0) + 1, kMaxFallSpeed);
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
    if (velocity > 0) set_velocity(ctx.world, x, y, 0);  // negative = a resting liquid's settle count; keep it
    return y;
  }
  swap_cells(ctx, x, y, x, y + dist);
  set_velocity(ctx.world, x, y + dist, v);
  return y + dist;
}

/// Whether wind moves windblown cells at row y this step: +1 or -1 (the
/// direction), or 0. A cell blown at w cells/step moves one cell on a fraction
/// of steps, the same steps for every cell, and only on steps whose scan runs
/// from the downwind edge (odd ticks scan left to right): each cell then moves
/// into the space its downwind neighbour just left, so a cloud travels as one
/// instead of only its edge peeling off.
int wind_step(const StepContext& ctx, int y) {
  const float w = ctx.world.wind_at(y);
  if (w == 0.0f) return 0;
  const int dir = w > 0.0f ? 1 : -1;
  const bool left_to_right = (ctx.tick & 1U) != 0;
  if (left_to_right == (dir > 0)) return 0;
  const double rate = std::min(1.0, 2.0 * std::fabs(static_cast<double>(w)));
  const double k = static_cast<double>(ctx.tick >> 1);
  return std::floor(k * rate) != std::floor((k - 1.0) * rate) ? dir : 0;
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

/// Liquids remember the horizontal direction they last flowed in (Chunk::flow)
/// and keep going that way until blocked, like a current. Choosing a random
/// side every step instead turns flow into a random walk: water creeps and
/// jitters rather than rushing out and levelling.
///
/// Settling: a cell on top of a full liquid layer always has room beside it, so
/// a partial top layer would shuffle sideways forever and never sleep. Each time
/// a cell's sideways flow reverses without heading for a drop, it counts up (as a
/// negative fall speed); falling, sliding down or flowing towards a drop resets
/// it. Only reversals count: a cell crossing a long, slightly higher terrace
/// keeps its heading until it finds the edge, while one in a partial top layer
/// bounces back and forth. Counting every move instead freezes a lake as a gentle
/// staircase. Once settled, the cell only steps sideways into an adjacent drop or
/// lighter liquid, so a level surface goes still while a breached container
/// still drains.
bool update_liquid(StepContext& ctx, int x, int y, MaterialId m, int velocity, Rng& rng) {
  const int prev = ctx.world.chunk_for(x, y).flow(World::local_index(x, y));
  const int first = prev != 0 ? prev : rng.direction();

  if (const int ny = fall(ctx, x, y, m, velocity); ny != y) {  // the flow direction travelled with the cell
    // Falling through the air (rain), it drifts with the wind.
    if (ctx.materials.props(m).windblown) {
      const int d = wind_step(ctx, ny);
      if (d != 0 && ctx.world.get(x + d, ny) == kEmpty && is_open(ctx.materials, ctx.world.get(x + d, ny + 1))) {
        swap_cells(ctx, x, ny, x + d, ny);
      }
    }
    return true;
  }
  if (const int d = slide_diagonal(ctx, x, y, m, first); d != 0) {
    set_flow(ctx.world, x + d, y + 1, d);
    set_velocity(ctx.world, x + d, y + 1, 0);  // it went down: no longer settling
    return true;
  }

  // Flow sideways up to `dispersion` cells, stopping early over a drop so the
  // liquid pours into holes instead of skipping across them.
  const int settle = velocity < 0 ? -velocity : 0;
  const bool settled = settle >= kSettleCount;
  // A settled cell looks only at its direct neighbours: anything further out is
  // beyond the 3x3 wake, so a change there would not wake it, and chunked
  // simulation would drift from updating every cell.
  const int dispersion = settled ? 1 : ctx.materials.dispersion(m);
  for (int d : {first, -first}) {
    int reach = 0;
    bool purposeful = false;  // heading for a drop or under a lighter liquid
    for (int k = 1; k <= dispersion; ++k) {
      const int nx = x + d * k;
      const MaterialId target = ctx.world.get(nx, y);
      if (!can_displace(ctx.materials, m, target)) break;
      reach = k;
      if (ctx.materials.phase(target) == Phase::Liquid ||
          can_displace(ctx.materials, m, ctx.world.get(nx, y + 1))) {
        purposeful = true;
        break;
      }
    }
    if (reach > 0 && (purposeful || !settled)) {
      swap_cells(ctx, x, y, x + d * reach, y);
      set_flow(ctx.world, x + d * reach, y, d);
      int count = settle;
      if (purposeful) count = 0;
      else if (d != prev && (rng.next() & 1U)) count = std::min(settle + 1, kSettleCount);
      set_velocity(ctx.world, x + d * reach, y, -count);
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
  if (p.windblown) {
    const int d = wind_step(ctx, y);
    if (d != 0 && ctx.world.get(x + d, y) == kEmpty) {
      swap_cells(ctx, x, y, x + d, y);
      return true;
    }
  }
  // Cloud rises to the cloud layer and no higher; above it, it sinks back.
  const Weather& sky = ctx.world.weather();
  const int ceiling = p.hovers && sky.clouds ? ctx.world.cloud_ceiling(x) : 0;
  const bool capped = p.hovers && sky.clouds && y <= ceiling;
  if (p.hovers && sky.clouds && y < ceiling && ctx.world.get(x, y + 1) == kEmpty) {
    swap_cells(ctx, x, y, x, y + 1);
    return true;
  }
  if (!capped && rng.chance(p.rise_chance)) {
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
      // Products may be derived from both reactants (in their authored roles),
      // read before either cell changes.
      const MaterialId first = r->swapped ? n : m, second = r->swapped ? m : n;
      const MaterialId self_to = r->a_derive ? ctx.materials.derive(r->a_to, *r->a_derive, first, second) : r->a_to;
      const MaterialId other_to = r->b_derive ? ctx.materials.derive(r->b_to, *r->b_derive, first, second) : r->b_to;
      transform(ctx, x, y, self_to, 0, rng);
      transform(ctx, nx, ny, other_to, 0, rng);
      if (r->heat != 0) {  // released (or absorbed) by the reaction
        Chunk& c = ctx.world.chunk_for(x, y);
        Chunk& nc = ctx.world.chunk_for(nx, ny);
        const int i = World::local_index(x, y), ni = World::local_index(nx, ny);
        c.set_temperature(i, c.temperature(i) + r->heat);
        nc.set_temperature(ni, nc.temperature(ni) + r->heat);
      }
      return 1;
    }
  }
  return result;
}

enum class StateResult { Unchanged, KeepAwake, Transformed };

/// Heat, reactions, temperature transitions, lifetime and emission.
/// Kept out of line so the common path in update_cell (plain sand or water at
/// ambient temperature) stays small: inlining this roughly doubled its cost.
[[gnu::noinline]] StateResult update_state(StepContext& ctx, int x, int y, MaterialId m, const MaterialProps& p,
                                          Rng& rng) {
  Chunk& c = ctx.world.chunk_for(x, y);
  const int i = World::local_index(x, y);
  bool keep_awake = false;

  // Heat: sources hold their temperature; anything off ambient shares heat.
  // A source already at its temperature with no heat flowing has nothing to do
  // and may sleep: a neighbour cooling or changing wakes it again. Keeping every
  // source awake made a buried lava pool cost CPU forever.
  //
  // A source only sustains itself above its low-transition threshold: cooled
  // below it (a cooling brush, a lone drop surrounded by ice), it stops
  // re-heating and its low transition applies (lava hardens, fire goes out).
  // Ordinary conduction cannot pull a source that far between two updates.
  if (p.emits_heat && p.has_low && c.temperature(i) <= p.low_temp) {
    // Released: transform now, before hot neighbours can re-heat it this step.
    if (rng.chance(p.low_chance)) {
      transform(ctx, x, y, p.low_to, 0, rng);
      return StateResult::Transformed;
    }
    keep_awake = true;
  } else if (p.emits_heat && c.temperature(i) != p.spawn_temp) {
    c.set_temperature(i, p.spawn_temp);
    ctx.world.wake(x, y);
  }
  // A cell off ambient with heat still to exchange stays awake through its
  // off-turn so it gets its turn: otherwise an exchange with a neighbour that
  // never runs heat code itself (one at ambient) is skipped, and chunked
  // simulation drifts from updating every cell. Cells with nothing to exchange
  // (gaps of 1, insulated) may sleep.
  if (c.temperature(i) != kAmbient) {
    const bool flows = heat_turn(ctx, x, y) ? exchange_heat(ctx, x, y, p.conductivity)
                                            : exchange_heat<false>(ctx, x, y, p.conductivity);
    if (flows) keep_awake = true;
  }

  // Contact reactions.
  if (p.has_reactions) {
    const int r = react(ctx, x, y, m, rng);
    if (r == 1) return StateResult::Transformed;
    if (r == 0) keep_awake = true;
  }

  // Temperature transitions (burning, boiling, condensing, melting, freezing).
  const Temperature t = c.temperature(i);
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

  // Crowding (dense cloud condenses into rain).
  if (p.crowd_count > 0) {
    keep_awake = true;
    int same = 0;
    for (int dy = -1; dy <= 1; ++dy)
      for (int dx = -1; dx <= 1; ++dx) {
        if (dx == 0 && dy == 0) continue;
        const MaterialId n = ctx.world.get(x + dx, y + dy);
        same += n != kOutOfBounds && ctx.materials.props(n).base == p.base ? 1 : 0;
      }
    if (rng.chance(same >= p.crowd_count ? p.crowd_chance : p.lone_chance)) {
      transform(ctx, x, y, p.crowd_to, 0, rng);
      return StateResult::Transformed;
    }
  }

  // Limited lifetime.
  if (!p.uses_motion && c.life(i) > 0) {
    c.set_life(i, static_cast<std::uint16_t>(c.life(i) - 1));
    if (c.life(i) == 0) {
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
        ctx.world.chunk_for(ex, ey).clear_updated(World::local_index(ex, ey));  // may move this step
      }
    }
  }

  return keep_awake ? StateResult::KeepAwake : StateResult::Unchanged;
}

void update_cell(StepContext& ctx, int x, int y) {
  Chunk& c = ctx.world.chunk_for(x, y);
  const int i = World::local_index(x, y);
  if (c.was_updated(i, ctx.tick)) return;
  const MaterialId m = c.material[i];
  if (m == kEmpty) {
    if (c.temperature(i) != kAmbient) {
      c.mark_updated(i, ctx.tick);
      update_air(ctx, x, y);
    }
    return;
  }
  c.mark_updated(i, ctx.tick);

  const MaterialProps& p = ctx.materials.props(m);
  Rng rng{ctx.world.seed(), x, y, ctx.tick};
  bool keep_awake = false;
  if (p.active_at_ambient || c.temperature(i) != kAmbient || (!p.uses_motion && c.life(i) != 0)) {
    const StateResult r = update_state(ctx, x, y, m, p, rng);
    if (r == StateResult::Transformed) return;
    keep_awake = r == StateResult::KeepAwake;
  }

  bool moved = false;
  switch (p.phase) {
    case Phase::Powder:
      moved = update_powder(ctx, x, y, m, c.fall_speed(i), rng);
      break;
    case Phase::Liquid:
      moved = update_liquid(ctx, x, y, m, c.fall_speed(i), rng);
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

namespace {

StepStats step_impl(World& world, const ChunkRegion* region) {
  StepStats stats;
  world.advance_tick();
  const std::uint32_t tick = world.tick();
  StepContext ctx{world, world.materials(), tick, stats};

  const int wc = world.width_chunks();
  const int hc = world.height_chunks();

  // Which chunks run this step (all of them without a region).
  int rx0 = 0, rx1 = wc - 1, ry0 = 0, ry1 = hc - 1;
  bool every_column = true;
  if (region) {
    ry0 = std::max(region->cy0, 0);
    ry1 = std::min(region->cy1, hc - 1);
    rx0 = region->cx0;
    rx1 = region->cx1;
    if (!world.wraps_x()) {
      rx0 = std::max(rx0, 0);
      rx1 = std::min(rx1, wc - 1);
      every_column = false;
    } else {
      every_column = rx1 - rx0 + 1 >= wc;
    }
  }
  auto in_region = [&](int cx, int cy) {
    if (cy < ry0 || cy > ry1) return false;
    if (every_column) return true;
    if (!world.wraps_x()) return cx >= rx0 && cx <= rx1;
    return (((cx - rx0) % wc) + wc) % wc <= rx1 - rx0;  // distance from the region's left edge, around the ring
  };

  // Only loaded chunks: a step costs what is loaded and active, not the world's size.
  world.begin_step();
  for (const int slot : world.loaded_slots()) {
    const int cx = slot % wc, cy = slot / wc;
    Chunk& c = world.chunk(cx, cy);
    if (region && !in_region(cx, cy)) {
      // Paused: keep the pending work for when the region comes back.
      c.dirty.reset();
      stats.paused_chunks += c.next_dirty.empty() ? 0 : 1;
      continue;
    }
    if (!c.next_dirty.empty()) c.touched = true;  // its pending work is consumed now
    c.dirty = c.next_dirty;
    c.next_dirty.reset();
    if (!c.dirty.empty()) world.note_active(cx, cy);
  }

  // Scan whole world rows, bottom to top, visiting only the dirty span of each
  // chunk in that row. Scanning chunk-by-chunk instead (all rows of one chunk,
  // then the next) makes material behave differently at chunk edges: liquids
  // pile up into walls along the 64-pixel grid.
  //
  // Each chunk row keeps the columns with dirty cells; a wake-up during the
  // step that gives a chunk its first dirty cells adds it. Columns are visited
  // in scan order by looking up the next one past the last visited, so a chunk
  // added ahead of the scan is still reached this row and one added behind it
  // waits for the next, exactly as scanning every column would.
  const bool left_to_right = (tick & 1U) != 0;
  for (int cy = hc - 1; cy >= 0; --cy) {
    if (world.active_columns(cy).empty()) continue;
    for (const int cx : world.active_columns(cy)) {
      const DirtyRect& rect = world.chunk(cx, cy).dirty;
      ++stats.active_chunks;
      stats.cells_scanned += rect.area();
    }
    const int base_y = cy << kChunkShift;
    for (int ly = kChunkMask; ly >= 0; --ly) {
      int last = left_to_right ? -1 : wc;
      for (;;) {
        // The next column past `last` in scan order (the list is sorted).
        const std::vector<int>& cols = world.active_columns(cy);
        int cx;
        if (left_to_right) {
          const auto it = std::upper_bound(cols.begin(), cols.end(), last);
          if (it == cols.end()) break;
          cx = *it;
        } else {
          const auto it = std::lower_bound(cols.begin(), cols.end(), last);
          if (it == cols.begin()) break;
          cx = *(it - 1);
        }
        last = cx;
        const DirtyRect& rect = world.chunk(cx, cy).dirty;
        if (ly < rect.min_y || ly > rect.max_y) continue;
        const int base_x = cx << kChunkShift;
        if (left_to_right) {
          for (int lx = rect.min_x; lx <= rect.max_x; ++lx) update_cell(ctx, base_x + lx, base_y + ly);
        } else {
          for (int lx = rect.max_x; lx >= rect.min_x; --lx) update_cell(ctx, base_x + lx, base_y + ly);
        }
      }
    }
  }
  world.end_step();
  return stats;
}

}  // namespace

StepStats step(World& world) { return step_impl(world, nullptr); }

StepStats step(World& world, const ChunkRegion& region) { return step_impl(world, &region); }

}  // namespace px
