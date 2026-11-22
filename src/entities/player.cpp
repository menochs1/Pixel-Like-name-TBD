#include "entities/player.hpp"

#include <algorithm>
#include <cmath>

namespace px {

int Player::left() const { return static_cast<int>(std::floor(x_)); }
int Player::top() const { return static_cast<int>(std::floor(y_)); }

void Player::place(float x, float y) {
  x_ = x;
  y_ = y;
  vx_ = vy_ = 0.0f;
  on_ground_ = in_liquid_ = false;
  coyote_ = walk_steps_ = 0;
}

bool Player::solid(const World& world, int x, int y) const {
  const MaterialId m = world.get(x, y);
  if (m == kOutOfBounds) return true;  // world edge or unloaded chunk
  const Phase phase = world.materials().phase(m);
  return phase == Phase::Solid || phase == Phase::Powder;
}

bool Player::blocked(const World& world, int x, int y) const {
  for (int yy = y; yy < y + p_.height; ++yy)
    for (int xx = x; xx < x + p_.width; ++xx)
      if (solid(world, xx, yy)) return true;
  return false;
}

bool Player::standing(const World& world, int x, int y) const {
  for (int xx = x; xx < x + p_.width; ++xx)
    if (solid(world, xx, y + p_.height)) return true;
  return false;
}

int Player::submerged_rows(const World& world, int x, int y) const {
  const int cx = x + p_.width / 2;
  int n = 0;
  for (int yy = y; yy < y + p_.height; ++yy) {
    const MaterialId m = world.get(cx, yy);
    n += m != kOutOfBounds && world.materials().phase(m) == Phase::Liquid ? 1 : 0;
  }
  return n;
}

void Player::place_on_ground(const World& world, float x, int from_y) {
  const int ix = static_cast<int>(std::floor(x));
  for (int y = std::max(from_y, 0); y + p_.height < world.height(); ++y) {
    if (!blocked(world, ix, y) && standing(world, ix, y)) {
      place(x, static_cast<float>(y));
      on_ground_ = true;
      return;
    }
  }
  place(x, static_cast<float>(from_y));
}

void Player::unstick(const World& world) {
  const int ix = left(), iy = top();
  if (!blocked(world, ix, iy)) return;
  // Something landed in (or grew into) the box: up first, the way a pile
  // pushes you up, then sideways.
  for (int k = 1; k <= p_.unstick_reach; ++k) {
    if (!blocked(world, ix, iy - k)) {
      y_ -= static_cast<float>(k);
      vy_ = std::min(vy_, 0.0f);
      return;
    }
  }
  for (int k = 1; k <= p_.unstick_reach; ++k) {
    for (const int dx : {k, -k}) {
      if (!blocked(world, ix + dx, iy)) {
        x_ += static_cast<float>(dx);
        return;
      }
    }
  }
  // Buried: stay put until the material moves.
}

void Player::move_x(const World& world, int dx) {
  const int s = dx > 0 ? 1 : -1;
  int ix = left(), iy = top();
  for (int k = 0; k < std::abs(dx); ++k) {
    if (!blocked(world, ix + s, iy)) {
      ix += s;
      continue;
    }
    // A low ledge: step up onto it (on the ground or swimming).
    bool stepped = false;
    if (on_ground_ || in_liquid_) {
      for (int up = 1; up <= p_.step_up && !stepped; ++up) {
        if (!blocked(world, ix + s, iy - up)) {
          ix += s;
          iy -= up;
          y_ -= static_cast<float>(up);
          stepped = true;
        }
      }
    }
    if (!stepped) {  // a wall
      x_ = static_cast<float>(ix);
      vx_ = 0.0f;
      return;
    }
  }
  x_ = static_cast<float>(ix) + (x_ - std::floor(x_));  // whole cells moved, fraction kept
}

void Player::move_y(const World& world, int dy) {
  const int s = dy > 0 ? 1 : -1;
  const int ix = left();
  int iy = top();
  for (int k = 0; k < std::abs(dy); ++k) {
    if (blocked(world, ix, iy + s)) {
      y_ = static_cast<float>(iy);
      if (s > 0) on_ground_ = true;
      vy_ = 0.0f;
      return;
    }
    iy += s;
  }
  y_ = static_cast<float>(iy) + (y_ - std::floor(y_));
}

void Player::occupy(World& world) {
  release(world);
  const MaterialId body = world.materials().require("body");
  const int x0 = left(), y0 = top();
  for (int y = y0; y < y0 + p_.height; ++y) {
    for (int x = x0; x < x0 + p_.width; ++x) {
      if (world.get(x, y) != kEmpty) continue;
      // Written directly, without waking: a new solid never lets anything
      // move that could not before. Temperature and shade are left as they
      // were, so releasing restores the air exactly.
      Chunk& c = world.chunk_for(x, y);
      c.material[World::local_index(x, y)] = body;
      c.touched = true;
      occupied_.push_back({x, y});
    }
  }
}

void Player::release(World& world) {
  if (occupied_.empty()) return;
  const MaterialId body = world.materials().require("body");
  for (const auto& [x, y] : occupied_) {
    if (world.get(x, y) != body) continue;
    world.chunk_for(x, y).material[World::local_index(x, y)] = kEmpty;
    world.wake(x, y);  // whatever rested on it may fall now
  }
  occupied_.clear();
}

void Player::update(const World& world, const PlayerInput& input) {
  unstick(world);
  int ix = left(), iy = top();
  in_liquid_ = submerged_rows(world, ix, iy) * 2 >= p_.height;
  const bool was_on_ground = standing(world, ix, iy);
  on_ground_ = was_on_ground;
  coyote_ = on_ground_ ? p_.coyote_steps : std::max(coyote_ - 1, 0);
  const bool jump_pressed = input.jump && !jump_was_held_;

  // Horizontal: accelerate toward the walking speed.
  const int dir = (input.right ? 1 : 0) - (input.left ? 1 : 0);
  if (dir != 0) facing_left_ = dir < 0;
  const float top_speed = p_.walk_speed * (in_liquid_ ? 0.6f : 1.0f);
  const float accel = on_ground_ ? p_.ground_accel : p_.air_accel;
  vx_ += std::clamp(static_cast<float>(dir) * top_speed - vx_, -accel, accel);

  // Vertical.
  if (in_liquid_) {
    vy_ += p_.liquid_gravity;
    if (input.jump) vy_ = std::max(vy_ - 0.3f, -p_.swim_speed);
    if (input.down) vy_ = std::min(vy_ + 0.3f, p_.swim_speed);
    // Near the surface, a fresh jump leaps out of the water.
    if (jump_pressed && submerged_rows(world, ix, iy) * 4 < p_.height * 3) vy_ = -p_.jump_speed * 0.8f;
    vy_ *= p_.liquid_drag;
    vx_ *= p_.liquid_drag + (1.0f - p_.liquid_drag) * 0.5f;
  } else {
    if (jump_pressed && coyote_ > 0) {
      vy_ = -p_.jump_speed;
      coyote_ = 0;
      on_ground_ = false;
    }
    if (!input.jump && jump_was_held_ && vy_ < 0.0f) vy_ *= p_.jump_cut;  // short hop
    vy_ = std::min(vy_ + p_.gravity, p_.max_fall);
  }
  jump_was_held_ = input.jump;

  // Move one axis at a time, cell by cell, so nothing is tunnelled through.
  const int dx = static_cast<int>(std::floor(x_ + vx_)) - left();
  x_ += vx_ - static_cast<float>(dx);  // fraction now; the whole cells move below
  if (dx != 0) move_x(world, dx);
  const int dy = static_cast<int>(std::floor(y_ + vy_)) - top();
  y_ += vy_ - static_cast<float>(dy);
  if (dy != 0) move_y(world, dy);

  // Walking down a slope: stay on the ground instead of hopping off each step.
  ix = left();
  iy = top();
  if (was_on_ground && vy_ >= 0.0f && !in_liquid_ && !standing(world, ix, iy)) {
    for (int down = 1; down <= p_.step_up; ++down) {
      if (blocked(world, ix, iy + down)) break;
      if (standing(world, ix, iy + down)) {
        y_ += static_cast<float>(down);
        vy_ = 0.0f;
        break;
      }
    }
  }
  on_ground_ = standing(world, left(), top());
  walk_steps_ = on_ground_ && std::fabs(vx_) > 0.3f ? walk_steps_ + 1 : 0;
}

}  // namespace px
