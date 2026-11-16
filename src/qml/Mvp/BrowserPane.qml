pragma ComponentBehavior: Bound
// qmllint disable unqualified
import QtQuick
import QtQuick.Controls
import "."

Rectangle {
    id: root
    objectName: "browserPane-" + paneIndex
    required property var controller
    required property var workspaceController
    required property int paneIndex
    property var settingsController: null
    readonly property bool multiplePanes: workspaceController.paneCount !== undefined && workspaceController.paneCount > 1
    property bool active: workspaceController.activePaneIndex === paneIndex
    property bool contentInteractionEnabled: true
    property string iconPrefix: Theme.iconPrefix
    property int displayMode: controller.displayMode
    signal newFolderRequested

    color: Theme.sensorWhite
    border.width: multiplePanes ? 1 : 0
    border.color: Theme.opticalGray
    clip: true

    function activate() { workspaceController.activatePane(paneIndex) }

    function resizeThumbnails(delta, position) {
        const width = Math.max(144, Math.min(320, controller.gridCellWidth + delta))
        if (width === controller.gridCellWidth) return
        // Keep the file under the pointer near the same vertical position on reflow.
        contactSheet.cancelFlick()
        const point = position === undefined ? Qt.point(0, 0) : position
        const row = Math.max(0, (contactSheet.contentY + point.y - contactSheet.originY) / contactSheet.cellHeight)
        const hitIndex = contactSheet.indexAt(point.x + contactSheet.contentX, point.y + contactSheet.contentY)
        const index = hitIndex >= 0 ? hitIndex : Math.floor(row) * contactSheet.gridColumns
        const fraction = row - Math.floor(row)
        activate()
        controller.gridCellWidth = width
        contactSheet.forceLayout()
        contactSheet.contentY = contactSheet.originY + Math.max(0, Math.min(
            (Math.floor(index / contactSheet.gridColumns) + fraction) * contactSheet.cellHeight - point.y,
            contactSheet.contentHeight - contactSheet.height))
        thumbnailSizeFeedback.restart()
    }

    Timer { id: thumbnailSizeFeedback; interval: 1200 }

    Rectangle {
        id: focusRail
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: root.multiplePanes ? 2 : 0
        color: Theme.activePaneAccent
        opacity: root.active ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: Theme.fast } }
    }

    PaneHeader {
        id: paneHeader
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: focusRail.bottom
        controller: root.controller
        workspaceController: root.workspaceController
        paneIndex: root.paneIndex
        settingsController: root.settingsController
        searchReturnTarget: contactSheet
        iconPrefix: root.iconPrefix
    }

    Item {
        id: blankState
        visible: root.controller.currentDirectory.length === 0
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: paneHeader.bottom
        anchors.bottom: parent.bottom

        Column {
            anchors.centerIn: parent
            width: Math.min(parent.width - 32, 300)
            spacing: 10
            Image {
                anchors.horizontalCenter: parent.horizontalCenter
                width: 34
                height: 34
                source: root.iconPrefix + "folder-pane-plus.svg"
                sourceSize: Qt.size(68, 68)
                opacity: 0.58
            }
            Text {
                width: parent.width
                text: blankDropArea.containsDrag ? qsTr("Drop to open here")
                      : root.active ? qsTr("Choose a folder from the sidebar")
                                    : qsTr("Select this area to choose a folder")
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                color: Theme.graphiteInk
                font.family: Theme.uiFont
                font.pixelSize: 13
                font.weight: Font.Medium
            }
            Text {
                width: parent.width
                text: root.active ? qsTr("The file tree and toolbar now control this pane")
                                  : qsTr("This pane keeps its own folder and view settings")
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                color: Theme.mutedInk
                font.family: Theme.uiFont
                font.pixelSize: 11
            }
        }
        MouseArea {
            anchors.fill: parent
            onClicked: root.activate()
        }
        DropArea {
            id: blankDropArea
            anchors.fill: parent
            onEntered: function(drag) {
                if (drag.hasUrls) {
                    root.activate()
                    drag.acceptProposedAction()
                }
            }
            onDropped: function(drop) {
                if (drop.hasUrls) {
                    root.controller.openDroppedUrls(drop.urls)
                    drop.acceptProposedAction()
                }
            }
            Rectangle {
                anchors.fill: parent
                anchors.margins: 7
                visible: blankDropArea.containsDrag
                radius: 6
                color: "#127893A6"
                border.color: Theme.primaryButton
                border.width: 1
            }
        }
    }

    DropArea {
        id: paneDropArea
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: paneHeader.bottom
        anchors.bottom: parent.bottom
        enabled: root.controller.currentDirectory.length > 0
        onEntered: function(drag) {
            if (drag.hasUrls) {
                root.activate()
                drag.acceptProposedAction()
            }
        }
        onDropped: function(drop) {
            if (drop.hasUrls) {
                root.controller.copyDroppedUrls(drop.urls)
                drop.acceptProposedAction()
            }
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: 7
            visible: paneDropArea.containsDrag
            color: "#0A7893A6"
            border.color: Theme.primaryButton
            border.width: 1
            radius: 6
        }
    }

    Column {
        objectName: "paneEmptyState"
        z: 2
        anchors.centerIn: contactSheet
        width: Math.min(contactSheet.width - 32, 420)
        visible: contactSheet.visible && contactSheet.count === 0
        spacing: 12
        Text {
            objectName: "paneEmptyMessage"
            width: parent.width
            text: root.controller.filterText.length > 0
                  ? qsTr("No files match “%1”").arg(root.controller.filterText)
                  : qsTr("No supported images in this folder\nChoose another folder or drop images here")
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WrapAnywhere
            color: Theme.mutedInk
            font.family: Theme.uiFont
            font.pixelSize: 13
            lineHeight: 1.45
        }
        AppIconButton {
            objectName: "clearEmptySearchButton"
            anchors.horizontalCenter: parent.horizontalCenter
            visible: root.controller.filterText.length > 0
            text: qsTr("Clear search")
            onClicked: {
                root.activate()
                root.controller.setFilterText("")
            }
        }
    }


    GridView {
        id: contactSheet
        objectName: "paneContactSheet-" + root.paneIndex
        visible: root.controller.currentDirectory.length > 0
        z: 1
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: paneHeader.bottom
        anchors.bottom: parent.bottom
        anchors.margins: 16
        clip: true
        model: root.controller.thumbnails
        // Choose the closest density instead of always stretching fewer, larger cards.
        // Keep cards at least 144px wide when the pane has room for that minimum.
        readonly property int gridColumns: Math.max(1, Math.min(
            Math.floor((width + 12) / 156),
            Math.round((width + 12) / (Math.max(144, root.controller.gridCellWidth) + 12))))
        readonly property int visualCellWidth: Math.max(1, Math.floor(width / gridColumns) - 12)
        cellWidth: root.displayMode === 1 ? width : Math.floor(width / gridColumns)
        cellHeight: root.displayMode === 1 ? 76 : Math.round(visualCellWidth * 0.75) + 52
        boundsBehavior: Flickable.StopAtBounds
        interactive: root.contentInteractionEnabled
        reuseItems: true
        cacheBuffer: Math.max(0, height)
        keyNavigationEnabled: root.active
        activeFocusOnTab: true
        property bool keyboardNavigating: false
        readonly property bool keyboardFocusVisible: activeFocus && keyboardNavigating
        function isBlankAt(x, y) {
            const item = itemAtIndex(indexAt(x + contentX, y + contentY))
            return !item || !item.containsThumbnailPoint(x, y)
        }
        onActiveFocusChanged: keyboardNavigating = activeFocus
        Keys.onPressed: function(event) {
            if ([Qt.Key_Left, Qt.Key_Right, Qt.Key_Up, Qt.Key_Down, Qt.Key_Home, Qt.Key_End].indexOf(event.key) >= 0) {
                keyboardNavigating = true
                root.activate()
                event.accepted = false
            } else if ((event.key === Qt.Key_Space || event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && currentItem) {
                keyboardNavigating = true
                root.activate()
                const tile = currentItem.fileTile
                root.workspaceController.selectPath(root.paneIndex, tile.path, false, event.key === Qt.Key_Space)
                if (event.key !== Qt.Key_Space) root.controller.activatePath(tile.path)
                event.accepted = true
            }
        }
        onInteractiveChanged: {
            if (!interactive)
                cancelFlick()
        }
        ThumbnailViewport { view: contactSheet; controller: root.controller }
        delegate: Item {
            readonly property alias fileTile: thumbnailTile
            function containsThumbnailPoint(x, y) {
                const point = thumbnailTile.mapFromItem(contactSheet, x, y)
                return point.x >= 0 && point.x < thumbnailTile.width &&
                       point.y >= 0 && point.y < thumbnailTile.height
            }
            property bool thumbnailDemand: false
            readonly property bool thumbnailReady: thumbnailTile.thumbnailReady
            required property int index
            required property string path
            required property string fileName
            required property string technicalLabel
            required property string fileType
            required property var dimensions
            required property int bitDepth
            required property string fileSizeText
            required property url thumbnailUrl
            required property bool isDirectory
            required property bool isSelected
            required property int selectionOrdinal
            width: contactSheet.cellWidth
            height: contactSheet.cellHeight
            ThumbnailTile {
                id: thumbnailTile
                x: root.displayMode === 1 ? 0 : 6
                width: root.displayMode === 1 ? Math.min(parent.width - 12, 760)
                                              : contactSheet.visualCellWidth
                height: root.displayMode === 1 ? 68 : parent.height - 6
                displayMode: root.displayMode
                controller: root.controller
                workspaceController: root.workspaceController
                settingsController: root.settingsController
                contentInteractionEnabled: root.contentInteractionEnabled
                path: parent.path
                fileName: parent.fileName
                technicalLabel: parent.technicalLabel
                fileType: parent.fileType
                dimensions: parent.dimensions
                bitDepth: parent.bitDepth
                fileSizeText: parent.fileSizeText
                requestThumbnail: parent.thumbnailDemand
                thumbnailUrl: parent.thumbnailUrl
                directory: parent.isDirectory
                keyboardCurrent: contactSheet.keyboardFocusVisible && parent.GridView.isCurrentItem
                selected: parent.isSelected
                selectionOrdinal: parent.selectionOrdinal
                onSelectionRequested: function(extend, toggle) {
                    contactSheet.currentIndex = parent.index
                    contactSheet.forceActiveFocus(Qt.MouseFocusReason)
                    contactSheet.keyboardNavigating = false
                    root.workspaceController.selectPath(root.paneIndex, path, extend, toggle)
                }
                onActivated: {
                    root.activate()
                    root.controller.activatePath(path)
                }
            }
        }

        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }


        MouseArea {
            anchors.fill: parent
            enabled: root.contentInteractionEnabled
            acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.BackButton | Qt.ForwardButton
            propagateComposedEvents: true
            onClicked: function(mouse) {
                root.activate()
                if (mouse.button === Qt.BackButton) {
                    root.controller.navigateBack()
                    return
                }
                if (mouse.button === Qt.ForwardButton) {
                    root.controller.navigateForward()
                    return
                }
                const blank = contactSheet.isBlankAt(mouse.x, mouse.y)
                if (blank && mouse.button === Qt.RightButton) {
                    workspaceMenu.popup(mouse.x, mouse.y)
                    return
                }
                if (blank) root.workspaceController.clearSelection()
                mouse.accepted = blank
            }
        }
    }

    Item {
        id: zoomArea
        objectName: "thumbnailZoomArea"
        z: 2
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: paneHeader.bottom
        anchors.bottom: parent.bottom
        enabled: root.contentInteractionEnabled && root.displayMode === 0 &&
                 root.controller.currentDirectory.length > 0

        component ThumbnailWheel: WheelHandler {
            target: null
            enabled: zoomArea.enabled
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            property real remainder: 0
            onActiveChanged: if (!active) remainder = 0
            onWheel: function(wheel) {
                if (wheel.angleDelta.y === 0 && wheel.pixelDelta.y === 0) {
                    remainder = 0
                    wheel.accepted = false
                    return
                }
                remainder += wheel.angleDelta.y !== 0 ? wheel.angleDelta.y / 120
                                                      : wheel.pixelDelta.y / 40
                const steps = remainder > 0 ? Math.floor(remainder) : Math.ceil(remainder)
                if (steps !== 0) {
                    remainder -= steps
                    root.resizeThumbnails(steps * 16, zoomArea.mapToItem(contactSheet, wheel.x, wheel.y))
                }
                wheel.accepted = true
            }
        }
        ThumbnailWheel { acceptedModifiers: Qt.ControlModifier }
        ThumbnailWheel { enabled: zoomArea.enabled && Qt.platform.os === "osx"; acceptedModifiers: Qt.MetaModifier }

        component ThumbnailPinch: PinchHandler {
            target: null
            enabled: zoomArea.enabled
            acceptedDevices: PointerDevice.TouchPad
            xAxis.enabled: false
            yAxis.enabled: false
            rotationAxis.enabled: false
            property real thumbnailWidth: 160
            onActiveChanged: if (active) {
                thumbnailWidth = root.controller.gridCellWidth
                contactSheet.cancelFlick()
                root.activate()
            }
            onScaleChanged: function(delta) {
                if (!active || delta === 1) return
                thumbnailWidth = Math.max(144, Math.min(320, thumbnailWidth * delta))
                root.resizeThumbnails(Math.round(thumbnailWidth) - root.controller.gridCellWidth,
                    zoomArea.mapToItem(contactSheet, centroid.position.x, centroid.position.y))
            }
        }
        ThumbnailPinch { acceptedModifiers: Qt.ControlModifier }
        // Qt maps the physical Control key to MetaModifier on macOS.
        ThumbnailPinch { enabled: zoomArea.enabled && Qt.platform.os === "osx"; acceptedModifiers: Qt.MetaModifier }
    }

    Rectangle {
        objectName: "thumbnailSizeFeedback"
        visible: thumbnailSizeFeedback.running
        z: 3
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 20
        width: sizeLabel.implicitWidth + 24
        height: 32
        radius: Theme.radius
        color: Theme.raisedSurface
        border.color: Theme.opticalGray
        Text {
            id: sizeLabel
            anchors.centerIn: parent
            text: qsTr("Thumbnails: %1 px").arg(root.controller.gridCellWidth)
            color: Theme.graphiteInk
            font.family: Theme.uiFont
            font.pixelSize: Theme.captionFontSize
        }
    }

    AppMenu {
        id: workspaceMenu
        AppMenuItem {
            text: Qt.platform.os === "osx" ? qsTr("Open in Finder")
                                          : qsTr("Open in File Explorer")
            onTriggered: root.controller.openCurrentDirectoryInFileManager()
        }
        AppMenuSeparator {}
        AppMenuItem { text: qsTr("Refresh"); onTriggered: root.controller.refresh() }
        AppMenuItem { text: qsTr("Select all"); onTriggered: root.controller.selectAll() }
        AppMenuItem { text: qsTr("Paste"); enabled: root.controller.canPaste; onTriggered: root.controller.pasteItems() }
        AppMenuSeparator {}
        AppMenuItem { text: qsTr("New folder…"); onTriggered: root.newFolderRequested() }
    }

}
