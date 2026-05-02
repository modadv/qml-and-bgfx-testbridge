# Recipe: Package Consumer Check

1. Build `testbridge`.
2. Install to a temporary prefix.
3. Configure `tests/cmake_package_consumer`.
4. Build the consumer and verify it links `TestBridge::testbridge`.
