#include "io/panorama_source.h"

#include "io/metadata_exiv2_lock.h"

#include <QBuffer>
#include <QColorSpace>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QLockFile>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QXmlStreamReader>
#include <QtEndian>

#include <exiv2/exiv2.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <setjmp.h>
#include <vector>

#include <jpeglib.h>

#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include <miniz.h>

namespace mvpview {
namespace {

constexpr qint64 MiB = 1024 * 1024;
constexpr qint64 maximumMetadataBytes = 4 * MiB;
constexpr qint64 maximumCoefficientBytes = 384 * MiB;
constexpr qint64 diskBudget = 6 * 1024 * MiB;
constexpr int previewWidth = 4096;
constexpr int tileBytes = panoramaTileExtent * panoramaTileExtent * 4;
constexpr quint32 rowMagic = 0x4c505231;
constexpr auto xmpNamespace = "http://ns.google.com/photos/1.0/panorama/";

struct JpegError {
    jpeg_error_mgr manager{};
    jmp_buf recovery;
    static void fail(j_common_ptr jpeg) {
        auto* error = reinterpret_cast<JpegError*>(jpeg->err);
        longjmp(error->recovery, 1);
    }
};

struct JpegInput {
    jpeg_source_mgr manager{};
    QFile file;
    std::array<JOCTET, 65536> buffer{};
    bool truncated = false;

    static JpegInput& input(j_decompress_ptr jpeg) {
        return *static_cast<JpegInput*>(jpeg->client_data);
    }
    static boolean refill(j_decompress_ptr jpeg) {
        auto& source = input(jpeg);
        const auto count = source.file.read(reinterpret_cast<char*>(source.buffer.data()),
                                            qsizetype(source.buffer.size()));
        if (count <= 0) {
            source.truncated = true;
            source.buffer[0] = 0xff;
            source.buffer[1] = JPEG_EOI;
        }
        jpeg->src->next_input_byte = source.buffer.data();
        jpeg->src->bytes_in_buffer = count > 0 ? size_t(count) : 2;
        return TRUE;
    }
    static void skip(j_decompress_ptr jpeg, long count) {
        if (count <= 0) return;
        auto& source = input(jpeg);
        const auto skipBytes = size_t(count);
        if (skipBytes > jpeg->src->bytes_in_buffer) {
            const auto offset = qint64(skipBytes - jpeg->src->bytes_in_buffer);
            if (!source.file.seek(source.file.pos() + offset)) source.truncated = true;
            jpeg->src->bytes_in_buffer = 0;
        } else {
            jpeg->src->bytes_in_buffer -= skipBytes;
            jpeg->src->next_input_byte += skipBytes;
        }
    }
};

class JpegReader {
public:
    ~JpegReader() {
        if (jpeg_.mem) jpeg_destroy_decompress(&jpeg_);
    }

    bool open(const QString& path) {
        input_.file.setFileName(path);
        if (!input_.file.open(QIODevice::ReadOnly)) return false;
        jpeg_.err = jpeg_std_error(&error_.manager);
        error_.manager.error_exit = JpegError::fail;
        error_.manager.output_message = [](j_common_ptr) {};
        if (setjmp(error_.recovery)) return false;
        jpeg_create_decompress(&jpeg_);
        input_.manager.init_source = [](j_decompress_ptr) {};
        input_.manager.fill_input_buffer = JpegInput::refill;
        input_.manager.skip_input_data = JpegInput::skip;
        input_.manager.resync_to_restart = jpeg_resync_to_restart;
        input_.manager.term_source = [](j_decompress_ptr) {};
        jpeg_.client_data = &input_;
        jpeg_.src = &input_.manager;
        return jpeg_read_header(&jpeg_, TRUE) == JPEG_HEADER_OK && !input_.truncated;
    }

