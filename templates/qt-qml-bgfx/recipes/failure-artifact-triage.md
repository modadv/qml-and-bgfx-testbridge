# Recipe: Failure Artifact Triage

1. Open the artifact directory printed by the failing test.
2. Read `manifest.json`.
3. Inspect `screenshot.png` and `app.log`.
4. Check `render.resources` for missing resources or invalid live shader state.
5. Use `qml.tree` and `qml.meta` captures to identify UI hook regressions.
6. If the TestBridge port never opened, check Qt runtime paths, app crash logs,
   port conflicts, and whether `ENABLE_TESTBRIDGE` is on.
7. If shader compilation failed, check `TESTBRIDGE_SHADERC`, include paths,
   shader stage/profile, and compiler stderr in the shader cache.
8. If the screenshot is blank, check `render.caps`, `render.stats.bgfx`, window
   visibility, and render item geometry.
