#pragma once

#include <QImage>
#include <QSize>
#include <QSet>
#include <QString>

#include <functional>

namespace mvpview {

inline constexpr int panoramaTileSize = 512;
inline constexpr int panoramaTileBorder = 2;
inline constexpr int panoramaTileExtent = panoramaTileSize + 2 * panoramaTileBorder;
inline constexpr int panoramaMaximumWidth = 32768;

struct PanoramaInfo {
    QSize size;
    bool canPreview = false;
    bool autoPreview = false;
    bool multipleScans = false;
    qsizetype coefficientBytes = 0;
    double heading = 0.0;
    double pitch = 0.0;
    double roll = 0.0;
    double initialHeading = 0.0;
    double initialPitch = 0.0;
    double initialRoll = 0.0;
    double initialHorizontalFov = 0.0;
    double initialVerticalFov = 0.0;
    QByteArray embeddedThumbnail;
};

struct PanoramaTile {
    int level = 0;
    int column = 0;
    int row = 0;

    friend bool operator==(const PanoramaTile&, const PanoramaTile&) = default;
    friend size_t qHash(const PanoramaTile& tile, size_t seed = 0) noexcept {
        return qHashMulti(seed, tile.level, tile.column, tile.row);
    }
};

using PanoramaCancelled = std::function<bool()>;

[[nodiscard]] bool isPanoramaJpeg(const QString& path);
[[nodiscard]] bool hasPanoramaDimensions(QSize size);
[[nodiscard]] PanoramaInfo probePanorama(const QString& path);
[[nodiscard]] QList<QSize> panoramaLevels(QSize source);
[[nodiscard]] QString panoramaCacheDirectory(const QString& path);
[[nodiscard]] QImage readPanoramaPreview(const QString& directory);
[[nodiscard]] QImage decodePanoramaPreview(const QString& path, const PanoramaInfo& info,
                                            const PanoramaCancelled& cancelled);
void savePanoramaPreview(const QString& directory, const QImage& preview);
[[nodiscard]] bool panoramaCacheComplete(const QString& directory, QSize source, const QSet<int>& rows);
[[nodiscard]] QSet<int> availablePanoramaRows(const QString& directory);
[[nodiscard]] int panoramaRowKey(int level, int row);
void invalidatePanoramaCache(const QString& directory);
[[nodiscard]] QImage readPanoramaTile(const QString& directory, PanoramaTile tile);
[[nodiscard]] bool buildPanoramaTiles(const QString& path, const PanoramaInfo& info,
                                      const QString& directory, const PanoramaCancelled& cancelled,
                                      const std::function<void(int level, int row)>& rowAvailable);

} // namespace mvpview