    QSize size() const { return {int(jpeg_.image_width), int(jpeg_.image_height)}; }
    bool rgbSupported() const {
        return jpeg_.data_precision == 8 &&
            (jpeg_.jpeg_color_space == JCS_GRAYSCALE || jpeg_.jpeg_color_space == JCS_YCbCr ||
             jpeg_.jpeg_color_space == JCS_RGB);
    }
    bool usesMultipleScans() {
        if (setjmp(error_.recovery)) return true;
        return jpeg_has_multiple_scans(&jpeg_);
    }
    qsizetype coefficientBytes() const {
        if (!jpeg_.max_h_samp_factor || !jpeg_.max_v_samp_factor) return 0;
        const qint64 mcuWidth = jpeg_.max_h_samp_factor * DCTSIZE;
        const qint64 mcuHeight = jpeg_.max_v_samp_factor * DCTSIZE;
        const qint64 mcus = ((jpeg_.image_width + mcuWidth - 1) / mcuWidth) *
                            ((jpeg_.image_height + mcuHeight - 1) / mcuHeight);
        int blocks = 0;
        for (int i = 0; i < jpeg_.num_components; ++i)
            blocks += jpeg_.comp_info[i].h_samp_factor * jpeg_.comp_info[i].v_samp_factor;
        return qsizetype(mcus * blocks * qint64(sizeof(JBLOCK)));
    }
    bool start(int denominator) {
        if (setjmp(error_.recovery)) return false;
        jpeg_.scale_num = 1;
        jpeg_.scale_denom = unsigned(denominator);
        jpeg_.out_color_space = JCS_EXT_RGBA;
        return jpeg_start_decompress(&jpeg_) && !input_.truncated;
    }
    QSize outputSize() const { return {int(jpeg_.output_width), int(jpeg_.output_height)}; }
    bool read(uchar* bytes) {
        if (setjmp(error_.recovery)) return false;
        JSAMPROW row = bytes;
        return jpeg_read_scanlines(&jpeg_, &row, 1) == 1 && !input_.truncated;
    }
    bool finish() {
        if (setjmp(error_.recovery)) return false;
        return jpeg_finish_decompress(&jpeg_) && !input_.truncated;
    }
private:
    JpegError error_;
    JpegInput input_;
    jpeg_decompress_struct jpeg_{};
};

QHash<QString, QString> panoramaTags(const QByteArray& packet) {
    QXmlStreamReader xml(packet);
    QHash<QString, QString> tags;
    while (!xml.atEnd()) {
        if (xml.readNext() != QXmlStreamReader::StartElement) continue;
        for (const auto& attribute : xml.attributes()) {
            if (attribute.namespaceUri() == QLatin1StringView(xmpNamespace))
                tags.insert(attribute.name().toString(), attribute.value().toString().trimmed());
        }
        if (xml.namespaceUri() == QLatin1StringView(xmpNamespace)) {
            const QString name = xml.name().toString();
            tags.insert(name, xml.readElementText().trimmed());
        }
    }
    return xml.hasError() ? QHash<QString, QString>{} : tags;
}

struct HeaderMetadata {
    QHash<QString, QString> tags;
    QByteArray thumbnail;
    QByteArray profile;
    int orientation = 1;
};

HeaderMetadata readHeaderMetadata(const QString& path, bool readProfile = false) {
    HeaderMetadata result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.read(2) != QByteArray::fromHex("ffd8")) return result;
    static const QByteArray xmpPrefix("http://ns.adobe.com/xap/1.0/\0", 29);
    static const QByteArray exifPrefix("Exif\0\0", 6);
    static const QByteArray iccPrefix("ICC_PROFILE\0", 12);
    std::array<QByteArray, 255> profileParts;
    int profilePartCount = 0;
    bool badProfile = false;
    while (file.pos() < maximumMetadataBytes) {
        char marker = 0;
        if (!file.getChar(&marker) || uchar(marker) != 0xff) break;
        do {
            if (!file.getChar(&marker)) return result;
        } while (uchar(marker) == 0xff);
        if (uchar(marker) == 0xda || uchar(marker) == 0xd9) break;
        if (uchar(marker) == 0x01 || (uchar(marker) >= 0xd0 && uchar(marker) <= 0xd7)) continue;
        std::array<uchar, 2> length{};
        if (file.read(reinterpret_cast<char*>(length.data()), 2) != 2) break;
        const int size = int(qFromBigEndian<quint16>(length.data())) - 2;
        if (size < 0 || file.pos() + size > file.size() || file.pos() + size > maximumMetadataBytes) break;
        if (uchar(marker) != (readProfile ? 0xe2 : 0xe1)) {
            if (!file.seek(file.pos() + size)) break;
            continue;
        }
        const auto data = file.read(size);
        if (readProfile) {
            if (!data.startsWith(iccPrefix) || size <= iccPrefix.size() + 2) continue;
            const int index = uchar(data[iccPrefix.size()]);
            const int count = uchar(data[iccPrefix.size() + 1]);
            if (index < 1 || index > count || (profilePartCount && profilePartCount != count) ||
                !profileParts[size_t(index - 1)].isEmpty()) {
                badProfile = true;
                continue;
            }
            profileParts[size_t(index - 1)] = data.sliced(iccPrefix.size() + 2);
            profilePartCount = count;
            continue;
        }
        if (data.startsWith(xmpPrefix)) {
            const auto tags = panoramaTags(data.sliced(xmpPrefix.size()));
            for (auto it = tags.cbegin(); it != tags.cend(); ++it) result.tags.insert(it.key(), it.value());
        } else if (data.startsWith(exifPrefix)) {
            try {
                const std::scoped_lock lock(exiv2MetadataMutex());
                Exiv2::ExifData exif;
                const int offset = int(exifPrefix.size());
                Exiv2::ExifParser::decode(exif, reinterpret_cast<const Exiv2::byte*>(data.constData() + offset),
                                           size_t(data.size() - offset));
                const auto orientation = exif.findKey(Exiv2::ExifKey("Exif.Image.Orientation"));
                if (orientation != exif.end() && orientation->count() > 0)
                    result.orientation = int(orientation->toInt64());
                const auto thumb = Exiv2::ExifThumbC(exif).copy();
                if (thumb.size() && thumb.size() < 65536)
                    result.thumbnail = QByteArray(reinterpret_cast<const char*>(thumb.c_data()), qsizetype(thumb.size()));
            } catch (const Exiv2::Error&) {
            }
        }
    }
    if (readProfile && !badProfile && std::all_of(profileParts.begin(),
        profileParts.begin() + profilePartCount, [](const QByteArray& part) { return !part.isEmpty(); })) {
        for (int i = 0; i < profilePartCount; ++i) result.profile.append(profileParts[size_t(i)]);
    }
    return result;
}

bool matchesDimension(const QHash<QString, QString>& tags, const QString& key, int expected) {
    const auto found = tags.constFind(key);
    if (found == tags.cend()) return true;
    bool valid = false;
    const int value = found->toInt(&valid);
    return valid && value == expected;
}

double taggedAngle(const QHash<QString, QString>& tags, const QString& key, double fallback) {
    bool parsed = false;
    const double value = tags.value(key).toDouble(&parsed);
    return parsed && std::isfinite(value) ? value : fallback;
}

QString rowPath(const QString& directory, PanoramaTile tile) {
    return directory + QStringLiteral("/%1-%2.tiles").arg(tile.level).arg(tile.row);
}

bool writeTileRow(const QString& directory, int level, int row, int width,
                  const std::vector<QByteArray>& scanlines, const PanoramaCancelled& cancelled) {
    const int columns = (width + panoramaTileSize - 1) / panoramaTileSize;
    QSaveFile file(rowPath(directory, {level, 0, row}));
    if (!file.open(QIODevice::WriteOnly)) return false;
    QDataStream stream(&file);
    stream << rowMagic << quint32(columns);
    for (int column = 0; column < columns; ++column) stream << quint64(0) << quint32(0);
    std::vector<std::pair<quint64, quint32>> entries;
    QByteArray bytes(tileBytes, Qt::Uninitialized);
    QByteArray compressed(tileBytes, Qt::Uninitialized);
    for (int column = 0; column < columns; ++column) {
        if (cancelled()) return false;
        const int left = ((column * panoramaTileSize - panoramaTileBorder) % width + width) % width;
        for (int line = 0; line < panoramaTileExtent; ++line) {
            char* dest = bytes.data() + line * panoramaTileExtent * 4;
            const QByteArray& source = scanlines[size_t(line)];
            int copied = 0;
            int start = left;
            while (copied < panoramaTileExtent) {
                const int span = std::min(width - start, panoramaTileExtent - copied);
                std::memcpy(dest + copied * 4, source.constData() + start * 4, size_t(span * 4));
                copied += span;
                start = 0;
            }
        }
        mz_ulong length = tileBytes;
        const int result = mz_compress2(reinterpret_cast<uchar*>(compressed.data()), &length,
                                        reinterpret_cast<const uchar*>(bytes.constData()), tileBytes, 1);
        const bool useCompressed = result == MZ_OK && length < tileBytes;
        const auto payloadLength = useCompressed ? qint64(length) : tileBytes;
        entries.emplace_back(quint64(file.pos()), quint32(payloadLength));
        if (file.write(useCompressed ? compressed.constData() : bytes.constData(), payloadLength) != payloadLength)
            return false;
    }
    if (!file.seek(8)) return false;
    for (const auto& [offset, length] : entries) stream << offset << length;
    return stream.status() == QDataStream::Ok && file.commit();
}

class TileLevel {
public:
    TileLevel(QSize size, int level)
        : size_(size), level_(level) {
        lines_.reserve(panoramaTileExtent);
    }

