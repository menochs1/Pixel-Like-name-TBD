# Pixel-Like-name-TBD

A 2D pixel-physics sandbox focused on emergent material interactions and fully
destructible terrain, written in C++20. Every pixel is a simulated cell of
material: sand piles up, water flows and levels out, oil burns, lava turns
water to steam and stone, and heat spreads through everything.

Development goals and guidance live in
[`.github/copilot-instructions.md`](.github/copilot-instructions.md).

## Status

Roadmap steps 1 (cellular automata, sand, water, stone, chunks) and 2 (fire,
smoke, oil, temperature) are in place, and step 3 has started with seeded
world generation and a world that wraps around:

- Chunked world (64×64 chunks) stored as a structure of arrays.
- Active-region tracking: each chunk keeps a dirty rectangle, so settled areas
  cost nothing to simulate.
- Data-driven materials: behaviour comes from a material's *phase*
  (solid / powder / liquid / gas) plus properties such as density and
  dispersion. Adding a material means adding a `MaterialDef`, not engine code.
- Deterministic: all randomness is a hash of (seed, x, y, tick), so the same
  seed and inputs always produce the same world, regardless of update order.
- Every cell has a temperature. Heat flows between neighbours by
  conductivity; fire and lava are heat sources; air drifts back to ambient.
- Burning, boiling, condensing, melting and freezing are all one data rule:
  above/below a temperature, turn into another material with some chance.
- Pair reactions (lava + water → stone + steam, fire + water → doused) come
  from a reaction table, not engine code. A reaction can **derive** its
  products from its parents (oil + sand → tar whose density, colour and
  flammability are mixed from the oil and sand) and release or absorb heat.
- Per-pixel properties: two pixels of the same element can differ. A pixel
  with its own density, conductivity, colour, ignition point or flammability
  holds a derived variant of its element. Each pixel also carries its own
  temperature and health (wear).
- Simulation is fully separate from rendering and runs headless.
- Climate and weather: it gets colder the higher you go; the world is cut
  into weather areas whose moisture builds up, drifts with the wind and is
  fed by steam rising out of sight, and rains (or snows) into view from above
  when wet enough. Wind pushes smoke, steam, flames, rain and snow.
- A parallax background: sky, far mountains and hills behind the world, a
  rock wall behind caves.
- Sandbox tools: paint and erase any material, heat and cool brushes (melt
  rock into lava, harden lava back into rock, boil, freeze, light and put out
  fires), and a pixel inspector that shows every property of the pixel under
  the cursor.
- Worlds can wrap around horizontally (`EdgeX::Wrap`): walk far enough to one
  side and you are back where you started. Sand, liquids, heat and painting all
  cross the seam as if it were not there.
- Seeded world generation: biomes, hills, caves, water and oil pools, sealed
  magma chambers, coal and trees, all from one seed. Every chunk is a pure
  function of (seed, x, y).
- Save/load and chunk streaming: only the chunks around the camera are in
  memory. Chunks you changed are written to disk when you move away; chunks
  you never changed are regenerated from the seed when you come back. Saves
  store only the changed chunks.

### Materials

| Material | Phase | Behaviour |
| --- | --- | --- |
| stone | solid | conducts heat; melts into lava above 1500 °C (only a heat brush gets that hot) |
| sand | powder | piles, sinks through liquids |
| water | liquid | boils at 100 °C, freezes below −2 °C, douses fire |
| oil | liquid | floats on water, ignites at 200 °C and burns for ~2.5 s |
| wood | solid | ignites at 260 °C and burns slowly |
| fire | solid | 900 °C heat source with a lifetime; throws flames, leaves smoke; goes out if cooled below 300 °C |
| flame | gas | short-lived rising tongue of fire |
| smoke | gas | rises and fades |
| steam | gas | rises, condenses back to water below 90 °C; rising out of sight, it feeds the rain |
| snow | powder | falls instead of rain where the air is freezing; melts above 1 °C |
| lava | liquid | 1200 °C heat source; with water makes stone + steam; hardens into stone if cooled below 600 °C |
| ice | solid | starts at −30 °C, melts above 1 °C |
| dirt | solid | topsoil |
| grass | solid | ignites at 200 °C, burns quickly |
| coal | solid | hard to light (350 °C), smoulders for ~15 s |
| leaves | solid | ignites at 180 °C, burns fast |
| basalt | solid | perfect insulator (conductivity 0); seals magma chambers |
| tar | liquid | made when oil soaks into sand; properties derived from both; burns long and slow |

