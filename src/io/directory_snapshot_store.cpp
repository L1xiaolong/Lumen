#include "io/directory_snapshot_store.h"

#include "diagnostics/diagnostics.h"

#include <QCollator>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>

namespace mvpview {
namespace {

constexpr quint32 kMagic = 0x4D56534E; // "MVSN"
constexpr quint32 kVersion = 1;
constexpr int kMaximumSnapshots = 12;

QString defaultRootDirectory() {
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
        QStringLiteral("/directory-snapshots");
}

QByteArray hashForDirectory(const QString& directory) {
    // Case-folded so a snapshot recorded from ~/Pictures is reused when the same folder arrives
    // through a differently cased path on case-insensitive filesystems.
    const QString normalized = QDir::cleanPath(directory).toCaseFolded();
    return QCryptographicHash::hash(normalized.toUtf8(), QCryptographicHash::Sha256).toHex();
}

QCollator scannerCollator() {
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    return collator;
}

// Mirrors the sort keys the DirectoryScanner prepares so the proxy model sorts restored
// records with the cached collator keys instead of per-comparison string collation.
void prepareSortKeys(ImageFileRecord& record, QCollator& collator,
                     QHash<QString, QCollatorSortKey>& typeKeys) {
    record.nameSortKey = collator.sortKey(record.fileName);
    auto type = typeKeys.constFind(record.fileType);
    if (type == typeKeys.cend()) {
        typeKeys.insert(record.fileType, collator.sortKey(record.fileType));
        type = typeKeys.constFind(record.fileType);
    }
    record.fileType = type.key();
    record.typeSortKey = type.value();
}

} // namespace

DirectorySnapshotStore::DirectorySnapshotStore(QString rootDirectory)
    : rootDirectory_(rootDirectory.isEmpty() ? defaultRootDirectory()
                                             : QDir::cleanPath(rootDirectory)) {
    writePool_.setMaxThreadCount(1);
    writePool_.setExpiryTimeout(10'000);
}

std::optional<QVector<ImageFileRecord>> DirectorySnapshotStore::load(
    const QString& directory) const {
    QFile file(pathForDirectory(directory));
    if (!file.open(QIODevice::ReadOnly)) return std::nullopt;
    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_6_5);
    quint32 magic = 0;
    quint32 version = 0;
    stream >> magic >> version;
    if (magic != kMagic || version != kVersion || stream.status() != QDataStream::Ok) {
        return std::nullopt;
    }
    QString storedDirectory;
    qint64 savedAtMs = 0;
    qint32 count = 0;
    stream >> storedDirectory >> savedAtMs >> count;
    if (count < 0 || count > maximumEntries || stream.status() != QDataStream::Ok) {
        return std::nullopt;
    }
    const QDir base(directory);
    if (storedDirectory != QDir::cleanPath(directory)) return std::nullopt;
    QVector<ImageFileRecord> records;
    records.reserve(count);
    QCollator collator = scannerCollator();
    QHash<QString, QCollatorSortKey> typeKeys;
    for (qint32 index = 0; index < count; ++index) {
        QString fileName;
        qint64 fileSize = 0;
        qint64 modifiedMs = 0;
        bool isDirectory = false;
        QString fileType;
        stream >> fileName >> fileSize >> modifiedMs >> isDirectory >> fileType;
        if (stream.status() != QDataStream::Ok) return std::nullopt;
        // Names with separators can never come from a single-directory scan and would corrupt
        // the reconciled model, so reject the whole snapshot instead of trusting such entries.
        if (fileName.isEmpty() || fileName.contains(QLatin1Char('/')) ||
            fileName.contains(QLatin1Char('\\'))) {
            return std::nullopt;
        }
        ImageFileRecord record;
        record.path = base.absoluteFilePath(fileName);
        record.fileName = fileName;
        record.fileSize = fileSize;
        record.modifiedAt = QDateTime::fromMSecsSinceEpoch(modifiedMs);
        record.isDirectory = isDirectory;
        record.fileType = fileType;
        prepareSortKeys(record, collator, typeKeys);
        records.push_back(std::move(record));
    }
    // Guard against truncated files that still decode within the declared count.
    if (!stream.atEnd()) return std::nullopt;
    return records;
}

void DirectorySnapshotStore::saveAsync(const QString& directory,
                                       const QVector<ImageFileRecord>& files) const {
    if (directory.isEmpty() || files.size() > maximumEntries) return;
    const QString target = pathForDirectory(directory);
    const QString storedDirectory = QDir::cleanPath(directory);
    writePool_.start([this, target, storedDirectory, files] {
        QDir().mkpath(rootDirectory_);
        QSaveFile output(target);
        if (!output.open(QIODevice::WriteOnly)) {
            diagnostics::event(diagnostics::Level::Warning, diagnostics::files(),
                QStringLiteral("directory.snapshot_write_failed"), {{"reason", output.errorString()}});
        } else {
            QDataStream stream(&output);
            stream.setVersion(QDataStream::Qt_6_5);
            stream << kMagic << kVersion << storedDirectory
                   << QDateTime::currentMSecsSinceEpoch() << static_cast<qint32>(files.size());
            for (const ImageFileRecord& file : files) {
                stream << file.fileName << file.fileSize
                       << file.modifiedAt.toMSecsSinceEpoch() << file.isDirectory
                       << (file.isDirectory ? QStringLiteral("folder") : file.fileType);
            }
            if (!output.commit()) {
                diagnostics::event(diagnostics::Level::Warning, diagnostics::files(),
                    QStringLiteral("directory.snapshot_write_failed"),
                    {{"reason", output.errorString()}});
            }
        }
        trimToRecentCount();
    });
}

QString DirectorySnapshotStore::pathForDirectory(const QString& directory) const {
    return rootDirectory_ + QLatin1Char('/') + QString::fromLatin1(hashForDirectory(directory)) +
        QStringLiteral(".snap");
}

void DirectorySnapshotStore::trimToRecentCount() const {
    const QDir root(rootDirectory_);
    const auto snapshots = root.entryInfoList({QStringLiteral("*.snap")}, QDir::Files,
                                              QDir::Time);
    for (qsizetype index = kMaximumSnapshots; index < snapshots.size(); ++index) {
        QFile::remove(snapshots.at(index).absoluteFilePath());
    }
}

} // namespace mvpview
