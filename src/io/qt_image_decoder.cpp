#include "io/qt_image_decoder.h"

#include "io/encoded_color_management.h"
#include "io/jpeg_stripe_cache.h"
#include "io/metadata_reader.h"

#include <QColorSpace>
#include <QFileInfo>
#include <QImageIOHandler>
#include <QImageReader>

#include <algorithm>
#include <atomic>

namespace mvpview {
namespace {
std::atomic_bool autoOrientation{true};
std::atomic_bool keepHighBitDepth{true};

const QList<QByteArray>& availableFormats() {
    static const QList<QByteArray> formats = [] {
        QList<QByteArray> result = QImageReader::supportedImageFormats();
        for (QByteArray& format : result) {
            format = format.toLower();
        }
        return result;
    }();
    return formats;
}

bool containsAny(const QList<QByteArray>& formats,
                 std::initializer_list<QByteArray> candidates) {
    return std::any_of(candidates.begin(), candidates.end(), [&formats](const QByteArray& value) {
        return formats.contains(value);
    });
}

QByteArray readerFormatForSuffix(const QString& suffix) {
    if (suffix == QStringLiteral("dib")) {
        return QByteArrayLiteral("bmp");
    }
    if (suffix == QStringLiteral("heic") || suffix == QStringLiteral("heif")) {
        const QList<QByteArray>& formats = availableFormats();
        const QByteArray requested = suffix.toLatin1();
        if (formats.contains(requested)) {
            return requested;
        }
        return formats.contains(QByteArrayLiteral("heic")) ? QByteArrayLiteral("heic")
                                                           : QByteArrayLiteral("heif");
    }
    return suffix.toLatin1();
}

bool swapsImageAxes(const QImageReader& reader) {
    return reader.autoTransform() &&
           reader.transformation().testFlag(QImageIOHandler::TransformationRotate90);
}

bool isJpeg(const QByteArray& format) {
    return format == "jpeg" || format == "jpg";
}

QSize scaledDecodeSize(QSize source, QSize maximum, bool preferNativeJpegScale) {
    if (source.isEmpty() || maximum.isEmpty() ||
        (source.width() <= maximum.width() && source.height() <= maximum.height())) {
        return source;
    }
    const QSize target = source.scaled(maximum, Qt::KeepAspectRatio);
    if (preferNativeJpegScale) {
        constexpr qint64 minimumNativeScalePercent = 97;
        for (const int divisor : {2, 4, 8}) {
            if (source.width() % divisor != 0 || source.height() % divisor != 0) continue;
            const QSize nativeSize(source.width() / divisor, source.height() / divisor);
            if (nativeSize.width() <= target.width() && nativeSize.height() <= target.height() &&
                100LL * nativeSize.width() >= minimumNativeScalePercent * target.width() &&
                100LL * nativeSize.height() >= minimumNativeScalePercent * target.height()) {
                return nativeSize;
            }
        }
    }
    return target;
}
}

bool QtImageDecoder::autoOrientationEnabled() {
    return autoOrientation.load(std::memory_order_relaxed);
}

bool QtImageDecoder::preserveHighBitDepth() {
    return keepHighBitDepth.load(std::memory_order_relaxed);
}

QStringList QtImageDecoder::supportedSuffixes() {
    static const QStringList suffixes = [] {
        const QList<QByteArray>& formats = availableFormats();
        QStringList result;
        if (containsAny(formats, {QByteArrayLiteral("jpg"), QByteArrayLiteral("jpeg")})) {
            result.append({QStringLiteral("jpg"), QStringLiteral("jpeg")});
        }
        if (formats.contains(QByteArrayLiteral("png"))) {
            result.append(QStringLiteral("png"));
        }
        if (formats.contains(QByteArrayLiteral("bmp"))) {
            result.append({QStringLiteral("bmp"), QStringLiteral("dib")});
        }
        if (containsAny(formats, {QByteArrayLiteral("heic"), QByteArrayLiteral("heif")})) {
            result.append({QStringLiteral("heic"), QStringLiteral("heif")});
        }
        return result;
    }();
    return suffixes;
}

void QtImageDecoder::setAutoOrientationEnabled(bool enabled) {
    autoOrientation.store(enabled, std::memory_order_relaxed);
}

void QtImageDecoder::setPreserveHighBitDepth(bool enabled) {
    keepHighBitDepth.store(enabled, std::memory_order_relaxed);
}

QString QtImageDecoder::cacheIdentity() const {
    const QString colorIdentity =
        EncodedColorManagement::isAvailable()
            ? QStringLiteral("lcms-%1").arg(EncodedColorManagement::version())
            : QStringLiteral("lcms-disabled");
    return QStringLiteral("qt-image-v9|qt-%1|formats-%2|%3|icc-%4|orientation-%5|depth-%6|display-%7")
        .arg(QString::fromLatin1(qVersion()), supportedSuffixes().join(QLatin1Char(',')),
             colorIdentity,
             EncodedColorManagement::isEnabled() ? QStringLiteral("on") : QStringLiteral("off"),
             autoOrientationEnabled() ? QStringLiteral("on") : QStringLiteral("off"),
             preserveHighBitDepth() ? QStringLiteral("native") : QStringLiteral("8"),
             displayColorSpaceKey(currentDisplayColorSpace()));
}

bool QtImageDecoder::canDecode(const QString& path) const {
    const QString suffix = QFileInfo(path).suffix().toLower();
    return supportedSuffixes().contains(suffix);
}

DecodeResult QtImageDecoder::decode(const DecodeRequest& request) const {
    if (request.isCancelled()) return {{}, QStringLiteral("Cancelled")};
    if (!canDecode(request.path)) {
        return {{}, QStringLiteral("Unsupported image format")};
    }

    const QString suffix = QFileInfo(request.path).suffix().toLower();
    const QByteArray readerFormat = readerFormatForSuffix(suffix);
    QImageReader reader(request.path, readerFormat);
    reader.setAutoTransform(autoOrientationEnabled());
    // The registry has already validated the suffix. Selecting the corresponding Qt
    // decoder explicitly avoids a second content probe for correctly named images.
    reader.setDecideFormatFromContent(false);
    QString fileFormat = QFileInfo(request.path).suffix().toUpper();
    QSize sourceSize = reader.size();
    bool swapAxes = sourceSize.isValid() && swapsImageAxes(reader);
    const bool regional = request.purpose == DecodePurpose::Region;
    if (regional && (!isJpeg(reader.format()) || request.sourceRegion.isEmpty() ||
                     !QRect(QPoint{}, sourceSize).contains(request.sourceRegion) ||
                     !reader.supportsOption(QImageIOHandler::ClipRect) ||
                     (reader.autoTransform() && reader.transformation() != QImageIOHandler::TransformationNone))) {
        return {{}, QStringLiteral("Regional JPEG decode unavailable")};
    }

    const auto prepareReader = [&request, regional](QImageReader& reader, QSize source) {
        if (source.isEmpty()) return request.prepareAllocation(512LL * 1024 * 1024);
        const QByteArray format = reader.format();
        const bool jpeg = isJpeg(format);
        const QSize output = regional ? request.sourceRegion.size() :
            scaledDecodeSize(source, request.maximumSize, jpeg && request.purpose == DecodePurpose::Preview);
        if (regional) reader.setClipRect(request.sourceRegion);
        else if (output != source) reader.setScaledSize(output);
        // PNG/JPEG/BMP have integer output up to RGBA64. Other plugins can expose
        // RGBA32F. Include native pixels even if a plugin scales after decoding,
        // and two output images for conversion/color or upload preparation.
        const bool integerFormat = jpeg || format == "png" || format == "bmp";
        return request.prepareAllocation(addedAllocationBytes(
            estimatedPixelBytes(source, integerFormat ? 8 : 16),
            estimatedPixelBytes(output, integerFormat ? 16 : 32)));
    };
    if (!prepareReader(reader, sourceSize)) return {{}, QStringLiteral("Cancelled")};
    QImage image;
#ifdef MVPVIEW_JPEG_STRIPES
    if (request.purpose != DecodePurpose::Thumbnail && isJpeg(reader.format()) &&
        (!reader.autoTransform() || reader.transformation() == QImageIOHandler::TransformationNone)) {
        image = JpegStripeCache::read(request, sourceSize, reader.scaledSize());
    }
#endif
    const bool fromStripes = !image.isNull();
    if (request.isCancelled()) return {{}, QStringLiteral("Cancelled")};
    if (!fromStripes) image = reader.read();
    if (image.isNull()) {
        if (regional) return {{}, reader.errorString()};
        // Some camera exports keep an old extension when replacing a damaged file. Let Qt
        // inspect the signature if the suffix-selected decoder could not read its contents.
        QImageReader contentReader(request.path);
        contentReader.setAutoTransform(autoOrientationEnabled());
        contentReader.setDecideFormatFromContent(true);
        sourceSize = contentReader.size();
        swapAxes = sourceSize.isValid() && swapsImageAxes(contentReader);
        if (!prepareReader(contentReader, sourceSize)) return {{}, QStringLiteral("Cancelled")};
        image = contentReader.read();
        if (image.isNull()) return {{}, contentReader.errorString()};
        fileFormat = QString::fromLatin1(QImageReader::imageFormat(request.path)).toUpper();
    }

    if (request.isCancelled()) return {{}, QStringLiteral("Cancelled")};
    if (regional && image.size() != request.sourceRegion.size()) {
        return {{}, QStringLiteral("JPEG decoder did not return the requested region")};
    }
    const bool sourceHighBitDepth = image.depth() > 32;
    const bool highBitDepth = sourceHighBitDepth && preserveHighBitDepth();
    if (highBitDepth) {
        if (image.format() != QImage::Format_RGBA64 &&
            image.format() != QImage::Format_RGBA16FPx4 &&
            image.format() != QImage::Format_RGBA32FPx4) {
            image.convertTo(QImage::Format_RGBA64);
        }
    } else if (image.format() != QImage::Format_RGBA8888) {
        image.convertTo(QImage::Format_RGBA8888);
    }

    const QFileInfo fileInfo(request.path);
    auto frame = std::make_shared<ImageFrame>();
    if (regional) frame->decodedRegion = request.sourceRegion;
    frame->descriptor.size = image.size();
    frame->descriptor.layout = PixelLayout::Interleaved;
    frame->descriptor.channelOrder = ChannelOrder::RGBA;
    frame->metadata.path = fileInfo.absoluteFilePath();
    frame->metadata.fileName = fileInfo.fileName();
    frame->metadata.format = fileFormat;
    frame->metadata.fileSize = fileInfo.size();
    if (swapAxes) sourceSize.transpose();
    frame->metadata.sourceSize = sourceSize.isValid() ? sourceSize : image.size();
    frame->metadata.modifiedAt = fileInfo.lastModified();
    frame->metadata.decoderName = QStringLiteral("Qt Image Formats");
    frame->metadata.decoderVersion = QString::fromLatin1(qVersion());
#ifdef MVPVIEW_JPEG_STRIPES
    if (fromStripes) {
        frame->metadata.decoderName = QStringLiteral("TurboJPEG (parallel stripes)");
        frame->metadata.decoderVersion = QStringLiteral(MVPVIEW_TURBOJPEG_VERSION);
    }
#endif
    EncodedColorManagement::normalizeToDisplay(image, frame->metadata);
    frame->descriptor.sampleType =
        image.format() == QImage::Format_RGBA16FPx4 ||
                image.format() == QImage::Format_RGBA32FPx4
            ? SampleType::Float
            : SampleType::UInt;
    frame->descriptor.storageBits = image.format() == QImage::Format_RGBA32FPx4
                                        ? 32
                                        : (image.depth() > 32 ? 16 : 8);
    frame->descriptor.validBits = frame->descriptor.storageBits;
    // Untagged files keep the documented sRGB assumption as their source space.
    applyDisplayColor(frame->descriptor.displayColor);
    if (frame->metadata.colorProfile) {
        frame->descriptor.sourceColor.colorSpace =
            frame->metadata.colorProfile->sourceDescription;
        frame->descriptor.sourceColor.primaries = QStringLiteral("Defined by ICC");
        frame->descriptor.sourceColor.transferFunction = QStringLiteral("Defined by ICC");
        if (!frame->metadata.colorProfile->converted) {
            frame->descriptor.displayColor = frame->descriptor.sourceColor;
        }
    }
    // Descriptive metadata is requested separately; ICC and orientation remain on this path.
    frame->storage = std::move(image);
    return {std::move(frame), {}};
}

} // namespace mvpview
