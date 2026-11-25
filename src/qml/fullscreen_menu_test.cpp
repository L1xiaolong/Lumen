#include "qml/fullscreen_menu_test.h"
#include "qml/app_settings.h"

#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfOutputFile.h>

#include <QCursor>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QNetworkProxy>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QScreen>
#include <QSemaphore>
#include <QtTest/QTest>

#include <array>
#include <atomic>
#include <exception>

#if defined(Q_OS_MACOS)
#include <CoreGraphics/CoreGraphics.h>
#elif defined(Q_OS_WIN)
#include <qt_windows.h>
#endif

namespace mvpview {
namespace {

template <typename Predicate>
QQuickItem* findVisibleItem(QQuickItem* parent, const Predicate& predicate) {
    if (!parent->isVisible()) return nullptr;
    if (predicate(parent)) return parent;
    for (QQuickItem* item : parent->childItems()) {
        if (auto* found = findVisibleItem(item, predicate)) return found;
    }
    return nullptr;
}

int failure(const char* message) {
    qCritical("Full-screen menu test: %s", message);
    return 1;
}

QPoint center(const QQuickItem& item) {
    return item.mapToScene(item.boundingRect().center()).toPoint();
}

enum class MouseAction { Move, Press, Release };

bool sendNativeMouse(QQuickWindow& window, const QPoint& position, MouseAction action,
                     Qt::MouseButton button = Qt::NoButton, int clickCount = 1) {
    if (action != MouseAction::Release && !window.isActive()) return false;
    const QPoint global = window.mapToGlobal(position);
#if defined(Q_OS_MACOS)
    const bool right = button == Qt::RightButton;
    const auto down = right ? kCGEventRightMouseDown : kCGEventLeftMouseDown;
    const auto up = right ? kCGEventRightMouseUp : kCGEventLeftMouseUp;
    const auto type = action == MouseAction::Move ? kCGEventMouseMoved
        : action == MouseAction::Press ? down : up;
    auto* event = CGEventCreateMouseEvent(nullptr, type,
        CGPoint{qreal(global.x()), qreal(global.y())}, right ? kCGMouseButtonRight : kCGMouseButtonLeft);
    if (!event) return false;
    CGEventSetIntegerValueField(event, kCGMouseEventClickState, clickCount);
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
    return true;
#elif defined(Q_OS_WIN)
    Q_UNUSED(clickCount);
    // Let Qt convert its desktop position to Win32 screen coordinates, including DPI scaling.
    if (action == MouseAction::Move) {
        QCursor::setPos(global);
        return QCursor::pos() == global;
    }
    if (action == MouseAction::Press) {
        POINT current{};
        if (!GetCursorPos(&current) || QCursor::pos() != global ||
            GetAncestor(WindowFromPoint(current), GA_ROOT) != reinterpret_cast<HWND>(window.winId()))
            return false;
    }
    INPUT input{};
    input.type = INPUT_MOUSE;
    const bool right = button == Qt::RightButton;
    const DWORD down = right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
    const DWORD up = right ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP;
    input.mi.dwFlags = action == MouseAction::Press ? down : up;
    return SendInput(1, &input, sizeof(input)) == 1;
#else
    Q_UNUSED(global);
    Q_UNUSED(button);
    Q_UNUSED(clickCount);
    return false;
#endif
}

bool moveNativeMouse(QQuickWindow& window, const QPoint& position) {
    if (!sendNativeMouse(window, position, MouseAction::Move)) return false;
    return QTest::qWaitFor([&] { return QCursor::pos() == window.mapToGlobal(position); }, 1000);
}

bool openNativeThumbnail(QQuickWindow& window, const QPoint& position) {
    // A delegate can exist before the desktop has presented its first frame.
    bool framePresented = false;
    QObject frameObserver;
    QObject::connect(&window, &QQuickWindow::frameSwapped, &frameObserver,
                     [&] { framePresented = true; }, Qt::QueuedConnection);
    window.update();
    if (!QTest::qWaitFor([&] { return framePresented; }, 1000)) return false;
    if (!moveNativeMouse(window, position)) return false;
    for (int clickCount = 1; clickCount <= 2; ++clickCount) {
        if (!sendNativeMouse(window, position, MouseAction::Press, Qt::LeftButton, clickCount)) return false;
        QTest::qWait(20);
        if (!sendNativeMouse(window, position, MouseAction::Release, Qt::LeftButton, clickCount)) return false;
        QTest::qWait(20);
    }
    return true;
}

class RightPressObserver final : public QObject {
  public:
    bool seen = false;

