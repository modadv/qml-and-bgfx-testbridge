# CMake Package Consumer Check

This fixture verifies that an installed TestBridge package can be consumed by an
external CMake project:

```cmake
find_package(TestBridge CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE TestBridge::testbridge)
```

The root CTest entry `testbridge_package_consumer` installs the current build to
a temporary prefix, configures this fixture, and builds it.
