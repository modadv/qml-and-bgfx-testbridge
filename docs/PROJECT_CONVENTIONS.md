# Project Conventions

## QML

- Every testable item must have a stable `objectName`.
- Avoid dynamically generated object names in test paths.
- Expose state that tests need through properties or invokable methods.
- Keep render surfaces discoverable by object name.

## C++

- TestBridge RPCs must not directly call bgfx.
- Renderer state should be copied into JSON snapshots at safe boundaries.
- Long-running or blocking operations should report diagnostics and timeouts.
- Keep release builds able to disable TestBridge.

## Rendering

- Keep shader slots whitelisted.
- Keep live shader apply/revert on the render thread or an equivalent safe frame boundary.
- Add render resource counters when adding textures, buffers, programs, or passes.
- Prefer explicit artifact output over console-only diagnostics.

## Tests

- Add focused C++ unit tests for pure logic.
- Add real app smoke tests for UI/render behavior.
- Add golden tests only for stable visual outputs.
- When updating goldens, document why the visual output changed.