## Download (Windows)

Prebuilt 64-bit Windows builds are attached to the
[GitHub Releases](https://github.com/menochs1/Pixel-Like-name-TBD/releases)
page (`PixelSandbox-win64.zip`); compiled builds are not kept in the
repository. Download the zip, right-click → **Extract All**, then run
`pixel_sandbox.exe` (keep `SDL2.dll` beside it). Windows SmartScreen may warn
because the exe is unsigned: choose **More info → Run anyway**.

## Building

Requirements: CMake 3.20+, a C++20 compiler, and SDL2 development files for the
interactive app (the core library, tests and benchmark need nothing else).

```bash
# Debian/Ubuntu: sudo apt install libsdl2-dev   macOS: brew install sdl2
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Cross-compile a Windows build from Linux (needs `g++-mingw-w64-x86-64-posix` and
the [SDL2 MinGW dev package](https://github.com/libsdl-org/SDL/releases)):

```bash
cmake -S . -B build-win -DCMAKE_BUILD_TYPE=Release -DPX_BUILD_TESTS=OFF \
  -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
  -DCMAKE_PREFIX_PATH=/path/to/SDL2-2.30.9/x86_64-w64-mingw32
cmake --build build-win
```

Ship `pixel_sandbox.exe` with `SDL2.dll` (from the SDL package's `bin/`) beside it;
the C++ runtime is linked statically.

Run the sandbox:

```bash
./build/pixel_sandbox
```

The sandbox opens on a generated 49152×4608 world that wraps around
horizontally, viewed through a 640×360 camera, with terrain generated at
scale 3 (hills, biomes, caves, pools and trees three times the size of the
original 16384×1536 world's). It is streamed: about 270 chunks (~8 MiB) are
in memory at a time instead of all 55296 (~1.5 GiB).
`--seed N` picks the world, `--load DIR` starts from a save, and `--demo`
opens the small hand-built demo scene instead (an oil pit beside a wooden
tower, a water basin and a lava crucible).

| Input | Action |
| --- | --- |
| `A` / `D`, ← / → | Walk |
| `W` / ↑ / `Space` | Jump (hold for higher); in water: swim up |
| `S` / ↓ | Swim down |
| `Tab` | Free camera on/off: `WASD` / arrows then move the view (`Shift` = faster) and the character waits |
| Middle mouse drag | Move the camera (free camera) |
| `Home` | Back to the starting point |
| Left mouse | Use the current tool: paint, heat or cool |
| Right mouse | Erase |
| Mouse wheel | Brush size |
| `1`–`5` | Sand / water / stone / oil / wood |
| `6`–`0` | Fire / lava / ice / steam / smoke |
| `Q` / `E` | Previous / next material (all, including dirt, coal, basalt…); choosing a material switches to painting |
| `H` | Heat brush |
| `J` | Cool brush |
| `I` | Pixel inspector on/off (on at start); its first line also shows the wind |
| `[` / `]` | Nudge the wind left / right (it also changes by itself) |
| `K` | Make it rain here (fills this area's weather with moisture) |
| `T` | Heat view (temperature map) |
| `P` | Pause / resume |
| `.` | Single step while paused |
| `F5` | Save (to `saves/quicksave`) |
| `F9` | Load (from `saves/quicksave`) |
| `R` | Regenerate the world (same seed; changes since the last load are discarded) |
| `N` | New world (new seed) |
| `C` | Clear world |
| `F` | Show active (dirty) regions |

The window title shows FPS, simulation time per step, active chunk count, the
seed and the camera position.

Run the tests, benchmark and stress test:

```bash
ctest --test-dir build --output-on-failure
./build/pixel_bench --scene rain --steps 600            # add --ppm out.ppm to save the final frame
./build/pixel_bench --scene gen --seed 42 --ppm w.ppm   # generate a world, time it, save a picture
./build/pixel_stress                                    # worst-case load, editing fuzz, long run; --quick for a short pass
```

For memory-safety checks, build with AddressSanitizer and UBSan:

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPX_SANITIZE=ON
cmake --build build-asan && ./build-asan/px_tests && ./build-asan/pixel_stress
```

## Layout

```
src/core/     material registry, chunk storage, world, simulator (no dependencies)
src/world/    seeded world generation, demo scenes
src/entities/ the player character (movement and collision; no rendering)
src/sandbox/  sandbox tools: heat/cool brushes, pixel inspector (no rendering)
src/save/     chunk encoding, saves, chunk store and streaming
src/render/   World -> ARGB pixel buffer, parallax background, built-in 3x5 font (no SDL)
src/app/      SDL2 window, input, fixed 60 Hz timestep
tools/        headless benchmark / snapshot tool, stress test
tests/        unit tests (tiny built-in harness, run via CTest)
```

## How the simulation works

Each step (`px::step`, fixed at 60 per second):

1. Every chunk's pending dirty rect becomes its current rect.
2. Chunks are visited bottom-to-top; inside each dirty rect, rows run
   bottom-to-top so falling material moves once per step. Horizontal scan
   direction alternates every step to avoid a left/right bias.
3. Each cell first updates its state, if it can change at all (room-temperature
   sand and water skip this entirely):
   - heat sources reset to their temperature; cells off ambient exchange heat
     with their 4 neighbours, conserving the total. A heat source cooled below
     its low-transition point stops re-heating and transforms (lava hardens,
     fire goes out); ordinary conduction never cools one that far;
   - reactions with a neighbour, temperature transitions, lifetime countdown
     and emission (fire throwing flames) may turn it into something else.
4. Then it runs the movement rule for its phase:
   - **Powder** falls (accelerating up to 3 cells/step), else slides diagonally.
   - **Liquid** falls, slides diagonally, else flows sideways up to its
     `dispersion` distance, stopping early over drops. Each liquid cell keeps
     the direction it last flowed in (`velocity_x`) until blocked, so water
     surges and spreads like a current instead of jittering in place.
     A cell that keeps bouncing back and forth without dropping (about 240
     reversals) *settles*: it then only steps into an adjacent drop or under
     a lighter liquid. A lake with a partial top row goes to sleep, while a
     breached container still drains. Only reversals count, so a cell crossing
     a long, slightly higher stretch keeps going until it finds the edge.
   - **Gas** rises with its `rise_chance`, bubbles up through liquids, sorts
     above denser gases and drifts sideways.
   - Movers displace lighter liquids/gases (sand sinks through water).
5. Every move wakes the 3×3 neighbourhood of both cells, possibly in
   neighbouring chunks. The wake goes into the current step's dirty rect (so a
   cell freed mid-step, like the one above a gap, still moves this step) and
   into the next step's. Cells that cannot move are not re-woken, so finished
   regions go to sleep.

   Any temperature change wakes its 3×3 too, since a sleeping neighbour may now
   have a gap worth exchanging. Cells that are still changing (burning,
   cooling, reacting) keep themselves awake.

