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

The sample renderer currently allowlists:

| Slot | Stage | Notes |
|---|---|---|
| `terrain_simple.vertex` | Vertex | Used by the fixed-grid fallback terrain program. |
| `terrain_simple.fragment` | Fragment | Used by the fixed-grid fallback terrain program. |
| `overlay_max_elevation.compute` | Compute | Available only when `render.caps.noCompute` is false. |

Agents must call `shader.list` before compiling and must not assume a slot is
available in every host project or render tier.

New engine modules should add provider fields before adding tests that need
those fields. That keeps Agent investigation data-driven instead of
screenshot-only.

## Minimum Agent Assertions

For a renderer to be considered Agent-operable, a smoke test should assert:

- `render.caps.available == true`.
- `render.stats.available == true`.
- `render.resources.available == true`.
- the primary QML render item name is present in `render.resources`.
- at least one draw program and texture are valid after the first rendered frame.
- `window.grab` returns a non-solid image region for the render item.
- live shader slots report active/pending/revert/error state when enabled.
