import QtQuick
import QtQuick.Controls

MenuSeparator {
    // Mirror AppMenuItem: an invisible row must collapse instead of reserving space.
    implicitHeight: visible ? 9 : 0
    contentItem: Rectangle {
        x: 8
        y: 4
        width: parent.width - 16
        height: 1
        color: Theme.opticalGray
    }
}
