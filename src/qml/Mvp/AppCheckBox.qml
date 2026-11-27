import QtQuick
import QtQuick.Controls
import "."

// Styled checkbox for dialog option rows. Uses the shared input tokens so it
// reads as part of the same control family as AppTextField and SettingsButton.
CheckBox {
    id: root

    spacing: 8
    implicitHeight: 32
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    opacity: enabled ? 1 : Theme.disabledOpacity
    font.family: Theme.uiFont
    font.pixelSize: Theme.captionFontSize

    indicator: Rectangle {
        implicitWidth: 18
        implicitHeight: 18
        x: root.leftPadding
        y: parent.height / 2 - height / 2
        radius: 4
        color: root.checked ? Theme.primaryButton : Theme.searchFieldSurface
        border.width: root.visualFocus ? 2 : 1
        border.color: root.visualFocus ? Theme.probeBlue
                    : root.checked ? Theme.primaryButton
                    : root.hovered ? Theme.searchFieldHoverBorder : Theme.searchFieldBorder
        Text {
            anchors.centerIn: parent
            visible: root.checked
            text: "✓"
            color: Theme.primaryButtonText
            font.family: Theme.uiFont
            font.pixelSize: 12
            font.weight: Theme.emphasisFontWeight
        }
    }

    contentItem: Text {
        text: root.text
        color: Theme.graphiteInk
        font.family: root.font.family
        font.pixelSize: root.font.pixelSize
        verticalAlignment: Text.AlignVCenter
        leftPadding: root.indicator.width + root.spacing
    }
}
