#include "core/world.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "core/rng.hpp"

namespace px {

World::World(int width_chunks, int height_chunks, MaterialRegistry& materials, std::uint64_t seed,
             EdgeX edge_x, ChunkStorage storage)
    : width_chunks_(width_chunks),
      height_chunks_(height_chunks),
      width_(width_chunks * kChunkSize),
      height_(height_chunks * kChunkSize),
      materials_(&materials),
      seed_(seed),
      wrap_x_(edge_x == EdgeX::Wrap) {
  if (width_chunks <= 0 || height_chunks <= 0) {
    throw std::invalid_argument("World: dimensions must be positive");
  }
  chunks_.resize(static_cast<std::size_t>(width_chunks) * static_cast<std::size_t>(height_chunks));
  active_rows_.resize(static_cast<std::size_t>(height_chunks));
  if (storage == ChunkStorage::AllLoaded) {
    for (std::size_t i = 0; i < chunks_.size(); ++i) {
      chunks_[i] = std::make_unique<Chunk>();
      loaded_.push_back(static_cast<int>(i));
    }
  }
}

Chunk& World::load_chunk(int cx, int cy, std::unique_ptr<Chunk> chunk) {
  auto& s = chunks_[slot(cx, cy)];
  if (!s && chunk) loaded_.push_back(static_cast<int>(slot(cx, cy)));
  s = std::move(chunk);
  return *s;
}

std::unique_ptr<Chunk> World::unload_chunk(int cx, int cy) {
  auto& s = chunks_[slot(cx, cy)];
  if (s) {
    const int id = static_cast<int>(slot(cx, cy));
    for (std::size_t i = 0; i < loaded_.size(); ++i) {
      if (loaded_[i] == id) {
        loaded_[i] = loaded_.back();
        loaded_.pop_back();
        break;
      }
    }
  }
  return std::move(s);
}

float World::wind_at(int y) const {
  const Weather& w = weather_;
  if (w.max_wind == 0.0f && w.wind_bias == 0.0f) return 0.0f;
  float shelter = 1.0f;
  if (w.ground_y > 0) {
    const int above = w.ground_y - y;
    if (above <= 0) return 0.0f;
    shelter = std::min(1.0f, static_cast<float>(above) / 120.0f);
  }
  if (wind_tick_ == tick_) return wind_base_ * shelter;  // computed once per step
  // Smooth noise over time: a slow swing plus faster, weaker gusts.
  auto noise = [&](float t, std::uint32_t salt) {
    const float f0 = std::floor(t);
    const int i = static_cast<int>(f0);
    const float f = t - f0, s = f * f * (3.0f - 2.0f * f);
    const auto at = [&](int k) {
      return static_cast<float>(cell_hash(seed_ ^ salt, k, 0, 0) & 0xFFFF) / 32767.5f - 1.0f;
    };
    return at(i) + (at(i + 1) - at(i)) * s;
  };
  const float t = static_cast<float>(tick_) / static_cast<float>(std::max(1, w.wind_period));
  const float n = std::clamp(0.8f * noise(t, 0x57494E44U) + 0.2f * noise(t * 6.0f, 0x47555354U), -1.0f, 1.0f);
  wind_base_ = w.max_wind * n + w.wind_bias;
  wind_tick_ = tick_;
  return wind_base_ * shelter;
}

void World::set_weather(const Weather& weather) {
  weather_ = weather;
  wind_tick_ = 0xFFFFFFFFU;
  moisture_.clear();
  raining_.clear();
  if (weather.region_width <= 0) return;
  const int areas = std::max(1, (width_ + weather.region_width - 1) / weather.region_width);
  moisture_.resize(static_cast<std::size_t>(areas));
  raining_.assign(static_cast<std::size_t>(areas), 0);
  for (int a = 0; a < areas; ++a) {  // each area somewhere along its way to the next rain
    moisture_[static_cast<std::size_t>(a)] =
        weather.rain_start * static_cast<float>(cell_hash(seed_ ^ 0x4D4F4953U, a, 0, 0) & 0xFFFF) / 65536.0f;
  }
}

int World::weather_area(int x) const {
  if (moisture_.empty()) return 0;
  return std::min(wrap_x(x) / std::max(1, weather_.region_width), static_cast<int>(moisture_.size()) - 1);
}

void World::begin_step() {
  for (auto& row : active_rows_) row.clear();
  stepping_ = true;
}

void World::note_active(int cx, int cy) {
  std::vector<int>& row = active_rows_[static_cast<std::size_t>(cy)];
  const auto it = std::lower_bound(row.begin(), row.end(), cx);
  if (it == row.end() || *it != cx) row.insert(it, cx);
}

int World::loaded_chunk_count() const {
  int n = 0;
  for (const auto& c : chunks_) n += c ? 1 : 0;
  return n;
}

void World::set(int x, int y, MaterialId material) {
  if (!in_bounds(x, y)) return;
  x = wrap_x(x);  // the shade and lifetime hashes must not depend on which copy of x was used
  Chunk& c = chunk_for(x, y);
  const int i = local_index(x, y);
  const MaterialProps& p = materials_->props(material);
  c.material[i] = material;
  c.set_shade(i, cell_hash(seed_, x, y, 0) & 0x0F);
  c.set_temperature(i, p.has_spawn_temp ? p.spawn_temp : ambient_at(y));  // the air's, unless it has its own
  c.motion[i] = 0;                    // at rest, no lifetime
  c.health[i] = Chunk::kFullHealth;
  if (p.lifetime_max > 0) {
    const std::uint32_t span = static_cast<std::uint32_t>(p.lifetime_max - p.lifetime_min) + 1;
    c.set_life(i, static_cast<std::uint16_t>(p.lifetime_min + cell_hash(seed_ ^ 0x11FE, x, y, tick_) % span));
  }
  c.clear_updated(i);  // a fresh cell may update in the coming step
  wake(x, y);
}

void World::set_temperature(int x, int y, int t) {
  if (!in_bounds(x, y)) return;
  chunk_for(x, y).set_temperature(local_index(x, y), t);
  wake(x, y);
}

bool World::damage(int x, int y, int amount) {
  if (!in_bounds(x, y) || amount <= 0) return false;
  Chunk& c = chunk_for(x, y);
  const int i = local_index(x, y);
  if (c.material[i] == kEmpty) return false;
  const int durability = materials_->props(c.material[i]).durability;
  // Health is a share of durability: convert the damage, rounding up so any hit counts.
  const int loss = durability == 0 ? 256 : (amount * 255 + durability - 1) / durability;
  if (loss >= c.health[i]) {
    set(x, y, kEmpty);
    return true;
  }
  c.health[i] = static_cast<std::uint8_t>(c.health[i] - loss);
  c.touched = true;
  return false;
}

void World::fill_rect(int x, int y, int w, int h, MaterialId material) {
  for (int yy = y; yy < y + h; ++yy) {
    for (int xx = x; xx < x + w; ++xx) set(xx, yy, material);
  }
}

void World::fill_circle(int cx, int cy, int radius, MaterialId material, float coverage) {
  const auto threshold = static_cast<std::uint32_t>(coverage * 65535.0f);
  for (int dy = -radius; dy <= radius; ++dy) {
    for (int dx = -radius; dx <= radius; ++dx) {
      if (dx * dx + dy * dy > radius * radius) continue;
      const int x = cx + dx;
      const int y = cy + dy;
      if (coverage < 1.0f && (cell_hash(seed_ ^ 0xB5, wrap_x(x), y, tick_) & 0xFFFF) > threshold) continue;
      set(x, y, material);
    }
  }
}

void World::clear() {
  for (std::size_t i = 0; i < chunks_.size(); ++i) {
    if (!chunks_[i]) continue;
    *chunks_[i] = std::move(*make_empty_chunk(static_cast<int>(i) / width_chunks_));
    chunks_[i]->touched = true;
  }
}

std::unique_ptr<Chunk> World::make_empty_chunk(int cy) const {
  auto c = std::make_unique<Chunk>();
  for (int ly = 0; ly < kChunkSize; ++ly) {
    const int t = ambient_at((cy << kChunkShift) + ly);
    if (t == kAmbient) continue;
    for (int lx = 0; lx < kChunkSize; ++lx) c->set_temperature(Chunk::index(lx, ly), t);
  }
  c->touched = false;
  return c;
}

void World::settle_temperatures() {
  for (std::size_t i = 0; i < chunks_.size(); ++i) {
    Chunk* c = chunks_[i].get();
    if (!c) continue;
    const int cy = static_cast<int>(i) / width_chunks_;
    for (int ly = 0; ly < kChunkSize; ++ly) {
      const int t = ambient_at((cy << kChunkShift) + ly);
      for (int lx = 0; lx < kChunkSize; ++lx) {
        const int k = Chunk::index(lx, ly);
        if (!materials_->props(c->material[k]).has_spawn_temp) c->set_temperature(k, t);
      }
    }
  }
}

void World::wake(int x, int y) {
  // Wake into both the step in progress and the next one. Waking only the next
  // step delays a neighbour that becomes free mid-step (e.g. the cell above a
  // gap) by one tick whenever it sits outside the current dirty rect, and those
  // delays pile up at chunk edges as visible terraces in flowing liquid.
  const int lx = x & kChunkMask;
  const int ly = y & kChunkMask;
  // Fast path: the 3x3 neighbourhood lies inside one chunk (true for ~94% of cells).
  if (lx > 0 && lx < kChunkMask && ly > 0 && ly < kChunkMask && in_bounds(x, y)) {
    Chunk& c = chunk_for(x, y);
    if (stepping_ && c.dirty.empty()) note_active(chunk_x(x), y >> kChunkShift);
    c.dirty.include_rect(lx - 1, ly - 1, lx + 1, ly + 1);
    c.next_dirty.include_rect(lx - 1, ly - 1, lx + 1, ly + 1);
    c.touched = true;
    return;
  }
  for (int dy = -1; dy <= 1; ++dy) {
    const int ny = y + dy;
    for (int dx = -1; dx <= 1; ++dx) {
      const int nx = x + dx;
      if (!in_bounds(nx, ny)) continue;
      Chunk& c = chunk_for(nx, ny);
      if (stepping_ && c.dirty.empty()) note_active(chunk_x(nx), ny >> kChunkShift);
      c.dirty.include(nx & kChunkMask, ny & kChunkMask);
      c.next_dirty.include(nx & kChunkMask, ny & kChunkMask);
      c.touched = true;
    }
  }
}

void World::wake_area(int x, int y, int r) {
  const int lx = x & kChunkMask;
  const int ly = y & kChunkMask;
  if (lx >= r && lx < kChunkSize - r && ly >= r && ly < kChunkSize - r && in_bounds(x, y)) {
    Chunk& c = chunk_for(x, y);
    if (stepping_ && c.dirty.empty()) note_active(chunk_x(x), y >> kChunkShift);
    c.dirty.include_rect(lx - r, ly - r, lx + r, ly + r);
    c.next_dirty.include_rect(lx - r, ly - r, lx + r, ly + r);
    c.touched = true;
    return;
  }
  // Marks one box within a single chunk, if that chunk is loaded.
  auto mark = [&](int bx0, int by0, int bx1, int by1) {
    if (!in_bounds(bx0, by0)) return;
    Chunk& c = chunk_for(bx0, by0);
    if (stepping_ && c.dirty.empty()) note_active(chunk_x(bx0), by0 >> kChunkShift);
    c.dirty.include_rect(bx0 & kChunkMask, by0 & kChunkMask, bx1 & kChunkMask, by1 & kChunkMask);
    c.next_dirty.include_rect(bx0 & kChunkMask, by0 & kChunkMask, bx1 & kChunkMask, by1 & kChunkMask);
    c.touched = true;
  };
  // The box crosses a chunk edge: split it into one rect per chunk touched.
  const int y0 = std::max(y - r, 0), y1 = std::min(y + r, height_ - 1);
  for (int sy = y0; sy <= y1;) {
    const int ey = std::min(y1, sy | kChunkMask);  // last row in this chunk row
    for (int sx = x - r; sx <= x + r;) {
      const int ex = std::min(x + r, sx | kChunkMask);  // last column in this chunk
      if (wrap_x_ || (sx >= 0 && ex < width_)) {
        mark(sx, sy, ex, ey);
      } else {
        // Walled world: clip to the columns that exist.
        const int cx0 = std::max(sx, 0), cx1 = std::min(ex, width_ - 1);
        if (cx0 <= cx1) mark(cx0, sy, cx1, ey);
      }
      sx = ex + 1;
    }
    sy = ey + 1;
  }
}

void World::wake_all() {
  for (auto& c : chunks_) {
    if (!c) continue;
    c->next_dirty.include(0, 0);
    c->next_dirty.include(kChunkMask, kChunkMask);
    c->touched = true;
  }
}

int World::active_chunk_count() const {
  int n = 0;
  for (const auto& c : chunks_) n += c && !c->next_dirty.empty() ? 1 : 0;
  return n;
}

std::size_t World::count(MaterialId material) const {
  std::size_t n = 0;
  for (const auto& c : chunks_) {
    if (!c) continue;
    for (MaterialId m : c->material) n += (m == material) ? 1 : 0;
  }
  return n;
}

}  // namespace px
