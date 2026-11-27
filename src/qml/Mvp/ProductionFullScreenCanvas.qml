import QtQuick

ImageCanvas {
    id: root
    property var controller: null
    property var settingsController: null
    signal contextMenuRequested(point position)
    signal closeRequested()
    presentationMode: 0
    viewSynchronized: false
    backgroundColor: Theme.canvasBackground
    smoothDisplay: !settingsController || settingsController.smoothDisplay === undefined
                   ? true : settingsController.smoothDisplay

    onControllerChanged: {
        if (controller)
            controller.attachCanvas(root)
    }
    Component.onCompleted: {
        if (controller)
            controller.attachCanvas(root)
    }

    // Open after the right click finishes without taking the canvas's mouse grab.
    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: function(eventPoint) { root.contextMenuRequested(eventPoint.position) }
    }

    // Double-click exits the presentation, matching the Escape shortcut. The
    // passive grab keeps image panning (the canvas's own left-button drag)
    // working; a tap that turns into a drag is cancelled automatically.
    TapHandler {
        acceptedButtons: Qt.LeftButton
        onDoubleTapped: root.closeRequested()
    }

    // Deliver the scene's pointer position to the pixel probe alongside the canvas hover events.
    HoverHandler {
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onPointChanged: root.probePixelAt(point.position)
        onHoveredChanged: {
            if (hovered)
                root.probePixelAt(point.position)
            else
                root.clearPixelProbe()
        }
    }
}
