import QtQuick

QtObject {
    required property var targetWindow
    // Includes TextField, TextArea, SpinBox editors and read-only selectable text.
    readonly property bool textInputFocused: {
        let item = targetWindow ? targetWindow.activeFocusItem : null
        while (item) {
            if (item instanceof TextInput || item instanceof TextEdit)
                return true
            item = item.parent
        }
        return false
    }
}
