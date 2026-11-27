pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import "."

Rectangle {
    id: root
    required property var controller
    required property var workspaceController
    required property int paneIndex
    property var settingsController: null
    property var searchReturnTarget: null
    property string iconPrefix: Theme.iconPrefix
    readonly property bool multiplePanes: workspaceController.paneCount !== undefined && workspaceController.paneCount > 1
    readonly property bool active: workspaceController.activePaneIndex === paneIndex
    readonly property bool searchVisible: searchPopup.visible
    readonly property bool searchInputFocused: paneSearch.activeFocus
    objectName: "paneHeader-" + paneIndex
    function activate() { workspaceController.activatePane(paneIndex) }
    function submitLocation() {
        activate()
        const error = controller.navigateToTypedPath(locationField.text)
        locationField.locationError = error
        if (error.length === 0) {
            locationField.text = controller.currentDirectory
            locationField.focus = false
            locationFrame.editingLocation = false
        } else {
            locationField.forceActiveFocus()
            locationField.selectAll()
        }
    }
    function beginLocationEdit() {
        activate()
        locationFrame.editingLocation = true
        locationField.forceActiveFocus()
        locationField.selectAll()
    }
    function openBreadcrumbMenu(anchorItem, folderPath, activeChildPath) {
        activate()
        if (!controller || controller.childFolders === undefined) return
        const items = controller.childFolders(folderPath)
        breadcrumbSubfolderPopup.activeChildPath = activeChildPath || ""
        breadcrumbSubfolderPopup.subfolders = items
        const pos = anchorItem.mapToItem(root, 0, anchorItem.height + 4)
        breadcrumbSubfolderPopup.x = Math.max(4, Math.min(pos.x, root.width - breadcrumbSubfolderPopup.width - 4))
        breadcrumbSubfolderPopup.y = pos.y
        breadcrumbSubfolderPopup.open()
    }
    function breadcrumbSegments(rawPath) {
        const path = String(rawPath || "").trim()
        if (path.length === 0) return []
        const segments = []
        if (path.startsWith("\\\\") || path.startsWith("//")) {
            const cleaned = path.replace(/[\\/]+$/, "")
            const parts = cleaned.slice(2).split(/[\\/]+/).filter(function(p) { return p.length > 0 })
            let acc = "\\\\"
            for (let i = 0; i < parts.length; ++i) {
                acc = i === 0 ? "\\\\" + parts[i] : acc + "\\" + parts[i]
                segments.push({ label: parts[i], fullPath: acc })
            }
            return segments
        }
        const winDrive = /^([A-Za-z]:)(?:[\\/](.*))?$/.exec(path)
        if (winDrive) {
            const drive = winDrive[1]
            segments.push({ label: drive, fullPath: drive + "/" })
            const rest = (winDrive[2] || "").split(/[\\/]+/).filter(function(p) { return p.length > 0 })
            let acc = drive
            for (let i = 0; i < rest.length; ++i) {
                acc += "/" + rest[i]
                segments.push({ label: rest[i], fullPath: acc })
            }
            return segments
        }
        const isAbsolute = path.startsWith("/")
        const parts = path.split(/[\\/]+/).filter(function(p) { return p.length > 0 })
        if (isAbsolute) {
            segments.push({ label: "/", fullPath: "/" })
        }
        let acc = ""
        for (let i = 0; i < parts.length; ++i) {
            acc = isAbsolute ? (acc + "/" + parts[i]) : (i === 0 ? parts[i] : acc + "/" + parts[i])
            segments.push({ label: parts[i], fullPath: acc })
        }
        return segments
    }
    function focusSearch() {
        if (controller.currentDirectory.length === 0) return
        activate()
        searchPopup.open()
        paneSearch.forceActiveFocus()
        paneSearch.selectAll()
    }
    function closeSearch() {
        searchPopup.close()
        // Popup's default restoration can focus a different pane's search.
        // Return explicitly to the file view belonging to this header.
        if (root.visible && searchReturnTarget && searchReturnTarget.visible) {
            activate()
            searchReturnTarget.forceActiveFocus(Qt.ShortcutFocusReason)
        }
    }
    onVisibleChanged: if (!visible) searchPopup.close()

        height: 38
        color: root.multiplePanes && root.active ? Theme.activePaneHeader : Theme.sensorWhite

        MouseArea {
            anchors.fill: parent
            onClicked: root.activate()
        }

        Row {
            id: navigationButtons
            y: 5
            anchors.left: parent.left
            anchors.leftMargin: 4
            spacing: 0

            AppIconButton {
                objectName: "paneBackButton-" + root.paneIndex
                width: 26
                height: 28
                controlSize: 26
                renderedIconSize: 16
                enabled: root.controller.canGoBack
                iconSource: root.iconPrefix + "back.svg"
                toolTipText: qsTranslate("BrowserPane", "Back")
                onClicked: {
                    root.activate()
                    root.controller.navigateBack()
                }
            }
            AppIconButton {
                objectName: "paneForwardButton-" + root.paneIndex
                width: 26
                height: 28
                controlSize: 26
                renderedIconSize: 16
                enabled: root.controller.canGoForward
                iconSource: root.iconPrefix + "forward.svg"
                toolTipText: qsTranslate("BrowserPane", "Forward")
                onClicked: {
                    root.activate()
                    root.controller.navigateForward()
                }
            }
            AppIconButton {
                objectName: "paneUpButton-" + root.paneIndex
                width: 26
                height: 28
                controlSize: 26
                renderedIconSize: 16
                enabled: root.controller.canGoUp
                iconSource: root.iconPrefix + "up.svg"
                toolTipText: qsTranslate("BrowserPane", "Parent folder")
                onClicked: {
                    root.activate()
                    root.controller.navigateUp()
                }
            }
        }

        Rectangle {
            id: locationFrame
            objectName: "paneLocationFrame-" + root.paneIndex
            property bool editingLocation: false
            readonly property bool showTextInput: editingLocation || locationField.activeFocus || root.controller.currentDirectory.length === 0
            anchors.left: navigationButtons.right
            anchors.leftMargin: 4
            anchors.right: selectionCount.visible ? selectionCount.left
                         : closeButton.visible ? closeButton.left : parent.right
            anchors.rightMargin: selectionCount.visible || closeButton.visible ? 6 : 8
            anchors.verticalCenter: navigationButtons.verticalCenter
            height: 28
            radius: 6
            color: showTextInput ? Theme.searchFieldSurface : Theme.paperWhite
            border.width: 1
            border.color: locationField.locationError.length > 0 ? Theme.danger
                        : locationField.activeFocus ? Theme.searchFieldFocusBorder
                        : locationHover.hovered ? Theme.searchFieldHoverBorder
                                                : Theme.borderSubtle
            Behavior on border.color { ColorAnimation { duration: Theme.motionFast } }
            Behavior on color { ColorAnimation { duration: Theme.motionFast } }

            HoverHandler { id: locationHover }

            Flickable {
                id: breadcrumbFlickable
                visible: !locationFrame.showTextInput
                anchors.fill: parent
                anchors.leftMargin: 6
                anchors.rightMargin: 27
                clip: true
                contentWidth: Math.max(width, breadcrumbRow.implicitWidth)
                contentHeight: height
                boundsBehavior: Flickable.StopAtBounds
                function scrollToEnd() {
                    contentX = Math.max(0, contentWidth - width)
                }
                onWidthChanged: Qt.callLater(scrollToEnd)
                onContentWidthChanged: Qt.callLater(scrollToEnd)

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.IBeamCursor
                    onClicked: root.beginLocationEdit()
                }

                Row {
                    id: breadcrumbRow
                    height: parent.height
                    spacing: 1
                    Repeater {
                        model: root.breadcrumbSegments(root.controller.currentDirectory)
                        delegate: Row {
                            id: crumbDelegate
                            required property var modelData
                            required property int index
                            readonly property var allSegments: root.breadcrumbSegments(root.controller.currentDirectory)
                            readonly property bool isLast: index === allSegments.length - 1
                            readonly property string nextPath: index + 1 < allSegments.length ? allSegments[index + 1].fullPath : ""
                            readonly property bool showChevron: !isLast || (root.controller && root.controller.childFolders !== undefined && root.controller.childFolders(modelData.fullPath).length > 0)
                            height: breadcrumbRow.height
                            spacing: 1

                            Rectangle {
                                id: crumbPill
                                anchors.verticalCenter: parent.verticalCenter
                                width: crumbLabel.implicitWidth + 10
                                height: 22
                                radius: 4
                                color: crumbMouse.pressed ? Theme.pressedSurface
                                     : crumbMouse.containsMouse ? Theme.softHover : "transparent"

                                Text {
                                    id: crumbLabel
                                    anchors.centerIn: parent
                                    text: crumbDelegate.modelData.label
                                    color: crumbDelegate.isLast ? Theme.graphiteInk : Theme.mutedInk
                                    font.family: Theme.uiFont
                                    font.pixelSize: 12
                                    font.weight: crumbDelegate.isLast ? Theme.emphasisFontWeight : Font.Normal
                                }

                                MouseArea {
                                    id: crumbMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        if (crumbDelegate.isLast) {
                                            root.beginLocationEdit()
                                        } else {
                                            root.activate()
                                            root.controller.navigateToTypedPath(crumbDelegate.modelData.fullPath)
                                        }
                                    }
                                }
                            }

                            Rectangle {
                                id: crumbChevron
                                visible: crumbDelegate.showChevron
                                anchors.verticalCenter: parent.verticalCenter
                                width: 18
                                height: 22
                                radius: 4
                                color: chevronMouse.pressed ? Theme.pressedSurface
                                     : chevronMouse.containsMouse ? Theme.softHover : "transparent"

                                Text {
                                    anchors.centerIn: parent
                                    text: "›"
                                    color: chevronMouse.containsMouse ? Theme.graphiteInk : Theme.faintInk
                                    font.family: Theme.uiFont
                                    font.pixelSize: 12
                                }

                                MouseArea {
                                    id: chevronMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.openBreadcrumbMenu(crumbChevron, crumbDelegate.modelData.fullPath, crumbDelegate.nextPath)
                                }
                            }
                        }
                    }
                }
            }

            TextField {
                id: locationField
                objectName: "paneLocationField-" + root.paneIndex
                property string locationError: ""
                function cancelLocationEdit() {
                    text = root.controller.currentDirectory
                    locationError = ""
                    focus = false
                    locationFrame.editingLocation = false
                }
                visible: locationFrame.showTextInput
                anchors.fill: parent
                anchors.rightMargin: 25
                placeholderText: qsTranslate("BrowserPane", "Enter a folder path")
                selectByMouse: true
                leftPadding: 9
                rightPadding: 5
                topPadding: 0
                bottomPadding: 0
                verticalAlignment: TextInput.AlignVCenter
                color: Theme.graphiteInk
                selectionColor: Theme.primaryButton
                selectedTextColor: Theme.primaryButtonText
                font.family: Theme.uiFont
                font.pixelSize: 13
                font.weight: Font.Normal
                background: Item {}
                onActiveFocusChanged: {
                    if (!activeFocus) {
                        locationFrame.editingLocation = false
                        locationError = ""
                    }
                }
                onTextEdited: locationError = ""
                onAccepted: root.submitLocation()
                Keys.onEscapePressed: function(event) {
                    cancelLocationEdit()
                    event.accepted = true
                }
                ToolTip.visible: activeFocus && locationError.length > 0
                ToolTip.text: locationError
                ToolTip.delay: 0
            }

            Binding {
                target: locationField
                property: "text"
                value: root.controller.currentDirectory
                when: !locationField.activeFocus
                restoreMode: Binding.RestoreNone
            }

            AppIconButton {
                id: locationDropButton
                objectName: "paneLocationDropButton-" + root.paneIndex
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: 25
                height: 26
                controlSize: 25
                renderedIconSize: 14
                enabled: root.controller.recentLocations.length > 0
                checked: locationPopup.opened
                iconSource: root.iconPrefix + "chevron-down.svg"
                toolTipText: qsTranslate("BrowserPane", "Recent folders")
                onClicked: {
                    root.activate()
                    if (locationPopup.opened)
                        locationPopup.close()
                    else
                        locationPopup.open()
                }
            }

            Connections {
                target: root.controller
                function onCurrentDirectoryChanged() {
                    locationField.locationError = ""
                }
            }

            Popup {
                id: locationPopup
                objectName: "paneLocationPopup-" + root.paneIndex
                x: 0
                y: locationFrame.height + 4
                width: Math.max(locationFrame.width, Math.min(360, root.width - 8))
                height: Math.min(286, recentLocationColumn.implicitHeight + 10)
                padding: 5
                closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

                background: Rectangle {
                    color: Theme.raisedSurface
                    border.color: Theme.borderStrong
                    radius: 8
                    ElevationShadow { radius: 8; spread: 2 }
                }
                enter: Transition {
                    ParallelAnimation {
                        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.motionFast; easing.type: Easing.OutCubic }
                        NumberAnimation { property: "scale"; from: 0.96; to: 1.0; duration: Theme.motionNormal; easing.type: Easing.OutCubic }
                    }
                }
                exit: Transition {
                    NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.motionFast; easing.type: Easing.InCubic }
                }

                contentItem: Flickable {
                    contentWidth: width
                    contentHeight: recentLocationColumn.implicitHeight
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds

                    Column {
                        id: recentLocationColumn
                        width: parent.width
                        spacing: 2

                        Text {
                            width: parent.width
                            height: 25
                            leftPadding: 8
                            verticalAlignment: Text.AlignVCenter
                            text: qsTranslate("BrowserPane", "Recent folders")
                            color: Theme.mutedInk
                            font.family: Theme.uiFont
                            font.pixelSize: Theme.metadataFontSize
                            font.weight: Theme.emphasisFontWeight
                        }

                        Repeater {
                            model: root.controller.recentLocations
                            delegate: Button {
                                id: recentLocationButton
                                required property string modelData
                                width: recentLocationColumn.width
                                height: 32
                                text: modelData
                                hoverEnabled: true
                                ToolTip.visible: hovered && recentPathLabel.truncated
                                ToolTip.delay: 500
                                ToolTip.text: modelData

                                background: Rectangle {
                                    color: recentLocationButton.hovered || recentLocationButton.visualFocus
                                           ? Theme.softHover : "transparent"
                                    radius: 4
                                }
                                contentItem: Text {
                                    id: recentPathLabel
                                    text: recentLocationButton.text
                                    textFormat: Text.PlainText
                                    elide: Text.ElideMiddle
                                    verticalAlignment: Text.AlignVCenter
                                    leftPadding: 8
                                    rightPadding: 8
                                    color: Theme.graphiteInk
                                    font.family: Theme.uiFont
                                    font.pixelSize: 12
                                }
                                onClicked: {
                                    root.activate()
                                    locationPopup.close()
                                    root.controller.openRecentDirectory(modelData)
                                }
                            }
                        }

                        Rectangle {
                            width: parent.width
                            height: 1
                            color: Theme.opticalGray
                        }
                        Button {
                            id: clearRecentButton
                            objectName: "clearRecentLocations-" + root.paneIndex
                            width: parent.width
                            height: 29
                            flat: true
                            text: qsTranslate("BrowserPane", "Clear recent folders")
                            onClicked: {
                                root.controller.clearRecentLocations()
                                locationPopup.close()
                            }
                            background: Rectangle {
                                color: clearRecentButton.hovered ? Theme.softHover : "transparent"
                                radius: 4
                            }
                            contentItem: Text {
                                text: clearRecentButton.text
                                color: Theme.mutedInk
                                verticalAlignment: Text.AlignVCenter
                                horizontalAlignment: Text.AlignLeft
                                leftPadding: 8
                                font.family: Theme.uiFont
                                font.pixelSize: 11
                            }
                        }
                    }
                }
            }
        }

        Popup {
            id: breadcrumbSubfolderPopup
            objectName: "paneBreadcrumbSubfolderPopup-" + root.paneIndex
            property var subfolders: []
            property string activeChildPath: ""
            popupType: Popup.Item
            width: 240
            height: subfolders.length === 0 ? 44 : Math.min(300, subfolderList.contentHeight + 10)
            padding: 5
            closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

            background: Rectangle {
                color: Theme.raisedSurface
                border.color: Theme.borderStrong
                border.width: 1
                radius: 8
                ElevationShadow { radius: 8; spread: 2 }
            }
            enter: Transition {
                ParallelAnimation {
                    NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.motionFast; easing.type: Easing.OutCubic }
                    NumberAnimation { property: "scale"; from: 0.96; to: 1.0; duration: Theme.motionNormal; easing.type: Easing.OutCubic }
                }
            }
            exit: Transition {
                NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.motionFast; easing.type: Easing.InCubic }
            }

            contentItem: Item {
                Text {
                    anchors.centerIn: parent
                    visible: breadcrumbSubfolderPopup.subfolders.length === 0
                    text: qsTranslate("BrowserPane", "No subfolders")
                    color: Theme.mutedInk
                    font.family: Theme.uiFont
                    font.pixelSize: Theme.captionFontSize
                }
                ListView {
                    id: subfolderList
                    anchors.fill: parent
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    visible: breadcrumbSubfolderPopup.subfolders.length > 0
                    model: breadcrumbSubfolderPopup.subfolders
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                    delegate: Item {
                        id: subfolderRow
                        required property var modelData
                        readonly property bool isCurrentChild: {
                            const left = String(modelData.path).replace(/[\\/]+$/, "")
                            const right = String(breadcrumbSubfolderPopup.activeChildPath).replace(/[\\/]+$/, "")
                            return Qt.platform.os === "windows"
                                ? left.toLowerCase() === right.toLowerCase()
                                : left === right
                        }
                        width: subfolderList.width
                        height: 30

                        Rectangle {
                            anchors.fill: parent
                            radius: 5
                            color: subfolderMouse.containsMouse ? Theme.explorerSelectionBg : "transparent"
                        }

                        Row {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            spacing: 8

                            Image {
                                anchors.verticalCenter: parent.verticalCenter
                                width: 15
                                height: 15
                                source: root.iconPrefix + "folder.svg"
                            }

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width - 24
                                text: subfolderRow.modelData.name
                                elide: Text.ElideRight
                                color: subfolderRow.isCurrentChild ? Theme.accent : Theme.graphiteInk
                                font.family: Theme.uiFont
                                font.pixelSize: 12
                                font.weight: subfolderRow.isCurrentChild ? Theme.emphasisFontWeight : Font.Normal
                            }
                        }

                        MouseArea {
                            id: subfolderMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                breadcrumbSubfolderPopup.close()
                                root.activate()
                                root.controller.navigateToTypedPath(subfolderRow.modelData.path)
                            }
                        }
                    }
                }
            }
        }

        Popup {
            id: searchPopup
            objectName: "paneSearchPopup-" + root.paneIndex
            parent: root
            popupType: Popup.Item
            x: Math.max(8, root.width - width - 8)
            y: root.height + 6
            width: Math.max(0, Math.min(360, root.width - 16))
            height: 44
            padding: 6
            modal: false
            dim: false
            focus: true
            closePolicy: Popup.NoAutoClose
            onOpened: { paneSearch.forceActiveFocus(); paneSearch.selectAll() }
            onClosed: {
                searchDebounce.stop()
                paneSearch.clear()
                if (root.controller) root.controller.setFilterText("")
            }
            background: Rectangle {
                radius: 8
                color: Theme.raisedSurface
                border.width: 1
                border.color: Theme.borderStrong
                ElevationShadow { radius: 8; spread: 2 }
            }
            enter: Transition {
                ParallelAnimation {
                    NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.motionFast; easing.type: Easing.OutCubic }
                    NumberAnimation { property: "scale"; from: 0.96; to: 1.0; duration: Theme.motionNormal; easing.type: Easing.OutCubic }
                }
            }
            exit: Transition {
                NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.motionFast; easing.type: Easing.InCubic }
            }
            contentItem: Item {
                TextField {
                    id: paneSearch
                    objectName: "paneSearchField-" + root.paneIndex
                    anchors.left: parent.left
                    anchors.right: closeSearchButton.left
                    anchors.rightMargin: 6
                    anchors.verticalCenter: parent.verticalCenter
                    height: 30
                    enabled: root.controller.currentDirectory.length > 0
                    selectByMouse: true
                    placeholderText: qsTranslate("TopToolbar.ui", "Search files")
                    Accessible.name: placeholderText
                    leftPadding: 28
                    rightPadding: 8
                    topPadding: 0
                    bottomPadding: 0
                    verticalAlignment: TextInput.AlignVCenter
                    color: Theme.graphiteInk
                    placeholderTextColor: Theme.faintInk
                    font.family: Theme.uiFont
                    font.pixelSize: 13
                    onActiveFocusChanged: if (activeFocus) root.activate()
                    onTextEdited: { root.activate(); searchDebounce.restart() }
                    Keys.onEscapePressed: function(event) {
                        root.closeSearch()
                        event.accepted = true
                    }
                    background: Rectangle {
                        radius: 4
                        color: Theme.searchFieldSurface
                        border.width: paneSearch.activeFocus ? 2 : 1
                        border.color: paneSearch.activeFocus ? Theme.searchFieldFocusBorder
                            : paneSearch.hovered ? Theme.searchFieldHoverBorder : Theme.searchFieldBorder
                        Image {
                            x: 8
                            anchors.verticalCenter: parent.verticalCenter
                            width: 14; height: 14
                            source: root.iconPrefix + "search.svg"
                        }
                    }
                }
                AppIconButton {
                    id: closeSearchButton
                    objectName: "paneCloseSearchButton-" + root.paneIndex
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    controlSize: 28
                    renderedIconSize: 14
                    iconSource: root.iconPrefix + "close.svg"
                    toolTipText: qsTr("Close search") + " · Esc"
                    onClicked: root.closeSearch()
                }
            }
        }
        Shortcut {
            sequence: "Esc"
            enabled: root.visible && root.active && searchPopup.visible
            onActivated: root.closeSearch()
        }
        Timer {
            id: searchDebounce
            interval: 120
            onTriggered: root.controller.setFilterText(paneSearch.text)
        }
        Connections {
            target: root.controller
            function onFilterTextChanged() {
                if (paneSearch.text !== root.controller.filterText) paneSearch.text = root.controller.filterText
            }
        }
        Component.onCompleted: paneSearch.text = root.controller.filterText

        Text {
            id: selectionCount
            visible: root.multiplePanes && root.width >= 300 && root.controller.selectionCount > 0
            anchors.right: closeButton.visible ? closeButton.left : parent.right
            anchors.rightMargin: closeButton.visible ? 7 : 8
            anchors.verticalCenter: navigationButtons.verticalCenter
            text: qsTranslate("BrowserPane", "%1 selected").arg(root.controller.selectionCount)
            color: Theme.mutedInk
            font.family: Theme.monoFont
            font.pixelSize: Theme.metadataFontSize
        }
        AppIconButton {
            id: closeButton
            objectName: "closePaneButton-" + root.paneIndex
            visible: root.multiplePanes
            anchors.right: parent.right
            anchors.rightMargin: 4
            anchors.verticalCenter: navigationButtons.verticalCenter
            width: 26
            height: 26
            iconSource: root.iconPrefix + "close.svg"
            toolTipText: qsTranslate("BrowserPane", "Close browser pane")
            onClicked: root.workspaceController.closePane(root.paneIndex)
        }
}
