pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import "."

Item {
    id: root
    property var labels: []
    property int currentIndex: 0
    property string tabObjectNamePrefix: "inspectorTab-"
    signal selected(int index)
    readonly property real tabWidth: width / Math.max(1, labels.length)
    height: visible ? 36 : 0

    Row {
        anchors.fill: parent
        Repeater {
            model: root.labels
            delegate: Button {
                id: tab
                required property int index
                required property string modelData
                objectName: root.tabObjectNamePrefix + index
                width: root.tabWidth
                height: 36
                text: modelData
                hoverEnabled: true
                Accessible.name: text
                Accessible.role: Accessible.PageTab
                Accessible.selected: root.currentIndex === index
                onClicked: root.selected(index)
                contentItem: Text {
                    text: tab.text
                    color: root.currentIndex === tab.index ? Theme.probeBlue : Theme.mutedInk
                    font.family: Theme.uiFont
                    font.pixelSize: 12
                    font.weight: root.currentIndex === tab.index ? Theme.emphasisFontWeight : Font.Normal
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    color: tab.hovered ? Theme.softHover : "transparent"
                    border.width: tab.visualFocus ? 2 : 0
                    border.color: Theme.probeBlue
                }
                Keys.onLeftPressed: root.selected((root.currentIndex + root.labels.length - 1) % root.labels.length)
                Keys.onRightPressed: root.selected((root.currentIndex + 1) % root.labels.length)
            }
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: Theme.borderSubtle
    }

    Rectangle {
        id: activeIndicator
        anchors.bottom: parent.bottom
        x: root.currentIndex * root.tabWidth
        width: root.tabWidth
        height: 2
        radius: 1
        color: Theme.probeBlue
        visible: root.labels.length > 0
        Behavior on x { NumberAnimation { duration: Theme.motionNormal; easing.type: Easing.OutCubic } }
        Behavior on width { NumberAnimation { duration: Theme.motionNormal; easing.type: Easing.OutCubic } }
    }
}