    bool append(const QByteArray& row, const QString& directory, const PanoramaCancelled& cancelled,
                const std::function<void(int, int)>& rowAvailable) {
        if (sourceRow_ == 0) {
            for (int border = 0; border < panoramaTileBorder; ++border) lines_.push_back(row);
        }
        lines_.push_back(row);
        ++sourceRow_;
        const bool last = sourceRow_ == size_.height();
        while (lines_.size() >= panoramaTileExtent || (last && tileRow_ * panoramaTileSize < size_.height())) {
            while (lines_.size() < panoramaTileExtent) lines_.push_back(row);
            if (!writeTileRow(directory, level_, tileRow_++, size_.width(), lines_, cancelled))
                return false;
            rowAvailable(level_, tileRow_ - 1);
            lines_.erase(lines_.begin(), lines_.begin() + panoramaTileSize);
        }
        return true;
    }

private:
    QSize size_;
    int level_;
    int sourceRow_ = 0;
    int tileRow_ = 0;
    std::vector<QByteArray> lines_;
};

QByteArray downsampleRow(const QByteArray& top, const QByteArray& bottom, int width) {
    QByteArray result(((width + 1) / 2) * 4, Qt::Uninitialized);
    const auto* a = reinterpret_cast<const uchar*>(top.constData());
    const auto* b = reinterpret_cast<const uchar*>(bottom.constData());
    auto* output = reinterpret_cast<uchar*>(result.data());
    for (int x = 0; x < width; x += 2) {
        const int next = (x + 1) % width;
        for (int c = 0; c < 3; ++c)
            output[(x / 2) * 4 + c] = uchar((int(a[x * 4 + c]) + a[next * 4 + c] +
                                              b[x * 4 + c] + b[next * 4 + c] + 2) / 4);
        output[(x / 2) * 4 + 3] = 255;
    }
    return result;
}

QString profilePath(const QString& directory) { return directory + QStringLiteral("/profile.icc"); }

void trimCache(const QString& directory, qint64 reservation) {
    QDir root(QFileInfo(directory).absolutePath());
    qint64 total = reservation;
    for (const auto& entry : root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time)) {
        if (entry.absoluteFilePath() == directory || entry.fileName().size() != 64) continue;
        QLockFile lock(entry.absoluteFilePath() + QStringLiteral(".lock"));
        qint64 size = 0;
        const QDir folder(entry.absoluteFilePath());
        for (const auto& part : folder.entryInfoList(QDir::Files)) size += part.size();
        if (total + size <= diskBudget || !lock.tryLock(0)) total += size;
        else QDir(entry.absoluteFilePath()).removeRecursively();
    }
}

} // namespace

