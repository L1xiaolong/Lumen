pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import "."

Item {
    id: root
    objectName: "propertiesHistogram"
    property var controller: null
    property bool active: false
    property int selectedChannel: 0
    property bool logScale: false
    property real hoverFraction: -1
    property string observedPath: controller ? controller.path : ""
    property var histogramData: {
        const revision = controller ? controller.histogramRevision : 0
        return controller ? controller.histogram(1) : ({})
    }
    readonly property var channels: histogramData.channels || []
    readonly property var selectedChannelData: selectedChannel >= 0 && selectedChannel < channels.length
        ? channels[selectedChannel] : null
    readonly property string pendingLabel: qsTr("Analyzing…")
    readonly property string unavailableLabel: qsTr("Histogram unavailable")
    readonly property var statistics: !selectedChannelData ? [] : [
        { id: "histogram-mean", label: qsTr("Mean"), value: formatStatistic(selectedChannelData.mean, 2), mono: true },
        { id: "histogram-variance", label: qsTr("Variance"), value: formatStatistic(selectedChannelData.variance, 2), mono: true },
        { id: "histogram-min", label: qsTr("Min"), value: formatStatistic(selectedChannelData.min, 0), mono: true },
        { id: "histogram-max", label: qsTr("Max"), value: formatStatistic(selectedChannelData.max, 0), mono: true },
        { id: "histogram-median", label: qsTr("Median"), value: formatStatistic(selectedChannelData.median, 1), mono: true }
    ]
    onPendingLabelChanged: graph.requestPaint()
    onUnavailableLabelChanged: graph.requestPaint()
    onSelectedChannelChanged: graph.requestPaint()
    onLogScaleChanged: graph.requestPaint()
    onHoverFractionChanged: graph.requestPaint()
    implicitHeight: layout.implicitHeight
    function channelName(channel) { return channel.name || "" }
    function formatStatistic(value, decimals) {
        const number = Number(value)
        if (!histogramData.floatingPoint) return number.toFixed(decimals)
        return Number.isInteger(number) ? String(number) : String(Number(number.toPrecision(6)))
    }
    function formatAxisValue(value) {
        if (!histogramData.floatingPoint) return String(Math.round(value))
        if (Math.abs(value) >= 10000 || (value !== 0 && Math.abs(value) < 0.001))
            return value.toExponential(1)
        return String(Number(value.toPrecision(4)))
    }
    function requestHistogram() {
        if (active && controller && controller.path.length > 0) controller.requestHistogram(1)
    }
    onActiveChanged: requestHistogram()
    onObservedPathChanged: { selectedChannel = 0; hoverFraction = -1; requestHistogram() }
    onChannelsChanged: if (selectedChannel >= channels.length) selectedChannel = 0
    onHistogramDataChanged: graph.requestPaint()
    Connections {
        target: Theme
        function onDarkModeChanged() { graph.requestPaint() }
    }
    Component.onCompleted: requestHistogram()

    Column {
        id: layout
        width: parent.width
        spacing: 12
        Item {
            width: parent.width
            height: 32
            Text {
                id: channelLabel
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Channel")
                font.family: Theme.uiFont
                font.pixelSize: 12
                color: Theme.mutedInk
            }
            SettingsComboBox {
                id: channelSelector
                objectName: "histogramChannelSelector"
                anchors.left: channelLabel.right
                anchors.leftMargin: 12
                anchors.right: logScaleButton.left
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                model: root.channels.map(function(channel) { return root.channelName(channel) })
                currentIndex: root.channels.length > 0 ? root.selectedChannel : -1
                enabled: root.histogramData.valid === true && root.channels.length > 0
                Accessible.name: channelLabel.text
                onActivated: function(index) { root.selectedChannel = index }
            }
            SettingsButton {
                id: logScaleButton
                objectName: "histogramLogScaleButton"
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                implicitWidth: 56
                text: root.logScale ? qsTr("Log") : qsTr("Linear")
                checkable: true
                checked: root.logScale
                enabled: root.histogramData.valid === true && root.channels.length > 0
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: qsTr("Toggle logarithmic histogram scale")
                onToggled: root.logScale = checked
            }
        }
        Rectangle {
            width: parent.width
            height: 190
            radius: Theme.radius
            color: Theme.raisedSurface
            border.color: Theme.opticalGray
            clip: true
            Canvas {
                id: graph
                objectName: "propertiesHistogramGraph"
                anchors.fill: parent
                anchors.margins: 8
                antialiasing: true

                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()
                onPaint: {
                    const context = getContext("2d")
                    context.reset()
                    const left = 28
                    const top = 8
                    const right = width - 24
                    const bottom = height - 22

                    context.strokeStyle = Theme.opticalGray
                    context.lineWidth = 1
                    for (let division = 0; division <= 4; ++division) {
                        const y = top + (bottom - top) * division / 4
                        context.beginPath()
                        context.moveTo(left, Math.round(y) + 0.5)
                        context.lineTo(right, Math.round(y) + 0.5)
                        context.stroke()
                    }
                    context.fillStyle = Theme.mutedInk
                    context.font = "12px \"" + (Theme.uiFont || "sans-serif") + "\""
                    context.textAlign = "center"
                    const minimum = Number(root.selectedChannelData?.axisMinimum ?? 0)
                    const maximumValue = Number(root.selectedChannelData?.axisMaximum ?? root.histogramData.maximumValue ?? 255)
                    for (let tick = 0; tick <= 4; ++tick) {
                        const fraction = tick / 4
                        const x = left + (right - left) * fraction
                        context.fillText(root.formatAxisValue(minimum * (1 - fraction) + maximumValue * fraction),
                                         x, height - 5)
                    }

                    if (root.histogramData.loading === true) {
                        context.fillStyle = Theme.mutedInk
                        context.font = "12px \"" + (Theme.uiFont || "sans-serif") + "\""
                        context.fillText(root.pendingLabel, (left + right) / 2, (top + bottom) / 2)
                        return
                    }
                    const channel = root.channels.length > root.selectedChannel
                                  ? root.channels[root.selectedChannel] : null
                    if (!channel || !channel.bins || channel.bins.length < 2) {
                        context.fillStyle = Theme.faintInk
                        context.font = "12px \"" + (Theme.uiFont || "sans-serif") + "\""
                        context.fillText(root.unavailableLabel,
                                         (left + right) / 2, (top + bottom) / 2)
                        return
                    }
                    const samples = Math.max(2, Math.min(channel.bins.length,
                                                         Math.floor(right - left)))
                    const values = []
                    let maximum = 0
                    for (let point = 0; point < samples; ++point) {
                        const start = Math.floor(point * channel.bins.length / samples)
                        const end = Math.max(start + 1,
                                             Math.floor((point + 1) * channel.bins.length / samples))
                        let count = 0
                        for (let bin = start; bin < end; ++bin)
                            count += Number(channel.bins[bin])
                        values.push(count)
                        maximum = Math.max(maximum, count)
                    }
                    if (maximum <= 0)
                        return
                    context.beginPath()
                    context.moveTo(left, bottom)
                    const logDenom = root.logScale ? Math.log(1 + maximum) : 1
                    for (let point = 0; point < samples; ++point) {
                        const x = left + point * (right - left) / (samples - 1)
                        const ratio = root.logScale
                            ? (logDenom > 0 ? Math.log(1 + values[point]) / logDenom : 0)
                            : (values[point] / maximum)
                        const y = bottom - (bottom - top) * ratio
                        context.lineTo(x, y)
                    }
                    context.lineTo(right, bottom)
                    context.closePath()
                    const color = channel.color || Theme.mutedInk
                    context.fillStyle = color
                    context.fill()
                    context.strokeStyle = color
                    context.lineWidth = 1.25
                    context.stroke()

                    if (root.hoverFraction >= 0 && root.hoverFraction <= 1) {
                        const hoverX = left + root.hoverFraction * (right - left)
                        const sampleIndex = Math.max(0, Math.min(samples - 1, Math.round(root.hoverFraction * (samples - 1))))
                        const sampleValue = minimum * (1 - root.hoverFraction) + maximumValue * root.hoverFraction
                        const sampleCount = values[sampleIndex]
                        context.strokeStyle = Theme.probeBlue
                        context.lineWidth = 1
                        context.beginPath()
                        context.moveTo(Math.round(hoverX) + 0.5, top)
                        context.lineTo(Math.round(hoverX) + 0.5, bottom)
                        context.stroke()

                        const readout = root.formatAxisValue(sampleValue) + " · " + sampleCount
                        context.fillStyle = Theme.graphiteInk
                        context.textAlign = "right"
                        context.fillText(readout, right, top + 11)
                    }
                }
            }
            MouseArea {
                anchors.fill: graph
                hoverEnabled: true
                acceptedButtons: Qt.NoButton
                onPositionChanged: function(mouse) {
                    const left = 28
                    const right = graph.width - 24
                    if (right > left && mouse.x >= left && mouse.x <= right)
                        root.hoverFraction = (mouse.x - left) / (right - left)
                    else
                        root.hoverFraction = -1
                }
                onExited: root.hoverFraction = -1
            }


        }
        Rectangle {
            width: parent.width
            height: visible ? statisticsFields.implicitHeight : 0
            visible: root.statistics.length > 0
            radius: Theme.radius
            color: Theme.raisedSurface
            border.color: Theme.opticalGray
            PropertyFieldList {
                id: statisticsFields
                objectName: "histogramStatistics"
                width: parent.width
                fields: root.statistics
            }
        }
    }
}
