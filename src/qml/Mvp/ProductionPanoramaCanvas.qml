pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls

PanoramaCanvas {
    id: canvas

    property var controller: null
    property bool pixelInspectionEnabled: true
    property string iconPrefix: Theme.iconPrefix
    signal contextMenuRequested(point position)
    signal closeRequested()

    onControllerChanged: if (controller) controller.attachPanorama(canvas)

    DragHandler {
        id: panDrag
        target: null
        acceptedButtons: Qt.LeftButton
        onTranslationChanged: function(delta) {
            if (active) canvas.drag(delta.x, delta.y)
        }
    }

    WheelHandler {
        target: null
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: function(event) {
            canvas.zoom(event.angleDelta.y / 120)
            event.accepted = true
        }
    }

    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: function(eventPoint) { canvas.contextMenuRequested(eventPoint.position) }
    }

    // Double-click exits the presentation, matching the Escape shortcut. A drag
    // to turn the panorama takes the grab and cancels the pending tap first.
    TapHandler {
        acceptedButtons: Qt.LeftButton
        onDoubleTapped: canvas.closeRequested()
    }

    HoverHandler {
        id: pixelHover
        enabled: canvas.pixelInspectionEnabled && canvas.visible && !panDrag.active
        blocking: false
        onHoveredChanged: refresh()
        onPointChanged: refresh()

        function refresh() {
            const position = point.position
            if (hovered && !controlColumn.contains(canvas.mapToItem(controlColumn, position)))
                canvas.inspectPixel(position)
            else
                canvas.clearPixelInspection()
        }
    }

    component FovField: Row {
        id: axis
        required property bool vertical
        readonly property real degrees: vertical ? canvas.verticalFieldOfView : canvas.horizontalFieldOfView
        readonly property string description: vertical ? qsTr("Vertical field of view (degrees)")
                                                       : qsTr("Horizontal field of view (degrees)")
        spacing: 1

        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: axis.vertical ? qsTr("V") : qsTr("H")
            color: Theme.mutedInk
            font.family: Theme.uiFont
            font.pixelSize: 11
        }

        TextField {
            id: field
            objectName: axis.vertical ? "panoramaVerticalFov" : "panoramaFov"
            Accessible.name: axis.description
            width: 44
            anchors.verticalCenter: parent.verticalCenter
            horizontalAlignment: TextInput.AlignHCenter
            selectByMouse: true
            color: Theme.graphiteInk
            font.family: Theme.monoFont
            font.pixelSize: 12
            inputMethodHints: Qt.ImhDigitsOnly

            Component.onCompleted: text = Math.round(axis.degrees).toString()
            onEditingFinished: {
                if (acceptableInput && text.length > 0 && Number(text) !== Math.round(axis.degrees)) {
                    if (axis.vertical)
                        canvas.verticalFieldOfView = Number(text)
                    else
                        canvas.horizontalFieldOfView = Number(text)
                }
                text = Math.round(axis.degrees).toString()
            }
            Connections {
                target: canvas
                function onLensChanged() {
                    if (!field.activeFocus)
                        field.text = Math.round(axis.degrees).toString()
                }
            }
            validator: IntValidator { bottom: axis.vertical ? 5 : 15; top: axis.vertical ? 150 : 120 }
            background: Rectangle {
                color: field.activeFocus ? Theme.accentSurface : Theme.softHover
                radius: 4
                border.width: field.activeFocus ? 1 : 0
                border.color: Theme.probeBlue
            }
            onActiveFocusChanged: if (activeFocus) selectAll()
            onAccepted: canvas.forceActiveFocus()
            Keys.onEscapePressed: function(event) {
                text = Math.round(axis.degrees).toString()
                canvas.forceActiveFocus()
                event.accepted = true
            }
            ToolTip.visible: hovered && !activeFocus
            ToolTip.text: axis.description
            ToolTip.delay: 500
        }
    }

    Column {
        id: controlColumn
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 20
        spacing: 8

        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            visible: canvas.detailMessage.length > 0
            width: detailText.implicitWidth + 20
            height: detailText.implicitHeight + 12
            color: Qt.rgba(Theme.raisedSurface.r, Theme.raisedSurface.g, Theme.raisedSurface.b, 0.92)
            border.color: Theme.opticalGray
            radius: 6
            Text {
                id: detailText
                anchors.centerIn: parent
                text: canvas.detailMessage
                color: Theme.graphiteInk
                font.family: Theme.uiFont
                font.pixelSize: 12
            }
        }

        Rectangle {
            width: controls.implicitWidth + 12
            height: controls.implicitHeight + 8
            color: Qt.rgba(Theme.raisedSurface.r, Theme.raisedSurface.g, Theme.raisedSurface.b, 0.92)
            border.color: Theme.opticalGray
            radius: 8

            Row {
                id: controls
                anchors.centerIn: parent
                spacing: 2

                AppIconButton {
                    objectName: "panoramaTurnLeft"
                    iconSource: canvas.iconPrefix + "back.svg"
                    toolTipText: qsTr("Look left")
                    autoRepeat: true
                    onClicked: canvas.turn(-1, 0)
                }
                AppIconButton {
                    objectName: "panoramaTurnRight"
                    iconSource: canvas.iconPrefix + "forward.svg"
                    toolTipText: qsTr("Look right")
                    autoRepeat: true
                    onClicked: canvas.turn(1, 0)
                }
                AppIconButton {
                    objectName: "panoramaTiltUp"
                    iconSource: canvas.iconPrefix + "up.svg"
                    toolTipText: qsTr("Look up")
                    autoRepeat: true
                    onClicked: canvas.turn(0, 1)
                }
                AppIconButton {
                    objectName: "panoramaTiltDown"
                    iconSource: canvas.iconPrefix + "down.svg"
                    toolTipText: qsTr("Look down")
                    autoRepeat: true
                    onClicked: canvas.turn(0, -1)
                }

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 1
                    height: 16
                    color: Theme.opticalGray
                }

                AppIconButton {
                    objectName: "panoramaZoomOut"
                    iconSource: canvas.iconPrefix + "zoom-out.svg"
                    toolTipText: qsTr("Zoom out")
                    autoRepeat: true
                    onClicked: canvas.zoom(-1)
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "FOV"
                    color: Theme.mutedInk
                    font.family: Theme.uiFont
                    font.pixelSize: 11
                }
                FovField { anchors.verticalCenter: parent.verticalCenter; vertical: false }
                FovField { anchors.verticalCenter: parent.verticalCenter; vertical: true }
                AppIconButton {
                    objectName: "panoramaZoomIn"
                    iconSource: canvas.iconPrefix + "zoom-in.svg"
                    toolTipText: qsTr("Zoom in")
                    autoRepeat: true
                    onClicked: canvas.zoom(1)
                }
                AppIconButton {
                    objectName: "panoramaResetView"
                    iconSource: canvas.iconPrefix + "fit.svg"
                    toolTipText: qsTr("Reset view")
                    onClicked: canvas.resetView()
                }
                AppIconButton {
                    objectName: "panoramaExitProjection"
                    iconSource: canvas.iconPrefix + "panorama.svg"
                    toolTipText: qsTr("Switch to flat view")
                    onClicked: if (canvas.controller) canvas.controller.setPanoramaEnabled(false)
                }
            }
        }
    }
}
