#include "platform/full_screen_presentation_controller.h"

#include <QScreen>
#include <QWindow>

#ifdef Q_OS_MACOS
#include <AppKit/AppKit.h>
#include <QGuiApplication>
#endif

namespace mvpview {

FullScreenPresentationController::~FullScreenPresentationController() { setActive(false); }

void FullScreenPresentationController::prepare(QWindow* window) {
    window_ = window;
    const QRect bounds = window->screen()->geometry();
    window->setGeometry(bounds);
    // Cocoa can constrain the initial native window rectangle when Qt creates the surface.
    // Reapply the exact screen bounds through Qt before showing the window.
    window->create();
    window->setGeometry(bounds);
}

void FullScreenPresentationController::setActive(bool active) {
#ifdef Q_OS_MACOS
    if (active_ == active || QGuiApplication::platformName() != QStringLiteral("cocoa")) return;
    active_ = active;
    if (!active_) {
        NSApp.presentationOptions = previousOptions_;
        return;
    }

    // Qt owns the viewer window and its input. The app-level presentation options only hide
    // the system bars while the borderless viewer is open in the current Space.
    previousOptions_ = NSApp.presentationOptions;
    NSApp.presentationOptions =
        (previousOptions_ & ~(NSApplicationPresentationAutoHideDock |
                              NSApplicationPresentationAutoHideMenuBar)) |
        NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar;
#elif defined(Q_OS_WIN)
    // A borderless window that exactly matches the monitor rectangle makes the DWM engage
    // its fullscreen optimization (independent flip). In that mode this window's OpenGL
    // presents stop reaching the composition, and the viewer froze on its first frame
    // until an occlusion or resize broke the optimization. Qt applies the monitor
    // rectangle while entering the fullscreen window state, which still covers the
    // taskbar; overhang the monitor by a pixel on each side right after that so the
    // window stays in ordinary composited presentation. The screen clips the extra
    // pixels away, so the visible result is identical.
    if (active && !window_.isNull()) {
        const QRect bounds = window_->screen()->geometry();
        window_->setGeometry(bounds.adjusted(-1, -1, 1, 1));
    }
#else
    Q_UNUSED(active);
#endif
}

} // namespace mvpview
