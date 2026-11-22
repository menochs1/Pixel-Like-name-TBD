#include "save/streamer.hpp"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <utility>
#include <vector>

#include "save/chunk_codec.hpp"

namespace px {

ChunkStreamer::ChunkStreamer(World& world, ChunkStore& store, const WorldGenerator* generator)
    : world_(world), store_(store), generator_(generator) {}

int ChunkStreamer::wrap_cx(int cx) const {
  const int wc = world_.width_chunks();
  return ((cx % wc) + wc) % wc;
}

bool ChunkStreamer::in_box(int cx, int cy, const ChunkRegion& box) const {
  if (cy < box.cy0 || cy > box.cy1) return false;
  if (!world_.wraps_x()) return cx >= box.cx0 && cx <= box.cx1;
  const int wc = world_.width_chunks();
  if (box.cx1 - box.cx0 + 1 >= wc) return true;
  return wrap_cx(cx - box.cx0) <= box.cx1 - box.cx0;  // distance from the box's left edge, around the ring
}

bool ChunkStreamer::load(int cx, int cy) {
  if (world_.loaded(cx, cy)) return true;
  bool ok = true;
  if (store_.contains(cx, cy)) {
    std::vector<std::uint8_t> bytes;
    const std::vector<MaterialId>* remap = nullptr;
    auto chunk = std::make_unique<Chunk>();
    std::string why;
    if (store_.read(cx, cy, bytes, &remap) &&
        decode_chunk(bytes.data(), bytes.size(), cx, cy, world_.seed(), remap, world_.materials().size(), *chunk, &why)) {
      world_.load_chunk(cx, cy, std::move(chunk));
      return true;
    }
    last_error_ = "chunk " + std::to_string(cx) + "," + std::to_string(cy) + ": " + (why.empty() ? "unreadable" : why);
    ok = false;  // fall through: regenerate rather than leave a hole
  }
  if (generator_) {
    generator_->generate_chunk(world_, cx, cy);
  } else {
    world_.load_chunk(cx, cy, world_.make_empty_chunk(cy));  // empty, air at rest
  }
  return ok;
}

bool ChunkStreamer::unload(int cx, int cy) {
  std::unique_ptr<Chunk> chunk = world_.unload_chunk(cx, cy);
  if (!chunk || !chunk->touched) return false;
  if (!store_.write(cx, cy, encode_chunk(*chunk, cx, cy, world_.seed()))) {
    last_error_ = "cannot write chunk " + std::to_string(cx) + "," + std::to_string(cy) + " to " +
                  store_.work_dir().string();
    world_.load_chunk(cx, cy, std::move(chunk));  // keep it rather than lose it
    return false;
  }
  return true;
}

void ChunkStreamer::unload_all() {
  for (int cy = 0; cy < world_.height_chunks(); ++cy)
    for (int cx = 0; cx < world_.width_chunks(); ++cx)
      if (world_.loaded(cx, cy)) unload(cx, cy);
}

ChunkStreamer::Stats ChunkStreamer::update(const ChunkRegion& required, const ChunkRegion& wanted,
                                           const ChunkRegion& keep, int budget) {
  Stats stats;
  const int wc = world_.width_chunks(), hc = world_.height_chunks();
  auto columns = [&](const ChunkRegion& box, auto&& fn) {
    const int y0 = std::max(box.cy0, 0), y1 = std::min(box.cy1, hc - 1);
    int x0 = box.cx0, x1 = box.cx1;
    if (!world_.wraps_x()) {
      x0 = std::max(x0, 0);
      x1 = std::min(x1, wc - 1);
    } else if (x1 - x0 + 1 >= wc) {
      x0 = 0;
      x1 = wc - 1;
    }
    for (int cy = y0; cy <= y1; ++cy)
      for (int cx = x0; cx <= x1; ++cx) fn(world_.wraps_x() ? wrap_cx(cx) : cx, cy);
  };

  // 1. Everything that must be there.
  columns(required, [&](int cx, int cy) {
    if (!world_.loaded(cx, cy)) {
      load(cx, cy);
      ++stats.loaded;
    }
  });

  // 2. Prefetch, nearest first, within the budget.
  if (budget > 0) {
    std::vector<std::pair<int, std::pair<int, int>>> todo;  // (distance, position)
    const int mx = (required.cx0 + required.cx1) / 2, my = (required.cy0 + required.cy1) / 2;
    columns(wanted, [&](int cx, int cy) {
      if (world_.loaded(cx, cy)) return;
      int dx = std::abs(cx - (world_.wraps_x() ? wrap_cx(mx) : mx));
      if (world_.wraps_x()) dx = std::min(dx, wc - dx);
      todo.push_back({dx * dx + (cy - my) * (cy - my), {cx, cy}});
    });
    std::sort(todo.begin(), todo.end());
    for (std::size_t i = 0; i < todo.size() && static_cast<int>(i) < budget; ++i) {
      load(todo[i].second.first, todo[i].second.second);
      ++stats.loaded;
    }
  }

  // 3. Everything far away goes.
  for (int cy = 0; cy < hc; ++cy) {
    for (int cx = 0; cx < wc; ++cx) {
      if (!world_.loaded(cx, cy) || in_box(cx, cy, keep)) continue;
      if (unload(cx, cy)) ++stats.written;
      if (!world_.loaded(cx, cy)) ++stats.unloaded;
    }
  }
  return stats;
}

}  // namespace px