bool isPanoramaJpeg(const QString& path) {
    const auto suffix = QFileInfo(path).suffix().toLower();
    return suffix == QStringLiteral("jpg") || suffix == QStringLiteral("jpeg") ||
           suffix == QStringLiteral("jpe") || suffix == QStringLiteral("jfif");
}

bool hasPanoramaDimensions(QSize size) {
    return !size.isEmpty() && size.width() <= panoramaMaximumWidth &&
        std::abs(qint64(size.width()) - 2 * qint64(size.height())) <= std::max(1, size.width() / 100);
}

PanoramaInfo probePanorama(const QString& path) {
    PanoramaInfo info;
    JpegReader reader;
    if (!reader.open(path)) return info;
    info.size = reader.size();
    const auto metadata = readHeaderMetadata(path);
    const auto& tags = metadata.tags;
    const auto projection = tags.value(QStringLiteral("ProjectionType"));
    const bool equirectangular = projection.compare(QStringLiteral("equirectangular"), Qt::CaseInsensitive) == 0;
    const bool boundsMatch = matchesDimension(tags, QStringLiteral("FullPanoWidthPixels"), info.size.width()) &&
        matchesDimension(tags, QStringLiteral("FullPanoHeightPixels"), info.size.height()) &&
        matchesDimension(tags, QStringLiteral("CroppedAreaImageWidthPixels"), info.size.width()) &&
        matchesDimension(tags, QStringLiteral("CroppedAreaImageHeightPixels"), info.size.height()) &&
        matchesDimension(tags, QStringLiteral("CroppedAreaLeftPixels"), 0) &&
        matchesDimension(tags, QStringLiteral("CroppedAreaTopPixels"), 0);
    const bool previewEnabled = tags.value(QStringLiteral("UsePanoramaViewer")).toLower() != QStringLiteral("false") &&
        tags.value(QStringLiteral("UsePanoramaViewer")) != QStringLiteral("0");
    info.canPreview = reader.rgbSupported() && metadata.orientation == 1 && hasPanoramaDimensions(info.size) &&
        (projection.isEmpty() || equirectangular) && boundsMatch;
    info.autoPreview = info.canPreview && equirectangular && previewEnabled;
    if (!info.canPreview) return info;
    info.multipleScans = reader.usesMultipleScans();
    info.coefficientBytes = reader.coefficientBytes();
    info.embeddedThumbnail = metadata.thumbnail;
    info.heading = taggedAngle(tags, QStringLiteral("PoseHeadingDegrees"), 0);
    info.pitch = std::clamp(taggedAngle(tags, QStringLiteral("PosePitchDegrees"), 0), -90.0, 90.0);
    info.roll = taggedAngle(tags, QStringLiteral("PoseRollDegrees"), 0);
    info.initialHeading = taggedAngle(tags, QStringLiteral("InitialViewHeadingDegrees"), info.heading);
    info.initialPitch = std::clamp(taggedAngle(tags, QStringLiteral("InitialViewPitchDegrees"), info.pitch), -90.0, 90.0);
    info.initialRoll = taggedAngle(tags, QStringLiteral("InitialViewRollDegrees"), 0);
    info.initialHorizontalFov = taggedAngle(tags, QStringLiteral("InitialHorizontalFOVDegrees"), 0);
    info.initialVerticalFov = taggedAngle(tags, QStringLiteral("InitialVerticalFOVDegrees"), 0);
    return info;
}

