# Recipe: Live Shader Iteration

1. Use `shader.list` to discover whitelisted slots.
2. Use `shader.compile` with source text and one whitelisted slot.
3. Use `shader.apply` with the returned hash and the same slot.
4. Poll `render.resources.liveShader.slots` until the slot reports the hash.
5. Capture `window.grab` and compare with a pixel heuristic or golden image.
6. Use `shader.revert` on failure or at the end of an experiment.

Template sample slots:

- `terrain_simple.vertex`
- `terrain_simple.fragment`
- `overlay_max_elevation.compute`
