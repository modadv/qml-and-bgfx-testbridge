import QtQuick 2.15
import QtQuick.Window 2.15
import TestBridgeLab.Engine 1.0

Window {
    id: root
    objectName: "main_window"
    width: 1180
    height: 760
    visible: true
    title: "TestBridge Lab"
    color: "#111820"

    Rectangle {
        id: incrementButton
        objectName: "lab_increment_click"
        x: 18
        y: 18
        width: 132
        height: 40
        color: "#2f7d68"
        border.color: "#7ad8bd"
        border.width: 1

        Text {
            anchors.centerIn: parent
            text: "Increment"
            color: "#f3fbf8"
            font.pixelSize: 16
        }

        MouseArea {
            anchors.fill: parent
            onClicked: labController.increment()
        }
    }

    Rectangle {
        objectName: "lab_reset_click"
        x: 162
        y: 18
        width: 92
        height: 40
        color: "#37414c"
        border.color: "#75899a"
        border.width: 1

        Text {
            anchors.centerIn: parent
            text: "Reset"
            color: "#f3fbf8"
            font.pixelSize: 16
        }

        MouseArea {
            anchors.fill: parent
            onClicked: labController.reset()
        }
    }

    Text {
        objectName: "lab_counter_label"
        x: 276
        y: 26
        text: "Counter: " + labController.counter
        color: "#e6edf3"
        font.pixelSize: 18
    }

    Text {
        objectName: "lab_status_label"
        anchors.right: parent.right
        anchors.rightMargin: 18
        y: 28
        text: "Qt Quick + bgfx test surface"
        color: "#9fb2c3"
        font.pixelSize: 14
    }

    Rectangle {
        id: frame
        x: 18
        y: 74
        width: root.width - 36
        height: root.height - 92
        color: "#18232e"
        border.color: "#2f4457"
        border.width: 1

        RenderViewportItem {
            id: engineView
            objectName: "lab_engine_view_3d"
            anchors.fill: parent
            anchors.margins: 1
            heightfieldSource: sampleHeightfieldUrl
            diffuseSource: sampleDiffuseUrl
            imageScaleX: 1.0
            imageScaleY: 1.0
            imageRotation: 0.0
            heightPixelSize: 0.02

            Component.onCompleted: {
                setOverlayUseScreenSpace(false)
                setOverlayDebugAxes(true)
                setOverlayRects([
                    {"id": 1, "x": -0.35, "y": -0.35, "w": 0.25, "h": 0.25},
                    {"id": 2, "x": 0.15, "y": 0.1, "w": 0.3, "h": 0.2}
                ])
            }
        }
    }
}
