#pragma once

#include <QString>

class QFileInfo;

namespace mvpview {

class SingleFileRename final {
  public:
    // An omitted file extension is inherited from the source. An empty result carries an error.
    [[nodiscard]] static QString resolveName(const QFileInfo& source, const QString& requestedName,
                                             QString& error);

    // Renames one file within its current directory. A sibling .mvpview.json sidecar is moved
    // transactionally with the image so RAW/YUV interpretation does not become detached.
    [[nodiscard]] static bool execute(const QString& sourcePath, const QString& destinationPath,
                                      QString* error = nullptr);
};

} // namespace mvpview
