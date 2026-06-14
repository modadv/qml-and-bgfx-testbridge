# Render-Path Refactor — Change Summary

Date: 2026-06-14
Branch baseline: `5a4d92e` (last commit before the work began)
Plan: *Minimalist-2026 hardening of the bgfx render path* (6 phases)

This document summarizes every change made across all stages of the render-path
refactor. The work benchmarked the existing bgfx subsystem against the
*disciplined* subset of a minimalist 2026 engine architecture and adopted the
parts that fit this project's compatibility floor, while deliberately skipping
the heavyweight parts that would break it.

## Goals

Fix a cluster of fragility / leak / maintainability problems without changing
the rendered pixels on either tier:

1. Hand-rolled GPU↔CPU sync via magic frame counters (`m_textureSwapDelay`,
   `m_deferSmapUseFrames`) at ~13 sites.
2. Two file-static global readback deques that leaked and could mis-collect on
   32-bit frame-id wraparound, never drained on shutdown/reset.
3. A 4,578-line `TerrainRenderer` god class mixing load, cache, uniforms,
   compute LOD, overlay, picking, readback, and live-shader reload.
4. A busy-spin render loop calling `update()` unconditionally every frame.
5. No present/renderer seam — `RenderScene` was welded to `TerrainRenderer` and
   the readback→upload present was inlined in the QFBO renderer.
6. Submission without `isValid` guards.

## Hard constraints honored

- **C++14 only** — no `std::optional`/`variant`/`string_view`, no structured
  bindings, no `if constexpr`.
- **GL 3.3 / GLES 3.0 baseline; Ubuntu 18.04 + Windows 10.** The `RenderCaps`
  tier split (Full vs NoCompute) and the `TESTBRIDGE_RENDER_TIER` override are
  preserved; Windows headless-bgfx and Linux GLX paths untouched.
- **No new GPU feature requirements** — everything new is CPU-side bookkeeping or
  reuses bgfx primitives already in use (frame fence, blit/readback).

## Deliberately NOT done (minimalism / compatibility)

Bindless/descriptor indexing, mesh/task shaders, ray query, GPU-driven culling at
100k-object scale, multithreaded encoders, and a full Frostbite-style render
graph — all unavailable on the GL3.3/GLES3.0/llvmpipe floor or unjustified for a
single-heightfield, single-viewport workload.

## Per-stage summary

### Phase 0 — Baseline + recon
- Captured a golden/behavioral regression baseline on both tiers and a
  handle/seam inventory.
- **Deliverable:** `docs/RENDER_BASELINE.md`.

### Phase 1 — `DeferredDeleteQueue` (fence-driven resource lifetime)
*Commits: `3649ecd`, `d542aa9`, `b1677b8`*
- New header-only, C++14 `src/engine/common/resource_arena.h`: a
  `DeferredDeleteQueue` with `enqueue(destroy, safeAfterFrame)` +
  `collect(retiredFrameId)` (once per `endFrame`) + `flushAll()` (shutdown /
  surface teardown). Driven by bgfx's real frame fence via `RenderDevice`.
- `RenderDevice` owns one queue (`render_device.{h,cpp}`).
- **Replaced all 13 `m_textureSwapDelay` / `m_deferSmapUseFrames` destroy
  delays** with enqueue-at-submit-frame. Texture swap became "bind new, enqueue
  old".
- **Folded both global readback deques** (`g_orphanedReads`,
  `g_orphanedOverlayReads`) into instance-owned queues drained by the same
  fence, with a shutdown drain — killing the leak + wraparound class.
- Added `isValid` submit/dispatch guards.
- **Verification helper:** `tests/verify_texture_swap.py`.

### Phase 2 — Render-on-demand + present/content seam
*Commits: `2fc19e3` (B2), `4afec06` (B1)*
- **B2 (on-demand):** the viewport renderer only calls `update()` when the scene
  reports it must keep producing frames. `IRenderContent::needsContinuousUpdate()`
  ORs renderer settling, pending auto-fit, in-flight readbacks, and camera
  view-dirty; otherwise the loop idles instead of pegging a core.
- **B1 (present seam):** extracted the surface + readback ring +
  `glTexSubImage2D` upload into `ReadbackPresenter`
  (`src/engine/quick/readback_presenter.{h,cpp}`), behind a thin
  `IRenderContent` interface (`src/engine/quick/render_content.h`). `RenderScene`
  is no longer welded to `TerrainRenderer`; the QFBO renderer is reduced to
  orchestration.

### Phase 3 — Decompose the god class
*Commits: `6f696dd`, `734cff8`, `fe8a893`, `876503f`*

Split `TerrainRenderer` one module at a time as **compiler/linker-verified
verbatim translation-unit moves** (same class, all member declarations remain in
`terrain_renderer.h`), each gated by build + both-tier smoke:

| Module | File | Concern |
|---|---|---|
| Core | `terrain_renderer.cpp` | ctor/dtor, init, lifecycle, `update()` orchestration, render submit |
| NoCompute | `terrain_renderer_simple.cpp` | fixed-grid mesh + CPU smap fallback |
| Overlay | `terrain_renderer_overlay.cpp` | rects, picking, overlay-max readback |
| Heightfield | `terrain_renderer_heightfield.cpp` | load / decode / cache / CPU copy |
| Shaders | `terrain_renderer_shaders.cpp` | program load + live-shader reload |
| Shared | `terrain_renderer_internal.h` | inline helpers shared across the TUs |

Each move was verified with a line-set diff (`comm`), one-definition-per-method
counts, a clean build, and the two-tier smoke gate.

