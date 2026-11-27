import QtQuick
import QtQuick.Controls
import "."

Dialog {
    id: root
    property string dialogTitle: qsTr("")
    property string message: qsTr("")
    property string confirmText: qsTr("Confirm")
    property bool showCancelButton: true
    property bool destructive: false
    property bool completedWithError: false
    signal confirmed()

    modal: true
    focus: true
    width: Math.min(440, parent ? parent.width - 48 : 440)
    height: dialogContent.implicitHeight + 40
    x: parent ? Math.round((parent.width - width) / 2) : 0
    y: parent ? Math.round((parent.height - height) / 2) : 0
    padding: 20
    closePolicy: Popup.CloseOnEscape
    function showConfirmation() {
        completedWithError = false
        errorLabel.text = ""
        open()
    }
    function confirmAction() {
        if (completedWithError) close()
        else confirmed()
    }
    function complete(errorMessage) {
        const error = String(errorMessage || "")
        if (error.length === 0) close()
        else {
            errorLabel.text = error
            completedWithError = true
        }
    }
    Overlay.modal: Rectangle { color: Theme.overlay }
    enter: Transition {
        ParallelAnimation {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.motionNormal; easing.type: Easing.OutCubic }
            NumberAnimation { property: "scale"; from: 0.97; to: 1.0; duration: Theme.motionNormal; easing.type: Easing.OutCubic }
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
        radius: 10
        border.color: Theme.borderStrong
        ElevationShadow { radius: 10; spread: 2 }
    }
    contentItem: Column {
        id: dialogContent
        spacing: 16
        Text {
            width: parent.width
            text: root.completedWithError ? qsTr("Operation incomplete") : root.dialogTitle
            color: Theme.graphiteInk
            font.family: Theme.uiFont
            font.pixelSize: Theme.titleFontSize
            font.weight: Theme.emphasisFontWeight
            wrapMode: Text.Wrap
        }
        Text {
            width: parent.width
            text: root.completedWithError ? errorLabel.text : root.message
            textFormat: Text.PlainText
            color: root.completedWithError ? Theme.danger : Theme.mutedInk
            wrapMode: Text.Wrap
            font.family: Theme.uiFont
            font.pixelSize: 12
            lineHeight: Theme.descriptionLineHeight
        }
        Text { id: errorLabel; visible: false }
        Flow {
            width: parent.width
            layoutDirection: Qt.RightToLeft
            spacing: 8
            SettingsButton {
                text: root.completedWithError ? qsTr("Close") : root.confirmText
                focus: !root.showCancelButton
                primary: !root.destructive || root.completedWithError
                destructive: root.destructive && !root.completedWithError
                onClicked: root.confirmAction()
            }
            SettingsButton {
                visible: root.showCancelButton && !root.completedWithError
                text: qsTr("Cancel")
                onClicked: root.close()
            }
        }
    }
}
