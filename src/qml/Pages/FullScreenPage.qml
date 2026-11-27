pragma ComponentBehavior: Bound

import QtQuick
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
    readonly property var panoramaCanvas: panoramaLoader.item
    readonly property bool panoramaActive: root.controller.panoramaEnabled
    readonly property string imageError: panoramaActive
        ? (panoramaCanvas ? panoramaCanvas.errorText : "") : controller.errorText
    property Item messageParent: root
    property bool loadingMessageVisible: false
    readonly property bool imageLoading: root.visible && root.controller.loading
    onImageLoadingChanged: loadingMessageVisible = false
    onVisibleChanged: {
        if (visible)
            return
        // The menu lives in the window overlay, which stays visible when this page is hidden.
        fullScreenContextMenu.dismiss()
        propertiesDialog.close()
    }
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
    readonly property string zoomText: panoramaActive
        ? (panoramaCanvas ? Math.round(panoramaCanvas.horizontalFieldOfView) + "° × "
                            + Math.round(panoramaCanvas.verticalFieldOfView) + "°" : "")
        : (navigationData.zoom || "—%")

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
    Shortcut { enabled: root.imageShortcutsEnabled; sequence: "Left"; onActivated: {
        if (root.panoramaActive && root.panoramaCanvas) root.panoramaCanvas.turn(-1, 0)
        else root.controller.showPrevious()
    } }
    Shortcut { enabled: root.imageShortcutsEnabled; sequence: "Right"; onActivated: {
        if (root.panoramaActive && root.panoramaCanvas) root.panoramaCanvas.turn(1, 0)
        else root.controller.showNext()
    } }
    Shortcut { enabled: root.imageShortcutsEnabled; sequence: "Space"; onActivated: root.controller.showNext() }
    Shortcut { enabled: root.imageShortcutsEnabled; sequence: "F"; onActivated: root.controller.fitImage() }
    Shortcut { enabled: root.imageShortcutsEnabled && !root.panoramaActive; sequence: "1"; onActivated: root.controller.actualPixels() }
    Shortcut { enabled: root.imageShortcutsEnabled && !!root.panoramaCanvas; sequence: "Up"; onActivated: root.panoramaCanvas.turn(0, 1) }
    Shortcut { enabled: root.imageShortcutsEnabled && !!root.panoramaCanvas; sequence: "Down"; onActivated: root.panoramaCanvas.turn(0, -1) }
    Shortcut { enabled: root.imageShortcutsEnabled && !!root.panoramaCanvas; sequences: ["+", "="]; onActivated: root.panoramaCanvas.zoom(1) }
    Shortcut { enabled: root.imageShortcutsEnabled && !!root.panoramaCanvas; sequence: "-"; onActivated: root.panoramaCanvas.zoom(-1) }
    Shortcut { enabled: root.imageShortcutsEnabled && root.controller.panoramaAvailable; sequence: "V";
        onActivated: root.controller.setPanoramaEnabled(!root.panoramaActive) }

    Rectangle { anchors.fill: parent; color: Theme.canvasBackground }

    Loader {
        id: canvasLoader
        anchors.fill: parent
        active: root.visible && !root.panoramaActive
        source: Qt.resolvedUrl("../Mvp/ProductionFullScreenCanvas.qml")
        onLoaded: if (item) {
            item.controller = root.controller
            item.settingsController = root.settingsController
        }
    }

    Loader {
        id: panoramaLoader
        anchors.fill: parent
        active: root.visible && root.panoramaActive
        source: Qt.resolvedUrl("../Mvp/ProductionPanoramaCanvas.qml")
        onLoaded: Qt.callLater(function() {
            if (panoramaLoader.item) panoramaLoader.item.controller = root.controller
        })
    }

    Connections {
        target: root.panoramaCanvas
        ignoreUnknownSignals: true
        function onImageFrameRendered() { root.imageFrameReady() }
        function onContextMenuRequested(position) { fullScreenContextMenu.popup(position.x, position.y) }
        // Double-click on the canvas exits, gated exactly like the Escape shortcut.
        function onCloseRequested() {
            if (root.imageShortcutsEnabled)
                root.closeRequested()
        }
    }

    Binding {
        target: root.panoramaCanvas
        property: "pixelInspectionEnabled"
        value: root.pixelValueVisible && !navigationHover.hovered
    }

    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 20
        width: enterPanorama.implicitWidth + 8
        height: enterPanorama.implicitHeight + 4
        radius: 7
        z: 26
        visible: root.controller.panoramaAvailable && !root.panoramaActive
        color: Theme.sensorWhite
        border.color: Theme.opticalGray

        AppIconButton {
            id: enterPanorama
            objectName: "openPanoramaPreview"
            anchors.centerIn: parent
            text: qsTr("View in 360°")
            iconSource: root.iconPrefix + "panorama.svg"
            shortcutText: "V"
            onClicked: root.controller.setPanoramaEnabled(true)
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
        // Double-click on the canvas exits, gated exactly like the Escape shortcut.
        function onCloseRequested() {
            if (root.imageShortcutsEnabled)
                root.closeRequested()
        }
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

        ImageReadout {
            id: imageName
            objectName: "fullScreenImageInfoText"
            anchors.left: parent.left
            anchors.right: parent.right
            text: [root.controller.fileName, root.resolutionText,
                   root.controller.fileSizeText, root.zoomText].join(", ")
            elide: Text.ElideRight
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

        ImageReadout {
            id: pixelLabel
            objectName: "fullScreenPixelInfoText"
            anchors.centerIn: parent
            text: root.panoramaActive
                  ? ((root.panoramaCanvas && root.panoramaCanvas.pixelReadout) || qsTr("Move over the image"))
                  : root.pixelText
            font.pixelSize: 11
        }
    }

    Rectangle {
        id: navigationOverlay
        objectName: "fullScreenNavigationOverlay"
        readonly property bool panorama: root.panoramaActive
        readonly property var viewportRectangles: panorama
            ? (root.panoramaCanvas ? root.panoramaCanvas.navigationRectangles : [])
            : [root.navigationData.viewport || Qt.rect(0, 0, 0, 0)]
        visible: root.thumbnailVisible && (panorama
                 ? root.panoramaCanvas && root.panoramaCanvas.navigationReady
                 : root.navigationData.visible === true)
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.margins: 16
        width: panorama ? 96 : (root.navigationData.width || 0)
        height: panorama ? 51 : (root.navigationData.height || 0)
        radius: 3
        color: "#991E2227"
        border.width: 1
        border.color: "#80FFFFFF"
        z: 25

        NavigationThumbnail {
            id: navigationImage
            anchors.fill: parent
            anchors.margins: 3
            path: root.controller.currentPath || ""
            sourceSize: Qt.size(90, 65)
            fillMode: Image.Stretch
            smooth: root.smoothDisplay
            mipmap: root.smoothDisplay
            opacity: 0.72
        }
        Repeater {
            model: 2
            Rectangle {
                required property int index
                readonly property rect viewport: navigationOverlay.viewportRectangles[index] || Qt.rect(0, 0, 0, 0)
                visible: width > 0 && height > 0
                x: navigationImage.x + viewport.x * navigationImage.width
                y: navigationImage.y + viewport.y * navigationImage.height
                width: viewport.width * navigationImage.width
                height: viewport.height * navigationImage.height
                color: "#18FFFFFF"
                border.width: 1
                border.color: "white"
            }
        }
        HoverHandler {
            id: navigationHover
            parent: navigationImage
            enabled: navigationOverlay.visible && navigationOverlay.panorama
            cursorShape: Qt.PointingHandCursor
        }
        TapHandler {
            parent: navigationImage
            enabled: navigationHover.enabled
            onTapped: function(eventPoint) {
                const position = eventPoint.position
                root.panoramaCanvas.lookAtSource(position.x / navigationImage.width,
                                                 position.y / navigationImage.height)
                root.forceActiveFocus()
            }
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
        visible: root.visible && ((root.imageLoading && root.loadingMessageVisible)
                                 || root.imageError.length > 0)
        anchors.centerIn: parent
        width: Math.min(460, parent.width - 32)
        height: Math.max(42, messageText.contentHeight + 20)
        radius: 6
        color: Theme.raisedSurface
        border.width: 1
        border.color: root.imageError.length > 0 ? Theme.dangerBorder : Theme.opticalGray
        Text {
            id: messageText
            anchors.centerIn: parent
            width: parent.width - 24
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            text: root.imageError.length > 0
                  ? root.imageError : qsTr("Loading %1…").arg(root.controller.fileName)
            color: root.imageError.length > 0 ? Theme.danger : Theme.graphiteInk
            font.family: Theme.uiFont
            font.pixelSize: 11
        }
    }

    AppMenu {
        id: fullScreenContextMenu
        objectName: "fullScreenContextMenu"
        // Menu.visible opens the popup. Hide the submenu's entry in its parent instead.
        delegate: AppMenuItem {
            visible: subMenu !== scaleMenu || !root.panoramaActive
        }
        onClosed: Qt.callLater(function() {
            if (root.imageShortcutsEnabled)
                root.forceActiveFocus()
        })
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
        AppMenuItem {
            visible: root.controller.panoramaAvailable
            text: root.panoramaActive ? qsTr("Switch to flat view") : qsTr("View in 360°")
            shortcutText: "V"
            onTriggered: root.controller.setPanoramaEnabled(!root.panoramaActive)
        }
        AppMenu {
            id: scaleMenu
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
        selectFileBaseName: true
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
