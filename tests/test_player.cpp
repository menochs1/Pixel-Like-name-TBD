// The player character: falling, walking, jumping, ledges, water, getting
// buried, and the wrap seam.

#include <algorithm>
#include <cmath>

#include "core/material.hpp"
#include "core/simulator.hpp"
#include "core/world.hpp"
#include "entities/player.hpp"
#include "test_framework.hpp"

namespace {

struct PlayerFixture {
  px::MaterialRegistry materials;
  px::BaseMaterials ids;
  PlayerFixture() : ids(px::register_base_materials(materials)) {}
};

void run(px::Player& p, const px::World& w, int steps, px::PlayerInput in = {}) {
  for (int i = 0; i < steps; ++i) p.update(w, in);
}

constexpr int kFloor = 100;  // top row of the floor in the test worlds

}  // namespace

TEST(player_is_16_by_32_and_lands_on_the_floor) {
  PlayerFixture f;
  px::World w(4, 2, f.materials, 1);
  w.fill_rect(0, kFloor, w.width(), 28, f.ids.stone);
  px::Player p;
  CHECK_EQ(p.params().width, 16);
  CHECK_EQ(p.params().height, 32);
  p.place(50, 10);
  run(p, w, 120);
  CHECK(p.on_ground());
  CHECK_EQ(p.top() + 32, kFloor);  // feet exactly on the floor
  CHECK(!p.blocked(w, p.left(), p.top()));
}

TEST(player_walks_and_stops_at_walls) {
  PlayerFixture f;
  px::World w(4, 2, f.materials, 1);
  w.fill_rect(0, kFloor, w.width(), 28, f.ids.stone);
  w.fill_rect(150, 0, 4, kFloor, f.ids.stone);  // a wall
  px::Player p;
  p.place_on_ground(w, 50);
  px::PlayerInput right;
  right.right = true;
  run(p, w, 30, right);
  CHECK(p.x() > 80);  // ~1.5 cells/step once up to speed
  CHECK(!p.facing_left());
  CHECK(p.walk_steps() > 0);
  run(p, w, 120, right);
  CHECK_EQ(p.left() + 16, 150);  // pressed against the wall, not inside it
  px::PlayerInput left;
  left.left = true;
  run(p, w, 10, left);
  CHECK(p.facing_left());
  CHECK(p.left() + 16 < 150);
}

TEST(player_steps_up_low_ledges_but_not_high_ones) {
  PlayerFixture f;
  px::World w(4, 2, f.materials, 1);
  w.fill_rect(0, kFloor, w.width(), 28, f.ids.stone);
  w.fill_rect(100, kFloor - 3, 50, 3, f.ids.stone);   // a 3-cell step
  w.fill_rect(200, kFloor - 10, 50, 10, f.ids.stone); // a 10-cell wall
  px::Player p;
  p.place_on_ground(w, 60);
  px::PlayerInput right;
  right.right = true;
  run(p, w, 45, right);
  CHECK(p.left() > 110);                  // walked onto the step
  CHECK(p.left() + 16 < 150);
  CHECK_EQ(p.top() + 32, kFloor - 3);     // standing on top of it
  run(p, w, 80, right);
  CHECK_EQ(p.left() + 16, 200);           // the tall one stops it
}

TEST(player_walks_down_slopes_without_hopping) {
  PlayerFixture f;
  px::World w(4, 2, f.materials, 1);
  w.fill_rect(0, kFloor, w.width(), 28, f.ids.stone);
  for (int i = 0; i < 20; ++i) w.fill_rect(0, kFloor - 20 + i, 60 + i * 3, 1, f.ids.stone);  // a staircase down
  px::Player p;
  p.place_on_ground(w, 20);
  px::PlayerInput right;
  right.right = true;
  int airborne = 0;
  for (int i = 0; i < 60; ++i) {
    p.update(w, right);
    airborne += p.on_ground() ? 0 : 1;
  }
  CHECK(airborne < 5);
}

TEST(player_jumps_about_its_own_height) {
  PlayerFixture f;
  px::World w(4, 3, f.materials, 1);
  w.fill_rect(0, 160, w.width(), 32, f.ids.stone);
  px::Player p;
  p.place_on_ground(w, 50);
  const int ground_top = p.top();
  px::PlayerInput jump;
  jump.jump = true;
  int highest = ground_top;
  for (int i = 0; i < 60; ++i) {
    p.update(w, jump);
    highest = std::min(highest, p.top());
  }
  CHECK(ground_top - highest >= 30);
  CHECK(ground_top - highest <= 42);
  CHECK(p.on_ground());  // came back down
  // A tap gives a smaller hop than holding the key.
  p.place_on_ground(w, 50);
  p.update(w, jump);
  int tap_high = p.top();
  for (int i = 0; i < 60; ++i) {
    p.update(w, {});
    tap_high = std::min(tap_high, p.top());
  }
  CHECK(ground_top - tap_high < ground_top - highest);
  // Holding jump does not bounce: one jump per press.
  p.place_on_ground(w, 50);
  int jumps = 0;
  bool was_ground = true;
  for (int i = 0; i < 200; ++i) {
    p.update(w, jump);
    if (was_ground && !p.on_ground()) ++jumps;
    was_ground = p.on_ground();
  }
  CHECK_EQ(jumps, 1);
}

