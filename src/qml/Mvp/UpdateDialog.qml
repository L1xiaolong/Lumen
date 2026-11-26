pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "."

Dialog {
    id: root
    objectName: "updateDialog"
    required property var controller
    readonly property string stateName: controller.updateState
    readonly property bool busy: ["checking", "downloading", "verifying", "ready", "installing"].includes(stateName)
    readonly property bool showNotes: stateName === "available"
    readonly property bool hasDownloadProgress: stateName === "downloading" && controller.updateDownloadProgress >= 0

    anchors.centerIn: parent
    width: Math.min(showNotes ? 560 : 420, parent ? parent.width - 48 : 560)
    height: Math.min(layout.implicitHeight + topPadding + bottomPadding,
                     parent ? parent.height - 48 : 560)
    padding: 24
    modal: true
    focus: true
    closePolicy: busy ? Popup.NoAutoClose : Popup.CloseOnEscape
    onOpened: if (closeButton.visible) closeButton.forceActiveFocus()
    onStateNameChanged: {
        if (showNotes)
            notesFlickable.contentY = 0
        if (opened)
            Qt.callLater(root.focusStateAction)
    }
    function focusStateAction() {
        if (!opened)
            return
        if (cancelButton.visible)
            cancelButton.forceActiveFocus()
        else if (openInstallerButton.visible)
            openInstallerButton.forceActiveFocus()
        else if (retryButton.visible)
            retryButton.forceActiveFocus()
        else if (closeButton.visible)
            closeButton.forceActiveFocus()
        else
            layout.forceActiveFocus()
    }

    Overlay.modal: Rectangle { color: Theme.overlay }
    background: Rectangle {
        radius: 12
        color: Theme.paperWhite
        border.color: Theme.opticalGray
    }

    contentItem: ColumnLayout {
        id: layout
        spacing: 20

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            Rectangle {
                Layout.preferredWidth: 40
                Layout.preferredHeight: 40
                Layout.alignment: Qt.AlignTop
                color: Theme.accentSurface
                radius: 8
                Image {
                    anchors.centerIn: parent
                    width: Theme.iconSize
                    height: Theme.iconSize
                    source: Theme.iconPrefix + "nav-downloads.svg"
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 4

                Text {
                    Layout.fillWidth: true
                    text: {
                        switch (root.stateName) {
                        case "available": return qsTr("Lumen %1 is available").arg(root.controller.latestVersion)
                        case "downloaded": return qsTr("Download complete")
                        case "error": return qsTr("Update failed")
                        case "latest": return qsTr("Lumen is up to date.")
                        default: return qsTr("Updating Lumen")
                        }
                    }
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    color: Theme.graphiteInk
                    font.family: Theme.uiFont
                    font.pixelSize: 20
                    font.weight: Font.DemiBold
                }
                Text {
                    Layout.fillWidth: true
                    visible: root.showNotes || (root.busy && root.stateName !== "checking") ||
                             root.stateName === "downloaded"
                    text: root.showNotes
                          ? qsTr("Current version %1").arg(root.controller.applicationVersion)
                          : qsTr("Version %1").arg(root.controller.latestVersion)
                    textFormat: Text.PlainText
                    color: Theme.mutedInk
                    font.family: Theme.uiFont
                    font.pixelSize: Theme.captionFontSize
                    wrapMode: Text.Wrap
                }
            }

            AppIconButton {
                id: closeButton
                Layout.alignment: Qt.AlignTop
                visible: !root.busy
                iconSource: Theme.iconPrefix + "close.svg"
                toolTipText: qsTr("Close")
                onClicked: root.close()
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: root.showNotes
            spacing: 8

            Text {
                text: qsTr("What’s new")
                color: Theme.graphiteInk
                font.family: Theme.uiFont
                font.pixelSize: Theme.bodyFontSize
                font.weight: Font.DemiBold
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredHeight: Math.min(320, notesText.implicitHeight + 32)
                color: Theme.raisedSurface
                border.color: notesFlickable.activeFocus ? Theme.keyboardFocus : Theme.opticalGray
                radius: 8

                Flickable {
                    id: notesFlickable
                    anchors.fill: parent
                    anchors.margins: 1
                    clip: true
                    contentWidth: width
                    contentHeight: notesText.implicitHeight + 30
                    boundsBehavior: Flickable.StopAtBounds
                    flickableDirection: Flickable.VerticalFlick
                    ScrollBar.vertical: ScrollBar {
                        policy: notesFlickable.contentHeight > notesFlickable.height
                                ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
                        palette.mid: Theme.mutedInk
                    }

                    activeFocusOnTab: contentHeight > height
                    Keys.onPressed: function(event) {
                        let offset
                        switch (event.key) {
                        case Qt.Key_Up: offset = -Theme.touchTarget; break
                        case Qt.Key_Down: offset = Theme.touchTarget; break
                        case Qt.Key_PageUp: offset = -height; break
                        case Qt.Key_PageDown: offset = height; break
                        default: return
                        }
                        contentY = Math.max(0, Math.min(contentHeight - height, contentY + offset))
                        event.accepted = true
                    }

                    Text {
                        id: notesText
                        x: 15
                        y: 15
                        width: Math.max(0, notesFlickable.width - 34)
                        text: root.controller.releaseNotes || qsTr("This release has no update notes.")
                        textFormat: root.controller.releaseNotes ? Text.RichText : Text.PlainText
                        wrapMode: Text.Wrap
                        color: Theme.graphiteInk
                        linkColor: Theme.accent
                        font.family: Theme.uiFont
                        font.pixelSize: Theme.bodyFontSize
                        onLinkActivated: function(link) {
                            if (link.startsWith("https://")) Qt.openUrlExternally(link)
                        }
                    }
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            visible: statusLabel.text.length > 0
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                Text {
                    id: statusLabel
                    Layout.fillWidth: true
                    text: {
                        switch (root.stateName) {
                        case "available":
                            if (!root.controller.canStartUpdate)
                                return qsTr("No compatible installer is available for this version.")
                            return root.controller.canInstallUpdate
                                ? qsTr("Lumen will restart when the update is ready.")
                                : qsTr("The installer will be ready to open after downloading.")
                        case "checking": return qsTr("Checking for updates…")
                        case "downloading": return qsTr("Downloading update…")
                        case "verifying": return qsTr("Verifying the update…")
                        case "ready": return qsTr("Waiting for file operations to finish…")
                        case "installing": return qsTr("Installing and restarting Lumen…")
                        case "downloaded": return qsTr("Open the installer to finish updating Lumen.")
                        case "error": return root.controller.updateError || qsTr("Couldn’t check for updates. Check your connection and try again.")
                        default: return ""
                        }
                    }
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                    lineHeight: Theme.descriptionLineHeight
                    color: Theme.mutedInk
                    font.family: Theme.uiFont
                    font.pixelSize: root.showNotes ? Theme.captionFontSize : Theme.bodyFontSize
                }
                Text {
                    visible: root.hasDownloadProgress
                    text: qsTr("%1%").arg(root.controller.updateDownloadProgress)
                    color: Theme.graphiteInk
                    font.family: Theme.uiFont
                    font.pixelSize: Theme.captionFontSize
                    font.weight: Font.DemiBold
                }
            }

            ProgressBar {
                objectName: "inAppUpdateProgress"
                Layout.fillWidth: true
                visible: root.busy
                indeterminate: !root.hasDownloadProgress
                from: 0
                to: 100
                value: Math.max(0, root.controller.updateDownloadProgress)
                palette.dark: Theme.accent
                palette.midlight: Theme.opticalGray
                Accessible.name: qsTr("Update progress")
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: ["available", "error", "downloading", "downloaded"].includes(root.stateName)
            spacing: 8

            Item { Layout.fillWidth: true }
            SettingsButton {
                objectName: "ignoreInAppUpdateButton"
                visible: root.showNotes
                text: qsTr("Skip this version")
                onClicked: {
                    root.controller.ignoreUpdate()
                    root.close()
                }
            }
            SettingsButton {
                objectName: "installInAppUpdateButton"
                visible: root.showNotes
                enabled: root.controller.canStartUpdate
                text: qsTr("Update now")
                primary: true
                onClicked: root.controller.startUpdate()
            }
            SettingsButton {
                id: retryButton
                objectName: "retryInAppUpdateButton"
                visible: root.stateName === "error"
                text: qsTr("Try again")
                primary: true
                onClicked: {
                    if (root.controller.canStartUpdate)
                        root.controller.startUpdate()
                    else
                        root.controller.checkForUpdates(true)
                }
            }
            SettingsButton {
                id: cancelButton
                objectName: "cancelInAppUpdateButton"
                visible: root.stateName === "downloading"
                text: qsTr("Cancel download")
                onClicked: root.controller.cancelUpdateDownload()
            }
            SettingsButton {
                id: openInstallerButton
                objectName: "openDownloadedInstallerButton"
                visible: root.stateName === "downloaded"
                text: qsTr("Open installer")
                primary: true
                onClicked: root.controller.openDownloadedUpdate()
            }
        }
    }
}
