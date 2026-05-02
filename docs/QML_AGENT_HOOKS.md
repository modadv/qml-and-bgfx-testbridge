# QML Agent Hooks

QML screens must expose stable automation anchors through `objectName`.

## Rules

- Every user-relevant control needs a unique, descriptive `objectName`.
- Render surfaces must expose an `objectName` and stable size constraints.
- Avoid deriving tests from visual text when a stable object name is available.
- Keep state visible through QML properties or invokable controller methods.

## Minimum Hooks

The starter app keeps these required hooks:

- `main_window`: top-level `ApplicationWindow`.
- `lab_engine_view_3d`: bgfx-backed `RenderViewportItem` render surface.
- `lab_increment_click`: sample command button.
- `lab_counter_label`: sample state readback label.

Agents should verify new QML work with `qml.find`, `qml.meta`, `qml.geometry`,
`qml.tree`, and direct interaction tools before relying on screenshots.
