#pragma once

#include <cstdint>

namespace px {

inline constexpr int kPlayerSpriteWidth = 16;
inline constexpr int kPlayerSpriteHeight = 32;

enum class PlayerPose : std::uint8_t { Stand, WalkA, WalkB, Jump };

/// Draws the 16x32 player sprite with its top-left at (x, y) into an ARGB
/// buffer of `width` x `height` pixels with row stride `pitch`, clipped to the
/// buffer. Drawn facing right; `facing_left` mirrors it.
void draw_player(std::uint32_t* pixels, int pitch, int width, int height, int x, int y, PlayerPose pose,
                 bool facing_left);

/// Picks the pose from movement: standing, alternating walk frames, or jumping.
PlayerPose player_pose(bool on_ground, int walk_steps);

}  // namespace px
