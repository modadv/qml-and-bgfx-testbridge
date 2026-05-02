# Bootstrap: Windows System Qt

1. Install Visual Studio C++ tools, CMake, Conan, Python, and Qt 5.15.
2. Initialize submodules:

```powershell
git submodule update --init --recursive
```

3. Configure and build:

```powershell
$env:QT_PREFIX = '<path-to-qt>'
cmake -S . -B .build-release\build -DCMAKE_PREFIX_PATH="$env:QT_PREFIX"
cmake --build .build-release\build --config Release --target testbridge_lab -- /m
ctest --test-dir .build-release\build -C Release --output-on-failure
```
