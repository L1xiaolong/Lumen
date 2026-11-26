#include "io/single_file_rename.h"

#include "io/raw_preset_store.h"
#include "io/image_transformer.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

namespace mvpview {
namespace {

QString portableFileNameError(const QString& fileName) {
    if (fileName.isEmpty() || fileName == QStringLiteral(".") ||
        fileName == QStringLiteral("..")) {
        return QCoreApplication::translate("FileOperations", "The filename is empty or reserved. Enter another name.");
    }
    if (fileName.size() > 240 || fileName.endsWith(QLatin1Char('.')) ||
        fileName.endsWith(QLatin1Char(' '))) {
        return QCoreApplication::translate("FileOperations", "The filename is too long or ends with a dot or space. Shorten it and remove trailing dots and spaces.");
    }
    static const QString invalidCharacters = QStringLiteral("<>:\"/\\|?*");
    for (const QChar character : fileName) {
        if (character.unicode() < 0x20 || invalidCharacters.contains(character)) {
            return QCoreApplication::translate("FileOperations", "The filename contains an invalid character. Remove reserved punctuation and control characters.");
        }
    }
    static const QRegularExpression reserved(
        QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\..*)?$"),
        QRegularExpression::CaseInsensitiveOption);
    return reserved.match(fileName).hasMatch()
               ? QCoreApplication::translate("FileOperations", "The filename is reserved on Windows. Use another name.")
               : QString{};
}

void setError(QString* destination, const QString& message) {
    if (destination) {
        *destination = message;
    }
}

} // namespace

QString SingleFileRename::resolveName(const QFileInfo& source, const QString& requestedName,
                                       QString& error) {
    QString name = requestedName.trimmed();
    if (name.isEmpty()) {
        error = QCoreApplication::translate("FileOperations", "Enter a name.");
        return {};
    }
    if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')) ||
        name == QStringLiteral(".") || name == QStringLiteral("..")) {
        error = QCoreApplication::translate("FileOperations", "The name contains unsupported characters. Use another name and try again.");
        return {};
    }

    const QString suffix = source.suffix();
    if (source.isFile() && !suffix.isEmpty() &&
        source.fileName().lastIndexOf(QLatin1Char('.')) > 0 &&
        name.lastIndexOf(QLatin1Char('.')) <= 0) {
        name += QLatin1Char('.') + suffix;
    }
    error.clear();
    return name;
}

bool SingleFileRename::execute(const QString& sourcePath, const QString& destinationPath,
                               QString* error) {
    const QFileInfo source(sourcePath);
    const QFileInfo destination(destinationPath);
    if (!source.exists() || (!source.isFile() && !source.isDir())) {
        setError(error, QCoreApplication::translate("FileOperations", "The source item no longer exists. Refresh the folder and select it again."));
        return false;
    }
    if (source.absolutePath() != destination.absolutePath()) {
        setError(error, QCoreApplication::translate("FileOperations", "Rename cannot move a file to another folder. Enter only a filename."));
        return false;
    }
    if (const QString nameError = portableFileNameError(destination.fileName());
        !nameError.isEmpty()) {
        setError(error, nameError);
        return false;
    }
    if (QFileInfo::exists(destinationPath)) {
        setError(error, QCoreApplication::translate("FileOperations", "An item with that name already exists. Use another name."));
        return false;
    }

    QVector<QPair<QString, QString>> auxiliaryFiles;
    if (source.isFile()) {
        auxiliaryFiles = {
            {RawPresetStore::sidecarPath(sourcePath), RawPresetStore::sidecarPath(destinationPath)},
            {ImageTransformer::backupPath(sourcePath),
             ImageTransformer::backupPath(destinationPath)},
            {ImageTransformer::backupManifestPath(sourcePath),
             ImageTransformer::backupManifestPath(destinationPath)},
            {RawPresetStore::sidecarPath(ImageTransformer::backupPath(sourcePath)),
             RawPresetStore::sidecarPath(ImageTransformer::backupPath(destinationPath))}};
    }
    for (const auto& [sourceAuxiliary, destinationAuxiliary] : auxiliaryFiles) {
        if (QFileInfo::exists(sourceAuxiliary) && QFileInfo::exists(destinationAuxiliary)) {
            setError(error, QCoreApplication::translate("FileOperations", "A parameter file or restore backup with that name already exists. Use another name."));
            return false;
        }
    }
    const bool renamed = source.isDir() ? QDir().rename(sourcePath, destinationPath)
                                        : QFile::rename(sourcePath, destinationPath);
    if (!renamed) {
        setError(error, QCoreApplication::translate("FileOperations", "Could not rename the item. Use another name and try again."));
        return false;
    }
    QVector<QPair<QString, QString>> moved;
    for (const auto& [sourceAuxiliary, destinationAuxiliary] : auxiliaryFiles) {
        if (!QFileInfo::exists(sourceAuxiliary)) continue;
        if (!QFile::rename(sourceAuxiliary, destinationAuxiliary)) {
            for (auto iterator = moved.crbegin(); iterator != moved.crend(); ++iterator)
                QFile::rename(iterator->second, iterator->first);
            if (!QFile::rename(destinationPath, sourcePath)) {
                setError(error, QCoreApplication::translate("FileOperations", "The sidecar rename failed and the image filename could not be restored"));
            } else {
                setError(error, QCoreApplication::translate("FileOperations", "The sidecar rename failed; the image filename was restored"));
            }
            return false;
        }
        moved.append({sourceAuxiliary, destinationAuxiliary});
    }
    if (error) {
        error->clear();
    }
    return true;
}

} // namespace mvpview
