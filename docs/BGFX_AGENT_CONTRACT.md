# bgfx Agent Contract

The render provider turns bgfx and engine internals into data an Agent can
inspect without attaching a debugger.

## Required Provider Data

- `render.caps`: backend renderer, feature tier, compute/indirect/image-RW
  support, software-backend detection, initialization generation, and frame id.
- `render.stats`: frame timings, draw/compute/blit counts, memory counters,
  bgfx view stats, and engine performance snapshots.
- `render.resources`: loaded heightfield/diffuse assets, mesh/grid state,
  material/shader state, live shader hash, and QML surface geometry.
- `window.grab`: PNG screenshot suitable for region analysis and golden checks.

## Live Shader Channel

The live shader channel is intentionally controlled:

- `shader.compile` writes source to a cache directory and invokes `shaderc`.
- `shader.apply` queues a known compiled artifact on the render item.
- `shader.revert` restores the stock shader for the slot.
- `shader.list` reports supported slots, cache records, and last applied hash.

New engine modules should add provider fields before adding tests that need
those fields. That keeps Agent investigation data-driven instead of
screenshot-only.
