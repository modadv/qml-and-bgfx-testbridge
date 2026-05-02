# Live Shader Direction

Live shader execution can materially improve Agent-led graphics development, but
it should be implemented as a controlled compile/cache/diagnostic pipeline.

bgfx runtime APIs consume compiled shader binaries. A host should not treat raw
GLSL/HLSL/MSL strings as something that can be passed directly to
`bgfx::createShader` across platforms.

Recommended architecture:

1. Agent submits shader source plus metadata: stage, entry point, renderer
   backend, profile, varying definition, include set, and intended program slot.
2. Host writes the source into a temporary workspace and invokes the same
   `shaderc` toolchain used by the build.
3. Compile output is cached by content hash and backend.
4. On success, the render thread swaps the compiled program at a safe frame
   boundary.
5. On failure, the RPC returns compiler diagnostics and keeps the previous
   program active.
6. Screenshot/golden tests and `render.stats/resources` validate the result.

This gives the Agent a fast edit-compile-observe loop while preserving bgfx's
cross-platform shader model and avoiding arbitrary GPU program swaps from the
RPC thread.

Implemented RPC surface:

- `shader.compile`: compile source and return diagnostics/artifact paths.
- `shader.apply`: apply a previously compiled program to an allowed slot.
- `shader.revert`: restore the previous known-good program.
- `shader.list`: report live shader slots, hashes, backend, and last errors.

The current implementation exposes controlled slots for all bgfx shader stages
used by the sample renderer:

- `terrain_simple.vertex`
- `terrain_simple.fragment`
- `overlay_max_elevation.compute`

Each slot uses the same `shaderc` compiler pipeline as the build, caches output
by content/backend hash, applies the compiled binary on the render thread, and
supports revert to the previous known-good program.

Useful environment variables:

- `TESTBRIDGE_SHADERC`: override shaderc executable path.
- `TESTBRIDGE_SHADER_CACHE_DIR`: override compile cache directory.
- `TESTBRIDGE_LIVE_SHADER_SELFTEST=1`: make the smoke test compile, apply, and
  revert live vertex, fragment, and available compute shader slots.

Do not expose unrestricted filesystem includes or direct backend API handles.