QList<QSize> panoramaLevels(QSize source) {
    QList<QSize> levels;
    while (source.width() > 1024) {
        levels.append(source);
        source = QSize((source.width() + 1) / 2, (source.height() + 1) / 2);
    }
    return levels;
}

QString panoramaCacheDirectory(const QString& path) {
    const QFileInfo info(path);
    const QString root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (root.isEmpty() || !info.isFile()) return {};
    QByteArray identity = info.absoluteFilePath().toUtf8();
    identity.append('\0').append(QByteArray::number(info.size()));
    identity.append('\0').append(QByteArray::number(info.lastModified().toMSecsSinceEpoch()));
    const auto key = QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex();
    return root + QStringLiteral("/panorama-v2/") + QString::fromLatin1(key);
}

QImage readPanoramaPreview(const QString& directory) {
    if (directory.isEmpty()) return {};
    QImageReader reader(directory + QStringLiteral("/preview.png"), "png");
    const auto size = reader.size();
    if (size.isEmpty() || size.width() > previewWidth || size.height() > previewWidth / 2 + 32)
        return {};
    return reader.read().convertToFormat(QImage::Format_RGBA8888);
}

QImage decodePanoramaPreview(const QString& path, const PanoramaInfo& info,
                             const PanoramaCancelled& cancelled) {
    if (info.multipleScans && info.coefficientBytes > maximumCoefficientBytes) {
        QBuffer buffer;
        buffer.setData(info.embeddedThumbnail);
        if (!buffer.open(QIODevice::ReadOnly)) return {};
        QImageReader thumbnail(&buffer, "jpeg");
        const QSize size = thumbnail.size();
        return size.width() > 0 && size.width() <= 512 && size.height() > 0 && size.height() <= 512 &&
            std::abs(size.width() - 2 * size.height()) <= qMax(1, size.width() / 100)
            ? thumbnail.read().convertToFormat(QImage::Format_RGBA8888) : QImage{};
    }
    JpegReader reader;
    if (cancelled() || !reader.open(path) || reader.size() != info.size ||
        !reader.rgbSupported() || (reader.usesMultipleScans() && reader.coefficientBytes() > maximumCoefficientBytes))
        return {};
    int scale = 1;
    while ((info.size.width() + scale - 1) / scale > previewWidth) scale *= 2;
    const auto profile = readHeaderMetadata(path, true).profile;
    if (!reader.start(scale)) return {};
    QImage image(reader.outputSize(), QImage::Format_RGBA8888);
    if (image.isNull()) return {};
    for (int y = 0; y < image.height(); ++y) {
        if (cancelled() || !reader.read(image.scanLine(y))) return {};
    }
    if (!reader.finish()) return {};
    if (!profile.isEmpty()) image.setColorSpace(QColorSpace::fromIccProfile(profile));
    return image;
}

