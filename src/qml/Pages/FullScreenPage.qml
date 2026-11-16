pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "../Mvp"

Item {
    id: root
    objectName: "fullScreenPage"

    required property var controller
    property var propertiesController: null
    property var settingsController: null
    property bool externalModalVisible: false
    property bool smoothDisplay: (!settingsController ||
                                  settingsController.smoothDisplay === undefined)
                                 ? true : settingsController.smoothDisplay
    property string iconPrefix: Theme.iconPrefix
    property string pixelText: qsTr("Move over the image")
    property string inspectedPath: ""
    property bool fileInformationVisible: true
    property bool thumbnailVisible: true
    property bool pixelValueVisible: true
    readonly property var imageCanvas: canvasLoader.item
    property Item messageParent: root
    property bool loadingMessageVisible: false
    readonly property bool imageLoading: root.visible && root.controller.loading
    onImageLoadingChanged: loadingMessageVisible = false
    onVisibleChanged: if (!visible) propertiesDialog.close()
    signal imageFrameReady()

    Timer {
        interval: 180
        running: root.imageLoading
        onTriggered: root.loadingMessageVisible = true
    }
    readonly property var navigationData: {
        if (!imageCanvas)
            return ({ "visible": false })
        const currentNavigationRevision = imageCanvas.navigationRevision
        return imageCanvas.navigationState(0)
    }
    readonly property string resolutionText:
        controller.imageSize && controller.imageSize.width > 0 && controller.imageSize.height > 0
            ? controller.imageSize.width + "*" + controller.imageSize.height : "—*—"
    readonly property string zoomText: navigationData.zoom || "—%"

    signal closeRequested()

    function setSmoothDisplay(enabled) {
        if (root.settingsController && root.settingsController.smoothDisplay !== undefined)
            root.settingsController.smoothDisplay = enabled
        else
            root.smoothDisplay = enabled
    }

    function requestMoveCurrentToTrash() {
        if (!root.settingsController || root.settingsController.confirmTrash) {
            trashDialog.showConfirmation()
            return
        }
        const error = root.controller.moveCurrentToTrash()
        if (error.length > 0) {
            trashDialog.showConfirmation()
            trashDialog.complete(error)
        }
    }

    function open(paths, initialIndex) {
        propertiesDialog.close()
        pixelText = qsTr("Move over the image")
        inspectedPath = ""
        controller.open(paths, initialIndex)
        forceActiveFocus()
    }

    function showPropertiesDialog() {
        inspectedPath = controller.currentPath
        propertiesDialog.openForPath(inspectedPath)
    }

    ShortcutContext { id: shortcutContext; targetWindow: root.Window.window }
    readonly property bool imageShortcutsEnabled: root.visible && !externalModalVisible &&
        !propertiesDialog.visible && !renameDialog.visible && !trashDialog.visible &&
        !fullScreenContextMenu.visible && !shortcutContext.textInputFocused

    Shortcut { enabled: root.imageShortcutsEnabled; sequence: "Esc"; onActivated: root.closeRequested() }
    Shortcut { enabled: root.imageShortcutsEnabled; sequence: "Left"; onActivated: root.controller.showPrevious() }
    Shortcut { enabled: root.imageShortcutsEnabled; sequences: ["Right", "Space"]; onActivated: root.controller.showNext() }
    Shortcut { enabled: root.imageShortcutsEnabled; sequence: "F"; onActivated: root.controller.fitImage() }
    Shortcut { enabled: root.imageShortcutsEnabled; sequence: "1"; onActivated: root.controller.actualPixels() }

    Rectangle { anchors.fill: parent; color: Theme.canvasBackground }

    Loader {
        id: canvasLoader
        anchors.fill: parent
        active: root.visible
        source: Qt.resolvedUrl("../Mvp/ProductionFullScreenCanvas.qml")
        onLoaded: if (item) {
            item.controller = root.controller
            item.settingsController = root.settingsController
        }
    }

    Connections {
        target: root.imageCanvas
        ignoreUnknownSignals: true
        function onPixelHovered(sourceSlot, pixel, valueText, valid) {
            if (!valid) {
                root.pixelText = qsTr("Move over the image")
                return
            }
            root.pixelText = "(" + pixel.x + "," + pixel.y + ") " + valueText
        }
        function onContextMenuRequested(position) {
            fullScreenContextMenu.popup(position.x, position.y)
        }
        function onImageFrameRendered() { root.imageFrameReady() }
    }

    Connections {
        target: root.controller
        function onCloseRequested() { root.closeRequested() }
        function onStateChanged() {
            if (propertiesDialog.opened &&
                    root.controller.currentPath !== root.inspectedPath) {
                root.inspectedPath = root.controller.currentPath
                if (root.propertiesController)
                    root.propertiesController.loadPath(root.inspectedPath)
            }
        }
    }

    Item {
        id: imageInfo
        objectName: "fullScreenImageInfo"
        visible: root.fileInformationVisible
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: 16
        width: Math.max(0, parent.width - 32)
        height: imageName.implicitHeight
        z: 25

        Text {
            id: imageName
            objectName: "fullScreenImageInfoText"
            anchors.left: parent.left
            anchors.right: parent.right
            text: root.controller.fileName + "," + root.resolutionText + "," +
                  root.controller.fileSizeText + "," + root.zoomText
            color: "#00FF00"
            elide: Text.ElideRight
            font.family: Theme.monoFont
            font.pixelSize: 12
            font.weight: Font.Bold
        }
    }

    Item {
        id: pixelInfo
        objectName: "fullScreenPixelInfo"
        visible: root.pixelValueVisible
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 16
        width: pixelLabel.implicitWidth
        height: pixelLabel.implicitHeight
        z: 25

        Text {
            id: pixelLabel
            objectName: "fullScreenPixelInfoText"
            anchors.centerIn: parent
            text: root.pixelText
            color: "#00FF00"
            font.family: Theme.monoFont
            font.pixelSize: 11
        }
    }

    Rectangle {
        id: navigationOverlay
        objectName: "fullScreenNavigationOverlay"
        visible: root.thumbnailVisible && root.navigationData.visible === true
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.margins: 16
        width: root.navigationData.width || 0
        height: root.navigationData.height || 0
        radius: 3
        color: "#991E2227"
        border.width: 1
        border.color: "#80FFFFFF"
        z: 25

        NavigationThumbnail {
            anchors.fill: parent
            anchors.margins: 3
            path: root.controller.currentPath || ""
            sourceSize: Qt.size(90, 65)
            fillMode: Image.Stretch
            smooth: root.smoothDisplay
            mipmap: root.smoothDisplay
            opacity: 0.72
        }
        Rectangle {
            property rect normalizedViewport: root.navigationData.viewport || Qt.rect(0, 0, 0, 0)
            x: 3 + normalizedViewport.x * (parent.width - 6)
            y: 3 + normalizedViewport.y * (parent.height - 6)
            width: normalizedViewport.width * (parent.width - 6)
            height: normalizedViewport.height * (parent.height - 6)
            color: "#18FFFFFF"
            border.width: 1
            border.color: "white"
        }
    }

    ImagePropertiesDialog {
        id: propertiesDialog
        parent: root
        controller: root.propertiesController
        iconPrefix: root.iconPrefix
        // Popup focus restoration is inconsistent after clicking the close button on macOS.
        // Return focus explicitly so the page-level Escape/navigation handlers keep working.
        onClosed: Qt.callLater(function() { root.forceActiveFocus() })
    }

    Rectangle {
        objectName: "fullScreenLoadingMessage"
        parent: root.messageParent
        z: 10001
        visible: root.visible && ((root.controller.loading && root.loadingMessageVisible)
                                 || root.controller.errorText.length > 0)
        anchors.centerIn: parent
        width: Math.min(420, messageText.implicitWidth + 36)
        height: 42
        radius: 6
        color: "#DCEEF1F1"
        border.width: 1
        border.color: "#BCC5C8"
        Text {
            id: messageText
            anchors.centerIn: parent
            text: root.controller.errorText.length > 0
                  ? root.controller.errorText : qsTr("Loading %1…").arg(root.controller.fileName)
            color: root.controller.errorText.length > 0 ? Theme.danger : Theme.graphiteInk
            font.family: Theme.uiFont
            font.pixelSize: 11
        }
    }

    AppMenu {
        id: fullScreenContextMenu
        objectName: "fullScreenContextMenu"
        // Menus can leave focus on their transient popup window after dismissal,
        // notably on macOS. Restore it so the page keeps receiving Escape and
        // navigation shortcuts.
        onClosed: Qt.callLater(function() { root.forceActiveFocus() })
        AppMenuItem { text: qsTr("Cut"); shortcutText: Qt.platform.os === "osx" ? "⌘X" : "Ctrl+X"; onTriggered: root.controller.copyCurrent(true) }
        AppMenuItem { text: qsTr("Copy"); shortcutText: Qt.platform.os === "osx" ? "⌘C" : "Ctrl+C"; onTriggered: root.controller.copyCurrent(false) }
        AppMenuItem { text: qsTr("Rename…"); onTriggered: renameDialog.openWith(root.controller.fileName) }
        AppMenuItem { text: qsTr("Move to Trash"); destructive: true; onTriggered: root.requestMoveCurrentToTrash() }
        AppMenuSeparator {}
        AppMenuItem { text: Qt.platform.os === "osx" ? qsTr("Reveal in Finder") : qsTr("Show in File Explorer"); onTriggered: root.controller.revealCurrent() }
        AppMenuSeparator {}
        AppMenu {
            title: qsTr("Display")
            objectName: "fullScreenDisplayMenu"
            AppMenuItem {
                objectName: "toggleFullScreenFileInformationAction"
                text: root.fileInformationVisible ? qsTr("Hide file information")
                                                  : qsTr("Show file information")
                onTriggered: root.fileInformationVisible = !root.fileInformationVisible
            }
            AppMenuItem {
                objectName: "toggleFullScreenThumbnailAction"
                text: root.thumbnailVisible ? qsTr("Hide thumbnail") : qsTr("Show thumbnail")
                onTriggered: root.thumbnailVisible = !root.thumbnailVisible
            }
            AppMenuItem {
                objectName: "toggleFullScreenPixelValueAction"
                text: root.pixelValueVisible ? qsTr("Hide pixel value")
                                             : qsTr("Show pixel value")
                onTriggered: root.pixelValueVisible = !root.pixelValueVisible
            }
        }
        AppMenu {
            title: qsTr("Scale")
            objectName: "fullScreenScaleMenu"
            AppMenuItem {
                objectName: "fullScreenFitAction"
                text: qsTr("Fit to window")
                shortcutText: "F"
                onTriggered: root.controller.fitImage()
            }
            AppMenuItem {
                objectName: "fullScreenActualPixelsAction"
                text: qsTr("100% (1:1)")
                shortcutText: "1"
                onTriggered: root.controller.actualPixels()
            }
        }
        AppMenuSeparator {}
        AppMenuItem {
            objectName: "fullScreenPropertiesAction"
            text: qsTr("Properties")
            onTriggered: root.showPropertiesDialog()
        }
    }

    AppTextInputDialog {
        id: renameDialog
        objectName: "fullScreenRenameDialog"
        parent: root
        dialogTitle: qsTr("Rename image")
        description: qsTr("Enter a new name for the current image")
        acceptText: qsTr("Rename")
        onSubmitted: function(text) { complete(root.controller.renameCurrentTo(text)) }
    }

    AppConfirmDialog {
        id: trashDialog
        parent: root
        dialogTitle: qsTr("Move to Trash?")
        message: qsTr("The current image will be moved to the system Trash.")
        confirmText: qsTr("Move")
        destructive: true
        onConfirmed: complete(root.controller.moveCurrentToTrash())
    }

    Shortcut { enabled: root.imageShortcutsEnabled; sequence: "PgUp"; onActivated: root.controller.showPrevious() }
    Shortcut { enabled: root.imageShortcutsEnabled; sequence: "PgDown"; onActivated: root.controller.showNext() }
    Shortcut { enabled: root.imageShortcutsEnabled; sequences: [StandardKey.Copy]; onActivated: root.controller.copyCurrent(false) }
    Shortcut { enabled: root.imageShortcutsEnabled; sequences: [StandardKey.Cut]; onActivated: root.controller.copyCurrent(true) }
}