Chunking is purely an optimisation: tests check that the chunked simulation
produces exactly the same materials and temperatures as updating every cell
every step, including with fire, lava, steam and ice.

### Climate and weather

Settings in `World::Weather` (`src/core/world.hpp`), the regional weather in
`src/core/weather.hpp`. All off by default; the app turns them on.

- **Colder higher up.** Above the mean ground level (`ground_y`) the
  temperature everything rests at (`World::ambient_at`) drops by `lapse_rate`
  per row: in the app 0.03 °C, so it freezes about 730 cells up. Air relaxes
  toward that temperature, generated terrain and sky start at it (so a cold
  sky is at rest and costs nothing), and a cell resting past one of its own
  transition points still changes: water left on a cold peak freezes, snow
  carried into warmer air melts.
- **Weather areas.** The world is cut into columns `region_width` wide
  (1024 in the app), each with its own moisture. It builds up by itself
  (`humidity_rate`), drifts with the wind high up, and is fed by world
  events: steam that rises above `vapour_y` (300 cells above the ground in the
  app, out of sight) leaves the world and becomes moisture over where it rose.
  Boil a lake and rain comes to that area.
- **Rain.** An area starts raining at `rain_start` and stops at `rain_stop`,
  losing about `rain_per_step` cells of water a step (more when very wet).
  Drops are placed along the top row of the simulated box, which reaches above
  the view, so rain falls into view from above; where the air there is below
  freezing, snow falls instead. Rain over areas nobody is looking at still
  uses moisture up. No rain falls into caves. Falling rain is drawn as short
  streaks so single drops read against the sky.
