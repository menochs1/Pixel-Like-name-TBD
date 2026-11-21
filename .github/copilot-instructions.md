# Project Instructions

Build this project as a 2D pixel-simulation **sandbox**. It is inspired by
Noita, Powder Toy, and cellular-automata simulations, but it does **not** share
Noita's gameplay loop and is not a generic platformer. There is no fixed
progression or combat-driven loop at the core: the player experiments with
materials and the world, and gameplay emerges from the simulation.

## Core vision

- **Every pixel owns its properties.** A pixel is not just a label such as
  "sand"; it carries its own element properties (e.g. temperature, density,
  conductivity, flammability, durability). Two pixels of the same
  base element can differ and evolve independently.
- **Elements react like chemistry.** When pixels interact, the result can be a
  **new material whose properties are derived from its parent materials**
  (e.g. blended or combined density, conductivity, flammability, melting point,
  color), not only a swap to a hand-authored material.
- Reactions are data-driven rules and property-derivation functions, not
  pair-specific engine code. New elements and reactions should be addable
  without engine changes.
- Emergent behavior from these rules is the central goal. Evaluate design
  decisions against simulation quality, scalability, memory use, performance,
  and extensibility.

## Engineering priorities

Implement in this order. Progress as of 2026-11-21: ✅ done · 🟡 partly done ·
⬜ not started.

1. Pixel simulation engine — ✅ Done: powders, liquids (with settling), gases,
   solids, temperature and heat flow, deterministic RNG, simulation separate
   from rendering.
2. Chunk management — ✅ Done for an in-memory world: 64×64 SoA chunks,
   dirty rects and sleeping, simulating only around the camera, horizontal
   wrapping. Chunk streaming (loading/unloading) is still to do.
3. Data-driven material interactions — ✅ Done: materials, temperature
   transitions and pair reactions are data. Reactions can derive products from
   their parents (runtime variants, interned) and release heat. Pixels can hold
   their own properties via variants, plus per-pixel temperature and health.
   A state-flags byte is planned for when the first flag is needed.
4. Deterministic world generation — 🟡 Partly done: seeded, chunk-independent
   generation with biomes, caves, pools, deposits and a wrapping seam is done.
   Save/load is not started.
5. Sandbox tools (spawn, erase, heat/cool, inspect pixel properties) — 🟡
   Partly done: spawn and erase brushes, a heat view and an active-region view.
   Not yet done: a heat/cool tool and a pixel inspector.
6. Character controller — ⬜ Not started.
7. Lighting — ⬜ Not started.
8. Optional combat and multiplayer — ⬜ Not started.

Do not prioritize UI over simulation quality. Keep systems modular and loosely
coupled, favor composition over inheritance, and avoid monolithic managers.
Separate simulation from rendering so the world simulation works without a
visual presentation.

## Simulation and architecture

- Represent each simulated pixel with a material type plus per-pixel
  properties: temperature, velocity, health/durability, flammability,
  conductivity, density, and state flags. Per-pixel values may diverge from the
  material's defaults; materials supply defaults and behavior, pixels hold state.
- Support derived materials: a reaction can produce a new (possibly runtime-
  generated) material whose properties are computed from its parents via
  declarative rules (e.g. average, min/max, weighted mix). Keep the material
  table extensible for generated entries, and keep results deterministic.
- Express material behavior as update rules. Support falling and sliding sand,
  flowing water, rising/dispersing smoke, and fire that consumes fuel, produces
  heat, and creates smoke.
- Make interactions data-driven rather than embedding pair-specific behavior
  throughout the engine. Examples include water plus lava producing stone and
  steam; fire igniting oil; acid dissolving stone; electricity propagating
  through water or conductive materials; fire melting ice; and steam
  condensing on cold surfaces. Reactions should be able to consume reactants,
  emit products and heat, and derive product properties from the reactants.
- Treat the following as a suggested modular organization, adapting it to the
  chosen engine and existing project conventions:

  - `Core`: simulation, materials, physics, chunks
  - `World`: terrain, biomes, generation
  - `Entities`: player, enemies, projectiles
  - `Rendering`: pixel renderer, lighting
  - `Utilities`: math, serialization

- Prefer chunked, data-oriented storage and Structure of Arrays (SoA) over
  object-per-pixel designs. Keep active-cell tracking and dirty-region updates
  in mind; use multithreading only where updates are safe, and consider GPU
  compute only when it benefits the selected engine and workload.
- Design for future support for 500+ material types and worlds exceeding
  10 million pixels. Keep material additions possible without widespread engine
  changes.
- Keep simulation rules and world data independent of visual effects, shaders,
  and rendering-specific state.

## World, rendering, and performance

- Plan for infinite or very large worlds, chunk streaming, deterministic
  generation from a seed, caves, biomes, deposits, and liquids.
- Use a pixel-art presentation with destructible terrain, dynamic lighting,
  and material-specific visual effects where appropriate. Make pixel
  properties and reaction results legible to the player (e.g. inspection view).
- Target at least 60 FPS and 100,000+ actively simulated pixels. Measure before
  optimizing, and explain performance and memory tradeoffs for architectural
  choices.

## Roadmap

1. Cellular automata, sand, water, stone, and chunks, with per-pixel properties.
2. Data-driven reaction system, including derived materials; fire, smoke, oil,
   and temperature.
3. Sandbox tools: spawn/erase materials, heat/cool, and pixel inspection.
4. Seeded world and cave generation, plus save/load (including derived
   materials).
5. Player controller and world-manipulation tools.
6. Lighting, explosions, and spells as sandbox tools.
7. Advanced chemistry, electricity, weather, and biome interactions.
8. Optional enemies, combat, and multiplayer.

## Development behavior

- Follow the repository's existing language, engine, architecture, and
  conventions. This repository may not yet have an engine or implementation;
  inspect it before proposing dependencies or building systems.
- Favor scalable, production-quality solutions; explain meaningful tradeoffs,
  consider simulation consequences, and suggest optimizations proactively.
- Add focused tests for new behavior when test infrastructure exists. Document
  public interfaces and update directly related documentation.
- Validate changes with the repository's existing build, lint, and test
  commands. Avoid introducing tools or dependencies unless they are necessary.