void savePanoramaPreview(const QString& directory, const QImage& preview) {
    if (directory.isEmpty() || preview.isNull() || !QDir().mkpath(directory)) return;
    QSaveFile file(directory + QStringLiteral("/preview.png"));
    if (file.open(QIODevice::WriteOnly) && preview.save(&file, "PNG", 85)) file.commit();
}

bool panoramaCacheComplete(const QString& directory, QSize source, const QSet<int>& rows) {
    if (directory.isEmpty() || !QFileInfo::exists(directory + QStringLiteral("/complete"))) return false;
    const auto levels = panoramaLevels(source);
    for (int level = 0; level < levels.size(); ++level) {
        const int expected = (levels[level].height() + panoramaTileSize - 1) / panoramaTileSize;
        for (int row = 0; row < expected; ++row) {
            if (!rows.contains(panoramaRowKey(level, row))) return false;
        }
    }
    return true;
}

int panoramaRowKey(int level, int row) { return level * 64 + row; }

QSet<int> availablePanoramaRows(const QString& directory) {
    QSet<int> rows;
    if (directory.isEmpty()) return rows;
    const auto entries = QDir(directory).entryList({QStringLiteral("*.tiles")}, QDir::Files);
    for (const auto& entry : entries) {
        const auto parts = entry.chopped(6).split(QLatin1Char('-'));
        if (parts.size() != 2) continue;
        bool levelOk = false;
        bool rowOk = false;
        const int level = parts[0].toInt(&levelOk);
        const int row = parts[1].toInt(&rowOk);
        if (levelOk && rowOk && level >= 0 && level < 8 && row >= 0 && row < 64)
            rows.insert(panoramaRowKey(level, row));
    }
    return rows;
}

void invalidatePanoramaCache(const QString& directory) {
    if (!directory.isEmpty()) QFile::remove(directory + QStringLiteral("/complete"));
}