### Phase 4 — Minimal declarative frame-pass list
*Commit: `348f01d`*
- New header-only `engine::FramePassList` (`src/engine/terrain/frame_graph.h`):
  records the fixed, linear pass topology with each pass's read/write resource
  set and a `validate()` that enforces producer-before-consumer ordering.
- **Deliberately not a scheduler** — no DAG, no aliasing, no barrier insertion.
  It is a *description* of pass ordering, not a driver of submission.
- `TerrainRenderer` builds the tier-appropriate topology
  (`decode → smap → overlay-max → terrain → axes → overlay-rects → present`, with
  CPU equivalents on NoCompute), validates it once at `init()`, and exposes it
  under the new `render.resources.framePasses` key.

### Phase 5 — Cleanup + docs
*Commits: `6779cb5` (docs), `5f3a50f` (cleanup)*
- **Docs:** updated `docs/ARCHITECTURE.md` (refreshed engine source inventory +
  a new "Render-path discipline" table), `docs/RENDER_PROVIDER_SDK.md`
  (`framePasses` + resource-lifetime/present sections), and `AGENTS.md` (a
  "Render path" rules section).
- **Cleanup (no behavioral change):**
  - Named the scattered `deferDestroyTexture` budgets:
    `kTextureRetireFrames` (5) and `kDmapRetireFrames` (60, longer because the
    dmap can also be referenced by an in-flight async decode/readback), at all 9
    call sites.
  - Named `kSmapUseDeferFrames` (3) — documented as a *use* deferral (keep
    sampling the old smap until the generate-smap dispatch retires) that the
    deferred-delete queue does not subsume.
  - Named `kMaxDrainFrames` (16) — the readback drain spin cap guarding against a
    stuck fence on teardown.
  - Removed the confirmed-dead `TerrainRenderer::cpuRecomputeRectMaxIfNeeded()`
    (uncalled repo-wide); left the `cpuComputeRectMax` toolkit function in place.

## New / changed files

**New**

| File | Lines | Role |
|---|---|---|
| `src/engine/common/resource_arena.h` | 117 | fence-driven `DeferredDeleteQueue` (header-only) |
| `src/engine/quick/render_content.h` | 32 | `IRenderContent` present/content interface |
| `src/engine/quick/readback_presenter.{h,cpp}` | 101 / 417 | surface + readback ring + present upload |
| `src/engine/terrain/terrain_renderer_simple.cpp` | 238 | NoCompute grid + CPU smap |
| `src/engine/terrain/terrain_renderer_overlay.cpp` | 1433 | overlay rects / picking / overlay-max |
| `src/engine/terrain/terrain_renderer_heightfield.cpp` | 1086 | heightfield/diffuse asset pipeline |
| `src/engine/terrain/terrain_renderer_shaders.cpp` | 396 | program load + live-shader reload |
| `src/engine/terrain/terrain_renderer_internal.h` | 27 | shared inline helpers |
| `src/engine/terrain/frame_graph.h` | 172 | declarative frame-pass list + validation |
| `docs/RENDER_BASELINE.md` | 62 | Phase 0 regression baseline |
| `tests/verify_texture_swap.py` | 553 | deferred-delete verification helper |

**Significantly changed**

| File | Before → After | Note |
|---|---|---|
| `src/engine/terrain/terrain_renderer.cpp` | 4578 → 1448 | god class reduced to orchestration (~68%) |
| `src/engine/quick/render_viewport_item.cpp` | 1053 → 679 | present logic moved to `ReadbackPresenter` |
| `src/engine/quick/render_viewport_item.h` | — | renderer holds a `ReadbackPresenter` |
| `src/engine/quick/render_scene.h` | — | implements `IRenderContent` |
| `src/engine/terrain/render_device.{h,cpp}` | — | owns the deferred-delete queue |
| `src/engine/terrain/terrain_renderer.h` | — | named frame-budget constants; dead decl removed |
| `src/engine/CMakeLists.txt` | — | new `.cpp` modules added to `engine_core` |
| `docs/ARCHITECTURE.md`, `docs/RENDER_PROVIDER_SDK.md`, `AGENTS.md` | — | document the new seams |

Aggregate: **23 files changed, +5,303 / −4,073**.

## Outcome

| Concern | Before | After |
|---|---|---|
| Resource lifetime | 13 hand-tuned "wait N frames" literals | one fence-driven `DeferredDeleteQueue`; two named budget tunables |
| Readback leaks | 2 global deques, wraparound-prone, never drained | instance-owned, fence-drained, flushed on teardown |
| Idle CPU | busy-spin every frame | render-on-demand; idles when static |
| God class | 4,578-line single TU | 5 cohesive TUs; core ~1,448 lines |
| Present coupling | welded to `TerrainRenderer`, inlined upload | `IRenderContent` seam + `ReadbackPresenter` |
| Pass ordering | implicit in `update()` | declarative `FramePassList`, validated at init, in `render.resources` |
| Submit safety | unguarded | `isValid`-guarded |

## Verification

Every phase was gated by a clean Release build and the both-tier smoke gate
(`testbridge_lab_smoke`, `_smoke_full`, `_smoke_nocompute_small`). The
decomposition moves were additionally verified verbatim via line-set diff and
one-definition-per-method counts. The Phase 4 `framePasses` payload was confirmed
live (`valid=true, count=7`) on both Full and NoCompute tiers.

Final full `ctest`: all render-path / golden / package / template tests pass. The
only failures are 3 **pre-existing, environmental, render-unrelated** reds:

- `testbridge_mcp_unit` — `ModuleNotFoundError: No module named 'fastmcp'`.
- `testbridge_lab_live_shader_full` / `_nocompute` — `shaderc` exits 1 on the
  `dx11` profile (missing `d3dcompiler_47.dll`).

These predate the refactor and are unaffected by it.
