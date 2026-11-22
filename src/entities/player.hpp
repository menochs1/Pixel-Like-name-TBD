#pragma once

#include <utility>
#include <vector>

#include "core/world.hpp"

namespace px {

/// Everything about how the player moves, in cells and simulation steps
/// (60 per second). Tweak these to change the feel.
struct PlayerParams {
  int width = 16;   // body size in cells
  int height = 32;
  float gravity = 0.25f;       // cells/step^2 added to the fall speed (900 cells/s^2)
  float max_fall = 6.0f;       // terminal fall speed, cells/step
  float walk_speed = 1.5f;     // cells/step (90 cells/s)
  float ground_accel = 0.35f;  // how quickly walking reaches full speed on the ground
  float air_accel = 0.15f;     // ...and in the air (less control)
  float jump_speed = 4.25f;    // initial upward speed: a jump of about 36 cells
  float jump_cut = 0.5f;       // releasing jump while rising multiplies the upward speed by this
  int coyote_steps = 5;        // a jump still works this many steps after walking off a ledge
  int step_up = 4;             // walks up ledges this high without jumping
  // In liquid (at least half the body submerged):
  float liquid_gravity = 0.04f;  // what is left of gravity after buoyancy
  float liquid_drag = 0.85f;     // speeds are multiplied by this every step
  float swim_speed = 1.2f;       // holding jump/down swims up/down at about this speed
  int unstick_reach = 12;        // how far it is pushed out when material lands inside it
};

struct PlayerInput {
  bool left = false, right = false;
  bool jump = false;  // held
  bool down = false;  // held: swim down
};

/// The player: a box of cells that walks, jumps, swims and stands on the
/// world. Solids and powders block it, except passable ones (tree trunks,
/// leaves); air, liquids and gases do not. Cells outside the world or in
/// unloaded chunks block it too.
///
/// It does not change the world (yet), and the world does not know about it:
/// sand can fall into its box, in which case it is pushed up out of it.
/// Deterministic: the same inputs give the same path. Like the camera, x is
/// never wrapped (the world wraps it), so the position is continuous across
/// the seam of a wrapping world.
class Player {
 public:
  explicit Player(PlayerParams params = {}) : p_(params) {}

  /// Places the player with its top-left corner at (x, y), at rest.
  void place(float x, float y);
  /// Places the player standing on the first ground at or below row `from_y`
  /// in the column under its centre, at horizontal position `x` (left edge).
  void place_on_ground(const World& world, float x, int from_y = 0);

  /// One simulation step.
  void update(const World& world, const PlayerInput& input);

  float x() const { return x_; }  // left edge
  float y() const { return y_; }  // top edge
  float vx() const { return vx_; }
  float vy() const { return vy_; }
  int left() const;               // integer box: [left, left + width) x [top, top + height)
  int top() const;
  bool on_ground() const { return on_ground_; }
  bool in_liquid() const { return in_liquid_; }
  bool facing_left() const { return facing_left_; }
  /// Steps spent walking on the ground (drives the walk animation); 0 when still.
  int walk_steps() const { return walk_steps_; }
  const PlayerParams& params() const { return p_; }
  PlayerParams& params() { return p_; }

  /// True if the player's box at integer position (x, y) overlaps anything solid.
  bool blocked(const World& world, int x, int y) const;

  /// Makes the body solid to the simulation for one step: fills the empty
  /// cells of its box with the hidden "body" material, so falling sand lands
  /// on it and liquids flow around it. Call release() right after the step,
  /// before anything reads or saves the world. Liquid or gas already in the
  /// box stays (the player is wading or swimming in it).
  void occupy(World& world);
  /// Empties the cells occupy() filled (waking them, so what rests on the
  /// body falls when it moves away).
  void release(World& world);

 private:
  bool solid(const World& world, int x, int y) const;
  bool standing(const World& world, int x, int y) const;
  int submerged_rows(const World& world, int x, int y) const;
  void unstick(const World& world);
  void move_x(const World& world, int dx);
  void move_y(const World& world, int dy);

  PlayerParams p_;
  float x_ = 0.0f, y_ = 0.0f;
  float vx_ = 0.0f, vy_ = 0.0f;
  bool on_ground_ = false;
  bool in_liquid_ = false;
  bool facing_left_ = false;
  bool jump_was_held_ = false;
  int coyote_ = 0;
  int walk_steps_ = 0;
  std::vector<std::pair<int, int>> occupied_;  // cells filled by occupy()
};

}  // namespace px
