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
#else
    Q_UNUSED(active);
#endif
}

} // namespace mvpview