QImage readPanoramaTile(const QString& directory, PanoramaTile tile) {
    QFile file(rowPath(directory, tile));
    if (directory.isEmpty() || !file.open(QIODevice::ReadOnly)) return {};
    QDataStream stream(&file);
    quint32 magic = 0;
    quint32 columns = 0;
    stream >> magic >> columns;
    const qint64 indexEnd = 8 + qint64(columns) * 12;
    if (magic != rowMagic || columns > panoramaMaximumWidth / panoramaTileSize ||
        tile.column < 0 || quint32(tile.column) >= columns ||
        !file.seek(8 + qint64(tile.column) * 12)) return {};
    quint64 offset = 0;
    quint32 length = 0;
    stream >> offset >> length;
    if (stream.status() != QDataStream::Ok || length < 4 || length > tileBytes ||
        offset < quint64(indexEnd) || offset > quint64(file.size()) ||
        length > quint64(file.size()) - offset || !file.seek(qint64(offset))) return {};
    const auto bytes = file.read(length);
    if (bytes.size() != length) return {};
    QImage result(panoramaTileExtent, panoramaTileExtent, QImage::Format_RGBA8888);
    if (result.isNull()) return {};
    if (length == tileBytes) {
        std::memcpy(result.bits(), bytes.constData(), tileBytes);
    } else {
        mz_ulong pixelLength = tileBytes;
        if (mz_uncompress(result.bits(), &pixelLength,
                          reinterpret_cast<const uchar*>(bytes.constData()), length) != MZ_OK ||
            pixelLength != tileBytes) return {};
    }
    QFile profile(profilePath(directory));
    if (profile.open(QIODevice::ReadOnly) && profile.size() <= maximumMetadataBytes)
        result.setColorSpace(QColorSpace::fromIccProfile(profile.readAll()));
    return result;
}

bool buildPanoramaTiles(const QString& path, const PanoramaInfo& info, const QString& directory,
                        const PanoramaCancelled& cancelled, const std::function<void(int, int)>& rowAvailable) {
    if (directory.isEmpty() || cancelled() || !QDir().mkpath(QFileInfo(directory).absolutePath())) return false;
    QLockFile lock(directory + QStringLiteral(".lock"));
    if (!lock.tryLock(0)) return false;
    if (panoramaCacheComplete(directory, info.size, availablePanoramaRows(directory))) return true;
    const QString sourceIdentity = panoramaCacheDirectory(path);
    const auto sizes = panoramaLevels(info.size);
    if (sizes.isEmpty()) return true;

    const qint64 space = qint64(info.size.width()) * info.size.height() * 6 + 64 * MiB;
    JpegReader reader;
    if (!reader.open(path) || reader.size() != info.size || !reader.rgbSupported() ||
        (reader.usesMultipleScans() && reader.coefficientBytes() >
            std::min(maximumCoefficientBytes, 512 * MiB - space / 24)) || cancelled()) return false;

    trimCache(directory, space);
    if (QStorageInfo(QFileInfo(directory).absolutePath()).bytesAvailable() < space ||
        !QDir().mkpath(directory) || !reader.start(1)) return false;
    const auto profile = readHeaderMetadata(path, true).profile;
    if (!profile.isEmpty()) {
        QSaveFile file(profilePath(directory));
        if (!file.open(QIODevice::WriteOnly) || file.write(profile) != profile.size() || !file.commit())
            return false;
    }

    std::vector<TileLevel> levels;
    std::vector<QByteArray> evenRows(size_t(sizes.size()));
    std::vector<int> processed(size_t(sizes.size()), 0);
    for (int level = 0; level < sizes.size(); ++level) levels.emplace_back(sizes[level], level);

    for (int y = 0; y < info.size.height(); ++y) {
        if (cancelled()) return false;
        QByteArray row(info.size.width() * 4, Qt::Uninitialized);
        if (!reader.read(reinterpret_cast<uchar*>(row.data()))) return false;
        for (size_t level = 0; level < levels.size(); ++level) {
            if (!levels[level].append(row, directory, cancelled, rowAvailable)) return false;
            if (level + 1 == levels.size()) break;
            const QSize size = sizes[qsizetype(level)];
            const int index = processed[level]++;
            if (index % 2 == 0 && index + 1 < size.height()) {
                evenRows[level] = std::move(row);
                break;
            }
            const auto& prior = index % 2 == 0 ? row : evenRows[level];
            row = downsampleRow(prior, row, size.width());
            evenRows[level].clear();
        }
    }
    if (cancelled() || !reader.finish() || panoramaCacheDirectory(path) != sourceIdentity) return false;
    QSaveFile marker(directory + QStringLiteral("/complete"));
    return marker.open(QIODevice::WriteOnly) && marker.write("1", 1) == 1 && marker.commit();
}

} // namespace mvpview
