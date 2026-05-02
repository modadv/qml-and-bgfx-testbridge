# Recipe: Add QML Control And Test

1. Add the control in QML.
2. Assign a stable `objectName`.
3. Expose state with a property or invokable method.
4. Add smoke assertions using `qml.find`, `qml.meta`, `qml.geometry`,
   `qml.tree`, and `qml.get`.
5. Trigger behavior with `qml.click`, `qml.mouse`, `qml.key`, or `qml.invoke`.
6. Run `testbridge_lab_smoke`.
