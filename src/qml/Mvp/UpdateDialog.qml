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

    anchors.centerIn: parent
    width: Math.min(540, parent ? parent.width - 48 : 540)
    height: Math.min(480, layout.implicitHeight + 48,
                     parent ? parent.height - 48 : 480)
    padding: 24
    modal: true
    focus: true
    closePolicy: busy ? Popup.NoAutoClose : Popup.CloseOnEscape
    onOpened: closeButton.forceActiveFocus()
    Overlay.modal: Rectangle { color: Theme.overlay }
    background: Rectangle {
        radius: 8
        color: Theme.paperWhite
        border.color: Theme.opticalGray
    }

    contentItem: ColumnLayout {
        id: layout
        spacing: 14

        RowLayout {
            Layout.fillWidth: true
            Text {
                Layout.fillWidth: true
                text: root.stateName === "error" ? qsTr("Update failed")
                      : root.showNotes ? qsTr("Lumen %1 is available").arg(root.controller.latestVersion)
                      : qsTr("Updating Lumen")
                wrapMode: Text.Wrap
                color: Theme.graphiteInk
                font.family: Theme.uiFont
                font.pixelSize: 18
                font.weight: Theme.emphasisFontWeight
            }
            AppIconButton {
                id: closeButton
                visible: !root.busy
                iconSource: Theme.iconPrefix + "close.svg"
                toolTipText: qsTr("Close")
                Accessible.name: toolTipText
                onClicked: root.close()
            }
        }

        Text {
            Layout.fillWidth: true
            visible: root.showNotes
            text: qsTr("Current version %1").arg(root.controller.applicationVersion)
            color: Theme.mutedInk
            font.family: Theme.uiFont
            font.pixelSize: Theme.captionFontSize
        }

        ScrollView {
            id: releaseNotesView
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredHeight: Math.min(280, notesText.implicitHeight)
            visible: root.showNotes
            clip: true
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            Text {
                id: notesText
                width: releaseNotesView.availableWidth
                text: root.controller.releaseNotes || qsTr("This release has no update notes.")
                textFormat: Text.MarkdownText
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

        Text {
            Layout.fillWidth: true
            visible: text.length > 0
            text: {
                switch (root.stateName) {
                case "checking": return qsTr("Checking for updates…")
                case "downloading": {
                    const progress = root.controller.updateDownloadProgress
                    return progress >= 0 ? qsTr("Downloading… %1%").arg(progress) : qsTr("Downloading…")
                }
                case "verifying": return qsTr("Verifying the update…")
                case "ready": return qsTr("Waiting for file operations to finish…")
                case "installing": return qsTr("Installing and restarting Lumen…")
                case "error": return root.controller.updateError || qsTr("Couldn’t check for updates. Check your connection and try again.")
                case "latest": return qsTr("Lumen is up to date.")
                case "available": return root.controller.canInstallUpdate
                    ? qsTr("Lumen will restart when the update is ready.")
                    : qsTr("Open the release page to download this update.")
                default: return ""
                }
            }
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            color: Theme.mutedInk
            font.family: Theme.uiFont
            font.pixelSize: Theme.bodyFontSize
        }

        ProgressBar {
            Layout.fillWidth: true
            visible: root.busy
            indeterminate: root.stateName !== "downloading" || root.controller.updateDownloadProgress < 0
            from: 0
            to: 100
            value: Math.max(0, root.controller.updateDownloadProgress)
            palette.dark: Theme.primaryButton
            palette.midlight: Theme.opticalGray
            Accessible.name: qsTr("Update progress")
        }

        Flow {
            Layout.fillWidth: true
            spacing: 8
            layoutDirection: Qt.RightToLeft
            SettingsButton {
                objectName: "installInAppUpdateButton"
                visible: root.showNotes && root.controller.canInstallUpdate
                text: qsTr("Update and restart")
                primary: true
                onClicked: root.controller.startUpdate()
            }
            SettingsButton {
                objectName: "retryInAppUpdateButton"
                visible: root.stateName === "error"
                text: qsTr("Try again")
                primary: true
                onClicked: root.controller.checkForUpdates(true)
            }
            SettingsButton {
                visible: (root.showNotes && !root.controller.canInstallUpdate) ||
                         (root.stateName === "error" && root.controller.releaseUrl.toString().length > 0)
                text: qsTr("Open release page")
                primary: root.showNotes
                onClicked: root.controller.openReleasePage()
            }
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
                objectName: "cancelInAppUpdateButton"
                visible: root.stateName === "downloading"
                text: qsTr("Cancel download")
                onClicked: root.controller.cancelUpdateDownload()
            }
        }
    }
}
