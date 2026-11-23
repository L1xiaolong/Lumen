import QtQuick

Rectangle {
    objectName: "thumbnailPanoramaBadge"
    Accessible.name: qsTr("Possible 360° panorama")
    width: badgeLabel.implicitWidth + 8
    height: 16
    radius: 4
    color: Theme.inspectionOverlayMuted

    Text {
        id: badgeLabel
        anchors.centerIn: parent
        text: "360"
        color: "#FFFFFF"
        font.family: Theme.uiFont
        font.pixelSize: 10
        font.weight: Font.DemiBold
    }
}
