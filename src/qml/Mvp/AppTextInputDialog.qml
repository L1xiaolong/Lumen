import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "."

Dialog {
    id: root

    property string dialogTitle: qsTr("")
    property string description: qsTr("")
    property string initialText: ""
    property string acceptText: qsTr("Save")
    property bool selectFileBaseName: false
    readonly property alias inputText: inputField.text

    signal submitted(string text)

    modal: true
    focus: true
    width: 380
    // The content column decides the height, so long descriptions and error
    // messages wrap and grow the dialog instead of clipping or overlapping.
    height: contentLayout.implicitHeight + topPadding + bottomPadding
    x: parent ? Math.round((parent.width - width) / 2) : 0
    y: parent ? Math.round((parent.height - height) / 2) : 0
    padding: 22
    topPadding: 20
    bottomPadding: 20
    closePolicy: Popup.CloseOnEscape

    function openWith(value) {
        initialText = value
        errorLabel.text = ""
        open()
    }

    function submit() {
        if (inputField.text.trim().length > 0)
            submitted(inputField.text)
    }

    function complete(errorMessage) {
        const error = String(errorMessage || "")
        if (error.length === 0)
            close()
        else
            errorLabel.text = error
    }

    onOpened: {
        inputField.text = initialText
        errorLabel.text = ""
        inputField.forceActiveFocus()
        const suffixStart = initialText.lastIndexOf(".")
        if (selectFileBaseName && suffixStart > 0 && suffixStart < initialText.length - 1)
            inputField.select(0, suffixStart)
        else
            inputField.selectAll()
    }

    Overlay.modal: Rectangle {
        color: Theme.overlay
    }

    background: Rectangle {
        color: Theme.paperWhite
        radius: 10
        border.color: Theme.opticalGray
        border.width: 1
    }

    contentItem: ColumnLayout {
        id: contentLayout
        spacing: 12

        Text {
            Layout.fillWidth: true
            text: root.dialogTitle
            color: Theme.graphiteInk
            font.family: Theme.uiFont
            font.pixelSize: Theme.titleFontSize
            font.weight: Theme.emphasisFontWeight
        }

        Text {
            Layout.fillWidth: true
            visible: root.description.length > 0
            text: root.description
            color: Theme.mutedInk
            wrapMode: Text.Wrap
            font.family: Theme.uiFont
            font.pixelSize: Theme.captionFontSize
        }

        AppTextField {
            id: inputField
            objectName: "appTextInputField"
            Layout.fillWidth: true
            onAccepted: root.submit()
            onTextEdited: errorLabel.text = ""
        }

        Text {
            id: errorLabel
            Layout.fillWidth: true
            visible: text.length > 0
            color: Theme.danger
            wrapMode: Text.Wrap
            font.family: Theme.uiFont
            font.pixelSize: Theme.metadataFontSize
        }

        RowLayout {
            Layout.alignment: Qt.AlignRight
            Layout.topMargin: 2
            spacing: 8
            SettingsButton {
                id: cancelButton
                text: qsTr("Cancel")
                onClicked: root.close()
            }
            SettingsButton {
                id: acceptButton
                objectName: "appTextInputAcceptButton"
                text: root.acceptText
                primary: true
                enabled: inputField.text.trim().length > 0
                onClicked: root.submit()
            }
        }
    }
}
