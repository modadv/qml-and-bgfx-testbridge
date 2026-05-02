# Task: Live Shader Iteration

1. Start the app with `TESTBRIDGE_RENDER_TIER=nocompute`.
2. Compile with `shader.compile`.
3. Apply with `shader.apply`.
4. Verify `render.resources.liveShader.activeHash`.
5. Capture `window.grab` and compare against expected visual behavior.
6. Revert with `shader.revert` before ending unless the change is intentional.