- **Wind** (`World::wind_at`): wanders between ±`max_wind` over about a minute
  plus gusts, as a function of seed and step (deterministic), plus a
  `wind_bias` the player can nudge. It is calm below the ground level and
  builds to full strength 120 cells above it. `windblown` materials drift with
  it: gases (steam, smoke, flame) and, while falling, water and snow. Drifting
  cells move on the same steps, from the downwind edge, so a body of smoke or
  a sheet of rain moves as one.
- **Crowding** is a general data rule (`crowd_count`, `crowd_to`,
  `crowd_chance`, `lone_chance`): a cell with enough neighbours of its own kind
  turns into something else, e.g. a dense gas condensing.

Water is conserved through the cycle except for the abstract parts: moisture
building up by itself, and rain that falls where nobody is looking. The
weather settings and the areas' moisture are not saved; a loaded world starts
its weather afresh. Chunked simulation stays identical to updating every cell
with cold air, vapour leaving, rain, snow and wind (tested).

### Background

`src/render/background.hpp`: a sky gradient and three silhouette layers
(far mountains, hills, a near wooded ridge) drawn behind the world wherever it
is empty, moving at 12%, 30% and 50% of the camera's speed, and paler with
distance. Below the terrain surface (from the generator), empty cells show a
rock wall instead, so caves have a backdrop. Silhouettes and the rock texture
are computed once and looked up per pixel; purely visual.

### The player

`src/entities/player.hpp`: a 16×32 character that walks, jumps, swims and
stands on the world. It is simulation, not rendering: deterministic and
tested headless. `src/render/sprites.cpp` draws it, with a standing pose, a
two-frame walk and a jump pose, mirrored when facing left.

- **Collision:** solids and powders (sand) block it; air, liquids and gases do
  not. Materials marked `passable` are scenery it walks through: generated
  tree trunks (`trunk`) and leaves, which are still solid to the simulation
  and still burn. Painted wood stays solid. The world edges and unloaded
  chunks count as walls. It moves one axis
  at a time, cell by cell, so it never passes through thin walls.
- **Movement:** walks at 90 cells/s with a little acceleration, steps up
  ledges up to 4 cells high (the terrain is pixel-rough), stays on the ground
  walking down slopes, and jumps about 36 cells. A short tap gives a lower
  hop, and a jump still works for a few steps after walking off a ledge.
- **Water:** with at least half the body under, it is buoyant and slowed;
  jump swims up, down dives, and a jump near the surface leaps out.
- **Solid to the simulation:** during each step its body fills the empty
  cells of its box with a hidden `body` material (`Player::occupy`), and they
  are emptied right after (`release`). Sand piles on its head and slides off
  its shoulders, water flows around it, and when it walks away what rested on
  it falls. The body is never in the world between steps, so it is never
  drawn, inspected or saved. If something is painted into its box anyway, it
  is pushed out upward.
- **Tuning:** every number (size, speeds, gravity, jump, step-up, swimming)
  is a field of `PlayerParams`.
- Saves remember where it stood (header format version 2 and later; older
  saves load with the character dropped in at the camera).

### Sandbox tools

`src/sandbox/tools.hpp` holds the tools, independent of any window, so they
are tested headless like the simulation.

