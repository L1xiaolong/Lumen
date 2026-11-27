pragma ComponentBehavior: Bound
// qmllint disable unqualified
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "."

Rectangle {
    id: root
    required property var controller
    property var workspaceController: null
    readonly property var favoritesModel: workspaceController ? workspaceController.favoriteFolderModel : null
    property bool browsingEnabled: true
    property string contextPath: ""
    property bool contextIsRecent: false
    readonly property bool contextIsFavorite: !!workspaceController &&
        !!workspaceController.favoriteFolders && workspaceController.favoriteFolders.length > 0 &&
        workspaceController.isFavoriteFolder(contextPath)
    property string iconPrefix: Theme.iconPrefix
    property string platformName: Qt.platform.os

    readonly property bool macStyle: platformName === "osx"
    readonly property int indentWidth: 16
    readonly property int itemHeight: 30
    readonly property int iconSize_: 16
    readonly property color selectionBg: Theme.explorerSelectionBg
    readonly property color hoverBg: Theme.softHover
    readonly property color sidebarText: Theme.graphiteInk
    readonly property string nativeFont: Theme.uiFont

    objectName: "folderNavigatorSurface"
    color: Theme.sidebarSurface
    enabled: browsingEnabled
    opacity: browsingEnabled ? 1 : 0.45
    clip: true

    function pathLabel(path) {
        const parts = String(path).split(/[\\/]/)
        return parts.length > 0 && parts[parts.length - 1].length > 0
                ? parts[parts.length - 1] : path
    }

    function favoriteEntry(path) {
        return { label: root.pathLabel(path), path: path, kind: "favorite",
                 remote: root.controller.isNetworkPath(path) }
    }

    function folderIcon(expanded) {
        return root.iconPrefix + (expanded ? "folder-open.svg" : "folder.svg")
    }
    function placeIcon(entry) {
        if (entry.remote) return root.iconPrefix + "nav-network.svg"
        const icons = {home:"nav-home", desktop:"nav-desktop", documents:"nav-documents",
                       downloads:"nav-downloads", pictures:"nav-pictures", drive:"nav-drive"}
        return root.iconPrefix + (icons[entry.kind] || "folder") + ".svg"
    }
    function samePath(left, right) {
        let a = String(left).replace(/[\\/]+$/, "")
        let b = String(right).replace(/[\\/]+$/, "")
        if (root.platformName === "windows") { a = a.toLowerCase(); b = b.toLowerCase() }
        return a === b
    }

    function quickAccessEntries() {
        const entries = []
        const seen = ({})
        const places = root.controller.nativeSidebarPlaces || []
        for (let index = 0; index < places.length; ++index) {
            const place = places[index]
            const key = root.platformName === "windows"
                    ? String(place.path).toLowerCase() : String(place.path)
            if (!seen[key]) {
                entries.push(place)
                seen[key] = true
            }
        }
        return entries
    }

    function recentEntries() {
        const entries = []
        const seen = ({})
        const recent = root.controller.recentFolders || []
        for (let index = 0; index < recent.length && index < 4; ++index) {
            const path = String(recent[index])
            const key = root.platformName === "windows" ? path.toLowerCase() : path
            if (!seen[key]) {
                entries.push({ label: root.pathLabel(path), path: path, kind: "recent",
                               remote: root.controller.isNetworkPath !== undefined &&
                                       root.controller.isNetworkPath(path) })
                seen[key] = true
            }
        }
        return entries
    }

    property var expandedDrives: ({})
    property bool recentExpanded: true
    property bool quickAccessExpanded: true
    property bool locationsExpanded: macStyle || platformName === "windows"

    ListModel {
        id: drivePlaces
        dynamicRoles: true
    }

    function syncDrivePlaces() {
        if (!controller || !drivePlaces)
            return
        const places = platformName === "windows" && browsingEnabled
                ? controller.nativeDrivePlaces || [] : []
        // Update by path so adding/removing a drive does not recreate other expanded trees.
        for (let row = drivePlaces.count - 1; row >= 0; --row) {
            const path = drivePlaces.get(row).place.path
            if (!places.some(function (place) { return place.path === path }))
                drivePlaces.remove(row)
        }
        for (let row = 0; row < places.length; ++row) {
            let existing = -1
            for (let index = row; index < drivePlaces.count; ++index) {
                if (drivePlaces.get(index).place.path === places[row].path) {
                    existing = index
                    break
                }
            }
            if (existing < 0)
                drivePlaces.insert(row, { place: places[row] })
            else {
                if (existing !== row)
                    drivePlaces.move(existing, row, 1)
                drivePlaces.setProperty(row, "place", places[row])
            }
        }
    }

    Component.onCompleted: syncDrivePlaces()
    onPlatformNameChanged: syncDrivePlaces()
    onBrowsingEnabledChanged: syncDrivePlaces()
    onControllerChanged: {
        syncDrivePlaces()
        Qt.callLater(revealActivePath)
    }

    Connections {
        target: root.controller
        function onNativeDrivePlacesChanged() { root.syncDrivePlaces() }
    }

    function belongsTo(path, parentPath) {
        const candidate = String(path).replace(/\\/g, "/")
        const parent = String(parentPath).replace(/\\/g, "/")
        const key = platformName === "windows" ? candidate.toLowerCase() : candidate
        const parentKey = platformName === "windows" ? parent.toLowerCase() : parent
        return key === parentKey || key.startsWith(parentKey.endsWith("/")
                                                   ? parentKey : parentKey + "/")
    }

    function setDriveExpanded(path, expanded) {
        const state = Object.assign({}, expandedDrives)
        state[path] = expanded
        expandedDrives = state
    }

    signal activePathChanged()
    function revealActivePath() {
        if (macStyle && controller.currentDirectory.length > 0)
            locationsExpanded = true
        const state = ({})
        const places = root.controller.nativeDrivePlaces || []
        for (const place of places)
            state[place.path] = root.belongsTo(root.controller.currentDirectory, place.path)
        expandedDrives = state
        activePathChanged()
    }

    Connections {
        target: root.controller
        function onCurrentDirectoryChanged() { root.revealActivePath() }
    }

    component SidebarPlace: Rectangle {
        id: place
        required property var entry
        property int iconX: entry.kind === "drive" ? 32 : 16

        Layout.fillWidth: true
        implicitHeight: root.itemHeight
        color: "transparent"
        readonly property bool isCurrentDir: root.samePath(entry.path, root.controller.currentDirectory)
        readonly property bool selected: entry.kind !== "recent" && entry.kind !== "favorite" && isCurrentDir

        ToolTip.visible: (entry.kind === "recent" || entry.kind === "favorite") &&
                         sidebarPlaceMouse.containsMouse
        ToolTip.delay: 500
        ToolTip.text: String(entry.path)

        Rectangle {
            anchors.fill: parent
            anchors.leftMargin: 6
            anchors.rightMargin: 6
            anchors.topMargin: 1
            anchors.bottomMargin: 1
            radius: 5
            color: parent.selected ? root.selectionBg
                   : sidebarPlaceMouse.containsMouse ? root.hoverBg : "transparent"
        }

        Image {
            x: place.iconX
            anchors.verticalCenter: parent.verticalCenter
            width: root.iconSize_
            height: root.iconSize_
            source: root.placeIcon(parent.entry)
            sourceSize: Qt.size(32, 32)
            opacity: parent.selected ? 0.95 : 0.72
        }

        Text {
            objectName: "sidebarPlaceLabel-" + parent.entry.path
            x: place.iconX + 24
            width: Math.max(0, parent.width - x - 12)
            anchors.verticalCenter: parent.verticalCenter
            text: parent.entry.label
            elide: Text.ElideMiddle
            color: parent.selected ? Theme.accent : root.sidebarText
            font.family: root.nativeFont
            font.pixelSize: 13
            font.weight: parent.isCurrentDir ? Theme.emphasisFontWeight : Theme.bodyFontWeight
        }

        MouseArea {
            id: sidebarPlaceMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onClicked: function(mouse) {
                const entry = parent.entry
                if (mouse.button === Qt.RightButton) {
                    root.contextPath = String(entry.path)
                    root.contextIsRecent = entry.kind === "recent"
                    sidebarMenu.popup()
                    return
                }
                if (entry.kind === "recent")
                    root.controller.openRecentDirectory(entry.path)
                else
                    root.controller.openDirectory(entry.path)
            }
        }
    }

    ScrollView {
        id: navigationScroll
        objectName: "navigationScroll"
        anchors.fill: parent
        clip: true
        contentWidth: availableWidth
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        ColumnLayout {
            id: navigationColumn
            width: navigationScroll.availableWidth
            spacing: 0

            readonly property var recentItems: root.browsingEnabled ? root.recentEntries() : []

            Item {
                Layout.fillWidth: true
                implicitHeight: 8
            }

            Rectangle {
                Layout.fillWidth: true
                visible: navigationColumn.recentItems.length > 0
                implicitHeight: visible ? 26 : 0
                color: "transparent"

                Text {
                    id: recentHeading
                    objectName: "nativeRecentHeading"
                    anchors.left: parent.left
                    anchors.leftMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Recent")
                    color: recentHeaderMouse.containsMouse ? Theme.graphiteInk : Theme.mutedInk
                    font.family: root.nativeFont
                    font.pixelSize: 11
                    font.weight: Theme.emphasisFontWeight
                }
                Image {
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    width: 11
                    height: 11
                    source: root.iconPrefix + "chevron-down.svg"
                    rotation: root.recentExpanded ? 0 : -90
                    opacity: recentHeaderMouse.containsMouse ? 0.8 : 0.4
                    Behavior on rotation { NumberAnimation { duration: Theme.motionFast; easing.type: Easing.OutCubic } }
                }
                MouseArea {
                    id: recentHeaderMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.recentExpanded = !root.recentExpanded
                }
            }

            Repeater {
                model: root.recentExpanded ? navigationColumn.recentItems : []
                delegate: SidebarPlace {
                    required property var modelData
                    entry: modelData
                }
            }

            Item {
                Layout.fillWidth: true
                visible: navigationColumn.recentItems.length > 0
                implicitHeight: visible ? (root.recentExpanded ? 14 : 6) : 0
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 26
                color: "transparent"

                Text {
                    id: quickAccessHeading
                    objectName: "nativeQuickAccessHeading"
                    anchors.left: parent.left
                    anchors.leftMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Quick Access")
                    color: quickHeaderMouse.containsMouse ? Theme.graphiteInk : Theme.mutedInk
                    font.family: root.nativeFont
                    font.pixelSize: 11
                    font.weight: Theme.emphasisFontWeight
                }
                Image {
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    width: 11
                    height: 11
                    source: root.iconPrefix + "chevron-down.svg"
                    rotation: root.quickAccessExpanded ? 0 : -90
                    opacity: quickHeaderMouse.containsMouse ? 0.8 : 0.4
                    Behavior on rotation { NumberAnimation { duration: Theme.motionFast; easing.type: Easing.OutCubic } }
                }
                MouseArea {
                    id: quickHeaderMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.quickAccessExpanded = !root.quickAccessExpanded
                }
            }

            Repeater {
                model: (root.browsingEnabled && root.quickAccessExpanded) ? root.quickAccessEntries() : []
                delegate: SidebarPlace {
                    required property var modelData
                    entry: modelData
                }
            }

            Item {
                Layout.fillWidth: true
                implicitHeight: root.quickAccessExpanded ? 14 : 6
            }

            AbstractButton {
                id: favoritesHeader
                objectName: "favoritesHeading"
                Layout.fillWidth: true
                implicitHeight: 26
                visible: !!root.favoritesModel
                checkable: true
                checked: true
                hoverEnabled: true
                focusPolicy: Qt.StrongFocus
                text: qsTr("Favorites")
                Accessible.name: text
                background: Rectangle {
                    color: favoritesHeader.visualFocus ? root.hoverBg : "transparent"
                    radius: 5
                }
                contentItem: Item {
                    Text {
                        x: 16
                        anchors.verticalCenter: parent.verticalCenter
                        text: favoritesHeader.text
                        color: favoritesHeader.hovered ? Theme.graphiteInk : Theme.mutedInk
                        font.family: root.nativeFont
                        font.pixelSize: Theme.metadataFontSize
                        font.weight: Theme.emphasisFontWeight
                    }
                    Image {
                        anchors.right: parent.right
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        width: 11
                        height: 11
                        source: root.iconPrefix + "chevron-down.svg"
                        rotation: favoritesHeader.checked ? 0 : -90
                        opacity: favoritesHeader.hovered ? 0.8 : 0.4
                        Behavior on rotation { NumberAnimation { duration: Theme.motionFast; easing.type: Easing.OutCubic } }
                    }
                }
            }

            Text {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 12
                Layout.bottomMargin: 5
                visible: favoritesHeader.visible && favoritesHeader.checked && favoriteRoots.count === 0
                text: qsTr("Right-click a folder to pin it here.")
                color: Theme.mutedInk
                font.family: root.nativeFont
                font.pixelSize: Theme.metadataFontSize
                wrapMode: Text.Wrap
            }

            Repeater {
                id: favoriteRoots
                model: root.favoritesModel
                delegate: ColumnLayout {
                    id: favoriteBranch
                    required property string display
                    readonly property string path: display
                    objectName: "favoriteBranch-" + path
                    property var childrenPaths: []
                    property bool loading: false
                    property bool available: true
                    Layout.fillWidth: true
                    spacing: 0
                    visible: favoritesHeader.checked

                    Connections {
                        target: root.workspaceController
                        function onFavoriteChildrenLoaded(path, children, available) {
                            if (path !== favoriteBranch.path || !favoriteDisclosure.checked)
                                return
                            favoriteBranch.childrenPaths = children
                            favoriteBranch.available = available
                            favoriteBranch.loading = false
                        }
                    }

                    SidebarPlace {
                        entry: root.favoriteEntry(favoriteBranch.path)
                        iconX: 32
                        AppIconButton {
                            id: favoriteDisclosure
                            objectName: "favoriteDisclosure-" + favoriteBranch.path
                            x: 6
                            anchors.verticalCenter: parent.verticalCenter
                            controlSize: 24
                            checkable: true
                            suppressCheckedBackground: true
                            focusPolicy: Qt.StrongFocus
                            toolTipText: checked ? qsTr("Collapse subfolders") : qsTr("Expand subfolders")
                            onToggled: {
                                if (!checked)
                                    return
                                favoriteBranch.loading = true
                                favoriteBranch.childrenPaths = []
                                root.workspaceController.loadFavoriteChildren(favoriteBranch.path)
                            }
                            contentItem: Item {
                                Image {
                                    anchors.centerIn: parent
                                    width: 12
                                    height: 12
                                    source: root.iconPrefix + "chevron-down.svg"
                                    rotation: favoriteDisclosure.checked ? 0 : -90
                                    opacity: 0.65
                                    Behavior on rotation { NumberAnimation { duration: Theme.motionFast; easing.type: Easing.OutCubic } }
                                }
                            }
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        Layout.leftMargin: 48
                        Layout.rightMargin: 12
                        Layout.topMargin: 3
                        Layout.bottomMargin: 4
                        visible: favoriteDisclosure.checked && favoriteBranch.childrenPaths.length === 0
                        text: favoriteBranch.loading ? qsTr("Loading subfolders…")
                            : favoriteBranch.available ? qsTr("No subfolders") : qsTr("Folder unavailable")
                        color: Theme.mutedInk
                        font.family: root.nativeFont
                        font.pixelSize: Theme.metadataFontSize
                        wrapMode: Text.Wrap
                    }
                    Repeater {
                        model: favoriteDisclosure.checked ? favoriteBranch.childrenPaths : []
                        delegate: SidebarPlace {
                            required property string modelData
                            objectName: "favoriteChild-" + modelData
                            entry: root.favoriteEntry(modelData)
                            iconX: 48
                        }
                    }
                }
            }

            Item {
                Layout.fillWidth: true
                implicitHeight: favoritesHeader.visible ? 8 : 0
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 26
                color: "transparent"

                Text {
                    id: locationsHeading
                    objectName: "nativeLocationsHeading"
                    anchors.left: parent.left
                    anchors.leftMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Locations")
                    color: locationsHeaderMouse.containsMouse ? Theme.graphiteInk : Theme.mutedInk
                    font.family: root.nativeFont
                    font.pixelSize: 11
                    font.weight: Theme.emphasisFontWeight
                }
                Image {
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    width: 11
                    height: 11
                    source: root.iconPrefix + "chevron-down.svg"
                    rotation: root.locationsExpanded ? 0 : -90
                    opacity: locationsHeaderMouse.containsMouse ? 0.8 : 0.4
                    Behavior on rotation { NumberAnimation { duration: Theme.motionFast; easing.type: Easing.OutCubic } }
                }
                MouseArea {
                    id: locationsHeaderMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.locationsExpanded = !root.locationsExpanded
                }
            }

            Repeater {
                model: root.locationsExpanded ? drivePlaces : []
                delegate: ColumnLayout {
                    id: driveBranch
                    required property var place
                    readonly property string drivePath: String(place.path)
                    readonly property bool expanded: root.expandedDrives[drivePath] === true
                    Layout.fillWidth: true
                    spacing: 0

                    function revealDrive() {
                        if (root.belongsTo(root.controller.currentDirectory, drivePath))
                            root.setDriveExpanded(drivePath, true)
                    }

                    Component.onCompleted: {
                        if (root.expandedDrives[drivePath] === undefined)
                            revealDrive()
                    }
                    Connections {
                        target: root
                        function onActivePathChanged() { driveBranch.revealDrive() }
                    }

                    SidebarPlace {
                        entry: driveBranch.place
                        Image {
                            x: 14
                            anchors.verticalCenter: parent.verticalCenter
                            width: 12
                            height: 12
                            source: root.iconPrefix + "chevron-down.svg"
                            rotation: driveBranch.expanded ? 0 : -90
                            opacity: 0.65
                        }
                        MouseArea {
                            objectName: "driveDisclosure-" + driveBranch.drivePath
                            width: root.indentWidth + 6
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.setDriveExpanded(driveBranch.drivePath,
                                                           !driveBranch.expanded)
                        }
                    }

                    Loader {
                        objectName: "driveTreeLoader-" + driveBranch.drivePath
                        Layout.fillWidth: true
                        visible: driveBranch.expanded
                        // Keep loaded branches alive so collapsing a drive preserves its children.
                        active: driveBranch.expanded || item !== null
                        sourceComponent: NavigationTree {
                            drivePath: driveBranch.drivePath
                        }
                        readonly property NavigationTree branchTree: item as NavigationTree
                        Layout.preferredHeight: visible && branchTree ? branchTree.implicitHeight : 0
                    }
                }
            }

            NavigationTree {
                visible: root.platformName !== "windows" && root.locationsExpanded
                // Effective visibility also follows the browse page during full-screen viewing.
                // Keep the model attached while hidden so TreeView retains expanded folders.
                model: root.platformName !== "windows" ? root.controller.folderTree : null
            }

        }
    }
    component NavigationTree: TreeView {
        id: folderTree
        objectName: "nativeFolderTree"
        Layout.fillWidth: true
        property string drivePath: ""
        property bool pendingReveal: true
        property bool collapseBeforeReveal: true
        implicitHeight: rows * root.itemHeight
        interactive: false
        model: drivePath.length > 0
               ? root.controller.folderTreeBranch(drivePath) : root.controller.folderTree
        rootIndex: drivePath.length > 0
                   ? undefined : root.controller.folderRootIndex

        function requestReveal() {
            pendingReveal = true
            Qt.callLater(revealCurrentFolder)
        }

        function resetToCurrentPath() {
            collapseBeforeReveal = true
            requestReveal()
        }

        function revealCurrentFolder() {
            if (!pendingReveal || !visible
                    || root.controller.currentDirectory.length === 0
                    || (drivePath.length > 0
                        && !root.belongsTo(root.controller.currentDirectory, drivePath)))
                return
            const currentIndex = drivePath.length > 0
                    ? root.controller.folderTreeIndex(root.controller.currentDirectory, drivePath)
                    : root.controller.currentFolderTreeIndex
            if (collapseBeforeReveal) {
                collapseRecursively()
                collapseBeforeReveal = false
            }
            expandToIndex(currentIndex)
            if ((drivePath.length > 0
                 && root.belongsTo(drivePath, root.controller.currentDirectory))
                    || rowAtIndex(currentIndex) >= 0)
                pendingReveal = false
        }

        Connections {
            target: root.controller
            function onCurrentDirectoryChanged() { folderTree.resetToCurrentPath() }
        }
        Connections {
            target: root
            function onActivePathChanged() { folderTree.resetToCurrentPath() }
        }
        // Each pane owns a branch proxy. Swapping it resets TreeView's expanded
        // rows even when both panes share the same underlying filesystem model.
        onModelChanged: requestReveal()
        onRootIndexChanged: requestReveal()
        Connections {
            target: folderTree.model
            function onModelReset() { folderTree.requestReveal() }
        }
        Connections {
            target: root.controller.folderTree
            function onDirectoryLoaded(path) {
                if (root.belongsTo(root.controller.currentDirectory, path))
                    Qt.callLater(folderTree.revealCurrentFolder)
            }
        }
        onVisibleChanged: if (visible) Qt.callLater(revealCurrentFolder)
        Component.onCompleted: {
            if (drivePath.length > 0)
                root.controller.loadFolderTreeChildren(drivePath)
            Qt.callLater(revealCurrentFolder)
        }

        columnWidthProvider: function (column) { return width }
        boundsBehavior: Flickable.StopAtBounds
        clip: true

        delegate: Item {
            id: treeDelegate
            required property TreeView treeView
            required property bool isTreeNode
            required property bool expanded
            required property bool hasChildren
            required property int depth
            required property int row
            required property int column
            required property string display
            required property string filePath

            implicitWidth: folderTree.width
            implicitHeight: root.itemHeight
            readonly property bool isDirectory: hasChildren || isTreeNode
            readonly property bool isCurrentFolder:
                filePath === root.controller.currentDirectory
            function toggleDirectoryExpansion() {
                if (!treeDelegate.isDirectory)
                    return
                folderTree.pendingReveal = false
                if (treeDelegate.expanded)
                    treeDelegate.treeView.collapse(treeDelegate.row)
                else {
                    treeDelegate.treeView.expand(treeDelegate.row)
                    root.controller.loadFolderTreeChildren(treeDelegate.filePath)
                }
            }

            Rectangle {
                anchors.fill: parent
                anchors.leftMargin: 6
                anchors.rightMargin: 6
                anchors.topMargin: 1
                anchors.bottomMargin: 1
                radius: 5
                color: treeDelegate.isCurrentFolder ? root.selectionBg
                       : disclosureMouse.containsMouse || directoryMouse.containsMouse
                         ? root.hoverBg : "transparent"
            }

            Repeater {
                model: Math.max(0, treeDelegate.depth + (folderTree.drivePath.length > 0 ? 1 : 0))
                delegate: Rectangle {
                    required property int index
                    x: index * root.indentWidth + 14
                    y: 0
                    width: 1
                    height: treeDelegate.height
                    color: Theme.borderSubtle
                }
            }

            Image {
                x: (treeDelegate.depth + (folderTree.drivePath.length > 0 ? 1 : 0)) * root.indentWidth + 8
                anchors.verticalCenter: parent.verticalCenter
                width: 12
                height: 12
                source: root.iconPrefix + "chevron-down.svg"
                visible: treeDelegate.isDirectory
                rotation: treeDelegate.expanded ? 0 : -90
                opacity: 0.65
                Behavior on rotation { NumberAnimation { duration: Theme.motionFast; easing.type: Easing.OutCubic } }
            }

            Image {
                x: (treeDelegate.depth + (folderTree.drivePath.length > 0 ? 1 : 0)) * root.indentWidth + root.indentWidth +
                   (root.macStyle ? 9 : 8)
                anchors.verticalCenter: parent.verticalCenter
                width: root.iconSize_
                height: root.iconSize_
                source: root.folderIcon(treeDelegate.expanded)
                sourceSize: Qt.size(32, 32)
                opacity: 0.72
            }

            Text {
                x: (treeDelegate.depth + (folderTree.drivePath.length > 0 ? 1 : 0)) * root.indentWidth + root.indentWidth +
                   root.iconSize_ + (root.macStyle ? 14 : 15)
                width: Math.max(0, parent.width - x - 10)
                anchors.verticalCenter: parent.verticalCenter
                text: treeDelegate.display
                elide: Text.ElideMiddle
                color: treeDelegate.isCurrentFolder ? Theme.accent : root.sidebarText
                font.family: root.nativeFont
                font.pixelSize: 13
                font.weight: treeDelegate.isCurrentFolder ? Theme.emphasisFontWeight : Theme.bodyFontWeight
            }

            MouseArea {
                id: disclosureMouse
                objectName: "folderDisclosure-" + treeDelegate.row
                x: (treeDelegate.depth + (folderTree.drivePath.length > 0 ? 1 : 0)) * root.indentWidth
                width: root.indentWidth + 6
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                enabled: treeDelegate.isDirectory
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton
                cursorShape: Qt.PointingHandCursor
                onClicked: treeDelegate.toggleDirectoryExpansion()
            }

            MouseArea {
                id: directoryMouse
                objectName: "folderDirectory-" + treeDelegate.row
                x: disclosureMouse.x + disclosureMouse.width
                width: Math.max(0, parent.width - x)
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                onClicked: function(mouse) {
                    if (mouse.button === Qt.RightButton) {
                        root.contextPath = treeDelegate.filePath
                        root.contextIsRecent = false
                        sidebarMenu.popup()
                        return
                    }
                    root.controller.openDirectory(treeDelegate.filePath)
                }
            }
        }

    }

    AppMenu {
        id: sidebarMenu
        objectName: "sidebarContextMenu"
        AppMenuItem {
            text: qsTr("Open folder")
            onTriggered: {
                if (root.contextIsRecent)
                    root.controller.openRecentDirectory(root.contextPath)
                else
                    root.controller.openDirectory(root.contextPath)
            }
        }
        AppMenuItem {
            text: qsTr("Open in new pane")
            visible: !!root.workspaceController
            enabled: !!root.workspaceController && root.workspaceController.canAddPane
            onTriggered: {
                if (!root.workspaceController || !root.workspaceController.canAddPane)
                    return
                root.workspaceController.addFileManagerPane()
                const pane = root.workspaceController.activePane
                if (pane)
                    pane.openDirectory(root.contextPath)
            }
        }
        AppMenuItem {
            objectName: "sidebarPinFolderAction"
            visible: !!root.favoritesModel
            text: root.contextIsFavorite ? qsTr("Unpin from Favorites") : qsTr("Pin to Favorites")
            onTriggered: root.workspaceController.setFolderFavorite(root.contextPath, !root.contextIsFavorite)
        }
        AppMenuSeparator {
            visible: root.samePath(root.contextPath, root.controller.currentDirectory)
        }
        AppMenuItem {
            visible: root.samePath(root.contextPath, root.controller.currentDirectory)
            text: Qt.platform.os === "osx" ? qsTr("Open in Finder") : qsTr("Open in File Explorer")
            onTriggered: root.controller.openCurrentDirectoryInFileManager()
        }
    }
}
