#include "core/world.hpp"

#include <stdexcept>

#include "core/rng.hpp"

namespace px {

World::World(int width_chunks, int height_chunks, const MaterialRegistry& materials, std::uint64_t seed)
    : width_chunks_(width_chunks),
      height_chunks_(height_chunks),
      width_(width_chunks * kChunkSize),
      height_(height_chunks * kChunkSize),
      materials_(&materials),
      seed_(seed) {
  if (width_chunks <= 0 || height_chunks <= 0) {
    throw std::invalid_argument("World: dimensions must be positive");
  }
  chunks_.resize(static_cast<std::size_t>(width_chunks) * static_cast<std::size_t>(height_chunks));
}

void World::set(int x, int y, MaterialId material) {
  if (!in_bounds(x, y)) return;
  Chunk& c = chunk_for(x, y);
  const int i = local_index(x, y);
  const MaterialProps& p = materials_->props(material);
  c.material[i] = material;
  c.temperature[i] = p.spawn_temp;  // ambient unless the material defines one
  c.life[i] = 0;
  if (p.lifetime_max > 0) {
    const std::uint32_t span = static_cast<std::uint32_t>(p.lifetime_max - p.lifetime_min) + 1;
    c.life[i] = static_cast<std::uint16_t>(p.lifetime_min + cell_hash(seed_ ^ 0x11FE, x, y, tick_) % span);
  }
  c.velocity_y[i] = 0;
  c.velocity_x[i] = 0;
  c.shade[i] = static_cast<std::uint8_t>(cell_hash(seed_, x, y, 0) & 0x0F);
  c.clock[i] = 0;
  wake(x, y);
}

void World::set_temperature(int x, int y, Temperature t) {
  if (!in_bounds(x, y)) return;
  chunk_for(x, y).temperature[local_index(x, y)] = t;
  wake(x, y);
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
      if (coverage < 1.0f && (cell_hash(seed_ ^ 0xB5, x, y, tick_) & 0xFFFF) > threshold) continue;
      set(x, y, material);
    }
  }
}

void World::clear() {
  for (Chunk& c : chunks_) c = Chunk{};
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
    c.dirty.include_rect(lx - 1, ly - 1, lx + 1, ly + 1);
    c.next_dirty.include_rect(lx - 1, ly - 1, lx + 1, ly + 1);
    return;
  }
  for (int dy = -1; dy <= 1; ++dy) {
    const int ny = y + dy;
    for (int dx = -1; dx <= 1; ++dx) {
      const int nx = x + dx;
      if (!in_bounds(nx, ny)) continue;
      Chunk& c = chunk_for(nx, ny);
      c.dirty.include(nx & kChunkMask, ny & kChunkMask);
      c.next_dirty.include(nx & kChunkMask, ny & kChunkMask);
    }
  }
}

void World::wake_all() {
  for (Chunk& c : chunks_) {
    c.next_dirty.include(0, 0);
    c.next_dirty.include(kChunkMask, kChunkMask);
  }
}

int World::active_chunk_count() const {
  int n = 0;
  for (const Chunk& c : chunks_) n += c.next_dirty.empty() ? 0 : 1;
  return n;
}

std::size_t World::count(MaterialId material) const {
  std::size_t n = 0;
  for (const Chunk& c : chunks_) {
    for (MaterialId m : c.material) n += (m == material) ? 1 : 0;
  }
  return n;
}

}  // namespace px