- **Heat and cool brushes** (`apply_thermal_brush`) pull temperatures toward a
  target (3000 °C or −100 °C), closing a share of the gap over the inner half
  of the brush and fading to the rim. They work on anything, air included,
  and never overshoot. The app applies them once per simulation step while
  the button is held, so the effect does not depend on frame rate (and they
  pause with the simulation). Everything else follows from the materials'
  own rules: rock melts into lava, lava hardens, water boils or freezes, fuel
  ignites, fire goes out.
- **Pixel inspector** (`inspect` / `describe`): material and, for a derived
  material, what it derives from; position; whether it is simulating;
  temperature; density and conductivity; health; its transitions (above /
  below which temperature it turns into what, and how fast); lifetime or
  motion. The app draws it with a built-in 3×5 pixel font
  (`src/render/text.hpp`), at the top left (or top right when the cursor is
  there).

### Wrapping worlds

`World(…, EdgeX::Wrap)` joins the left and right edges. Every accessor accepts
any x and maps it onto the ring, so simulation code reads `x - 1` or `x + 8`
without special cases. The world is a whole number of chunks wide, so a cell
and its wrapped copy share the same chunk-local index; only chunk columns
wrap. The top and bottom stay solid. Tests check that chunked simulation is
still bit-identical to updating every cell on a wrapping world, and that
water levels out all the way round.

## World generation

`WorldGenerator` (`src/world/world_gen.hpp`) builds a world from a seed:

- **Surface:** rolling hills whose shape depends on the biome: sandy desert
  dunes, grassy meadows with trees and lakes in the deepest valleys, and bare
  rocky highlands with peaks. Biome terrain is blended with smooth weights, so
  there are no cliffs where biomes meet.
- **Underground:** stone with coal seams and pockets of dirt and sand. Winding
  tunnels and large caverns grow with depth, with some regions riddled and
  others nearly solid. A few tunnels break through to the surface as entrances.
- **Pools:** oval cavities filled with water (shallow), water or oil (middle)
  up to a flat level. Below that level, a stone shell overrides the caves, so
  the liquid is contained and starts at rest. Caves can still break into the
  air above it.
- **Magma chambers:** the deepest pools are lava, completely filled and sealed
  in insulating basalt. Lava open to air or rock would keep heating its
  surroundings forever and keep those chunks awake; sealed, it sleeps until
  you dig in.

Design points:

- **Chunk-independent.** Each cell is a pure function of (seed, x, y), with no
  flood fills or passes that carry state between chunks. A chunk generated
  alone, in any order or on any thread, comes out identical (tested), which
  is what save/load and streaming need. `generate()` fills chunks in parallel.
- **Seamless wrap.** All noise is periodic in x with the world width, so
  terrain, caves and biomes join up exactly across the seam (tested).
- **Generated at rest.** Only cells that can move or change are woken. On the
  4096×1536 app world, generation takes ~0.3 s on 4 threads, and the world
  then settles to a handful of active chunks, averaging ~0.2 ms per step.
- **Scales with size.** Depths are fractions of the world height and feature
  sizes are rounded to fit the width, so any size works.
- **Feature scale.** `WorldGenParams::scale` multiplies the size of every
  feature: biomes, hills, dunes, peaks, soil depth, tunnels, caverns, pools,
  deposits and trees. A world three times larger in each direction with
  scale 3 is the same world, three times bigger (tested: biomes, surface and
  underground line up). The app uses scale 3. Saves record the scale (header
  format version 3).

### Adding a material

```cpp
px::MaterialDef acid;
acid.name = "acid";
acid.phase = px::Phase::Liquid;
acid.density = 1100;
acid.color = {120, 220, 60};
acid.dispersion = 6;
registry.add(acid);
registry.add_reaction("acid", "stone", "empty", "smoke", 0.05f);  // acid eats stone
registry.finalize();
```

Only the first material named in a reaction looks for partners, so name the
rarer or more active one first.

### Derived materials

A reaction can compute a product's properties from the two cells that
reacted, instead of producing a fixed material:

