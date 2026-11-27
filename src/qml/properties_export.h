#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

namespace mvpview {

// Plain-text rendering for the properties inspector's Export action. The layout
// mirrors the inspector tabs: property groups stay nested under their
// Overview / Advanced / Histogram / Warnings sections, and field values are
// aligned per group. Kept free of view and loader state so the format can be
// unit-tested.
namespace PropertiesExport {

// Builds the complete UTF-8 text document for one image. `histogram` is the
// controller's native-channel histogram map (source 1); its per-channel
// statistics are rendered as an aligned table when valid. `exportedAt` is only
// used for the "Exported" header line; the caller controls it for tests.
[[nodiscard]] QString documentText(const QString& fileName, const QString& filePath,
                                   const QVariantList& overviewGroups,
                                   const QVariantList& advancedGroups,
                                   const QStringList& warnings,
                                   const QVariantMap& histogram = {},
                                   const QDateTime& exportedAt = QDateTime::currentDateTime());

// Default export file name for an image: "<base>_properties_<yyyyMMddhhmm>",
// e.g. "IMG_4012_properties_202611262248" for a 2026-11-26 22:48 export.
[[nodiscard]] QString suggestedFileName(const QString& sourcePath,
                                        const QDateTime& timestamp = QDateTime::currentDateTime());

} // namespace PropertiesExport
} // namespace mvpview