TEST(player_can_still_jump_just_after_walking_off_a_ledge) {
  PlayerFixture f;
  px::World w(4, 3, f.materials, 1);
  w.fill_rect(0, 160, w.width(), 32, f.ids.stone);
  w.fill_rect(0, 100, 80, 60, f.ids.stone);  // a ledge 60 cells high
  px::Player p;
  p.place_on_ground(w, 40);
  px::PlayerInput right;
  right.right = true;
  while (p.on_ground()) p.update(w, right);  // walk off the edge
  px::PlayerInput jump = right;
  jump.jump = true;
  const int before = p.top();
  p.update(w, right);
  p.update(w, jump);  // two steps late: within the grace period
  for (int i = 0; i < 6; ++i) p.update(w, jump);
  CHECK(p.top() < before);  // it went up, not down
}

TEST(player_floats_and_swims_in_water) {
  PlayerFixture f;
  px::World w(4, 4, f.materials, 1);
  w.fill_rect(0, 236, w.width(), 20, f.ids.stone);
  w.fill_rect(0, 116, w.width(), 120, f.ids.water);  // deep water, surface at row 116
  px::Player p;
  p.place(100, 130);
  run(p, w, 30);
  CHECK(p.in_liquid());
  CHECK(p.top() + 32 < 236);  // sinking slowly, not dropping to the bottom
  px::PlayerInput up;
  up.jump = true;
  run(p, w, 200, up);
  CHECK(p.top() < 130);  // swam up to the surface
  px::PlayerInput dive;
  dive.down = true;
  const int at_surface = p.top();
  run(p, w, 60, dive);
  CHECK(p.top() > at_surface + 20);  // dove
}

// The player's body is solid to the simulation during a step: sand piles on
// its head and pours off its shoulders instead of falling through it, and
// falls once it walks away.
TEST(sand_piles_on_the_player_and_falls_when_it_leaves) {
  PlayerFixture f;
  px::World w(4, 3, f.materials, 2);
  w.fill_rect(0, 160, w.width(), 32, f.ids.stone);
  px::Player p;
  p.place_on_ground(w, 60);
  w.fill_rect(62, 40, 12, 40, f.ids.sand);  // a load of sand above its head
  const auto sand = w.count(f.ids.sand);
  bool clear = true;
  for (int i = 0; i < 300; ++i) {
    p.update(w, {});
    p.occupy(w);
    px::step(w);
    p.release(w);
    clear = clear && !p.blocked(w, p.left(), p.top());
  }
  CHECK(clear);                          // nothing ever fell into its box
  CHECK_EQ(p.top() + 32, 160);           // still standing on the floor
  CHECK_EQ(w.get(68, p.top() - 1), f.ids.sand);  // with sand on its head
  CHECK_EQ(w.count(f.ids.sand), sand);   // and no cell left behind or lost
  CHECK_EQ(w.count(f.materials.require("body")), std::size_t{0});
  px::PlayerInput right;
  right.right = true;
  for (int i = 0; i < 120; ++i) {
    p.update(w, right);
    p.occupy(w);
    px::step(w);
    p.release(w);
  }
  int on_floor = 0;  // the pile fell where it stood
  for (int x = 50; x < 90; ++x) on_floor += w.get(x, 159) == f.ids.sand ? 1 : 0;
  CHECK(on_floor > 10);
}

TEST(player_is_pushed_out_when_sand_is_painted_into_it) {
  PlayerFixture f;
  px::World w(4, 3, f.materials, 2);
  w.fill_rect(0, 160, w.width(), 32, f.ids.stone);
  px::Player p;
  p.place_on_ground(w, 60);
  w.fill_rect(60, 150, 16, 10, f.ids.sand);  // under its feet
  for (int i = 0; i < 20; ++i) p.update(w, {});
  CHECK(!p.blocked(w, p.left(), p.top()));
  CHECK_EQ(p.top() + 32, 150);  // on top of it
}

TEST(player_crosses_the_wrap_seam) {
  PlayerFixture f;
  px::World w(4, 2, f.materials, 3, px::EdgeX::Wrap);
  w.fill_rect(0, kFloor, w.width(), 28, f.ids.stone);
  px::Player p;
  p.place_on_ground(w, static_cast<float>(w.width() - 40));
  px::PlayerInput right;
  right.right = true;
  run(p, w, 60, right);
  CHECK(p.x() > static_cast<float>(w.width()));  // continuous position past the seam...
  CHECK(p.on_ground());      // ...still walking on the (wrapped) floor
  CHECK_EQ(p.top() + 32, kFloor);
}

TEST(player_treats_world_edges_and_unloaded_chunks_as_walls) {
  PlayerFixture f;
  px::World w(4, 2, f.materials, 4, px::EdgeX::Wall, px::ChunkStorage::Streamed);
  for (int cx = 0; cx < 2; ++cx)
    for (int cy = 0; cy < 2; ++cy) w.load_chunk(cx, cy, std::make_unique<px::Chunk>());  // only the left half
  w.fill_rect(0, kFloor, 128, 28, f.ids.stone);
  px::Player p;
  p.place_on_ground(w, 60);
  px::PlayerInput right, left;
  right.right = true;
  left.left = true;
  run(p, w, 120, right);
  CHECK_EQ(p.left() + 16, 128);  // stopped at the unloaded chunk
  run(p, w, 200, left);
  CHECK_EQ(p.left(), 0);  // stopped at the world edge
}