```cpp
px::ReactionDef r;
r.a = "oil"; r.b = "sand";
r.a_to = "tar"; r.b_to = "tar";
r.chance = 0.02f;
r.heat = 0;                                         // degrees added to both products
px::Derivation d;
d.density = {px::Derivation::Op::Mean};             // also A, B, Min, Max, Mix(weight)
d.color = {px::Derivation::Op::Mix, 0.6f};
d.ignition = {px::Derivation::Op::A};
d.flammability = {px::Derivation::Op::Mix, 0.5f};
r.a_derive = r.b_derive = d;
registry.add_reaction(r);
```

- The product element (`tar`) supplies phase and behaviour. Each rule sets one
  property from the parents: density, conductivity, colour, ignition
  temperature or flammability. Parents may themselves be derived, so chains
  of reactions compound.
- `MaterialRegistry::derive` adds the result to the material table at runtime
  as a variant of its element (`tar#1`, `tar#2`…). Results are rounded to fixed
  steps and interned, so equal results share one id and chains converge on a
  bounded set (tested).
- Variants react like their base element: the reaction table is indexed by
  base element, so it stays sized to the authored materials.
- Up to `kMaxMaterials` (16384) materials exist in total; storage is reserved
  up front so deriving mid-step never moves the property table. When full,
  reactions produce the plain element.

This is how pixels hold their own properties without storing every property
in every pixel. Fixed properties live in a (possibly derived) material;
changing state (temperature, health, motion) lives in the pixel.

A per-cell bit marks cells that have already moved or updated this step, so
nothing moves twice. The bits are stamped with the step number and cleared on
first use in a new step, so chunks a step never touches cost nothing.

### Cell layout

Cells are stored per chunk as a structure of arrays, packed by what each field
really needs (`src/core/chunk.hpp`):

| Field | Bits | Holds |
| --- | --- | --- |
| material | 16 | material id |
| thermal | 16 | temperature (12 bits, −273…3822 °C) + colour shade (4 bits) |
| motion | 16 | powders/liquids: fall speed or settle count + flow direction; everything else: remaining lifetime |
| health | 8 | share of the material's durability left (`World::damage`) |
| updated | 1 | already moved this step |

No material needs both motion and a lifetime, so they share a field. The
registry rejects a powder or liquid with a lifetime. Simulation code goes
through accessors (`temperature()`, `flow()`, `life()`…), so the packing stays
in one place. Packing cut memory from 10 to ~6 bytes per cell with no
measurable change in speed; health brings it to 7 (43 MiB for the app world).
A state-flags byte will join when the first flag (wet, charged…) needs it.

## Saving and streaming

`src/save/` keeps a world on disk as well as in memory:

- **Chunk encoding** (`chunk_codec`): every cell's material, temperature,
  shade, motion/lifetime and health, plus the chunk's pending work, so a
  chunk read back continues exactly as if it had never left. Each field is
  run-length encoded; the shade is stored relative to the position-derived
  shade cells get when placed. A checksum rejects damaged data. Terrain chunks
  come out at tens to a few hundred bytes (28.6 KiB in memory).
- **Touched chunks.** A chunk records whether anything changed it (a write, a
  wake-up, pending work consumed) since it was generated or loaded. Untouched
  chunks are never written: they come back identical by regenerating them
  from the seed (or empty, in a world without a generator).
- **Saves** (`world_save`): a directory with a header (size, edges, seed, step
  counter, generator settings, camera position, material table) and one file
  per touched chunk. Materials are stored by name and derived materials by
  their recipe, so saves load even when materials are added or reordered. A
  save is written beside the target and then swapped in, so a failed save
  never leaves a broken one. A freshly generated world saves in a few
  kilobytes.
- **Streaming** (`ChunkStreamer`, `ChunkStore`): around the simulated region,
  chunks one ring out are loaded at once, two rings out are prefetched a few
  per frame, and anything beyond three rings is unloaded. Touched chunks go to
  a session directory in the system temp folder, layered over the loaded save
  (if any), which is never modified until you save again. Unloaded chunks act
  as walls, but the loaded margin means nothing that simulates ever reaches
  one.

