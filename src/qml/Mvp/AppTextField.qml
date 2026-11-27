import QtQuick
import QtQuick.Controls
import "."

// Unified single-line text input used by dialogs. Mirrors the search-field
// tokens so every editable field in the app shares one visual style.
TextField {
    id: root

    selectByMouse: true
    hoverEnabled: true
    implicitHeight: 38
    leftPadding: 11
    rightPadding: 11
    verticalAlignment: TextInput.AlignVCenter
    color: Theme.graphiteInk
    selectionColor: Theme.primaryButton
    selectedTextColor: Theme.primaryButtonText
    placeholderTextColor: Theme.faintInk
    font.family: Theme.uiFont
    font.pixelSize: Theme.bodyFontSize

    background: Rectangle {
        radius: 6
        color: Theme.searchFieldSurface
        border.width: root.activeFocus ? 2 : 1
        border.color: root.activeFocus ? Theme.searchFieldFocusBorder
                    : root.hovered ? Theme.searchFieldHoverBorder : Theme.searchFieldBorder
    }
}
