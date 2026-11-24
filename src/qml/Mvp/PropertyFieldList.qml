pragma ComponentBehavior: Bound
import QtQuick
import "."

Column {
    id: root
    property var fields: []
    spacing: 0

    Repeater {
        model: root.fields || []
        delegate: Item {
            id: row
            required property int index
            required property var modelData
            objectName: "propertyField-" + (modelData.id || index)
            readonly property bool stacked: width < 320 && !!modelData.type
            width: root.width
            height: Math.max(36, (stacked ? label.implicitHeight + valueBox.height + 4
                                         : Math.max(label.implicitHeight, valueBox.height)) + 16)
            Column {
                id: label
                x: 12
                y: row.stacked ? 8 : Math.round((row.height - height) / 2)
                width: row.stacked ? row.width - 24 : Math.min(172, (row.width - 24) * (row.modelData.type ? 0.46 : 0.38))
                Text {
                    id: labelText
                    width: parent.width
                    text: (row.modelData.label || "").replace(/\./g, ".\u200b")
                    color: Theme.mutedInk
                    font.family: Theme.uiFont
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                }
                Text {
                    visible: text.length > 0
                    width: parent.width
                    text: row.modelData.type || ""
                    color: Theme.mutedInk
                    font.family: Theme.uiFont
                    font.pixelSize: 10
                    wrapMode: Text.WrapAnywhere
                }
            }
            Item {
                id: valueBox
                x: row.stacked ? 12 : label.x + label.width + 10
                y: row.stacked ? label.y + label.height + 4 : Math.round((row.height - height) / 2)
                width: Math.max(20, row.width - x - 12)
                height: row.modelData.matrix ? matrix.implicitHeight : valueText.implicitHeight
                TextEdit {
                    id: valueText
                    objectName: "propertyValue-" + (row.modelData.id || row.index)
                    visible: !row.modelData.matrix
                    width: parent.width
                    text: String(row.modelData.value ?? "")
                    textFormat: TextEdit.PlainText
                    readOnly: true
                    selectByMouse: true
                    activeFocusOnTab: true
                    wrapMode: TextEdit.Wrap
                    color: Theme.graphiteInk
                    selectionColor: Theme.explorerSelectionBg
                    selectedTextColor: Theme.graphiteInk
                    font.family: Theme.uiFont
                    font.pixelSize: 12
                    Accessible.name: (row.modelData.label || "") + ": " + text
                    Keys.onEscapePressed: root.forceActiveFocus()
                }
                Grid {
                    id: matrix
                    visible: !!row.modelData.matrix
                    width: parent.width
                    columns: 3
                    rowSpacing: 6
                    columnSpacing: 4
                    Repeater {
                        model: row.modelData.matrix || []
                        delegate: Text {
                            required property string modelData
                            width: (matrix.width - 8) / 3
                            text: modelData
                            wrapMode: Text.WrapAnywhere
                            verticalAlignment: Text.AlignVCenter
                            height: Math.max(18, implicitHeight)
                            color: Theme.graphiteInk
                            font.family: Theme.uiFont
                            font.pixelSize: 12
                        }
                    }
                }
            }
            Rectangle {
                visible: row.index < root.fields.length - 1
                x: 12; width: parent.width - 24
                anchors.bottom: parent.bottom
                height: 1
                color: Theme.opticalGray
                opacity: 0.55
            }
        }
    }
}
