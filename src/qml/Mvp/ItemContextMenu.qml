import QtQuick
import "."

AppMenu {
    id: root

    required property var controller
    property var workspaceController: controller
    property string itemPath: ""
    property bool isFolder: false
    readonly property bool isRawFile: {
        const path = itemPath.toLowerCase()
        return !isFolder && (path.endsWith(".raw") || path.endsWith(".yuv"))
    }
    readonly property bool folderIsFavorite: isFolder && !!workspaceController &&
        !!workspaceController.favoriteFolders && workspaceController.favoriteFolders.length > 0 &&
        workspaceController.isFavoriteFolder(itemPath)
    signal openRequested()
    signal resizeRequested()

    // Menu.visible controls the popup. Filter the entry in its parent instead.
    delegate: AppMenuItem {
        visible: subMenu !== imageOperations || !root.isFolder
    }

    AppMenuItem {
        visible: !root.isFolder
        text: qsTr("Open full screen")
        onTriggered: root.openRequested()
    }
    AppMenuItem {
        visible: root.isFolder
        text: qsTr("Open folder")
        onTriggered: root.openRequested()
    }
    AppMenuItem {
        visible: !root.isFolder
        text: qsTr("Compare selected")
        enabled: root.workspaceController.canCompare
        onTriggered: root.workspaceController.compareSelected()
    }
    AppMenuItem {
        visible: root.isRawFile
        text: qsTr("RAW/YUV parameters…")
        enabled: root.controller.canEditRaw
        onTriggered: root.controller.editSelectedRawParameters()
    }
    ImageOperationsMenu {
        id: imageOperations
        controller: root.controller
        onResizeRequested: root.resizeRequested()
    }
    AppMenuItem {
        visible: !root.isFolder && root.controller.canRestoreSelected
        text: qsTr("Restore original")
        onTriggered: root.controller.restoreSelected()
    }
    AppMenuItem {
        visible: root.isFolder
        text: qsTr("Paste into folder")
        enabled: root.controller.canPaste
        onTriggered: root.controller.pasteItemsInto(root.itemPath)
    }
    AppMenuItem {
        objectName: "itemPinFolderAction"
        visible: root.isFolder && !!root.workspaceController && !!root.workspaceController.favoriteFolderModel
        text: root.folderIsFavorite ? qsTranslate("FolderNavigator", "Unpin from Favorites")
                                    : qsTranslate("FolderNavigator", "Pin to Favorites")
        onTriggered: root.workspaceController.setFolderFavorite(root.itemPath, !root.folderIsFavorite)
    }
    AppMenuSeparator {}
    AppMenuItem {
        text: qsTr("Cut")
        shortcutText: Qt.platform.os === "osx" ? "⌘X" : "Ctrl+X"
        onTriggered: root.controller.copySelected(true)
    }
    AppMenuItem {
        text: qsTr("Copy")
        shortcutText: Qt.platform.os === "osx" ? "⌘C" : "Ctrl+C"
        onTriggered: root.controller.copySelected(false)
    }
    AppMenuItem {
        text: qsTr("Rename…")
        enabled: root.controller.selectionCount === 1
        onTriggered: root.controller.renameSelected()
    }
    AppMenuItem {
        text: qsTr("Move to Trash")
        destructive: true
        onTriggered: root.controller.moveSelectedToTrash()
    }
    AppMenuSeparator {}
    AppMenuItem {
        text: (Qt.platform.os === "osx" ? qsTr("Reveal in Finder") : qsTr("Show in File Explorer"))
        enabled: root.controller.selectionCount === 1
        onTriggered: root.controller.revealSelected()
    }
    AppMenuItem {
        text: qsTr("Properties")
        enabled: root.controller.selectionCount === 1
        onTriggered: root.controller.showSelectedProperties()
    }
}
