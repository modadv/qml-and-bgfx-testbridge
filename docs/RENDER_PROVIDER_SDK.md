# Render Provider SDK

TestBridge is app-agnostic. A host renderer integrates through JSON providers.

## Required Providers

```cpp
bridge.setRenderCapsProvider([] { return nlohmann::json{...}; });
bridge.setRenderStatsProvider([] { return nlohmann::json{...}; });
bridge.setRenderResourcesProvider([] { return nlohmann::json{...}; });
```

Providers should return plain values only. Do not expose native handles as
operational capabilities. Numeric handle IDs are acceptable as diagnostics.

## Recommended `render.caps`

- backend name
- renderer tier
- compute/indirect/image load-store flags
- software renderer detection
- backend renderer/version strings
- frame generation and last frame ID

## Recommended `render.stats`

- CPU update/upload/submit timings
- texture load timings
- bgfx draw/compute/blit counts
- GPU timing when available
- memory counters when available

## Recommended `render.resources`

- active render item object name
- scene/camera snapshot
- asset paths
- texture table
- program table
- buffer table
- live shader state
- resource validity flags

## Threading Rule

Collect renderer state at a safe point owned by the renderer, then let the RPC
thread read the copied snapshot or ask the GUI thread for plain data. Do not let
RPC handlers mutate bgfx directly.
