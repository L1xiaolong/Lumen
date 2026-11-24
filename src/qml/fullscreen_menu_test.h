#pragma once

#include <QString>
#include <QWindow>

class QQuickWindow;

namespace mvpview {

class AppSettings;

QString createFullScreenMenuFixture(const QString& directory);
int testFullScreenMenu(QQuickWindow& window, QWindow::Visibility initialVisibility,
                       const QString& fixturePath, AppSettings& settings);

} // namespace mvpview
