#include "io/jpeg_stripe_cache.h"

#include <QBuffer>
#include <QColorSpace>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QLockFile>
#include <QSaveFile>
#include <QScopeGuard>
#include <QStandardPaths>
#include <turbojpeg.h>

#include <array>
#include <algorithm>
#include <cstring>
#include <future>
#include <numeric>

namespace mvpview::JpegStripeCache {
namespace {
constexpr int count = 8;
constexpr qint64 maxFile = 128LL << 20;
constexpr quint32 signature = 0x4c535431;

QString entryPath(const QString& path) {
    const QString root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (root.isEmpty()) return {};
    const QFileInfo info(path);
    const QByteArray identity = info.absoluteFilePath().toUtf8() + '|' + QByteArray::number(info.size()) +
        '|' + QByteArray::number(info.lastModified().toMSecsSinceEpoch());
    return root + QStringLiteral("/jpeg-stripes-v1/") +
           QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex()) +
           QStringLiteral(".stripes");
}

int boundary(size_t index, int height) {
    return index == count ? height : (height * int(index) / count / 8) * 8;
}

void trim(const QDir& directory) {
    qint64 total = 0;
    const auto entries = directory.entryInfoList({QStringLiteral("*.stripes")}, QDir::Files, QDir::Time);
    for (const auto& entry : entries) {
        total += entry.size();
        if (total > (512LL << 20)) QFile::remove(entry.absoluteFilePath());
    }
}

bool decodeBand(QByteArray& bytes, QSize expected, QRect local, int scale, uchar* output, int stride,
                QImage* metadata) {
    const auto decoder = tj3Init(TJINIT_DECOMPRESS);
    if (!decoder) return false;
    const auto release = qScopeGuard([&] { tj3Destroy(decoder); });
    const auto* input = reinterpret_cast<const uchar*>(bytes.constData());
    if (tj3DecompressHeader(decoder, input, size_t(bytes.size())) ||
        tj3Get(decoder, TJPARAM_JPEGWIDTH) != expected.width() ||
        tj3Get(decoder, TJPARAM_JPEGHEIGHT) != expected.height() ||
        tj3Get(decoder, TJPARAM_SUBSAMP) != TJSAMP_444) return false;
    const int divisor = std::gcd(scale, 8);
    if (tj3SetScalingFactor(decoder, {scale / divisor, 8 / divisor})) return false;
    const int padding = local.x() % scale;
    const tjregion crop{local.x() - padding, local.y(), local.width() + padding, local.height()};
    if (tj3SetCroppingRegion(decoder, crop)) return false;
    QImage aligned;
    if (padding) {
        aligned = QImage(crop.w, crop.h, QImage::Format_RGBA8888);
        if (aligned.isNull()) return false;
    }
    if (tj3Decompress8(decoder, input, size_t(bytes.size()), padding ? aligned.bits() : output,
                       padding ? int(aligned.bytesPerLine()) : stride, TJPF_RGBA)) return false;
    if (padding) {
        for (int row = 0; row < local.height(); ++row)
            std::memcpy(output + qsizetype(row) * stride, aligned.constScanLine(row) + padding * 4, size_t(stride));
    }
    if (metadata) {
        QBuffer buffer(&bytes);
        if (!buffer.open(QIODevice::ReadOnly)) return false;
        QImageReader reader(&buffer, "jpeg");
        reader.setAutoTransform(false);
        reader.setClipRect({0, 0, 1, 1});
        *metadata = reader.read();
        if (metadata->isNull()) return false;
    }
    return true;
}
}

bool eligible(QSize source) {
    const auto bytes = estimatedPixelBytes(source, 4);
    const int limit = QImageReader::allocationLimit();
    return source.height() >= count * 8 && bytes >= (64LL << 20) && bytes <= (512LL << 20) &&
           (limit == 0 || bytes <= qint64(limit) * 1024 * 1024);
}

bool exists(const QString& path) {
    return QFileInfo::exists(entryPath(path));
}

