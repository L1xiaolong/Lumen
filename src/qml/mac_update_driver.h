#pragma once

#include <QString>
#include <QUrl>

#include <functional>
#include <memory>

namespace mvpview {

class MacUpdateDriver final {
  public:
    MacUpdateDriver(std::function<void(int)> progress,
                    std::function<void(const QString&)> state,
                    std::function<void(const QString&)> error);
    ~MacUpdateDriver();

    [[nodiscard]] static bool isConfigured();
    void start(const QUrl& appcast, const QString& version);
    void cancel();
    void install();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mvpview
