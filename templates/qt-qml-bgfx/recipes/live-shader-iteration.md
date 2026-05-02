# Recipe: Live Shader Iteration

1. Use `shader.compile` with source text and a whitelisted slot.
2. Use `shader.apply` with the returned hash.
3. Wait for `render.resources.liveShader.activeHash`.
4. Capture `window.grab` and compare.
5. Use `shader.revert` on failure or at the end of an experiment.
