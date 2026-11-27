import QtQuick
import "."

// Pure Qt Quick layered soft shadow for floating surfaces (menus, tooltips, dialogs).
// Avoids QtQuick.Effects so packaged runtimes can keep effectsplugin pruned.
Item {
    id: root
    property real radius: 8
    property int spread: 1

    anchors.fill: parent
    z: -1

    Rectangle {
        anchors.fill: parent
        anchors.margins: -3 * root.spread
        anchors.topMargin: 0
        anchors.bottomMargin: -5 * root.spread
        radius: root.radius + 3 * root.spread
        color: Theme.elevationAmbient
    }
    Rectangle {
        anchors.fill: parent
        anchors.margins: -1 * root.spread
        anchors.topMargin: 1
        anchors.bottomMargin: -3 * root.spread
        radius: root.radius + 1 * root.spread
        color: Theme.elevationShadow
    }
}
