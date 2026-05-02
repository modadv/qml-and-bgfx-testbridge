# Agent Workflows

## Build And Baseline

```powershell
git submodule update --init --recursive
cmake -S . -B .build-release\build
cmake --build .build-release\build --config Release --target testbridge_lab -- /m
ctest --test-dir .build-release\build -C Release --output-on-failure
```

## Inspect A Running App

1. Start the app with `TESTBRIDGE_PORT`.
2. Use MCP tools or JSON-RPC:
   - `app_describe`
   - `qml_tree`
   - `qml_hit`
   - `render_caps`
   - `render_stats`
   - `render_resources`
   - `screenshot`
3. Prefer `qml.meta` and `qml.geometry` over reading QML by eye.

## Add A QML Feature

1. Add stable `objectName` values.
2. Expose important state with `Q_PROPERTY` or `Q_INVOKABLE`.
3. Add or update smoke assertions with `qml.find`, `qml.meta`,
   `qml.geometry`, `qml.tree`, and user-equivalent input.
4. Run `testbridge_lab_smoke`.

## Add A Render Feature

1. Add render resource/state fields to the snapshot.
2. Add a visual oracle: pixel heuristic or golden image.
3. Run Full and NoCompute smoke tests.
4. Save failure artifacts if the visual result is ambiguous.

## Live Shader Iteration

1. Compile with `shader.compile`.
2. Apply with `shader.apply`.
3. Poll `render.resources.liveShader.slots` until the target slot reports the
   compiled hash.
4. Verify with `window.grab`, render stats, and golden diff when available.
5. Revert with `shader.revert` if the result is wrong or the experiment is done.

Sample slots:

- `terrain_simple.vertex`
- `terrain_simple.fragment`
- `overlay_max_elevation.compute`

## Failure Triage

Open the artifact directory printed by the failed smoke test. Start with:

- `manifest.json`
- `screenshot.png`
- `app.log`
- `diff.png` and `metrics.json` when golden diff is active

Use the manifest RPC captures to decide whether the failure is UI, engine,
shader, asset, timing, or environment related.
