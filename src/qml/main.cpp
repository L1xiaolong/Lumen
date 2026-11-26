#include "diagnostics/diagnostics.h"
#include "diagnostics/diagnostics_controller.h"
#include <QStandardPaths>
#include "io/default_image_decoder.h"
#include "browser/file_clipboard.h"
#include "platform/full_screen_presentation_controller.h"
#include "qml/app_settings.h"
#include "core/settings_migration.h"
#include "qml/browse_controller.h"
#include "qml/browse_workspace_controller.h"
#include "qml/compare_controller.h"
#include "qml/qml_image_canvas.h"
#include <QPointer>
#include "qml/image_properties_controller.h"
#include "qml/full_screen_controller.h"
#include "qml/raw_parameters_controller.h"
#include "qml/thumbnail_image_provider.h"
#include "qml/window_chrome_controller.h"

#ifdef MVPVIEW_ENABLE_UI_TESTS
#include "qml/fullscreen_menu_test.h"
#endif

#include <QCoreApplication>
#include <algorithm>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QIcon>
#include <QImageReader>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QScreen>
#include <QSettings>
#include <QStyleHints>
#include <QTimer>
#include <QUrl>
#include <rhi/qrhi.h>

static int runApplication(int argc, char* argv[], mvpview::diagnostics::Service& diagnosticService) {
    QGuiApplication app(argc, argv);
    constexpr int minimumImageAllocationLimitMiB = 512;
    const int imageAllocationLimitMiB = QImageReader::allocationLimit();
    if (imageAllocationLimitMiB > 0 && imageAllocationLimitMiB < minimumImageAllocationLimitMiB) {
        QImageReader::setAllocationLimit(minimumImageAllocationLimitMiB);
    }
    mvpview::FileClipboard::initialize();
#ifdef Q_OS_WIN
    // Qt defaults to D3D11 on Windows. Some Intel drivers crash while Qt Quick creates
    // or migrates RHI resources during full-screen and cross-monitor transitions.
    // OpenGL avoids that driver path while preserving hardware-accelerated rendering.
    // Honor Qt's backend override for driver troubleshooting. An explicit C++
    // API request takes precedence over QSG_RHI_BACKEND in Qt.
    if (qEnvironmentVariableIsEmpty("QSG_RHI_BACKEND") &&
        qEnvironmentVariableIsEmpty("QT_QUICK_BACKEND")) {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    }
#endif
    QGuiApplication::setQuitOnLastWindowClosed(false);
    QCoreApplication::setApplicationName(QStringLiteral("Lumen"));
    QCoreApplication::setOrganizationName(QStringLiteral("MvpView"));
    QCoreApplication::setApplicationVersion(QStringLiteral(MVPVIEW_PROJECT_VERSION));
#ifdef Q_OS_WIN
    // Match Explorer, shortcuts and the installer, including every DPI size.
    app.setWindowIcon(QIcon(QStringLiteral(":/brand/Lumen.ico")));
#elif !defined(Q_OS_MACOS)
    // macOS should keep the icon declared by CFBundleIconFile for the whole
    // application lifetime. Setting the generic runtime PNG here replaces the
    // padded macOS artwork in the Dock after launch.
    app.setWindowIcon(QIcon(QStringLiteral(":/brand/app_icon.png")));
#endif

    QQuickStyle::setStyle(QStringLiteral("Basic"));
    QSettings settings;
    mvpview::AppSettings appSettings(&app);
    const auto synchronizeNativeTheme = [&app, &appSettings] {
        const auto scheme = appSettings.theme() == QStringLiteral("system") ? Qt::ColorScheme::Unknown
            : appSettings.darkTheme() ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light;
        app.styleHints()->setColorScheme(scheme);
    };
    QObject::connect(&appSettings, &mvpview::AppSettings::themeChanged, &app, synchronizeNativeTheme);
    synchronizeNativeTheme();
    mvpview::DiagnosticsController diagnosticsController(diagnosticService);
    QObject::connect(&appSettings, &mvpview::AppSettings::diagnosticsChanged, &app, [&] {
        diagnosticService.configure(appSettings.loggingEnabled(), mvpview::diagnostics::parseLevel(appSettings.logLevel()));
    });
    bool lastMainWindowStateWasMaximized =
        settings.value(QStringLiteral("window/maximized"), false).toBool();

    QString initialDirectory;
    QString screenshotPath;
    QString selectedPath;
    QStringList initialComparePaths;
    QStringList initialFullScreenPaths;
    QString displayMode;
    int initialFileManagerCount = 1;
    bool nativeScreenshot = false;
    bool showSettings = false;
    int settingsStartupSection = 0;
    int screenshotDelay = 1800;
    const QStringList arguments = app.arguments();
    for (int i = 1; i < arguments.size(); ++i) {
        if (arguments.at(i) == QStringLiteral("--screenshot") && i + 1 < arguments.size()) {
            screenshotPath = arguments.at(++i);
        } else if (arguments.at(i) == QStringLiteral("--screenshot-native")) {
            nativeScreenshot = true;
        } else if (arguments.at(i) == QStringLiteral("--show-settings")) {
            showSettings = true;
        } else if (arguments.at(i) == QStringLiteral("--show-diagnostics-settings")) {
            showSettings = true;
            settingsStartupSection = 4;
        } else if (arguments.at(i) == QStringLiteral("--show-appearance-settings")) {
            showSettings = true;
            settingsStartupSection = 1;
        } else if (arguments.at(i) == QStringLiteral("--show-shortcuts-settings")) {
            showSettings = true;
            settingsStartupSection = 2;
        } else if (arguments.at(i) == QStringLiteral("--show-color-settings")) {
            showSettings = true;
            settingsStartupSection = 1;
        } else if (arguments.at(i) == QStringLiteral("--show-update-settings")) {
            showSettings = true;
            settingsStartupSection = 3;
        } else if (arguments.at(i) == QStringLiteral("--select") && i + 1 < arguments.size()) {
            selectedPath = QFileInfo(arguments.at(++i)).absoluteFilePath();
        } else if (arguments.at(i) == QStringLiteral("--display-mode") &&
                   i + 1 < arguments.size()) {
            displayMode = arguments.at(++i).toLower();
        } else if (arguments.at(i) == QStringLiteral("--screenshot-delay") &&
                   i + 1 < arguments.size()) {
            screenshotDelay = qMax(250, arguments.at(++i).toInt());
        } else if (arguments.at(i) == QStringLiteral("--file-managers") &&
                   i + 1 < arguments.size()) {
            initialFileManagerCount = qBound(1, arguments.at(++i).toInt(), 4);
        } else if (arguments.at(i) == QStringLiteral("--qml-fullscreen") && i + 1 < arguments.size()) {
            initialFullScreenPaths.append(QFileInfo(arguments.at(++i)).absoluteFilePath());
        } else if (arguments.at(i) == QStringLiteral("--qml-compare")) {
            while (i + 1 < arguments.size() && !arguments.at(i + 1).startsWith(QLatin1Char('-'))) {
                initialComparePaths.append(QFileInfo(arguments.at(++i)).absoluteFilePath());
            }
        } else if (!arguments.at(i).startsWith(QLatin1Char('-'))) {
            initialDirectory = arguments.at(i);
        }
    }
    if (initialDirectory.isEmpty() && !appSettings.restoreLastDirectory() &&
        appSettings.installedUpdateVersion().isEmpty()) {
        initialDirectory = QDir::homePath();
    }
#ifdef MVPVIEW_ENABLE_UI_TESTS
    const QString uiTestDirectory = qEnvironmentVariable("LUMEN_FULLSCREEN_TEST_DIRECTORY");
    const auto uiTestVisibility = qEnvironmentVariable("LUMEN_FULLSCREEN_TEST_MODE") ==
        QStringLiteral("maximized") ? QWindow::Maximized : QWindow::Windowed;
    QString uiTestFixture;
    if (!uiTestDirectory.isEmpty()) {
        uiTestFixture = qEnvironmentVariable("LUMEN_FULLSCREEN_TEST_EXR");
        if (uiTestFixture.isEmpty())
            uiTestFixture = mvpview::createFullScreenMenuFixture(uiTestDirectory);
        const QFileInfo fixture(uiTestFixture);
        if (!fixture.isFile() || fixture.suffix().compare(QStringLiteral("exr"), Qt::CaseInsensitive) != 0) {
            qCritical() << "The full-screen menu test requires an EXR image" << uiTestFixture;
            return 1;
        }
        initialDirectory = fixture.absolutePath();
        displayMode = QStringLiteral("grid");
        lastMainWindowStateWasMaximized = uiTestVisibility == QWindow::Maximized;
        appSettings.setAutomaticUpdateChecks(false);
    }
#endif
    // The browse page can use the software scene graph for deterministic CI
    // captures. ComparePage contains a QQuickRhiItem and therefore must keep a
    // hardware RHI backend even when --screenshot-native was not specified.
    if (!screenshotPath.isEmpty() && !nativeScreenshot && initialComparePaths.isEmpty() &&
        initialFullScreenPaths.isEmpty()) {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    }

    const auto decoder = mvpview::createDefaultImageDecoder();
    // Keep filesystem restoration out of the pre-window startup path. On a cold first launch,
    // QML and shader caches are also being populated; touching a slow or unavailable previous
    // directory here can otherwise prevent the first frame from appearing at all.
    mvpview::BrowseWorkspaceController browseController(decoder, initialDirectory, true);
    while (browseController.paneCount() < initialFileManagerCount)
        browseController.addFileManagerPane();
    mvpview::CompareController compareController(browseController.loader());
    mvpview::ImagePropertiesController imagePropertiesController(browseController.loader());
    mvpview::ImagePropertiesController fullScreenPropertiesController(browseController.loader());
    mvpview::FullScreenController fullScreenController(browseController.loader());
    mvpview::FullScreenPresentationController fullScreenPresentation;
    mvpview::RawParametersController rawParametersController(browseController.loader());
    QObject::connect(&appSettings, &mvpview::AppSettings::colorDisplayChanged, &app, [&] {
        mvpview::diagnostics::event(mvpview::diagnostics::Level::Info, mvpview::diagnostics::settings(), QStringLiteral("settings.color_display"),
            {{"colorProfiles", appSettings.applyEmbeddedColorProfiles()}, {"highBitDepth", appSettings.preserveHighBitDepth()},
             {"displayColorSpace", appSettings.displayColorSpace()},
             {"exifOrientation", appSettings.honorExifOrientation()}}, true);
        browseController.loader()->clearCache();
        fullScreenController.reload();
        compareController.reload();
    });

    mvpview::WindowChromeController windowChrome;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("windowChrome"), &windowChrome);
    QObject::connect(&appSettings, &mvpview::AppSettings::languageChanged,
                     &engine, &QQmlApplicationEngine::retranslate);
    QObject::connect(&appSettings, &mvpview::AppSettings::languageChanged,
                     &imagePropertiesController, &mvpview::ImagePropertiesController::retranslate);
    QObject::connect(&appSettings, &mvpview::AppSettings::languageChanged,
                     &fullScreenPropertiesController, &mvpview::ImagePropertiesController::retranslate);
    QObject::connect(&appSettings, &mvpview::AppSettings::languageChanged,
                     &browseController, &mvpview::BrowseWorkspaceController::retranslate);
    engine.rootContext()->setContextProperty(QStringLiteral("fullScreenPropertiesController"), &fullScreenPropertiesController);
    engine.rootContext()->setContextProperty(QStringLiteral("appSettings"), &appSettings);
    engine.rootContext()->setContextProperty(QStringLiteral("diagnosticsController"), &diagnosticsController);
    engine.rootContext()->setContextProperty(QStringLiteral("systemUiFontFamily"),
                                             app.font().family());
    engine.rootContext()->setContextProperty(
        QStringLiteral("systemFixedFontFamily"),
        QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
    engine.rootContext()->setContextProperty(QStringLiteral("browseController"), &browseController);
    engine.rootContext()->setContextProperty(QStringLiteral("compareController"), &compareController);
    engine.rootContext()->setContextProperty(QStringLiteral("imagePropertiesController"),
                                             &imagePropertiesController);
    engine.rootContext()->setContextProperty(QStringLiteral("fullScreenController"),
                                             &fullScreenController);
    engine.rootContext()->setContextProperty(QStringLiteral("fullScreenPresentation"),
                                             &fullScreenPresentation);
    engine.rootContext()->setContextProperty(QStringLiteral("rawParametersController"),
                                             &rawParametersController);
    engine.rootContext()->setContextProperty(QStringLiteral("initialComparePaths"), initialComparePaths);
    engine.rootContext()->setContextProperty(QStringLiteral("initialFullScreenPaths"), initialFullScreenPaths);
    engine.rootContext()->setContextProperty(QStringLiteral("showSettingsOnStartup"), showSettings);
    engine.rootContext()->setContextProperty(QStringLiteral("settingsStartupSection"),
                                             settingsStartupSection);
    engine.addImageProvider(QStringLiteral("thumbnail"),
                            new mvpview::ThumbnailImageProvider(decoder,
                                                                browseController.loader()));
#ifdef Q_OS_WIN
    engine.addImageProvider(QStringLiteral("system-folder"),
                            new mvpview::SystemFolderIconProvider());
#endif
    mvpview::diagnostics::event(mvpview::diagnostics::Level::Info, mvpview::diagnostics::startup(), QStringLiteral("qml.loading"),
        mvpview::diagnostics::startupContext(), true);
#ifdef MVPVIEW_ENABLE_UI_TESTS
    // The browse page composes asynchronously after the window appears. UI tests assert on
    // the fully built workspace right after the event loop starts, so they need the loader
    // synchronous instead.
    if (!uiTestDirectory.isEmpty())
        engine.rootContext()->setContextProperty(QStringLiteral("uiTestSynchronousStartup"), true);
#endif
    engine.load(QUrl(QStringLiteral("qrc:/MvpViewQml/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }
    auto* mainWindow = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
    if (!mainWindow) {
        return 1;
    }
    mvpview::diagnostics::event(mvpview::diagnostics::Level::Info, mvpview::diagnostics::startup(), QStringLiteral("qml.loaded"),
        mvpview::diagnostics::startupContext(), true);
    QObject::connect(mainWindow, &QQuickWindow::sceneGraphInitialized, mainWindow, [mainWindow] {
        const auto* renderer = mainWindow->rendererInterface();
        auto* rhi = static_cast<QRhi*>(renderer->getResource(mainWindow, QSGRendererInterface::RhiResource));
        QJsonObject context{{"graphicsApi", static_cast<int>(renderer->graphicsApi())}};
        if (rhi) {
            const auto driver = rhi->driverInfo();
            context.insert(QStringLiteral("device"), QString::fromUtf8(driver.deviceName));
            context.insert(QStringLiteral("deviceType"), static_cast<int>(driver.deviceType));
            context.insert(QStringLiteral("textureSizeMax"), rhi->resourceLimit(QRhi::TextureSizeMax));
        }
        mvpview::diagnostics::event(mvpview::diagnostics::Level::Info, mvpview::diagnostics::render(), QStringLiteral("scenegraph.initialized"),
            mvpview::diagnostics::startupContext(std::move(context)), true);
    }, Qt::DirectConnection);
    // The canvas buffer is interpreted in the colour space the platform reports for the window
    // surface, so "auto" has to follow it. The report is only populated once the platform window
    // exists, and these callbacks may arrive from the render thread, so they are queued to the
    // main thread through the settings object.
    const auto presentedWindow = [mainWindow] {
        auto* window = mainWindow->property("presentationWindow").value<QQuickWindow*>();
        return window && window->isVisible() ? window : mainWindow;
    };
    const auto applySurfaceColorSpace = [presentedWindow, &appSettings] {
        appSettings.setSurfaceColorSpace(presentedWindow()->format().colorSpace());
    };
    QObject::connect(mainWindow, &QQuickWindow::sceneGraphInitialized, &appSettings,
                     [applySurfaceColorSpace] { applySurfaceColorSpace(); });
    if (auto* viewer = mainWindow->findChild<QQuickWindow*>(QStringLiteral("imageViewerWindow"))) {
        QObject::connect(viewer, &QQuickWindow::sceneGraphInitialized, &appSettings,
                         applySurfaceColorSpace, Qt::QueuedConnection);
        QObject::connect(viewer, &QWindow::screenChanged, &appSettings, applySurfaceColorSpace);
        QObject::connect(viewer, &QWindow::visibleChanged, &appSettings, applySurfaceColorSpace);
    }
    mainWindow->create();
    windowChrome.attach(mainWindow);
    const auto synchronizeChromeVisibility = [&] {
        windowChrome.setSuspended(mainWindow->visibility() == QWindow::FullScreen);
    };
    QObject::connect(mainWindow, &QWindow::visibilityChanged,
                     &windowChrome, synchronizeChromeVisibility);
    applySurfaceColorSpace();
    QObject::connect(mainWindow, &QWindow::screenChanged, mainWindow, [applySurfaceColorSpace] {
        mvpview::diagnostics::event(mvpview::diagnostics::Level::Info, mvpview::diagnostics::render(), QStringLiteral("window.screen_changed"), {}, true);
        applySurfaceColorSpace();
    });
    QObject::connect(mainWindow, &QWindow::visibilityChanged, mainWindow, [](QWindow::Visibility visibility) {
        mvpview::diagnostics::event(mvpview::diagnostics::Level::Info, mvpview::diagnostics::render(), QStringLiteral("window.visibility"), {{"visibility", static_cast<int>(visibility)}}, true);
    });
    // QML emits this signal only for a real application close; closing a compare/full-screen
    // session is deliberately kept inside QML.
    QObject::connect(mainWindow, SIGNAL(quitApplicationRequested()), &app, SLOT(quit()),
                     Qt::QueuedConnection);
    QTimer updateInstallTimer;
    updateInstallTimer.setInterval(150);
    QObject::connect(&appSettings, &mvpview::AppSettings::updateStateChanged, &updateInstallTimer,
                     [&] {
        if (appSettings.updateState() == QStringLiteral("ready")) {
            if (!updateInstallTimer.isActive()) updateInstallTimer.start();
        } else {
            updateInstallTimer.stop();
        }
        if (appSettings.updateState() == QStringLiteral("error"))
            mainWindow->setProperty("forceApplicationClose", false);
    });
    QObject::connect(&updateInstallTimer, &QTimer::timeout, &app, [&] {
        if (mvpview::BrowseController::hasPendingFileOperations()) return;
        if (const auto* pane = browseController.activeBrowsePane();
            pane && !pane->currentDirectory().isEmpty()) {
            QSettings().setValue(QStringLiteral("browser/lastDirectory"), pane->currentDirectory());
        }
        mainWindow->setProperty("forceApplicationClose", true);
        appSettings.installPreparedUpdate();
    });
    QObject::connect(&appSettings, &mvpview::AppSettings::quitForUpdateRequested, mainWindow,
                     [mainWindow] { QMetaObject::invokeMethod(mainWindow, "beginApplicationExit"); });
#ifdef MVPVIEW_ENABLE_UI_TESTS
    // A window sized to the display can be reported as maximized by AppKit.
    if (!uiTestDirectory.isEmpty() && uiTestVisibility == QWindow::Windowed)
        mainWindow->resize(mainWindow->minimumSize());
#endif
    if (lastMainWindowStateWasMaximized) {
        mainWindow->showMaximized();
    } else {
        mainWindow->showNormal();
    }
    mvpview::diagnostics::event(mvpview::diagnostics::Level::Info, mvpview::diagnostics::startup(),
        QStringLiteral("window.shown"), mvpview::diagnostics::startupContext(), true);
    // Remember the most recent stable state so closing from the taskbar still restores to
    // normal/maximized instead of starting minimized.
    // The Hidden fallback is gated by applicationExitPending so rejecting a compare/full-screen
    // close can never terminate the process.
    QObject::connect(
        mainWindow, &QWindow::visibilityChanged, &app,
        [mainWindow, &lastMainWindowStateWasMaximized](QWindow::Visibility visibility) {
#ifdef Q_OS_WIN
            if (visibility == QWindow::Hidden
                && mainWindow->property("applicationExitPending").toBool()) {
                QCoreApplication::quit();
                return;
            }
#endif
            if (mainWindow->property("fullScreenUsesWindow").toBool()) return;
            if (visibility == QWindow::Maximized) {
                lastMainWindowStateWasMaximized = true;
            } else if (visibility == QWindow::Windowed) {
                lastMainWindowStateWasMaximized = false;
            }
        });
    QObject::connect(&app, &QCoreApplication::aboutToQuit, mainWindow, [mainWindow] {
        // Make any transient helper window accept shutdown so none can strand the event loop
        // after the main window has disappeared.
        mainWindow->setProperty("forceApplicationClose", true);
        mainWindow->setProperty("applicationExitPending", true);
        const auto windows = QGuiApplication::allWindows();
        for (QWindow* window : windows) {
            if (window != mainWindow) window->close();
        }
    });
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app,
                     [&lastMainWindowStateWasMaximized] {
                         if (mvpview::AppSettings::softwareCacheDeletionRequested())
                             return;
                         QSettings().setValue(QStringLiteral("window/maximized"),
                                              lastMainWindowStateWasMaximized);
                     });
    auto firstFrameConnection = std::make_shared<QMetaObject::Connection>();
    *firstFrameConnection = QObject::connect(mainWindow, &QQuickWindow::frameSwapped, &browseController,
        [&browseController, &diagnosticService, firstFrameConnection] {
            QObject::disconnect(*firstFrameConnection);
            mvpview::diagnostics::event(mvpview::diagnostics::Level::Info, mvpview::diagnostics::startup(),
                QStringLiteral("startup.first_frame"), mvpview::diagnostics::startupContext(), true);
            // The user already sees the window, so the deferred folder restore owns the next
            // milestone. Crash capture only needs to be ready for crashes after this point;
            // spawning its helper process before the first frame would only compete with it.
            browseController.startDeferredInitialDirectory();
            diagnosticService.startCrashCapture({});
            mvpview::diagnostics::event(mvpview::diagnostics::Level::Info, mvpview::diagnostics::startup(),
                QStringLiteral("crash_capture.started"),
                mvpview::diagnostics::startupContext({{"status", diagnosticService.crashStatus()},
                                                      {"error", diagnosticService.error()}}), true);
        }, Qt::QueuedConnection);

    if (!displayMode.isEmpty()) {
        const int mode = displayMode == QStringLiteral("list")
                             ? 1
                             : displayMode == QStringLiteral("gallery") ? 2 : 0;
        browseController.setActiveDisplayMode(mode);
    }

    if (!selectedPath.isEmpty()) {
        QTimer::singleShot(700, &browseController,
                           [&browseController, selectedPath] {
                               if (auto* pane = browseController.activeBrowsePane())
                                   pane->selectPath(selectedPath);
                           });
    }

    if (!screenshotPath.isEmpty()) {
        QTimer::singleShot(screenshotDelay, &app,
                           [&app, mainWindow, presentedWindow, screenshotPath] {
            if (!presentedWindow()->grabWindow().save(screenshotPath)) {
                app.exit(2);
                return;
            }
            mainWindow->setProperty("forceApplicationClose", true);
            app.quit();
        });
    }
#ifdef MVPVIEW_ENABLE_UI_TESTS
    if (!uiTestDirectory.isEmpty()) {
        QTimer::singleShot(0, &app, [&app, &appSettings, mainWindow, uiTestVisibility, uiTestFixture] {
            app.exit(mvpview::testFullScreenMenu(*mainWindow, uiTestVisibility, uiTestFixture, appSettings));
        });
    }
#endif
    return app.exec();
}

int main(int argc, char* argv[]) {
    // Anchor the shared startup clock before any other work so every milestone below shares
    // one timeline from process start.
    (void) mvpview::diagnostics::startupElapsedMs();
    QCoreApplication::setApplicationName(QStringLiteral("Lumen"));
    QCoreApplication::setOrganizationName(QStringLiteral("MvpView"));
    QCoreApplication::setApplicationVersion(QStringLiteral(MVPVIEW_PROJECT_VERSION));
#ifdef MVPVIEW_ENABLE_UI_TESTS
    const QString uiTestDirectory = qEnvironmentVariable("LUMEN_FULLSCREEN_TEST_DIRECTORY");
    if (!uiTestDirectory.isEmpty()) {
        QStandardPaths::setTestModeEnabled(true);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           QDir(uiTestDirectory).filePath(QStringLiteral("settings")));
    } else
#endif
    if (!mvpview::migrateLegacySettings())
        qWarning("Could not finish migrating legacy settings to Lumen");
    mvpview::diagnostics::event(mvpview::diagnostics::Level::Info, mvpview::diagnostics::startup(),
        QStringLiteral("settings.migrated"), mvpview::diagnostics::startupContext(), true);
    int result = 0;
    {
        const QSettings diagnosticSettings;
        mvpview::diagnostics::Options diagnosticOptions;
        diagnosticOptions.root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/diagnostics");
        diagnosticOptions.loggingEnabled = diagnosticSettings.value(QStringLiteral("diagnostics/loggingEnabled"), true).toBool();
        diagnosticOptions.crashEnabled = diagnosticSettings.value(QStringLiteral("diagnostics/crashReportingEnabled"), true).toBool();
        diagnosticOptions.level = mvpview::diagnostics::parseLevel(diagnosticSettings.value(QStringLiteral("diagnostics/logLevel"), QStringLiteral("Info")).toString());
        mvpview::diagnostics::Service diagnosticService(diagnosticOptions);
        mvpview::diagnostics::event(mvpview::diagnostics::Level::Info, mvpview::diagnostics::startup(), QStringLiteral("application.start"),
            mvpview::diagnostics::startupContext(), true);
        result = runApplication(argc, argv, diagnosticService);
        diagnosticService.markCleanExit();
    }
    // Active diagnostics and QML cache handles are closed at this point. Removing their parent
    // directories earlier would leave a partial reset on platforms that lock open files.
    const QString deletionError = mvpview::AppSettings::finishSoftwareCacheDeletion();
    if (!deletionError.isEmpty())
        qWarning().noquote() << deletionError;
    return result;
}
