# Task: Add A QML Panel

1. Add the component under `src/app/qml`.
2. Give every interactive item a stable `objectName`.
3. Expose state through properties or invokable methods.
4. Add smoke assertions that find the item, read geometry, and exercise input.
5. Run `ctest --test-dir .build-release\build -C Release --output-on-failure`.
