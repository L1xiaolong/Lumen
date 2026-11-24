import QtQuick
import QtQuick.Controls
import "Mvp"
import "Pages"

ApplicationWindow {
    id: window
    objectName: "qmlMainWindow"
    // C++ shows the window after restoring its last normal/maximized state.
    visible: false
    width: 1440
    height: 900
    minimumWidth: 980
    minimumHeight: 640
    title: qsTr("Lumen")
    // Keep the title for task switching and accessibility; omit its visible
    // decoration so the toolbar and system caption occupy a single surface.
    flags: windowChrome.supported
           ? Qt.Window | Qt.CustomizeWindowHint | Qt.WindowSystemMenuHint
             | Qt.WindowMinimizeButtonHint | Qt.WindowMaximizeButtonHint
             | Qt.WindowCloseButtonHint | Qt.ExpandedClientAreaHint | Qt.NoTitleBarBackgroundHint
           : Qt.Window
    topPadding: 0
    readonly property real unifiedTitleBarHeight: windowChrome.supported ? 44 : 0
    readonly property rect titleBarLeftControls: showingCompare && comparePage
        ? comparePage.titleBarLeftControls : browsePage.titleBarLeftControls
    readonly property rect titleBarRightControls: showingCompare && comparePage
        ? comparePage.titleBarRightControls : browsePage.titleBarRightControls
    color: Theme.sensorWhite
    palette.window: Theme.sensorWhite
    palette.windowText: Theme.graphiteInk
    palette.base: Theme.paperWhite
    palette.alternateBase: Theme.raisedSurface
    palette.text: Theme.graphiteInk
    palette.button: Theme.raisedSurface
    palette.buttonText: Theme.graphiteInk
    palette.highlight: Theme.explorerSelectionBg
    palette.highlightedText: Theme.graphiteInk
    palette.placeholderText: Theme.faintInk
    palette.mid: Theme.opticalGray
    palette.dark: Theme.opticalGray
    palette.toolTipBase: Theme.raisedSurface
    palette.toolTipText: Theme.graphiteInk
    palette.link: Theme.probeBlue
    property var comparePage: compareLoader.item
    property var fullScreenPage: fullScreenLoader.item
    property var settingsCard: settingsLoader.item
    readonly property bool modalVisible: updateDialog.visible || (settingsCard && settingsCard.visible)
    property var pendingComparePaths: []
    property int pendingSettingsSection: 0
    property var settingsReturnFocus: null
    function openCompare(paths) {
        pendingComparePaths = paths
        showingCompare = true
        compareLoader.active = true
        if (comparePage) { comparePage.open(paths); pendingComparePaths = [] }
    }
    function openSettings(section) {
        if (settingsCard && settingsCard.visible)
            return
        settingsReturnFocus = presentationWindow.activeFocusItem
        pendingSettingsSection = section === undefined ? 0 : section
        settingsLoader.active = true
        if (settingsCard) { settingsCard.currentSection = pendingSettingsSection; settingsCard.open() }
    }
    property bool showingCompare: false
    property bool showingFullScreen: false
    property bool fullScreenTransitioning: false
    property bool fullScreenUsesWindow: false
    readonly property Window presentationWindow: fullScreenUsesWindow ? imageViewer : window
    readonly property Item presentationOverlay: fullScreenUsesWindow ? imageViewer.Overlay.overlay : window.Overlay.overlay
    property var pendingFullScreenPaths: []
    property int pendingFullScreenIndex: 0
    property bool forceApplicationClose: false
    property bool applicationExitPending: false
    signal quitApplicationRequested()

    Binding {
        target: Theme
        property: "darkMode"
        value: appSettings.darkTheme
    }
    Binding {
        target: Theme
        property: "canvasBackgroundMode"
        value: appSettings.canvasBackground
    }

    function screenAtWindowCenter() {
        const centerX = x + width / 2
        const centerY = y + height / 2
        const screens = Qt.application.screens
        for (let i = 0; i < screens.length; ++i) {
            const candidate = screens[i]
            if (centerX >= candidate.virtualX && centerX < candidate.virtualX + candidate.width
                    && centerY >= candidate.virtualY
                    && centerY < candidate.virtualY + candidate.height)
                return candidate
        }
        return screen
    }

    function beginFullScreenView() {
        if (!fullScreenTransitioning || showingFullScreen || !fullScreenPage)
            return

        // The canvas is laid out at its final size before it draws the first image.
        fullScreenPage.open(pendingFullScreenPaths, pendingFullScreenIndex)
        pendingFullScreenPaths = []
        showingFullScreen = true
        if (fullScreenUsesWindow) {
            // Cocoa already considers the borderless screen-sized window zoomed. Request that
            // state so Qt does not unzoom it as it would with showNormal/show.
            if (Qt.platform.os === "osx")
                imageViewer.showMaximized()
            else
                imageViewer.showFullScreen()
            imageViewer.raise()
            imageViewer.requestActivate()
        }
        fullScreenPage.forceActiveFocus()
    }

    function openFullScreen(paths, initialIndex) {
        if (!paths || paths.length === 0 || showingFullScreen || fullScreenTransitioning)
            return
        showingCompare = false
        pendingFullScreenPaths = paths
        pendingFullScreenIndex = initialIndex
        fullScreenUsesWindow = visibility !== Window.FullScreen
        if (fullScreenUsesWindow) {
            imageViewer.screen = screenAtWindowCenter()
            fullScreenPresentation.prepare(imageViewer)
        }
        fullScreenTransitioning = true
        fullScreenLoader.active = true
        if (fullScreenPage)
            beginFullScreenView()
    }

    function closeCompare() {
        if (!showingCompare)
            return
        compareController.setHoldCandidate(false)
        compareController.closeSession()
        showingCompare = false
        Qt.callLater(function() { browsePage.forceActiveFocus() })
    }

    function closeFullScreen() {
        if (!showingFullScreen && !fullScreenTransitioning)
            return

        showingFullScreen = false
        fullScreenTransitioning = false
        pendingFullScreenPaths = []
        if (settingsCard && settingsCard.visible)
            settingsCard.close()
        fullScreenController.closeSession()
        if (fullScreenUsesWindow) {
            imageViewer.hide()
            window.requestActivate()
        }
        fullScreenUsesWindow = false
        browseController.refreshAll()
        Qt.callLater(function() { browsePage.forceActiveFocus() })
    }

    function beginApplicationExit() {
        if (applicationExitPending)
            return
        applicationExitPending = true
        compareController.closeSession()
        fullScreenController.closeSession()
        pendingFullScreenPaths = []
        fullScreenTransitioning = false
        imageViewer.hide()
        showingCompare = false
        showingFullScreen = false
        quitApplicationRequested()
    }

    onClosing: function(close) {
        if (!forceApplicationClose && !applicationExitPending && (showingFullScreen || fullScreenTransitioning)) {
            close.accepted = false
            Qt.callLater(function() { window.closeFullScreen() })
            return
        }
        if (!forceApplicationClose && !applicationExitPending && showingCompare) {
            close.accepted = false
            // Leave the native close callback before replacing the compare page. On Windows,
            // changing the window contents synchronously from this callback can hide the native
            // window even though the close event was rejected.
            Qt.callLater(function() { window.closeCompare() })
            return
        }

        close.accepted = true
        beginApplicationExit()
    }
    onVisibilityChanged: {
        if (showingFullScreen && !fullScreenUsesWindow
                && (visibility === Window.Windowed || visibility === Window.Maximized))
            closeFullScreen()
    }
    onActiveChanged: {
        if (active && imageViewer.visible && !imageViewer.active)
            Qt.callLater(function() {
                if (window.active && imageViewer.visible && !imageViewer.active)
                    imageViewer.requestActivate()
            })
    }

    ApplicationWindow {
        id: imageViewer
        objectName: "imageViewerWindow"
        transientParent: window
        title: window.title
        flags: Qt.Window | Qt.FramelessWindowHint
        color: Theme.canvasBackground
        topPadding: 0
        palette: window.palette
        visible: false
        onVisibleChanged: fullScreenPresentation.setActive(visible)
        onClosing: function(close) {
            if (window.forceApplicationClose || window.applicationExitPending)
                return
            close.accepted = false
            Qt.callLater(window.closeFullScreen)
        }
    }

    Component.onCompleted: {
        appSettings.startAutomaticUpdateCheck()
        if (appSettings.updateState === "error" && appSettings.latestVersion.length > 0)
            Qt.callLater(updateDialog.open)
        if (initialFullScreenPaths.length > 0)
            Qt.callLater(function() { window.openFullScreen(initialFullScreenPaths, 0) })
        else if (initialComparePaths.length >= 2)
            window.openCompare(initialComparePaths)
        if (showSettingsOnStartup) {
            window.openSettings(settingsStartupSection)
        }
    }
    Connections {
        target: browseController
        function onCompareRequested(paths) {
            window.openCompare(paths)
        }
    }

    BrowsePage {
        id: browsePage
        anchors.fill: parent
        titleBarHeight: window.unifiedTitleBarHeight || Theme.toolbarHeight
        captionButtonsWidth: windowChrome.captionButtonsWidth
        controller: browseController.activePane
        enabled: !window.fullScreenUsesWindow
        workspaceController: browseController
        propertiesController: imagePropertiesController
        rawController: rawParametersController
        settingsController: appSettings
        visible: !window.showingCompare && (!window.showingFullScreen || window.fullScreenUsesWindow)
        externalModalVisible: window.modalVisible
        onFullScreenRequested: function(paths, initialIndex) {
            window.openFullScreen(paths, initialIndex)
        }
        onSettingsRequested: window.openSettings()
    }
    WindowCaptionButtons {
        objectName: "windowCaptionButtons"
        anchors.top: parent.top
        anchors.right: parent.right
        z: 200
        controller: windowChrome
        enabled: !window.fullScreenUsesWindow
        maximized: window.visibility === Window.Maximized
        visible: windowChrome.supported && window.visibility !== Window.FullScreen
    }
    Loader {
        id: compareLoader
        anchors.fill: parent
        active: false
        asynchronous: true
        visible: window.showingCompare && !window.showingFullScreen
        onLoaded: {
            if (window.pendingComparePaths.length >= 2) {
                item.open(window.pendingComparePaths)
                window.pendingComparePaths = []
            }
        }
        sourceComponent: Component {
    ComparePage {
        titleBarHeight: window.unifiedTitleBarHeight || Theme.toolbarHeight
        captionButtonsWidth: windowChrome.captionButtonsWidth
        controller: compareController
        externalModalVisible: window.modalVisible
        settingsController: appSettings
        onCloseRequested: window.closeCompare()
    }

        }
    }
    Loader {
        id: fullScreenLoader
        parent: window.presentationWindow.contentItem
        anchors.fill: parent
        active: false
        asynchronous: true
        visible: window.showingFullScreen
        onLoaded: window.beginFullScreenView()
        sourceComponent: Component {
            FullScreenPage {
                controller: fullScreenController
                externalModalVisible: window.modalVisible
                propertiesController: fullScreenPropertiesController
                settingsController: appSettings
                messageParent: window.presentationWindow.contentItem
                onCloseRequested: window.closeFullScreen()
                onImageFrameReady: window.fullScreenTransitioning = false
                onImageErrorChanged: {
                    if (imageError.length > 0)
                        window.fullScreenTransitioning = false
                }
            }
        }
    }
    Connections {
        target: fullScreenController
        function onFilesystemChanged() { browseController.refreshAll() }
    }

    ApplicationNotices {
        objectName: "applicationNotices"
        parent: window.presentationWindow.contentItem
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.rightMargin: 16
        anchors.bottomMargin: 48
        width: Math.min(380, parent.width - 32)
        z: 100
        visible: !window.fullScreenTransitioning
        updatesVisible: !window.showingFullScreen && !window.modalVisible
        settingsController: appSettings
        diagnostics: typeof diagnosticsController !== "undefined" ? diagnosticsController : null
        onDiagnosticsRequested: window.openSettings(4)
        onUpdatesRequested: updateDialog.open()
    }

    UpdateDialog {
        id: updateDialog
        parent: window.presentationOverlay
        controller: appSettings
    }
    Connections {
        target: appSettings
        function onUpdateDetailsRequested() { updateDialog.open() }
    }

    Loader {
        id: settingsLoader
        active: false
        asynchronous: true
        onLoaded: { item.currentSection = window.pendingSettingsSection; item.open() }
        sourceComponent: Component {
            SettingsCard {
                parent: window.presentationOverlay
                settingsController: appSettings
                onUpdateDetailsRequested: updateDialog.open()
                onClosed: {
                    if (window.settingsReturnFocus)
                        window.settingsReturnFocus.forceActiveFocus()
                }
                diagnostics: typeof diagnosticsController !== "undefined" ? diagnosticsController : null
            }
        }
    }

    ShortcutContext { id: shortcutContext; targetWindow: window.presentationWindow }
    Shortcut {
        context: Qt.ApplicationShortcut
        enabled: window.presentationWindow.active &&
                  !window.modalVisible && !shortcutContext.textInputFocused
        sequence: {
            const revision = appSettings.shortcutsRevision
            return appSettings.shortcutFor("settings")
        }
        onActivated: window.openSettings()
    }
}