  protected:
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::MouseButtonPress &&
            static_cast<QMouseEvent*>(event)->button() == Qt::RightButton)
            seen = true;
        return false;
    }
};

class DelayedUpdateProxy final : public QNetworkProxyFactory {
  public:
    QList<QNetworkProxy> queryProxy(const QNetworkProxyQuery&) override {
        const bool onGuiThread = QThread::isMainThread();
        queriedOnGuiThread = onGuiThread;
        queryStarted.release();
        if (!onGuiThread)
            finishQuery.tryAcquire(1, 5000);
        // An HTTPS update request cannot use this proxy, so no server is contacted.
        return {QNetworkProxy(QNetworkProxy::FtpCachingProxy)};
    }

    QSemaphore queryStarted;
    QSemaphore finishQuery;
    std::atomic_bool queriedOnGuiThread{false};
};

} // namespace

QString createFullScreenMenuFixture(const QString& directory) {
    namespace Exr = OPENEXR_IMF_NAMESPACE;
    const QString photos = QDir(directory).filePath(QStringLiteral("photos"));
    if (!QDir().mkpath(photos)) return {};
    const auto path = QDir(photos).filePath(QStringLiteral("test.exr"));
    constexpr int width = 4032;
    constexpr int height = 3024;
    try {
        Exr::Header header(width, height);
        Exr::FrameBuffer buffer;
        std::array<float, width * 3> row{};
        for (std::size_t x = 0; x < width; ++x) {
            row[3 * x] = float(x) / width;
            row[3 * x + 1] = 0.5f;
            row[3 * x + 2] = 0.25f;
        }
        constexpr std::array names{"R", "G", "B"};
        for (std::size_t channel = 0; channel < names.size(); ++channel) {
            header.channels().insert(names[channel], Exr::Channel(Exr::FLOAT));
            // A zero row stride reuses the same scanline for each row of the large image.
            buffer.insert(names[channel], Exr::Slice(Exr::FLOAT,
                reinterpret_cast<char*>(row.data() + channel), 3 * sizeof(float), 0));
        }
        Exr::OutputFile file(QFile::encodeName(path).constData(), header);
        file.setFrameBuffer(buffer);
        file.writePixels(height);
    } catch (const std::exception& error) {
        qCritical("Could not create full-screen menu fixture: %s", error.what());
        return {};
    }
    return path;
}

