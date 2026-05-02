# Agent Workflows

## Build And Baseline

```powershell
cmake -S . -B .build-release\build
cmake --build .build-release\build --config Release --target testbridge_lab -- /m
ctest --test-dir .build-release\build -C Release --output-on-failure
```

## Inspect A Running App

1. Start the app with `TESTBRIDGE_PORT`.
2. Use MCP tools or JSON-RPC:
   - `app_describe`
   - `qml_tree`
   - `render_resources`
   - `screenshot`
3. Prefer `qml.meta` and `qml.geometry` over reading QML by eye.

## Add A QML Feature

1. Add stable `objectName` values.
2. Expose important state with `Q_PROPERTY` or `Q_INVOKABLE`.
3. Add or update smoke assertions.
4. Run `testbridge_lab_smoke`.

## Add A Render Feature

1. Add render resource/state fields to the snapshot.
2. Add a visual oracle: pixel heuristic or golden image.
3. Run Full and NoCompute smoke tests.
4. Save failure artifacts if the visual result is ambiguous.

## Live Shader Iteration

1. Compile with `shader.compile`.
2. Apply with `shader.apply`.
3. Verify with `render.resources.liveShader`, `window.grab`, and golden diff.
4. Revert with `shader.revert` if the result is wrong.

## Failure Triage

Open the artifact directory printed by the failed smoke test. Start with:

- `manifest.json`
- `screenshot.png`
- `app.log`
- `diff.png` and `metrics.json` when golden diff is active

Use the manifest RPC captures to decide whether the failure is UI, engine,
shader, asset, timing, or environment related.
