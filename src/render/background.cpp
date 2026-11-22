#include "render/background.hpp"

#include <algorithm>
#include <cmath>

#include "core/rng.hpp"

namespace px {
namespace {

constexpr int kProfileLength = 4096;  // silhouettes repeat every this many background columns
constexpr int kRockSize = 256;        // the cave wall texture repeats every this many cells
constexpr float kHorizon = 0.62f;     // where layers sit on screen when nothing pulls them

std::uint32_t argb(int r, int g, int b) {
  auto c = [](int v) { return static_cast<std::uint32_t>(std::clamp(v, 0, 255)); };
  return 0xFF000000U | (c(r) << 16) | (c(g) << 8) | c(b);
}

/// Smooth noise in [-1, 1] over a repeating lattice of `period` points.
float noise1(std::uint64_t seed, float u, int period) {
  const float f0 = std::floor(u);
  const int i = static_cast<int>(f0);
  const float f = u - f0, s = f * f * (3.0f - 2.0f * f);
  auto at = [&](int k) {
    k = ((k % period) + period) % period;
    return static_cast<float>(cell_hash(seed, k, 0, 0) & 0xFFFF) / 32767.5f - 1.0f;
  };
  return at(i) + (at(i + 1) - at(i)) * s;
}

float noise2(std::uint64_t seed, float u, float v, int period) {
  const float fu0 = std::floor(u), fv0 = std::floor(v);
  const int iu = static_cast<int>(fu0), iv = static_cast<int>(fv0);
  const float fu = u - fu0, fv = v - fv0;
  const float su = fu * fu * (3.0f - 2.0f * fu), sv = fv * fv * (3.0f - 2.0f * fv);
  auto at = [&](int a, int b) {
    a = ((a % period) + period) % period;
    b = ((b % period) + period) % period;
    return static_cast<float>(cell_hash(seed, a, b, 0) & 0xFFFF) / 32767.5f - 1.0f;
  };
  const float top = at(iu, iv) + (at(iu + 1, iv) - at(iu, iv)) * su;
  const float bottom = at(iu, iv + 1) + (at(iu + 1, iv + 1) - at(iu, iv + 1)) * su;
  return top + (bottom - top) * sv;
}

/// A silhouette: `octaves` of repeating noise, scaled to `amplitude` pixels.
std::vector<std::int16_t> profile(std::uint64_t seed, float feature, int octaves, float amplitude, float sharpness) {
  std::vector<std::int16_t> h(kProfileLength);
  for (int x = 0; x < kProfileLength; ++x) {
    float sum = 0.0f, amp = 1.0f, norm = 0.0f;
    for (int o = 0; o < octaves; ++o) {
      const int period = std::max(1, static_cast<int>(static_cast<float>(kProfileLength) / feature)) << o;
      sum += amp * noise1(seed + static_cast<std::uint64_t>(o) * 977, static_cast<float>(x) * static_cast<float>(period) /
                                                                         static_cast<float>(kProfileLength), period);
      norm += amp;
      amp *= 0.5f;
    }
    float n = 0.5f + 0.5f * sum / norm;  // 0..1
    n = std::pow(n, sharpness);          // > 1: lone peaks over low ground
    h[static_cast<std::size_t>(x)] = static_cast<std::int16_t>(n * amplitude);
  }
  return h;
}

}  // namespace

ParallaxBackground::ParallaxBackground(std::uint64_t seed, int ground_y, std::function<int(int)> surface)
    : ground_y_(ground_y), surface_(std::move(surface)) {
  // Far to near: paler and bluer with distance, like haze.
  layers_[0] = {0.12f, argb(118, 136, 168), profile(seed ^ 0x4D544E31U, 520.0f, 4, 150.0f, 1.6f)};  // mountains
  layers_[1] = {0.30f, argb(84, 112, 122), profile(seed ^ 0x48494C4CU, 300.0f, 3, 80.0f, 1.2f)};    // hills
  layers_[2] = {0.50f, argb(56, 84, 70), profile(seed ^ 0x4E454152U, 180.0f, 3, 46.0f, 1.0f)};      // near ridge
  // Little tree tops along the near ridge.
  for (int x = 0; x < kProfileLength; ++x) {
    const std::uint32_t h = cell_hash(seed ^ 0x54524545U, x / 7, 0, 0);
    if ((h & 3U) == 0) {
      const int dx = x % 7 - 3;
      layers_[2].height[static_cast<std::size_t>(x)] =
          static_cast<std::int16_t>(layers_[2].height[static_cast<std::size_t>(x)] + std::max(0, 6 - dx * dx));
    }
  }
  // Cave wall: blotchy rock with darker seams.
  rock_.resize(static_cast<std::size_t>(kRockSize) * kRockSize);
  for (int y = 0; y < kRockSize; ++y) {
    for (int x = 0; x < kRockSize; ++x) {
      const float big = noise2(seed ^ 0x524F434BU, static_cast<float>(x) / 32.0f, static_cast<float>(y) / 32.0f, kRockSize / 32);
      const float small = noise2(seed ^ 0x47524954U, static_cast<float>(x) / 6.0f, static_cast<float>(y) / 6.0f,
                                 static_cast<int>(kRockSize / 6.0f));
      const float seam = std::fabs(noise2(seed ^ 0x5345414DU, static_cast<float>(x) / 20.0f, static_cast<float>(y) / 20.0f,
                                          static_cast<int>(kRockSize / 20.0f)));
      const int v = static_cast<int>(10.0f * big + 5.0f * small) - (seam < 0.06f ? 10 : 0);
      rock_[static_cast<std::size_t>(y * kRockSize + x)] = argb(44 + v, 38 + v, 36 + v);
    }
  }
}

int ParallaxBackground::surface_at(int x) const {
  if (!surface_) return ground_y_;
  // Cache a window of columns around the camera; the generator is slow per call.
  const int size = 4096;
  if (surface_cache_.empty() || x < cache_origin_ || x >= cache_origin_ + size) {
    cache_origin_ = x - size / 2;
    surface_cache_.assign(static_cast<std::size_t>(size), 0x7FFFFFFF);
  }
  int& v = surface_cache_[static_cast<std::size_t>(x - cache_origin_)];
  if (v == 0x7FFFFFFF) v = surface_(x);
  return v;
}

void ParallaxBackground::render(std::uint32_t* pixels, int pitch, int w, int h, double cam_x, double cam_y) const {
  // Where the ground would be on screen; each layer's horizon sits between that
  // and a fixed line, nearer layers following the ground more closely.
  const double ground_on_screen = ground_y_ - cam_y;
  int horizon[3];
  for (int k = 0; k < 3; ++k) {
    const double f = layers_[static_cast<std::size_t>(k)].parallax;
    horizon[k] = static_cast<int>(std::lround(ground_on_screen * f + static_cast<double>(h) * kHorizon * (1.0 - f)));
  }
  // Sky: a vertical gradient, fixed to the screen (it is infinitely far away).
  std::vector<std::uint32_t> sky(static_cast<std::size_t>(h));
  for (int y = 0; y < h; ++y) {
    const float t = std::clamp(static_cast<float>(y) / (static_cast<float>(h) * kHorizon), 0.0f, 1.0f);
    sky[static_cast<std::size_t>(y)] = argb(static_cast<int>(54 + 96 * t), static_cast<int>(86 + 100 * t),
                                            static_cast<int>(150 + 70 * t));
  }
  const int rock_dx = static_cast<int>(std::floor(cam_x * 0.85)), rock_dy = static_cast<int>(std::floor(cam_y * 0.85));
  for (int sx = 0; sx < w; ++sx) {
    int top[3];
    for (int k = 0; k < 3; ++k) {
      const Layer& L = layers_[static_cast<std::size_t>(k)];
      const long u = static_cast<long>(std::floor(cam_x * L.parallax)) + sx;
      const long i = ((u % kProfileLength) + kProfileLength) % kProfileLength;
      top[k] = horizon[k] - L.height[static_cast<std::size_t>(i)];
    }
    const int wx = static_cast<int>(std::floor(cam_x)) + sx;
    const int cave_from = surface_at(wx) + 6 - static_cast<int>(std::floor(cam_y));  // screen row where caves begin
    const int rx = ((rock_dx + sx) % kRockSize + kRockSize) % kRockSize;
    for (int sy = 0; sy < h; ++sy) {
      std::uint32_t c;
      if (sy >= cave_from) {
        const int ry = ((rock_dy + sy) % kRockSize + kRockSize) % kRockSize;
        c = rock_[static_cast<std::size_t>(ry * kRockSize + rx)];
      } else if (sy >= top[2]) {
        c = layers_[2].colour;
      } else if (sy >= top[1]) {
        c = layers_[1].colour;
      } else if (sy >= top[0]) {
        c = layers_[0].colour;
      } else {
        c = sky[static_cast<std::size_t>(sy)];
      }
      pixels[static_cast<std::ptrdiff_t>(sy) * pitch + sx] = c;
    }
  }
}

}  // namespace px
