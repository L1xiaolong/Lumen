#pragma once

#include <QObject>

class QWindow;

namespace mvpview {

class FullScreenPresentationController final : public QObject {
    Q_OBJECT

public:
    ~FullScreenPresentationController() override;
    Q_INVOKABLE void prepare(QWindow* window);
    Q_INVOKABLE void setActive(bool active);

private:
#ifdef Q_OS_MACOS
    bool active_ = false;
    unsigned long previousOptions_ = 0;
#endif
};

} // namespace mvpview
