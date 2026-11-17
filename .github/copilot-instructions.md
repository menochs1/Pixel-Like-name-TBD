# Project Instructions

Build this project as a 2D pixel-simulation sandbox inspired by Noita, Powder
Toy, and cellular-automata simulations—not as a generic platformer. The central
goal is emergent gameplay from material pixels that interact and change state.
Evaluate design decisions against simulation quality, scalability, memory use,
performance, and extensibility.

## Engineering priorities

Implement in this order:

1. Pixel simulation engine
2. Chunk management
3. Data-driven material interactions
4. Deterministic world generation
5. Character controller
6. Combat
7. Lighting
8. Optional multiplayer

Do not prioritize UI over simulation quality. Keep systems modular and loosely
coupled, favor composition over inheritance, and avoid monolithic managers.
Separate simulation from rendering so the world simulation works without a
visual presentation.

## Simulation and architecture

- Represent each simulated pixel with a material type, temperature, velocity,
  health/durability, flammability, conductivity, density, and state flags.
- Express material behavior as update rules. Support falling and sliding sand,
  flowing water, rising/dispersing smoke, and fire that consumes fuel, produces
  heat, and creates smoke.
- Make interactions data-driven rather than embedding pair-specific behavior
  throughout the engine. Examples include water plus lava producing stone and
  steam; fire igniting oil; acid dissolving stone; electricity propagating
  through water or conductive materials; fire melting ice; and steam
  condensing on cold surfaces.
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
  particle-heavy combat, and material-specific visual effects where appropriate.
- Target at least 60 FPS and 100,000+ actively simulated pixels. Measure before
  optimizing, and explain performance and memory tradeoffs for architectural
  choices.

## Roadmap

1. Cellular automata, sand, water, stone, and chunks.
2. Fire, smoke, oil, and temperature.
3. Seeded world and cave generation, plus save/load.
4. Player controller, destruction tools, and basic enemies.
5. Spells, explosions, and lighting.
6. Advanced chemistry, electricity, weather, and biome interactions.

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
