#pragma once

#include <QObject>
#include <QPointer>

class QWindow;

namespace mvpview {

class FullScreenPresentationController final : public QObject {
    Q_OBJECT

public:
    ~FullScreenPresentationController() override;
    Q_INVOKABLE void prepare(QWindow* window);
    Q_INVOKABLE void setActive(bool active);

private:
    // The window prepared for presentation; used by the Windows path to adjust geometry
    // once the platform has applied the fullscreen window state.
    QPointer<QWindow> window_;
#ifdef Q_OS_MACOS
    bool active_ = false;
    unsigned long previousOptions_ = 0;
#endif
};

} // namespace mvpview
