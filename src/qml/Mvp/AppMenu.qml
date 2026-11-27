import QtQuick
import QtQuick.Controls

Menu {
    id: control
    // Keep all custom menus attached to the Qt Quick window's input and focus routing.
    popupType: Popup.Item
    implicitWidth: 220
    padding: 6
    topPadding: 6
    bottomPadding: 6
    margins: 8

    delegate: AppMenuItem {}

    enter: Transition {
        ParallelAnimation {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.motionFast; easing.type: Easing.OutCubic }
            NumberAnimation { property: "scale"; from: 0.96; to: 1.0; duration: Theme.motionNormal; easing.type: Easing.OutCubic }
        }
    }
    exit: Transition {
        ParallelAnimation {
            NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.motionFast; easing.type: Easing.InCubic }
            NumberAnimation { property: "scale"; from: 1.0; to: 0.97; duration: Theme.motionFast; easing.type: Easing.InCubic }
        }
    }

    background: Rectangle {
        color: Theme.raisedSurface
        radius: 8
        border.color: Theme.borderStrong
        border.width: 1
    }
}
