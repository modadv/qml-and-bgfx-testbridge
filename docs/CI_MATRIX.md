# CI Matrix

The smoke suite is parameterized through environment variables so a CI system
can exercise renderer tiers, window sizes, and platforms without changing test
code.

## CTest Entries

The default configure now registers:

- `testbridge_lab_smoke`: default environment.
- `testbridge_lab_smoke_full`: `TESTBRIDGE_RENDER_TIER=full`, `1180x760`.
- `testbridge_lab_smoke_nocompute_small`: `TESTBRIDGE_RENDER_TIER=nocompute`,
  `960x640`.
- `testbridge_lab_live_shader_full`: `TESTBRIDGE_RENDER_TIER=full`, `1180x760`,
  plus live vertex, fragment, and compute shader compile/apply/revert self-test.
- `testbridge_lab_live_shader_nocompute`: `TESTBRIDGE_RENDER_TIER=nocompute`,
  `960x640`, plus live vertex and fragment shader compile/apply/revert
  self-test. Compute slots are skipped when `render.caps.noCompute` is true.

Run all:

```powershell
ctest --test-dir .build-release\build -C Release --output-on-failure
```

Run a tier:

```powershell
ctest --test-dir .build-release\build -C Release -L nocompute --output-on-failure
```

Run live shader tests:

```powershell
ctest --test-dir .build-release\build -C Release -L shader --output-on-failure
```

## Environment Matrix

- `TESTBRIDGE_RENDER_TIER=full|nocompute|auto`
- `TESTBRIDGE_WINDOW_WIDTH`
- `TESTBRIDGE_WINDOW_HEIGHT`
- `TESTBRIDGE_GOLDEN_BGFX_REGION`
- `TESTBRIDGE_UPDATE_GOLDEN=1`
- `TESTBRIDGE_IMAGE_MEAN_ABS_THRESHOLD`
- `TESTBRIDGE_IMAGE_CHANGED_RATIO_THRESHOLD`
- `TESTBRIDGE_LIVE_SHADER_SELFTEST=1`
- `TESTBRIDGE_SHADERC`
- `TESTBRIDGE_SHADER_CACHE_DIR`

Use platform CI jobs for Windows, Linux, and macOS. Each job should run the same
CTest labels where the platform has the required Qt/bgfx dependencies.

## Image Oracle

When `TESTBRIDGE_GOLDEN_BGFX_REGION` is set, the smoke test crops the bgfx QML
item region from `window.grab`, compares it to the golden PNG, and writes
`actual_crop.png`, `expected.png`, `diff.png`, and `metrics.json` on failure.

Without a golden path, the test still verifies the region is visible, non-solid,
and has luminance variation.
