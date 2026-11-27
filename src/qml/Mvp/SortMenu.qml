import QtQuick
import "."

// Single sort-mode menu shared by the toolbar control and the workspace context
// menu, so both always offer the same wording and mode order.
AppMenu {
    id: root

    required property var controller

    title: qsTr("Sort by")

    AppMenuItem {
        objectName: "sortByNameAction"
        text: (root.controller.sortMode === 0 ? "✓  " : "    ") + qsTr("Name")
        onTriggered: root.controller.setSortMode(0)
    }
    AppMenuItem {
        objectName: "sortByModifiedAction"
        text: (root.controller.sortMode === 1 ? "✓  " : "    ") + qsTr("Modified time")
        onTriggered: root.controller.setSortMode(1)
    }
    AppMenuItem {
        objectName: "sortBySizeAction"
        text: (root.controller.sortMode === 2 ? "✓  " : "    ") + qsTr("File size")
        onTriggered: root.controller.setSortMode(2)
    }
    AppMenuItem {
        objectName: "sortByTypeAction"
        text: (root.controller.sortMode === 3 ? "✓  " : "    ") + qsTr("File type")
        onTriggered: root.controller.setSortMode(3)
    }
}
