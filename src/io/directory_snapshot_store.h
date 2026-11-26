#pragma once

#include "io/directory_scanner.h"

#include <QThreadPool>

#include <optional>

namespace mvpview {

// Persisted directory listings so the browser can present the previous folder instantly on the
// next launch, regardless of how many entries it contains, while a fresh scan reconciles the
// snapshot against the real directory (stale entries disappear, new entries appear).
//
// The store keeps a bounded set of most-recently-written snapshots. Each file stores only entry
// names plus size/mtime/type; paths are rebuilt from the directory when loading, and collator
// sort keys are recomputed so the restored records behave exactly like freshly scanned ones.
class DirectorySnapshotStore final {
  public:
    explicit DirectorySnapshotStore(QString rootDirectory = {});

    // Blocking read intended for worker threads. Returns nothing when no usable snapshot
    // exists for the directory (missing, corrupt, foreign directory, or superseded format).
    [[nodiscard]] std::optional<QVector<ImageFileRecord>> load(const QString& directory) const;

    // Serializes the listing on the store's own thread and atomically replaces any previous
    // snapshot for the directory. Listings beyond the entry cap are ignored entirely.
    void saveAsync(const QString& directory, const QVector<ImageFileRecord>& files) const;

    [[nodiscard]] QString rootDirectory() const { return rootDirectory_; }

    // Largest listing worth persisting; larger folders simply keep using the incremental scan.
    static constexpr qsizetype maximumEntries = 100'000;

  private:
    [[nodiscard]] QString pathForDirectory(const QString& directory) const;
    void trimToRecentCount() const;

    QString rootDirectory_;
    // One writer thread keeps concurrent snapshots for the same directory ordered.
    mutable QThreadPool writePool_;
};

} // namespace mvpview
