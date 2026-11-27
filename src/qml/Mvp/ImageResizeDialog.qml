import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "."

Dialog {
    id: root
    objectName: "imageResizeDialog"

    property var controller: null
    property bool percentageMode: false
    property bool keepAspectRatio: true
    property int sourceWidth: 0
    property int sourceHeight: 0
    property bool updatingFields: false
    property string errorText: ""
    property bool awaitingResize: false

    Connections {
        target: root.controller
        ignoreUnknownSignals: true
        function onTransformFinished(path, error) {
            if (!root.awaitingResize) return
            root.awaitingResize = false
            if (error.length > 0) root.errorText = error
            else root.close()
        }
    }

    modal: true
    focus: true
    width: 430
    height: contentLayout.implicitHeight + topPadding + bottomPadding
    x: parent ? Math.round((parent.width - width) / 2) : 0
    y: parent ? Math.round((parent.height - height) / 2) : 0
    padding: 22
    topPadding: 18
    bottomPadding: 18
    closePolicy: awaitingResize ? Popup.NoAutoClose : Popup.CloseOnEscape

    function openForSelection(sourceController) {
        controller = sourceController
        const size = controller ? controller.selectedImageSize : Qt.size(0, 0)
        sourceWidth = Number(size.width || 0)
        sourceHeight = Number(size.height || 0)
        percentageMode = false
        keepAspectRatio = true
        errorText = ""
        updatingFields = true
        widthField.text = sourceWidth > 0 ? String(sourceWidth) : ""
        heightField.text = sourceHeight > 0 ? String(sourceHeight) : ""
        updatingFields = false
        open()
        widthField.forceActiveFocus()
        widthField.selectAll()
    }

    function setPercentageMode(percentage) {
        if (percentageMode === percentage)
            return
        percentageMode = percentage
        updatingFields = true
        if (percentage) {
            widthField.text = "100"
            heightField.text = "100"
        } else {
            widthField.text = String(sourceWidth)
            heightField.text = String(sourceHeight)
        }
        updatingFields = false
    }

    function updateLinkedField(changedWidth) {
        if (updatingFields || !keepAspectRatio || sourceWidth <= 0 || sourceHeight <= 0)
            return
        updatingFields = true
        if (percentageMode) {
            if (changedWidth)
                heightField.text = widthField.text
            else
                widthField.text = heightField.text
        } else if (changedWidth) {
            const widthValue = Number(widthField.text)
            if (widthValue > 0)
                heightField.text = String(Math.max(1, Math.round(widthValue * sourceHeight / sourceWidth)))
        } else {
            const heightValue = Number(heightField.text)
            if (heightValue > 0)
                widthField.text = String(Math.max(1, Math.round(heightValue * sourceWidth / sourceHeight)))
        }
        updatingFields = false
    }

    function applyResize() {
        if (awaitingResize || !controller || sourceWidth <= 0 || sourceHeight <= 0)
            return
        let targetWidth = Number(widthField.text)
        let targetHeight = Number(heightField.text)
        const minimum = percentageMode ? 0.01 : 1
        if (!Number.isFinite(targetWidth) || !Number.isFinite(targetHeight) ||
                targetWidth < minimum || targetHeight < minimum) {
            errorText = qsTr("Enter positive numeric values.")
            return
        }
        if (percentageMode) {
            targetWidth = Math.max(1, Math.round(sourceWidth * targetWidth / 100))
            targetHeight = Math.max(1, Math.round(sourceHeight * targetHeight / 100))
        }
        // Check the range before Qt converts the JavaScript numbers to C++ int.
        if (Math.round(targetWidth) > 100000 || Math.round(targetHeight) > 100000) {
            errorText = qsTranslate("FileOperations", "Enter dimensions between 1 and 100,000 pixels.")
            return
        }
        const error = controller.resizeSelected(Math.round(targetWidth), Math.round(targetHeight))
        if (error.length > 0) {
            errorText = error
            return
        }
        if (controller.transformBusy === true) awaitingResize = true
        else close()
    }

    Overlay.modal: Rectangle { color: Theme.overlay }
    background: Rectangle {
        radius: 10
        color: Theme.paperWhite
        border.width: 1
        border.color: Theme.opticalGray
    }

    contentItem: ColumnLayout {
        id: contentLayout
        spacing: 12

        Text {
            Layout.fillWidth: true
            text: qsTr("Resize image")
            color: Theme.graphiteInk
            font.family: Theme.uiFont
            font.pixelSize: Theme.titleFontSize
            font.weight: Theme.emphasisFontWeight
        }
        Text {
            Layout.fillWidth: true
            text: root.sourceWidth > 0
                  ? qsTr("Original: %1 × %2 px").arg(root.sourceWidth).arg(root.sourceHeight)
                  : qsTr("Image dimensions unavailable")
            color: Theme.mutedInk
            font.family: Theme.monoFont
            font.pixelSize: Theme.metadataFontSize
        }

        // Segmented unit switch: the active segment is raised and accented.
        Item {
            id: unitSwitch
            objectName: "unitSwitch"
            implicitWidth: 196
            implicitHeight: 34
            enabled: !root.awaitingResize

            ButtonGroup { id: units }

            Rectangle {
                anchors.fill: parent
                radius: 6
                color: Theme.softHover
                border.width: 1
                border.color: Theme.opticalGray
            }

            Button {
                id: pixelsSegment
                x: 2
                y: 2
                width: (parent.width - 6) / 2
                height: parent.height - 4
                text: qsTr("Pixels")
                hoverEnabled: true
                focusPolicy: Qt.StrongFocus
                checkable: true
                checked: !root.percentageMode
                ButtonGroup.group: units
                onClicked: root.setPercentageMode(false)
                background: Rectangle {
                    radius: 4
                    color: !root.percentageMode ? Theme.raisedSurface : "transparent"
                    border.width: pixelsSegment.visualFocus ? 2 : 0
                    border.color: Theme.probeBlue
                }
                contentItem: Text {
                    text: pixelsSegment.text
                    color: !root.percentageMode ? Theme.accent : Theme.mutedInk
                    font.family: Theme.uiFont
                    font.pixelSize: Theme.captionFontSize
                    font.weight: !root.percentageMode ? Theme.emphasisFontWeight : Theme.bodyFontWeight
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }
            Button {
                id: percentSegment
                x: parent.width - width - 2
                y: 2
                width: (parent.width - 6) / 2
                height: parent.height - 4
                text: qsTr("Percent")
                hoverEnabled: true
                focusPolicy: Qt.StrongFocus
                checkable: true
                checked: root.percentageMode
                ButtonGroup.group: units
                onClicked: root.setPercentageMode(true)
                background: Rectangle {
                    radius: 4
                    color: root.percentageMode ? Theme.raisedSurface : "transparent"
                    border.width: percentSegment.visualFocus ? 2 : 0
                    border.color: Theme.probeBlue
                }
                contentItem: Text {
                    text: percentSegment.text
                    color: root.percentageMode ? Theme.accent : Theme.mutedInk
                    font.family: Theme.uiFont
                    font.pixelSize: Theme.captionFontSize
                    font.weight: root.percentageMode ? Theme.emphasisFontWeight : Theme.bodyFontWeight
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }

        GridLayout {
            Layout.fillWidth: true
            enabled: !root.awaitingResize
            columns: 2
            columnSpacing: 14
            rowSpacing: 10

            Text {
                objectName: "resizeWidthLabel"
                Layout.alignment: Qt.AlignVCenter
                text: root.percentageMode ? qsTr("Horizontal (%)") : qsTr("Width (px)")
                color: Theme.mutedInk
                font.family: Theme.uiFont
                font.pixelSize: Theme.metadataFontSize
            }
            AppTextField {
                id: widthField
                objectName: "resizeWidthField"
                Layout.fillWidth: true
                inputMethodHints: Qt.ImhFormattedNumbersOnly
                validator: DoubleValidator { bottom: 0.01; top: 100000 }
                onTextEdited: root.updateLinkedField(true)
                onAccepted: root.applyResize()
            }
            Text {
                objectName: "resizeHeightLabel"
                Layout.alignment: Qt.AlignVCenter
                text: root.percentageMode ? qsTr("Vertical (%)") : qsTr("Height (px)")
                color: Theme.mutedInk
                font.family: Theme.uiFont
                font.pixelSize: Theme.metadataFontSize
            }
            AppTextField {
                id: heightField
                objectName: "resizeHeightField"
                Layout.fillWidth: true
                inputMethodHints: Qt.ImhFormattedNumbersOnly
                validator: DoubleValidator { bottom: 0.01; top: 100000 }
                onTextEdited: root.updateLinkedField(false)
                onAccepted: root.applyResize()
            }
        }

        AppCheckBox {
            objectName: "keepAspectCheckBox"
            text: qsTr("Keep aspect ratio")
            enabled: !root.awaitingResize
            checked: root.keepAspectRatio
            onToggled: root.keepAspectRatio = checked
        }

        Text {
            Layout.fillWidth: true
            text: root.awaitingResize ? qsTr("Processing…") : root.errorText.length > 0 ? root.errorText : qsTr("Interpolation: bicubic")
            color: root.errorText.length > 0 ? Theme.danger : Theme.mutedInk
            wrapMode: Text.Wrap
            font.family: Theme.uiFont
            font.pixelSize: Theme.metadataFontSize
        }

        RowLayout {
            Layout.alignment: Qt.AlignRight
            Layout.topMargin: 2
            spacing: 8
            SettingsButton {
                text: qsTr("Cancel")
                enabled: !root.awaitingResize
                onClicked: root.close()
            }
            SettingsButton {
                objectName: "resizeAcceptButton"
                text: qsTr("Resize")
                primary: true
                enabled: !root.awaitingResize && root.sourceWidth > 0 && root.sourceHeight > 0
                onClicked: root.applyResize()
            }
        }
    }
}
