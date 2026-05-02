# Recipe: Failure Artifact Triage

1. Open the artifact directory printed by the failing test.
2. Read `manifest.json`.
3. Inspect `screenshot.png` and `app.log`.
4. Check `render.resources` for missing resources or invalid live shader state.
5. Use `qml.tree` and `qml.meta` captures to identify UI hook regressions.