int testFullScreenMenu(QQuickWindow& window, QWindow::Visibility initialVisibility,
                       const QString& fixturePath, AppSettings& settings) {
    const QByteArray inputMode = qgetenv("LUMEN_FULLSCREEN_TEST_NATIVE_INPUT");
    const bool nativeThumbnail = inputMode == "1";
    const bool nativeMouse = nativeThumbnail || inputMode == "viewer";
#ifdef Q_OS_MACOS
    if (nativeMouse && !CGPreflightPostEventAccess())
        return failure("macOS did not grant this test process permission to post mouse events");
#endif
    window.raise();
    window.requestActivate();
    if (!QTest::qWaitFor([&] {
            return window.isExposed() && window.visibility() == initialVisibility &&
                   (!nativeThumbnail || window.isActive());
        }, 5000)) {
        qCritical() << "Expected window state" << initialVisibility << "got" << window.visibility()
                    << "active" << window.isActive() << "exposed" << window.isExposed();
        return failure("initial window state was not reached");
    }
    const QRect initialGeometry = window.geometry();
    const QPoint originalCursor = QCursor::pos();
    const auto restoreCursor = qScopeGuard([originalCursor] { QCursor::setPos(originalCursor); });

    // Enter as soon as the delegate exists, while its thumbnail may still be decoding.
    QQuickItem* tile = nullptr;
    if (!QTest::qWaitFor([&] {
            tile = findVisibleItem(window.contentItem(), [&](const QQuickItem* item) {
                if (item->objectName() != QStringLiteral("thumbnailMouseArea") || !item->parentItem())
                    return false;
                const QString tilePath = item->parentItem()->property("path").toString();
                return QFileInfo(tilePath) == QFileInfo(fixturePath);
            });
            return tile;
        }, 5000)) return failure("EXR thumbnail delegate did not appear");
    const QPoint tileCenter = center(*tile);
    QElapsedTimer openTime;
    openTime.start();
    if (nativeThumbnail) {
        if (!openNativeThumbnail(window, tileCenter)) return failure("could not double-click the EXR thumbnail");
    } else {
        QTest::mouseClick(&window, Qt::LeftButton, Qt::NoModifier, tileCenter);
        QTest::mouseDClick(&window, Qt::LeftButton, Qt::NoModifier, tileCenter);
        QTest::mouseRelease(&window, Qt::LeftButton, Qt::NoModifier, tileCenter);
    }
    if (!QTest::qWaitFor([&] { return window.property("showingFullScreen").toBool(); }, 3000))
        return failure("double-click did not open the EXR viewer");
    auto* presentationWindow = window.property("presentationWindow").value<QQuickWindow*>();
    if (!presentationWindow || presentationWindow == &window || !presentationWindow->isVisible())
        return failure("the image viewer did not open in its own Qt window");
    auto& viewer = *presentationWindow;
    if (!QTest::qWaitFor([&] { return viewer.isExposed(); }, 3000))
        return failure("the image viewer was not exposed");
    qInfo() << "Full-screen viewer exposed after" << openTime.elapsed() << "ms";
    // The browser is hidden to stop background rendering. A hidden window's native
    // geometry can change until it is shown again, so check placement after exit.
    // The Windows viewer can overhang the monitor by one pixel to stay composited.
    const auto viewerCoversScreen = [&viewer] { return viewer.geometry().contains(viewer.screen()->geometry()); };
    if (window.isVisible() || !viewerCoversScreen()) {
        qCritical() << "Browser" << initialVisibility << initialGeometry << "now" << window.visibility()
                    << window.geometry() << "viewer" << viewer.geometry() << "screen" << viewer.screen()->geometry();
        return failure("the browser remained visible or the viewer did not cover its screen");
    }
    if (!QTest::qWaitFor([&] { return !window.property("fullScreenTransitioning").toBool(); }, 12000))
        return failure("full-screen EXR did not render");
    qInfo() << "Full-screen EXR first frame after" << openTime.elapsed() << "ms";
    if (window.isVisible() || !viewerCoversScreen())
        return failure("the browser became visible or the viewer stopped covering its screen while rendering the EXR");

    auto* page = findVisibleItem(viewer.contentItem(), [](const QQuickItem* item) {
        return item->objectName() == QStringLiteral("fullScreenPage");
    });
    auto* menu = page ? page->findChild<QObject*>(QStringLiteral("fullScreenContextMenu")) : nullptr;
    if (!menu) return failure("no full-screen context menu");
    const QPoint where(viewer.width() / 2, viewer.height() / 3);
    RightPressObserver press;
    viewer.installEventFilter(&press);
    if (nativeMouse) {
        if (!QTest::qWaitFor([&] {
                return QGuiApplication::applicationState() == Qt::ApplicationActive &&
                       QGuiApplication::focusWindow() == &viewer;
            }, 3000)) return failure("the viewer did not receive desktop focus");
        if (!moveNativeMouse(viewer, where) || !sendNativeMouse(viewer, where, MouseAction::Press, Qt::RightButton))
            return failure("could not send the native right press to the viewer");
    } else {
        QTest::mousePress(&viewer, Qt::RightButton, Qt::NoModifier, where);
    }
    const bool receivedPress = QTest::qWaitFor([&] { return press.seen; }, 1000);
    const bool interruptedPress = menu->property("visible").toBool() || viewer.mouseGrabberItem();
    if (nativeMouse) {
        if (!sendNativeMouse(viewer, where, MouseAction::Release, Qt::RightButton))
            return failure("could not send the native right release");
    } else {
        QTest::mouseRelease(&viewer, Qt::RightButton, Qt::NoModifier, where);
    }
    if (!receivedPress) return failure("the viewer did not receive the right press");
    if (interruptedPress) return failure("the right press opened the menu or grabbed the mouse");
    if (!QTest::qWaitFor([&] { return menu->property("opened").toBool(); }, 3000))
        return failure("right-click did not open the menu");

    auto* proxy = new DelayedUpdateProxy;
    QNetworkProxyFactory::setApplicationProxyFactory(proxy);
    auto releaseProxy = qScopeGuard([proxy] { proxy->finishQuery.release(); });
    settings.checkForUpdates();
    if (!QTest::qWaitFor([&] { return proxy->queryStarted.available() > 0; }, 3000))
        return failure("the update check did not query the proxy");
    if (proxy->queriedOnGuiThread)
        return failure("the update check queried the proxy on the UI thread");

    for (int index = 0; index < 3; ++index) {
        QQuickItem* row = nullptr;
        if (!QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem*, row), Q_ARG(int, index)) ||
            !row || !row->isVisible() || row->window() != &viewer)
            return failure("menu row was not displayed in the viewer window");
        const QPoint rowCenter = center(*row);
        if (nativeMouse) {
            if (!sendNativeMouse(viewer, rowCenter, MouseAction::Move))
                return failure("could not move the desktop cursor to the menu row");
        } else {
            // Align the desktop cursor in case a native event follows the injected one.
            QCursor::setPos(viewer.mapToGlobal(rowCenter));
            QTest::mouseMove(&viewer, rowCenter);
        }
        if (!QTest::qWaitFor([&] { return row->property("highlighted").toBool(); }, 1000)) {
            qCritical() << "Menu row" << index << "position" << rowCenter << "cursor" << QCursor::pos()
                        << "hovered" << row->property("hovered") << "active" << viewer.isActive();
            return failure("menu row did not highlight on hover");
        }
    }
    if (settings.updateState() != QStringLiteral("checking"))
        return failure("the update request finished before the hover test");
    releaseProxy.dismiss();
    proxy->finishQuery.release();
    if (!QTest::qWaitFor([&] { return settings.updateState() == QStringLiteral("error"); }, 3000))
        return failure("the update result did not return to the UI thread");

    if (nativeMouse) {
        QTest::keyClick(&viewer, Qt::Key_Escape);
        if (!QTest::qWaitFor([&] { return !menu->property("visible").toBool(); }, 1000) ||
            !window.property("showingFullScreen").toBool())
            return failure("Escape did not dismiss the menu while leaving the viewer open");
        QTest::keyClick(&viewer, Qt::Key_Escape);
    } else {
        // Exercise the same exit path on runners that grant exposure without desktop focus.
        if (!QMetaObject::invokeMethod(&window, "closeFullScreen"))
            return failure("could not request full-screen exit");
    }
    if (!QTest::qWaitFor([&] {
            return window.visibility() == initialVisibility && window.geometry() == initialGeometry && !viewer.isVisible() &&
                   !window.property("showingFullScreen").toBool() &&
                   !window.property("fullScreenTransitioning").toBool() &&
                   !menu->property("visible").toBool();
        }, 5000)) {
        qCritical() << "Expected browser" << initialVisibility << initialGeometry << "got" << window.visibility()
                    << window.geometry() << "viewer visible" << viewer.isVisible()
                    << "menu visible" << menu->property("visible").toBool();
        return failure("the browser and its window placement were not restored");
    }
    return 0;
}

} // namespace mvpview