A test runs a streamed world through a camera path that crosses the wrap
seam, with fires, water and sand painted along the way and a save halfway,
next to the same world held fully in memory. The two must end identical, and
the halfway save must load as the in-memory world was at that moment. With
unloading deliberately broken, the test fails.

## Performance

A step costs what is loaded and active, not the size of the world: the
simulator loops over loaded chunks only, and each chunk row keeps a list of
the columns with work this step (a wake-up during the step adds to it). The
scan order is exactly the same as visiting every column. Before this, an
idle step in the 49152×4608 world took ~7 ms just walking empty columns; it
now takes ~0.03 ms.

Measured with `pixel_bench --scene rain --steps 600` (2048×1152 world,
~118k falling sand and water cells at the start, single thread, Release):

| Metric | Value |
| --- | --- |
| Mean step | ~5.0 ms |
| p95 step | ~8 ms |
| Cells scanned per step | ~166k |
| Memory | 7 bytes + 1 bit per cell (~28.6 KiB per chunk; 10M cells ≈ 68 MiB) |

`pixel_stress` also covers a small world fully on fire (p95 3.5 ms) and lava
raining into a lake (p95 3.1 ms). Timings vary run to run on shared machines by
±15%.

### Slower machines

The app keeps its cost bounded so it stays playable on low-end laptops:

- **Only the area around the camera is simulated**: the view plus a 2-chunk
  margin (~165 of the 1536 chunks). Everything further away is paused, keeping
  its pending work, and resumes when the camera returns (`step(world, region)`).
  Activity off screen used to cost as much as activity on screen.
- **No catch-up spiral**: at most 2 steps per frame and 10 ms of simulation per
  frame. When a machine cannot keep up, the backlog is dropped and the game runs
  in slow motion at a steady frame rate, instead of running up to 4 steps a
  frame, falling further behind and stuttering.
- **Heat is cheaper**: cells exchange heat on a checkerboard (each neighbour pair
  once per step instead of from both sides), neighbours inside a chunk are read
  by index offset, and one 5×5 wake replaces up to five 3×3 wakes.
- **Heat view** colours come from a lookup table (7× faster to draw).

Measured with a scripted heavy painting session (fire, oil, lava, water, sand
and steam dragged across the whole view for 50 s) on a 2.1 GHz server core:

| Build | Before | After |
| --- | --- | --- |
| Release | 12 fps avg, 6 min | 50 fps avg, 37 min |
| Sanitizer build (stand-in for a ~5–8× slower CPU) | 3 fps avg, 1 min | 13 fps avg, 7 min |

## Known limitations / next steps

- Single-threaded. A 10-million-cell world with ~3M cells moving at once takes
  ~120 ms per step; the app avoids this by simulating only around the camera.
  Multithreading is the next big performance step for heavy scenes in view; it
  has to keep the world-wide bottom-to-top row order that stops liquids
  terracing at chunk edges.
- Heat moves at most one neighbour per step, so it spreads ~15 cells through
  stone in 10 seconds. Temperatures are whole degrees and a 1° gap does not
  flow, so buried layers can settle a degree per layer above ambient.
- Steam and smoke drift as scattered particles rather than billowing clouds.
- The world is still finite: 49152×4608 for the app, with an index slot per
  chunk (8 bytes each, 432 KiB), even though only the chunks near the camera
  are in memory. Unbounded worlds would need a sparse chunk index.
- Chunks are loaded on the main thread; prefetching a few per frame keeps
  this smooth at camera speed, but a much faster camera could stutter.
  Generating on a worker thread would fix that.
- Only horizontal wrapping; the top and bottom are solid.
- Derived materials are never freed: a variant no cell uses any more keeps its
  slot until the registry is re-finalized. Rounding keeps the count low in
  practice. Saves store every variant the registry knows, used or not.
- Nothing deals damage yet (`World::damage` is ready for digging tools and
  explosions).
- Flicker (fire, lava) is now purely a rendering effect animated from the
  world tick, so a still lava pool costs the simulation nothing.
