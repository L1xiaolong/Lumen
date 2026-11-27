import QtQuick
import QtQuick.Controls

ToolTip {
    id: control
    property string path: ""
    property string fileName: path.split(/[\\/]/).pop() || path
    property string technicalLabel: ""
    property bool directory: false
    readonly property string details: {
        if (!directory)
            return technicalLabel
        const separatorIndex = Math.max(path.lastIndexOf("/"), path.lastIndexOf("\\"))
        return separatorIndex >= 0 ? path.slice(0, separatorIndex) : ""
    }
    text: details.length > 0 ? fileName + "\n" + details : fileName
    delay: 800
    timeout: 8000
    padding: 12
    margins: 8
    implicitWidth: Math.min(480, Math.max(240, Math.max(nameLabel.implicitWidth, detailsLabel.implicitWidth) + 24))
    contentItem: Column {
        spacing: 6
        Text {
            id: nameLabel
            width: parent.width
            text: control.fileName
            textFormat: Text.PlainText
            wrapMode: Text.WrapAnywhere
            color: Theme.graphiteInk
            font.family: Theme.uiFont
            font.pixelSize: 12
            font.weight: Font.Medium
        }
        Text {
            id: detailsLabel
            width: parent.width
            text: control.details
            textFormat: Text.PlainText
            wrapMode: Text.WrapAnywhere
            visible: text.length > 0
            color: Theme.mutedInk
            font.family: Theme.uiFont
            font.pixelSize: 11
        }
    }
    enter: Transition {
        ParallelAnimation {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.motionFast; easing.type: Easing.OutCubic }
            NumberAnimation { property: "scale"; from: 0.96; to: 1.0; duration: Theme.motionFast; easing.type: Easing.OutCubic }
        }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.motionFast; easing.type: Easing.InCubic }
    }
    background: Rectangle {
        color: Theme.raisedSurface
        radius: 6
        border.width: 1
        border.color: Theme.borderSubtle
        ElevationShadow { radius: 6; spread: 1 }
    }
}
