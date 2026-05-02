# Recipe: Add QML Control And Test

1. Add the control in QML.
2. Assign a stable `objectName`.
3. Expose state with a property or invokable method.
4. Add smoke assertions using `qml.find`, `qml.geometry`, and `qml.get`.
5. Run `testbridge_lab_smoke`.
