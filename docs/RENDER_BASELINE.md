# Render baseline & seam map (Phase 0)

Baseline captured on branch `render-minimalist-2026` before the minimalist-2026
render-path refactor (`~/.claude/plans/rustling-dancing-cocke.md`). Use this as the
regression reference for Phases 1–5.

## Verification environment (non-obvious)

The app and the real-GUI ctest cases need a runtime env that is **not** set up by
default (see triage below). For every build/test command, first apply:

```powershell
$qtbin = "E:\DevEnv\conan_home\p\b\qtd78c6f08175fb\p\bin"   # from conanrunenv-release-x86_64.bat
$env:PATH = "$qtbin;$env:PATH"
$env:QT_PLUGIN_PATH = "$qtbin\archdatadir\plugins"
$env:QT_QPA_PLATFORM_PLUGIN_PATH = "$qtbin\archdatadir\plugins\platforms"
$env:TESTBRIDGE_SHADERC = (Resolve-Path ".build-release\build\external\bgfx.cmake\cmake\bgfx\Release\shaderc.exe").Path
```

The ctest Python (`.conda\envs\py314`) needs `pytest websockets` (installed).

## Baseline test status

| Test | Status | Notes |
|------|--------|-------|
| build `testbridge_lab` | ✅ green | incremental |
| `testbridge_lab_smoke` / `_full` / `_nocompute_small` | ✅ green | **render regression gate, both tiers** |
| `template_manifest`, `testbridge_package_consumer` | ✅ green | |
| `testbridge_mcp_unit` | ❌ pre-existing | missing `fastmcp` python dep; not render code |
| `testbridge_lab_live_shader_full` / `_nocompute` | ❌ pre-existing | shaderc DX11 needs `d3dcompiler_47.dll` at runtime; not render code |
| `testbridge_lab_golden_probe` | ⏭ disabled | |

Render correctness is also self-checked by the smoke harness'
`_assert_bgfx_region_has_pixels` (varied pixels in the bgfx region) plus
`render.caps`/`render.stats`/`render.resources` assertions — these catch gross
render regressions without managing golden image files.

## Seam map — worklist for Phases 1 & 3

### Magic frame-count delays (replace with fence-driven deferred deletion — Phase 1)
`src/engine/terrain/terrain_renderer.cpp`: `m_textureSwapDelay` set to `5` at
560(reset),576,2080,2110,2178,2203,2237,4359 and to `60` at 3837,3962,4052;
consumed at 754-760. `m_deferSmapUseFrames = 3` at 2179, consumed at 822-823,2526.
Header decls: `terrain_renderer.h:344-345,358`.

### Global readback deques (fold into instance-owned, fence-drained — Phase 1)
- `g_orphanedOverlayReads` + `g_orphanedOverlayMutex` — `terrain_renderer.cpp:117-141`.
- `g_orphanedReads` + `g_orphanedMutex` + `stash/releaseOrphanedReads` —
  `render_viewport_item.cpp:33-55`, used at 320,367,434,459,509.
Both keyed on `frameId <= currentFrame` (wraparound) and never drained on shutdown.

### Raw handles (migrate to GpuHandle RAII — Phase 1/3)
38 raw `bgfx::*Handle` members in `terrain_renderer.h`; `ViewSurface` handles in
`render_device.h` (framebuffer/colorTex/depthTex/readbackTex[]).

### God class (decompose — Phase 3)
`TerrainRenderer` ≈ 4,578 lines mixing heightfield load, texture cache, uniforms,
compute LOD, overlay rects + picking, readback, live-shader reload.

### Fence primitive to reuse
`RenderDevice::instance().lastFrameId()` / `generation()` (`render_device.cpp`) —
bgfx's real retired-frame id; drive all deferred deletion off this.
