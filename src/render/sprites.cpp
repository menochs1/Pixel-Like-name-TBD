#include "render/sprites.hpp"

namespace px {
namespace {

// The player, facing right, as pictures. Upper body (rows 0-18) is shared by
// every pose; the legs (rows 19-31) change. Keys: k outline, h hair, s skin,
// e eye, c coat, d coat shade, b belt, p trousers, o boots, '.' transparent.
constexpr const char* kUpper[19] = {
    "......kkkkk.....",  //  0
    ".....khhhhhk....",  //  1
    "....khhhhhhhk...",  //  2
    "....khhhhhhhhk..",  //  3
    "....khhssssshk..",  //  4
    "....khssssseskk.",  //  5
    "....kssssssssk..",  //  6
    ".....ksssssssk..",  //  7
    "......kkssssk...",  //  8
    ".....kkcckkk....",  //  9
    "....kccccccck...",  // 10
    "...kcccccccccck.",  // 11
    "...kdkcccccckdk.",  // 12
    "...kdkcccccckdk.",  // 13
    "...kdkcccccckdk.",  // 14
    "...kdkcccccckdk.",  // 15
    "...kdkbbbbbbkdk.",  // 16
    "...ksk.ppppkksk.",  // 17
    "....kk.ppppk.kk.",  // 18
};

constexpr const char* kLegsStand[13] = {
    ".....kppppppk...", ".....kppkkppk...", ".....kppkkppk...", ".....kppkkppk...",
    ".....kppkkppk...", ".....kppkkppk...", ".....kppkkppk...", ".....kppkkppk...",
    ".....kppkkppk...", ".....kookkook...", ".....kookkook...", "....koookkoook..",
    "....kkkkkkkkkk..",
};

constexpr const char* kLegsWalkA[13] = {
    ".....kppppppk...", "....kppkkkppk...", "....kppk.kppk...", "...kppk..kppk...",
    "...kppk...kppk..", "...kppk...kppk..", "..kppk....kppk..", "..kppk.....kppk.",
    "..kppk.....kppk.", "..kook.....kook.", "..kook.....kook.", ".koook....koook.",
    ".kkkkk....kkkkk.",
};

constexpr const char* kLegsWalkB[13] = {
    ".....kppppppk...", ".....kppkkppk...", ".....kppkkppk...", "......kppkppk...",
    "......kppkppk...", "......kpppppk...", ".......kpppk....", ".......kpppk....",
    ".......kpppk....", ".......kooook...", ".......kooook...", "......koooook...",
    "......kkkkkkk...",
};

constexpr const char* kLegsJump[13] = {
    ".....kppppppk...", "....kppkkkppk...", "...kppk..kppk...", "..kppk...kppk...",
    "..kppk...kppk...", "..kook...kook...", "..kook...kook...", ".koook..koook...",
    ".kkkkk..kkkkk...", "................", "................", "................",
    "................",
};

std::uint32_t colour(char key) {
  switch (key) {
    case 'k': return 0xFF1E1820U;  // outline
    case 'h': return 0xFF6B3F2AU;  // hair
    case 's': return 0xFFE8BE96U;  // skin
    case 'e': return 0xFF141420U;  // eye
    case 'c': return 0xFF3C6EAAU;  // coat
    case 'd': return 0xFF2C5282U;  // coat shade (arms)
    case 'b': return 0xFF5A4030U;  // belt
    case 'p': return 0xFF463C5AU;  // trousers
    case 'o': return 0xFF3A2A1EU;  // boots
    default: return 0;             // transparent
  }
}

}  // namespace

PlayerPose player_pose(bool on_ground, int walk_steps) {
  if (!on_ground) return PlayerPose::Jump;
  if (walk_steps == 0) return PlayerPose::Stand;
  return (walk_steps / 8) % 2 == 0 ? PlayerPose::WalkA : PlayerPose::WalkB;  // a stride every 8 steps
}

void draw_player(std::uint32_t* pixels, int pitch, int width, int height, int x, int y, PlayerPose pose,
                 bool facing_left) {
  const char* const* legs = kLegsStand;
  if (pose == PlayerPose::WalkA) legs = kLegsWalkA;
  else if (pose == PlayerPose::WalkB) legs = kLegsWalkB;
  else if (pose == PlayerPose::Jump) legs = kLegsJump;
  for (int r = 0; r < kPlayerSpriteHeight; ++r) {
    const int py = y + r;
    if (py < 0 || py >= height) continue;
    const char* row = r < 19 ? kUpper[r] : legs[r - 19];
    for (int k = 0; k < kPlayerSpriteWidth; ++k) {
      const std::uint32_t c = colour(row[facing_left ? kPlayerSpriteWidth - 1 - k : k]);
      const int px_ = x + k;
      if (c == 0 || px_ < 0 || px_ >= width) continue;
      pixels[static_cast<std::ptrdiff_t>(py) * pitch + px_] = c;
    }
  }
}

}  // namespace px
