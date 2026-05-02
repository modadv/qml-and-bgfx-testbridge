# Template Usage

This repository can be used directly as a starter kit or copied into a new
project with `scripts/new_project.py`.

## Create A Project

```powershell
py -3 scripts\new_project.py "Terrain Studio" <workspace>\terrain-studio
```

By default the script copies the full starter kit layout. In this repository
`external/bgfx.cmake` is a Git submodule, not vendored source. Generated
projects should either keep it as a submodule or use `--no-external` and provide
their own bgfx integration.

For the submodule path, initialize after generation or fresh clone:

```powershell
git submodule update --init --recursive
```

The expected top-level submodule commit is:

```text
external/bgfx.cmake 4e42ca1ef501a1e29d25975d735198fa5fad0903
```

Do not commit direct source files under `external/bgfx.cmake` into the main
repository.

After generation:

```powershell
cd <workspace>\terrain-studio
cmake -S . -B .build-release\build
cmake --build .build-release\build --config Release --target terrain_studio -- /m
ctest --test-dir .build-release\build -C Release --output-on-failure
```

## CMake Options

- `ENABLE_TESTBRIDGE=ON`: build and start the local Agent JSON-RPC endpoint.
- `ENABLE_LIVE_SHADER=ON`: enable live shader compile/apply/revert handlers.
- `ENABLE_AGENT_MCP=ON`: run MCP tool unit tests under CTest.
- `ENABLE_RENDER_GOLDEN_TESTS=ON`: register optional golden probe tests.

Production builds should normally set `ENABLE_TESTBRIDGE=OFF`.

## What To Replace

- Replace `src/app/qml/Main.qml` with your first real shell.
- Keep stable `objectName` values for controls and render surfaces.
- Replace or extend `src/engine` while preserving render provider snapshots.
- Add project-specific smoke tests under `tests/`.
- Add project-specific golden baselines under `tests/goldens/`.

The starter app is intentionally small; its purpose is to prove the Agent loop.
