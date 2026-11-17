# Pixel-Like-name-TBD

A 2D pixel-physics sandbox focused on emergent material interactions and fully
destructible terrain, written in C++20. Every pixel is a simulated cell of
material: sand piles up, water flows and levels out, sand sinks through water.

Development goals and guidance live in
[`.github/copilot-instructions.md`](.github/copilot-instructions.md).

## Status

Roadmap step 1 (cellular automata, sand, water, stone, chunks) is in place:

- Chunked world (64×64 chunks) stored as a structure of arrays.
- Active-region tracking: each chunk keeps a dirty rectangle, so settled areas
  cost nothing to simulate.
- Data-driven materials: behaviour comes from a material's *phase*
  (solid / powder / liquid / gas) plus properties such as density and
  dispersion. Adding a material means adding a `MaterialDef`, not engine code.
- Deterministic: all randomness is a hash of (seed, x, y, tick), so the same
  seed and inputs always produce the same world, regardless of update order.
- Simulation is fully separate from rendering and runs headless.

## Building

Requirements: CMake 3.20+, a C++20 compiler, and SDL2 development files for the
interactive app (the core library, tests and benchmark need nothing else).

```bash
# Debian/Ubuntu: sudo apt install libsdl2-dev   macOS: brew install sdl2
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Run the sandbox:

```bash
./build/pixel_sandbox
```

| Input | Action |
| --- | --- |
| Left mouse | Paint selected material |
| Right mouse | Erase |
| Mouse wheel | Brush size |
| `1` / `2` / `3` | Sand / water / stone |
| `Space` | Pause / resume |
| `.` | Single step while paused |
| `R` | Rebuild demo scene |
| `C` | Clear world |
| `D` | Show active (dirty) regions |

The window title shows FPS, simulation time per step and active chunk count.

Run the tests and benchmark:

```bash
ctest --test-dir build --output-on-failure
./build/pixel_bench --scene rain --steps 600            # add --ppm out.ppm to save the final frame
```

## Layout

```
src/core/     material registry, chunk storage, world, simulator (no dependencies)
src/world/    scene / world generation
src/render/   World -> ARGB pixel buffer (no SDL)
src/app/      SDL2 window, input, fixed 60 Hz timestep
tools/        headless benchmark / snapshot tool
tests/        unit tests (tiny built-in harness, run via CTest)
```

## How the simulation works

Each step (`px::step`, fixed at 60 per second):

1. Every chunk's pending dirty rect becomes its current rect.
2. Chunks are visited bottom-to-top; inside each dirty rect, rows run
   bottom-to-top so falling material moves once per step. Horizontal scan
   direction alternates every step to avoid a left/right bias.
3. Each cell runs the rule for its phase:
   - **Powder** falls (accelerating up to 3 cells/step), else slides diagonally.
   - **Liquid** falls, slides diagonally, else flows sideways up to its
     `dispersion` distance, stopping early over drops.
   - Movers displace lighter liquids/gases (sand sinks through water).
4. Every move wakes the 3×3 neighbourhood of both cells into the *next* step's
   dirty rects, possibly in neighbouring chunks. Cells that cannot move are not
   re-woken, so finished regions go to sleep.

A per-cell clock byte stops a cell that has already moved this step from being
updated again.

## Performance

Measured with `pixel_bench --scene rain --steps 600` (2048×1152 world,
~118k falling sand and water cells at the start, single thread, Release):

| Metric | Value |
| --- | --- |
| Mean step | 4.3 ms |
| p95 step | 5.9 ms |
| Cells scanned per step | ~210k |
| Memory | 5 bytes/cell (~20 KiB per chunk; 10M cells ≈ 50 MiB) |

## Known limitations / next steps

- A liquid surface with a partial top layer keeps shuffling sideways and never
  fully sleeps; a settle heuristic will fix this.
- Single-threaded. The planned path is a checkerboard chunk update (chunks in
  the same phase never touch each other's cells), which the order-independent
  RNG already supports.
- The world is a fixed grid of chunks; streaming/infinite worlds come with
  seeded world generation (roadmap step 3).
- Roadmap step 2 next: temperature, fire, smoke, oil and a data-driven reaction
  table (e.g. water + lava → stone + steam).
