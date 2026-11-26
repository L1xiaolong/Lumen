import QtQuick
import "."

AppMenu {
    id: root
    objectName: "imageOperationsMenu"
    required property var controller
    signal resizeRequested()

    title: qsTr("Image operations")
    enabled: controller.canTransform

    AppMenuItem {
        text: qsTr("Rotate 90° counterclockwise")
        onTriggered: root.controller.rotateSelectedCounterClockwise()
    }
    AppMenuItem {
        text: qsTr("Rotate 90° clockwise")
        onTriggered: root.controller.rotateSelectedClockwise()
    }
    AppMenuItem {
        text: qsTr("Resize image…")
        onTriggered: root.resizeRequested()
    }
}