void build(const QString& path, QSize source) {
    if (!eligible(source)) return;
    const QString entry = entryPath(path);
    const QDir directory = QFileInfo(entry).absoluteDir();
    if (entry.isEmpty() || QFileInfo::exists(entry) || !directory.mkpath(QStringLiteral("."))) return;
    QLockFile lock(entry + QStringLiteral(".lock"));
    if (!lock.tryLock(0) || QFileInfo::exists(entry)) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > maxFile) return;
    const QByteArray data = file.read(maxFile + 1);
    if (data.size() != file.size() || data.size() > maxFile) return;
    const auto* input = reinterpret_cast<const uchar*>(data.constData());
    const auto transform = tj3Init(TJINIT_TRANSFORM);
    if (!transform) return;
    const auto destroy = qScopeGuard([&] { tj3Destroy(transform); });
    if (tj3DecompressHeader(transform, input, size_t(data.size())) ||
        tj3Get(transform, TJPARAM_JPEGWIDTH) != source.width() ||
        tj3Get(transform, TJPARAM_JPEGHEIGHT) != source.height() ||
        tj3Get(transform, TJPARAM_PRECISION) != 8 || tj3Get(transform, TJPARAM_LOSSLESS) ||
        tj3Get(transform, TJPARAM_SUBSAMP) != TJSAMP_444 ||
        tj3Get(transform, TJPARAM_COLORSPACE) != TJCS_YCbCr) return;
    std::array<tjtransform, count> transforms{};
    std::array<uchar*, count> parts{};
    std::array<size_t, count> sizes{};
    const auto release = qScopeGuard([&] { for (auto* part : parts) tj3Free(part); });
    for (size_t index = 0; index < count; ++index) {
        const int top = boundary(index, source.height());
        transforms[index].r = {0, top, source.width(), boundary(index + 1, source.height()) - top};
        transforms[index].options = TJXOPT_CROP;
    }
    if (tj3Transform(transform, input, size_t(data.size()), count, parts.data(), sizes.data(), transforms.data()))
        return;
    QSaveFile output(entry);
    if (!output.open(QIODevice::WriteOnly)) return;
    QDataStream header(&output);
    header << signature << qint32(source.width()) << qint32(source.height());
    for (size_t index = 0; index < count; ++index) {
        if (sizes[index] == 0 || sizes[index] > size_t(maxFile)) return;
        const auto bytes = QByteArray::fromRawData(reinterpret_cast<const char*>(parts[index]), qsizetype(sizes[index]));
        header << quint32(bytes.size());
        const auto hash = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
        if (output.write(hash) != hash.size() || output.write(bytes) != bytes.size() || output.pos() > maxFile) return;
    }
    if (header.status() == QDataStream::Ok && entry == entryPath(path) && output.commit()) trim(directory);
}

QImage read(const DecodeRequest& request, QSize source, QSize scaledSize) {
    if (!eligible(source)) return {};
    int scale = 8;
    if (!scaledSize.isEmpty()) {
        const auto numerator = [](int target, int original) { return (target * 8 + original - 1) / original; };
        scale = std::clamp(std::max(numerator(scaledSize.width(), source.width()),
                                    numerator(scaledSize.height(), source.height())), 1, 8);
    }
    QFile file(entryPath(request.path));
    if (!file.open(QIODevice::ReadOnly)) return {};
    bool corrupt = false;
    const auto repair = qScopeGuard([&] { if (corrupt) file.remove(); });
    QDataStream header(&file);
    quint32 magic = 0;
    qint32 width = 0, height = 0;
    header >> magic >> width >> height;
    if (magic != signature || QSize(width, height) != source || file.size() > maxFile) {
        corrupt = true;
        return {};
    }
    const auto scaled = [scale](int extent) { return (extent * scale + 7) / 8; };
    const QSize nativeSize(scaled(width), scaled(height));
    const QRect region = request.purpose == DecodePurpose::Region ? request.sourceRegion : QRect(QPoint{}, nativeSize);
    if (region.isEmpty() || !QRect(QPoint{}, nativeSize).contains(region)) return {};
    QImage result(region.size(), QImage::Format_RGBA8888);
    if (result.isNull()) return {};
    auto* pixels = result.bits();
    const int stride = int(result.bytesPerLine());
    QImage metadata;
    std::array<std::future<bool>, count> jobs;
    for (size_t index = 0; index < count; ++index) {
        if (request.isCancelled()) return {};
        quint32 length = 0;
        header >> length;
        const auto hash = file.read(32);
        if (!length || hash.size() != 32 || length > file.bytesAvailable()) {
            corrupt = true;
            return {};
        }
        const int top = boundary(index, height);
        const int bottom = boundary(index + 1, height);
        const QSize originalBand(width, bottom - top);
        const QRect band(0, scaled(top), nativeSize.width(), scaled(bottom) - scaled(top));
        const QRect intersection = band.intersected(region);
        if (intersection.isEmpty()) {
            if (!file.seek(file.pos() + length)) return {};
            continue;
        }
        auto bytes = file.read(length);
        auto* output = pixels + qsizetype(intersection.y() - region.y()) * stride;
        QImage* bandMetadata = intersection.top() == region.top() ? &metadata : nullptr;
        try {
            jobs[index] = std::async(std::launch::async, [bytes = std::move(bytes), hash, band, originalBand,
                                                         intersection, scale, output, stride, bandMetadata]() mutable {
                return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256) == hash &&
                    decodeBand(bytes, originalBand, intersection.translated(-band.topLeft()), scale, output, stride, bandMetadata);
            });
        } catch (const std::system_error&) {
            return {};
        }
    }
    for (auto& job : jobs) {
        if (job.valid() && !job.get()) {
            corrupt = true;
            return {};
        }
    }
    result.setColorSpace(metadata.colorSpace());
    result.setDotsPerMeterX(metadata.dotsPerMeterX());
    result.setDotsPerMeterY(metadata.dotsPerMeterY());
    for (const QString& key : metadata.textKeys()) result.setText(key, metadata.text(key));
    if (!scaledSize.isEmpty() && result.size() != scaledSize)
        return result.scaled(scaledSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    return result;
}
}
