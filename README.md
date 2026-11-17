# Pixel-Like-name-TBD

A 2D pixel-physics sandbox focused on emergent material interactions and fully
destructible terrain, written in C++20. Every pixel is a simulated cell of
material: sand piles up, water flows and levels out, oil burns, lava turns
water to steam and stone, and heat spreads through everything.

Development goals and guidance live in
[`.github/copilot-instructions.md`](.github/copilot-instructions.md).

## Status

Roadmap steps 1 (cellular automata, sand, water, stone, chunks) and 2 (fire,
smoke, oil, temperature) are in place:

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
  from a reaction table, not engine code.
- Simulation is fully separate from rendering and runs headless.

### Materials

| Material | Phase | Behaviour |
| --- | --- | --- |
| stone | solid | inert, conducts heat |
| sand | powder | piles, sinks through liquids |
| water | liquid | boils at 100 °C, freezes below −2 °C, douses fire |
| oil | liquid | floats on water, ignites at 200 °C and burns for ~2.5 s |
| wood | solid | ignites at 260 °C and burns slowly |
| fire | solid | 900 °C heat source with a lifetime; throws flames, leaves smoke |
| flame | gas | short-lived rising tongue of fire |
| smoke | gas | rises and fades |
| steam | gas | rises, condenses back to water below 90 °C |
| lava | liquid | 1200 °C heat source; with water makes stone + steam |
| ice | solid | starts at −30 °C, melts above 1 °C |

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

| Input | Action |
| --- | --- |
| Left mouse | Paint selected material |
| Right mouse | Erase |
| Mouse wheel | Brush size |
| `1`–`5` | Sand / water / stone / oil / wood |
| `6`–`0` | Fire / lava / ice / steam / smoke |
| `T` | Heat view (temperature map) |
| `Space` | Pause / resume |
| `.` | Single step while paused |
| `R` | Rebuild demo scene |
| `C` | Clear world |
| `D` | Show active (dirty) regions |

The window title shows FPS, simulation time per step and active chunk count.
The demo scene has an oil pit with a plank leading into a wooden tower (drop
fire on the oil), a water basin, and a lava crucible on the right.

Run the tests, benchmark and stress test:

```bash
ctest --test-dir build --output-on-failure
./build/pixel_bench --scene rain --steps 600            # add --ppm out.ppm to save the final frame
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
src/world/    scene / world generation
src/render/   World -> ARGB pixel buffer (no SDL)
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
     with their 4 neighbours, conserving the total;
   - reactions with a neighbour, temperature transitions, lifetime countdown
     and emission (fire throwing flames) may turn it into something else.
4. Then it runs the movement rule for its phase:
   - **Powder** falls (accelerating up to 3 cells/step), else slides diagonally.
   - **Liquid** falls, slides diagonally, else flows sideways up to its
     `dispersion` distance, stopping early over drops. Each liquid cell keeps
     the direction it last flowed in (`velocity_x`) until blocked, so water
     surges and spreads like a current instead of jittering in place.
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

A per-cell clock byte stops a cell that has already moved this step from being
updated again.

## Performance

Measured with `pixel_bench --scene rain --steps 600` (2048×1152 world,
~118k falling sand and water cells at the start, single thread, Release):

| Metric | Value |
| --- | --- |
| Mean step | ~5.6 ms |
| p95 step | ~10.5 ms |
| Cells scanned per step | ~166k |
| Memory | 10 bytes/cell (~40 KiB per chunk; 10M cells ≈ 100 MiB) |

`pixel_stress` also covers the app-sized world fully on fire (p95 7.4 ms) and
lava raining into a lake (p95 6.8 ms). Timings vary run to run on shared
machines by ±15%.

## Known limitations / next steps

- A liquid surface with a partial top layer keeps shuffling sideways and never
  fully sleeps; a settle heuristic will fix this.
- Single-threaded. A 10-million-cell world with ~3M cells moving at once takes
  ~115 ms per step (the app's 512×320 world and the 2048×1152 benchmark stay
  well inside 16.7 ms). The planned path is a checkerboard chunk update (chunks in
  the same phase never touch each other's cells), which the order-independent
  RNG already supports.
- Heat moves at most one neighbour per step, so it spreads ~15 cells through
  stone in 10 seconds. Temperatures are whole degrees and a 1° gap does not
  flow, so buried layers can settle a degree per layer above ambient.
- Steam and smoke drift as scattered particles rather than billowing clouds.
- The world is a fixed grid of chunks; streaming/infinite worlds come with
  seeded world generation (roadmap step 3, next).
