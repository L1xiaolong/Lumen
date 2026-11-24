import QtQuick
import QtQuick.Controls

Menu {
    id: control
    // Keep all custom menus attached to the Qt Quick window's input and focus routing.
    popupType: Popup.Item
    implicitWidth: 220
    padding: 6
    topPadding: 6
    bottomPadding: 6
    margins: 8

    delegate: AppMenuItem {}

    background: Rectangle {
        color: Theme.raisedSurface
        radius: 8
        border.color: Theme.opticalGray
        border.width: 1
    }
}
