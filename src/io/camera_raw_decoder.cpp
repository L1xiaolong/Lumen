#include "io/camera_raw_decoder.h"
#include "core/bayer_image_renderer.h"
#include "io/encoded_color_management.h"
#include "io/qt_image_decoder.h"

#include <QBuffer>
#include <QColorSpace>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>

#if MVPVIEW_HAS_LIBRAW
#include <libraw.h>
#endif

namespace mvpview {
namespace {

QStringList cameraRawSuffixes() {
    return {QStringLiteral("dng"), QStringLiteral("cr2"), QStringLiteral("cr3"),
            QStringLiteral("crw"), QStringLiteral("nef"), QStringLiteral("nrw"),
            QStringLiteral("arw"), QStringLiteral("sr2"), QStringLiteral("srf"),
            QStringLiteral("raf"), QStringLiteral("rw2"), QStringLiteral("orf"),
            QStringLiteral("pef"), QStringLiteral("srw"), QStringLiteral("x3f"),
            QStringLiteral("rwl")};
}

#if MVPVIEW_HAS_LIBRAW

struct ProcessedImageDeleter {
    void operator()(libraw_processed_image_t* image) const {
        if (image) {
            LibRaw::dcraw_clear_mem(image);
        }
    }
};

using ProcessedImage = std::unique_ptr<libraw_processed_image_t, ProcessedImageDeleter>;

QString libRawError(int code) {
    const char* message = libraw_strerror(code);
    return message ? QString::fromLocal8Bit(message) : QStringLiteral("Unknown LibRaw error");
}

int openFile(LibRaw& processor, const QString& path) {
#if defined(Q_OS_WIN)
    return processor.open_file(reinterpret_cast<const wchar_t*>(path.utf16()));
#else
    const QByteArray nativePath = QFile::encodeName(path);
    return processor.open_file(nativePath.constData());
#endif
}

QImage processedImageToQImage(const libraw_processed_image_t& processed, QString* error,
                              const DecodeRequest& request, bool* hasExifTransform = nullptr) {
    if (request.isCancelled()) {
        if (error) *error = QStringLiteral("Cancelled");
        return {};
    }
    if (processed.type == LIBRAW_IMAGE_JPEG) {
        if (processed.data_size > static_cast<unsigned int>(std::numeric_limits<int>::max())) {
            if (error) {
                *error = QStringLiteral("LibRaw embedded JPEG is too large");
            }
            return {};
        }
        QByteArray encoded = QByteArray::fromRawData(
            reinterpret_cast<const char*>(processed.data), static_cast<int>(processed.data_size));
        QBuffer buffer(&encoded);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer, "JPEG");
        reader.setAutoTransform(QtImageDecoder::autoOrientationEnabled());
        if (hasExifTransform) {
            *hasExifTransform = reader.autoTransform() && reader.transformation() != QImageIOHandler::TransformationNone;
        }
        QImage image = reader.read();
        if (request.isCancelled()) {
            if (error) *error = QStringLiteral("Cancelled");
            return {};
        }
        if (image.isNull() && error) {
            *error = QStringLiteral("Invalid LibRaw embedded JPEG preview: %1")
                         .arg(reader.errorString());
        }
        return image;
    }
    if (processed.type != LIBRAW_IMAGE_BITMAP ||
        (processed.bits != 8 && processed.bits != 16) ||
        (processed.colors != 3 && processed.colors != 4)) {
        if (error) {
            *error = QStringLiteral("Unsupported LibRaw output: type %1, %2-bit, %3 channels")
                         .arg(static_cast<int>(processed.type))
                         .arg(processed.bits)
                         .arg(processed.colors);
        }
        return {};
    }

    const qsizetype bytesPerChannel = processed.bits / 8;
    const qsizetype bytesPerPixel = processed.colors * bytesPerChannel;
    const qsizetype expected = static_cast<qsizetype>(processed.width) * processed.height *
                               bytesPerPixel;
    if (expected <= 0 || expected > processed.data_size) {
        if (error) {
            *error = QStringLiteral("LibRaw returned a truncated bitmap");
        }
        return {};
    }

    // Qt's RGBX64 is four 16-bit channels; LibRaw RGB is only three. Pack an
    // owned opaque RGBA image for either channel count. A raw QImage view would
    // also outlive dcraw_clear_mem() when source and destination formats match.
    const bool highBitDepth = processed.bits == 16;
    QImage result(processed.width, processed.height,
                  highBitDepth ? QImage::Format_RGBA64 : QImage::Format_RGBA8888);
    if (result.isNull()) {
        if (error) *error = QStringLiteral("Could not allocate the LibRaw display image");
        return {};
    }
    const qsizetype sourceStride = static_cast<qsizetype>(processed.width) * bytesPerPixel;
    for (int y = 0; y < processed.height; ++y) {
        if (request.isCancelled()) {
            if (error) *error = QStringLiteral("Cancelled");
            return {};
        }
        const auto* row = processed.data + y * sourceStride;
        if (highBitDepth) {
            const auto* input = reinterpret_cast<const quint16*>(row);
            auto* output = reinterpret_cast<QRgba64*>(result.scanLine(y));
            for (int x = 0; x < processed.width; ++x) {
                const auto* pixel = input + x * processed.colors;
                output[x] = QRgba64::fromRgba64(pixel[0], pixel[1], pixel[2], 65535);
            }
        } else {
            auto* output = result.scanLine(y);
            for (int x = 0; x < processed.width; ++x) {
                const auto* pixel = row + x * processed.colors;
                output[x * 4] = pixel[0]; output[x * 4 + 1] = pixel[1];
                output[x * 4 + 2] = pixel[2]; output[x * 4 + 3] = 255;
            }
        }
    }
    // LibRaw output_color=1 is sRGB. Tag the bitmap explicitly because the memory image does
    // not carry an embedded profile of its own, then move it into the application display space.
    result.setColorSpace(QColorSpace(QColorSpace::SRgb));
    const QColorSpace displaySpace = displayQColorSpace(currentDisplayColorSpace());
    if (result.colorSpace() != displaySpace) {
        const QImage converted = result.convertedToColorSpace(displaySpace);
        if (!converted.isNull()) {
            return converted;
        }
    }
    return result;
}

void addSensorProperties(ImageMetadata& metadata, LibRaw& processor) {
    const auto& data = processor.imgdata;
    ImageMetadata::PropertyGroup sensor{QStringLiteral("camera-raw-sensor"),
        QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "RAW sensor")), {}, {}};
    const auto add = [&](QString label, QString value) {
        sensor.properties.append({std::move(label), std::move(value), {}});
    };
    const auto& sizes = data.sizes;
    const auto& color = data.color;
    const auto& identity = data.idata;
    const auto dimensions = [](unsigned width, unsigned height) {
        return QStringLiteral("%1 × %2 px").arg(width).arg(height);
    };
    const auto values = [](const auto& items, qsizetype count) {
        QStringList result;
        for (qsizetype i = 0; i < count; ++i) result.append(QString::number(items[i], 'g', 8));
        return result.join(QStringLiteral(", "));
    };
    if (sizes.raw_width && sizes.raw_height)
        add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "Sensor dimensions")), dimensions(sizes.raw_width, sizes.raw_height));
    if (sizes.width && sizes.height) {
        add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "Active dimensions")), dimensions(sizes.width, sizes.height));
        add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "Active origin")), QStringLiteral("(%1, %2)").arg(sizes.left_margin).arg(sizes.top_margin));
    }
    if (sizes.raw_pitch) add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "Sensor row stride")), QStringLiteral("%1 B").arg(sizes.raw_pitch));
    if (sizes.pixel_aspect > 0.0) add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "Pixel aspect ratio")), QString::number(sizes.pixel_aspect, 'g', 8));
    if (identity.raw_count) add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "RAW image count")), QString::number(identity.raw_count));
    if (color.raw_bps) add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "Sensor bit depth")), QString::number(color.raw_bps));
    if (identity.colors > 0) add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "Sensor colors")), QString::number(identity.colors));
    if (identity.filters) {
        add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "CFA colors")), QString::fromLatin1(identity.cdesc));
        QStringList rows;
        const int period = identity.filters == 9 ? 6 : 2;
        for (int y = 0; y < period; ++y) {
            QString row;
            for (int x = 0; x < period; ++x) {
                const int index = processor.COLOR(y, x);
                row.append(index >= 0 && index < 4 && identity.cdesc[index]
                    ? QLatin1Char(identity.cdesc[index]) : QLatin1Char('?'));
            }
            rows.append(row);
        }
        add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "CFA pattern")), rows.join(QStringLiteral(" / ")));
    }
    add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "Black level")), QString::number(color.black));
    add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "Channel black offsets")), QStringLiteral("%1, %2, %3, %4")
        .arg(color.cblack[0]).arg(color.cblack[1]).arg(color.cblack[2]).arg(color.cblack[3]));
    if (color.maximum) add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "Saturation level")), QString::number(color.maximum));
    if (color.cam_mul[0] > 0.0f) add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "As-shot white balance")), values(color.cam_mul, 4));
    if (color.pre_mul[0] > 0.0f) add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "Daylight white balance")), values(color.pre_mul, 4));
    if (color.profile_length) add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "Embedded ICC size")), QStringLiteral("%1 B").arg(color.profile_length));
    if (identity.dng_version) {
        const auto version = identity.dng_version;
        add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "DNG version")), QStringLiteral("%1.%2.%3.%4")
            .arg(version >> 24).arg((version >> 16) & 255).arg((version >> 8) & 255).arg(version & 255));
    }
    metadata.formatProperties.append(std::move(sensor));
}

ImageMetadata metadataFor(const QString& path, LibRaw& processor, bool includeSensorProperties) {
    const QFileInfo fileInfo(path);
    ImageMetadata metadata;
    metadata.path = fileInfo.absoluteFilePath();
    metadata.fileName = fileInfo.fileName();
    metadata.format = fileInfo.suffix().toUpper();
    metadata.fileSize = fileInfo.size();
    metadata.modifiedAt = fileInfo.lastModified();
    metadata.decoderName = QStringLiteral("LibRaw");
    metadata.decoderVersion = QString::fromLatin1(libraw_version());

    ImageMetadata::Camera camera;
    camera.make = QString::fromLocal8Bit(processor.imgdata.idata.make).trimmed();
    camera.model = QString::fromLocal8Bit(processor.imgdata.idata.model).trimmed();
    camera.lens = QString::fromLocal8Bit(processor.imgdata.lens.Lens).trimmed();
    if (camera.lens.isEmpty()) {
        camera.lens = QString::fromLocal8Bit(processor.imgdata.lens.makernotes.Lens).trimmed();
    }
    camera.exposureSeconds = processor.imgdata.other.shutter;
    camera.aperture = processor.imgdata.other.aperture;
    camera.focalLengthMm = processor.imgdata.other.focal_len;
    const double iso = processor.imgdata.other.iso_speed;
    if (std::isfinite(iso) && iso > 0.0 && iso <= std::numeric_limits<int>::max()) {
        camera.iso = static_cast<int>(std::lround(iso));
    }
    if (processor.imgdata.other.timestamp > 0) {
        camera.capturedAt = QDateTime::fromSecsSinceEpoch(processor.imgdata.other.timestamp);
    }
    camera.sensorSize = {processor.imgdata.sizes.raw_width, processor.imgdata.sizes.raw_height};
    metadata.camera = std::move(camera);
    const int imageWidth = processor.imgdata.sizes.iwidth > 0 ? processor.imgdata.sizes.iwidth
                                                               : processor.imgdata.sizes.width;
    const int imageHeight = processor.imgdata.sizes.iheight > 0
                                ? processor.imgdata.sizes.iheight
                                : processor.imgdata.sizes.height;
    metadata.sourceSize = {imageWidth, imageHeight};
    if (includeSensorProperties) addSensorProperties(metadata, processor);
    return metadata;
}

ImageOrientation cameraOrientation(int flip) {
    switch (flip) {
    case 3:
        return ImageOrientation::Rotate180;
    case 5:
        return ImageOrientation::Rotate270Clockwise;
    case 6:
        return ImageOrientation::Rotate90Clockwise;
    default:
        return ImageOrientation::Normal;
    }
}

bool hasChannelBlackCalibration(const libraw_colordata_t& color) {
    return std::any_of(std::begin(color.cblack) + 1, std::begin(color.cblack) + 4,
                       [&](unsigned level) { return level != color.cblack[0]; }) ||
           (color.cblack[4] != 0 && color.cblack[5] != 0);
}

QRect cameraImageArea(const libraw_data_t& data) {
    const auto& sizes = data.sizes;
    const QRect active(0, 0, sizes.width, sizes.height);
    if (data.idata.dng_version) {
        // LibRaw exposes DNG DefaultCrop and DefaultUserCrop in absolute sensor coordinates.
        for (int index : {1, 0}) {
            const auto& crop = sizes.raw_inset_crops[index];
            const QRect relative(int(crop.cleft) - sizes.left_margin, int(crop.ctop) - sizes.top_margin,
                                 crop.cwidth, crop.cheight);
            if (relative.isValid() && active.contains(relative)) return relative;
        }
    }
    return active;
}

QImage cropDevelopedImage(QImage image, const QRect& area, const QSize& active, ImageOrientation orientation) {
    if (area == QRect(QPoint{}, active) || image.isNull()) return image;
    const QSize oriented = orientedImageSize(active, orientation);
    auto inverse = orientation;
    if (orientation == ImageOrientation::Rotate90Clockwise) inverse = ImageOrientation::Rotate270Clockwise;
    if (orientation == ImageOrientation::Rotate270Clockwise) inverse = ImageOrientation::Rotate90Clockwise;
    const QPoint first = displayToSourcePixel(area.topLeft(), oriented, inverse);
    const QPoint last = displayToSourcePixel(area.bottomRight(), oriented, inverse);
    const QRect whole(QPoint(std::min(first.x(), last.x()), std::min(first.y(), last.y())),
                      QPoint(std::max(first.x(), last.x()), std::max(first.y(), last.y())));
    const auto x = [&](int column) { return qRound(double(column) * image.width() / oriented.width()); };
    const auto y = [&](int row) { return qRound(double(row) * image.height() / oriented.height()); };
    const int left = std::min(x(whole.left()), image.width() - 1);
    const int top = std::min(y(whole.top()), image.height() - 1);
    return image.copy(left, top, std::max(1, x(whole.right() + 1) - left),
                      std::max(1, y(whole.bottom() + 1) - top));
}

std::optional<RawImageParameters> rawParametersFor(LibRaw& processor, const QRect& area, bool applyOrientation) {
    const auto& sizes = processor.imgdata.sizes;
    const int width = area.width();
    const int height = area.height();
    const auto& identity = processor.imgdata.idata;
    if (width <= 0 || height <= 0 || identity.filters == 0 || identity.filters == 9 || identity.colors != 3) {
        return std::nullopt;
    }

    const int x = area.x(), y = area.y();
    const auto colorAt = [&](int dy, int dx) {
        const int channel = processor.COLOR(y + dy, x + dx);
        return channel == 3 ? 1 : channel;
    };
    const int cfa[4] = {colorAt(0, 0), colorAt(0, 1), colorAt(1, 0), colorAt(1, 1)};
    BayerPattern pattern;
    if (cfa[0] == 0 && cfa[1] == 1 && cfa[2] == 1 && cfa[3] == 2) {
        pattern = BayerPattern::RGGB;
    } else if (cfa[0] == 1 && cfa[1] == 0 && cfa[2] == 2 && cfa[3] == 1) {
        pattern = BayerPattern::GRBG;
    } else if (cfa[0] == 1 && cfa[1] == 2 && cfa[2] == 0 && cfa[3] == 1) {
        pattern = BayerPattern::GBRG;
    } else if (cfa[0] == 2 && cfa[1] == 1 && cfa[2] == 1 && cfa[3] == 0) {
        pattern = BayerPattern::BGGR;
    } else {
        return std::nullopt;
    }

    RawImageParameters parameters;
    parameters.size = {width, height};
    parameters.format = RawPixelFormat::Raw16;
    parameters.rowStride =
        sizes.raw_pitch > 0 ? static_cast<qsizetype>(sizes.raw_pitch)
                            : static_cast<qsizetype>(width) * static_cast<qsizetype>(sizeof(quint16));
    parameters.bayerPattern = pattern;
    parameters.demosaic = true;
    parameters.orientation = applyOrientation ? cameraOrientation(sizes.flip) : ImageOrientation::Normal;

    const unsigned maximum = processor.imgdata.color.maximum;
    if (maximum > 0 && maximum <= 65535) {
        parameters.whiteLevel = static_cast<int>(maximum);
        int validBits = 0;
        while (validBits < 16 && ((1u << validBits) - 1u) < maximum) {
            ++validBits;
        }
        parameters.validBitsOverride = validBits;
    }
    const auto& color = processor.imgdata.color;
    const auto uniformBlack = hasChannelBlackCalibration(color) ? 0u : color.cblack[0];
    parameters.blackLevel = static_cast<int>(std::min<quint64>(quint64(color.black) + uniformBlack, 65534u));
    if (parameters.whiteLevel > parameters.blackLevel) {
        parameters.whiteLevel = std::max(parameters.whiteLevel, parameters.blackLevel + 1);
    }

    // Canon often stores gains with green at 1024. The renderer expects relative RGB gains.
    const auto& multipliers = processor.imgdata.color.cam_mul;
    const double green = multipliers[1];
    if (std::isfinite(green) && green > 0.0) {
        const double red = multipliers[0] / green;
        const double blue = multipliers[2] / green;
        if (std::isfinite(red) && red > 0.0 && red <= 64.0 &&
            std::isfinite(blue) && blue > 0.0 && blue <= 64.0) {
            parameters.whiteBalanceGains = {red, 1.0, blue};
        }
    }

    bool validMatrix = true;
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            const float value = processor.imgdata.color.rgb_cam[row][column];
            validMatrix = validMatrix && std::isfinite(value);
            parameters.colorCorrectionMatrix[static_cast<std::size_t>(row * 3 + column)] = value;
        }
    }
    if (!validMatrix) {
        parameters.colorCorrectionMatrix = {1.0, 0.0, 0.0, 0.0, 1.0, 0.0,
                                            0.0, 0.0, 1.0};
    }
    return parameters;
}

DecodeResult frameFromImage(QImage image, ImageMetadata metadata,
                            std::optional<RawImageParameters> rawParameters,
                            ImageOrientation orientation,
                            const QSize& maximumSize,
                            const std::shared_ptr<const PlaneBufferSet>& mosaic = {},
                            bool sourceSamplesPending = false,
                            bool applicationDeveloped = false) {
    if (image.isNull()) {
        return {{}, QStringLiteral("LibRaw produced an empty image")};
    }
    const QSize sensorLimit = orientedImageSize(maximumSize, orientation);
    if (!sensorLimit.isEmpty() &&
        (image.width() > sensorLimit.width() || image.height() > sensorLimit.height())) {
        image = image.scaled(sensorLimit, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    if (orientation != ImageOrientation::Normal) {
        const QColorSpace colorSpace = image.colorSpace();
        image = orientedImage(std::move(image), orientation);
        image.setColorSpace(colorSpace);
    }
    const bool highBitDepth = image.depth() > 32;
    if (image.format() != (highBitDepth ? QImage::Format_RGBA64 : QImage::Format_RGBA8888)) {
        image = image.convertToFormat(highBitDepth ? QImage::Format_RGBA64
                                                   : QImage::Format_RGBA8888);
    }
    auto frame = std::make_shared<ImageFrame>();
    frame->descriptor.size = image.size();
    frame->descriptor.sampleType = SampleType::UInt;
    // A retained mosaic is the frame's sample storage, so it reports the sensor's own depth.
    frame->descriptor.storageBits = mosaic ? 16 : (highBitDepth ? 16 : 8);
    frame->descriptor.validBits =
        mosaic && rawParameters ? rawParameters->validBits() : (highBitDepth ? 16 : 8);
    frame->descriptor.layout = mosaic ? PixelLayout::Bayer : PixelLayout::Interleaved;
    frame->descriptor.channelOrder = mosaic ? ChannelOrder::Bayer : ChannelOrder::RGBA;
    frame->metadata = std::move(metadata);
    frame->rawParameters = std::move(rawParameters);
    const bool rendersSensorMosaic = frame->rawParameters && !frame->rawParameters->demosaic;
    const bool rendersApplicationRgb = applicationDeveloped;
    if (rendersSensorMosaic) {
        frame->descriptor.sourceColor.colorSpace = QStringLiteral("CFA sensor samples");
        frame->descriptor.sourceColor.primaries = QStringLiteral("Sensor CFA");
        frame->descriptor.sourceColor.transferFunction = QStringLiteral("Linear sensor values");
        frame->descriptor.sourceColor.matrixCoefficients = QStringLiteral("None");
        applyDisplayColor(frame->descriptor.displayColor, true);
        frame->descriptor.displayColor.matrixCoefficients =
            QStringLiteral("CFA false-colour channel mapping");
    } else if (rendersApplicationRgb) {
        frame->descriptor.sourceColor.colorSpace = QStringLiteral("CFA sensor samples");
        frame->descriptor.sourceColor.primaries = QStringLiteral("Sensor CFA");
        frame->descriptor.sourceColor.transferFunction = QStringLiteral("Linear sensor values");
        frame->descriptor.sourceColor.matrixCoefficients = QStringLiteral("None");
        // User-defined develop transform: CCM contract primaries with the requested gamma.
        frame->descriptor.displayColor.colorSpace = QStringLiteral("User-defined RGB");
        frame->descriptor.displayColor.primaries = QStringLiteral("sRGB / BT.709 (CCM contract)");
        frame->descriptor.displayColor.transferFunction = QStringLiteral("Power %1")
            .arg(frame->rawParameters->displayGamma, 0, 'g', 6);
        frame->descriptor.displayColor.matrixCoefficients = QStringLiteral("RGB");
    } else {
        frame->descriptor.sourceColor.colorSpace = QStringLiteral("Camera RAW");
        frame->descriptor.sourceColor.primaries = QStringLiteral("Camera native");
        frame->descriptor.sourceColor.transferFunction = QStringLiteral("Linear sensor values");
        frame->descriptor.sourceColor.matrixCoefficients = QStringLiteral("Camera matrix");
        applyDisplayColor(frame->descriptor.displayColor);
        frame->descriptor.displayColor.matrixCoefficients = QStringLiteral("Camera matrix");
    }
    frame->sourceSamplesPending = sourceSamplesPending;
    if (mosaic) {
        auto planes = std::make_shared<PlaneBufferSet>(*mosaic);
        planes->displayImage = image;
        frame->storage = std::shared_ptr<const PlaneBufferSet>(std::move(planes));
    } else {
        frame->storage = std::move(image);
    }
    return {std::move(frame), {}};
}

// Copies the sensor mosaic LibRaw unpacked so exact probes can read true sensor samples.
// The probe uses the same active rectangle as LibRaw, starting at (left_margin, top_margin).
std::shared_ptr<PlaneBufferSet> mosaicPlanes(LibRaw& processor,
                                            const RawImageParameters& parameters, const QRect& imageArea) {
    const auto& sizes = processor.imgdata.sizes;
    const auto& rawdata = processor.imgdata.rawdata;
    const unsigned filters = processor.imgdata.idata.filters;
    // Monochrome, X-Trans, and multi-channel RAW variants have no plain 2x2 Bayer mosaic.
    if (!rawdata.raw_image || filters == 0 || filters == 9 || sizes.width == 0 ||
        sizes.height == 0 || sizes.raw_pitch < sizeof(quint16) ||
        (sizes.raw_pitch % sizeof(quint16)) != 0) {
        return {};
    }
    const QRect crop = imageArea.translated(sizes.left_margin, sizes.top_margin);
    const QByteArray packed = packedMosaicPlane(
        reinterpret_cast<const quint16*>(rawdata.raw_image),
        static_cast<qsizetype>(sizes.raw_pitch / sizeof(quint16)),
        QSize(sizes.raw_width, sizes.raw_height), crop, parameters.littleEndian, 16);
    if (packed.isEmpty()) {
        return {};
    }
    auto planes = std::make_shared<PlaneBufferSet>();
    planes->planes = {{0, static_cast<qsizetype>(crop.width()) * 2, packed.size()}};
    planes->storage = packed;
    // LibRaw's processed bitmap stays the rendered image; the mosaic only feeds exact probes.
    planes->renderFromDisplayImage = true;
    return planes;
}

void applyProcessingParameters(RawImageParameters& parameters, const RawImageParameters& requested) {
    parameters.demosaic = requested.demosaic;
    parameters.bayerSampling = requested.bayerSampling;
    parameters.blackLevel = requested.blackLevel;
    parameters.whiteLevel = requested.whiteLevel;
    parameters.validBitsOverride = requested.validBitsOverride;
    parameters.whiteBalanceGains = requested.whiteBalanceGains;
    parameters.colorCorrectionMatrix = requested.colorCorrectionMatrix;
    parameters.displayGamma = requested.displayGamma;
}

DecodeResult decodeWithLibRaw(const DecodeRequest& request) {
    const QFileInfo sourceInfo(request.path);
    const bool applyOrientation = QtImageDecoder::autoOrientationEnabled();
    const bool applyEmbeddedColor = EncodedColorManagement::isEnabled();
    const QString sourceKey = QStringLiteral("camera:%1:%2:%3:%4:%5")
        .arg(sourceInfo.absoluteFilePath()).arg(sourceInfo.size()).arg(sourceInfo.lastModified().toMSecsSinceEpoch())
        .arg(applyOrientation).arg(applyEmbeddedColor);
    // Reuse only routes that already develop the sensor plane. A plain preview
    // still uses the camera's embedded rendering, as it did before source caching.
    if (request.sourceCache && (request.purpose == DecodePurpose::Full ||
                               (request.purpose == DecodePurpose::Preview && request.rawParameters))) {
        if (const auto source = request.sourceCache->get(sourceKey); source && source->planes && source->parameters) {
            auto parameters = *source->parameters;
            if (request.rawParameters) applyProcessingParameters(parameters, *request.rawParameters);
            // Demosaiced Full frames use a bounded CPU fallback and native GPU planes.
            // CFA display instead uploads this image, so an unbounded Full request must
            // keep its native size, exactly like a fresh LibRaw decode.
            const QSize maximum = request.maximumSize.isEmpty() && parameters.demosaic &&
                                  !request.requireDisplayImage
                ? QSize(960, 720) : request.maximumSize;
            const QSize size = maximum.isEmpty() ? QSize{}
                : parameters.size.scaled(orientedImageSize(maximum, parameters.orientation),
                                         Qt::KeepAspectRatio);
            if (!request.prepareAllocation(estimatedPixelBytes(
                    size.isEmpty() ? parameters.size : size, 32)))
                return {{}, QStringLiteral("Cancelled")};
            const auto cancelled = [&request] { return request.isCancelled(); };
            QImage image = parameters.demosaic
                ? renderBayerImage(source->planes->storage, parameters, size, cancelled)
                : cfaMosaicImage(source->planes->storage, parameters, size, cancelled);
            if (request.isCancelled()) return {{}, QStringLiteral("Cancelled")};
            if (!image.isNull()) {
                const bool full = request.purpose == DecodePurpose::Full;
                auto planes = std::make_shared<PlaneBufferSet>(*source->planes);
                planes->renderFromDisplayImage = !full || !parameters.demosaic;
                return frameFromImage(std::move(image), source->metadata, parameters,
                                      parameters.orientation, {},
                                      full ? planes : nullptr, !full, parameters.demosaic);
            }
            // Preserve LibRaw's fallback if this sensor/parameter combination
            // cannot be developed by the application's Bayer renderer.
        }
    }
    // LibRaw is a large object (roughly 800 KiB in supported releases). Keep it
    // off the smaller platform worker-thread stack while retaining RAII cleanup.
    if (request.isCancelled()) return {{}, QStringLiteral("Cancelled")};
    auto processor = std::make_unique<LibRaw>();
    processor->set_progress_handler([](void* context, enum LibRaw_progress, int, int) {
        return static_cast<const DecodeRequest*>(context)->isCancelled() ? 1 : 0;
    }, const_cast<DecodeRequest*>(&request));
    const auto openForDevelopment = [&] {
        // LibRaw chooses the file's camera matrix during open_file().
        processor->imgdata.params.use_camera_wb = 1;
        processor->imgdata.params.use_camera_matrix = applyEmbeddedColor ? 1 : 0;
        return openFile(*processor, request.path);
    };
    const int opened = openForDevelopment();
    if (opened != LIBRAW_SUCCESS) {
        return {{}, QStringLiteral("LibRaw could not open %1: %2")
                        .arg(sourceInfo.fileName(), libRawError(opened))};
    }
    ImageMetadata metadata = metadataFor(request.path, *processor, request.purpose == DecodePurpose::Full);
    const QRect declaredArea = cameraImageArea(processor->imgdata);
    const QSize declaredActive(processor->imgdata.sizes.width, processor->imgdata.sizes.height);
    const bool hasDefaultCrop = declaredArea.size() != declaredActive;
    std::optional<RawImageParameters> rawParameters = rawParametersFor(*processor, declaredArea, applyOrientation);
    // The RAW parameters editor owns processing values and the optional Quad Bayer sampling
    // override; the file owns dimensions and the base CFA phase.
    if (rawParameters && request.rawParameters) {
        applyProcessingParameters(*rawParameters, *request.rawParameters);
    }
    const ImageOrientation fileOrientation = cameraOrientation(processor->imgdata.sizes.flip);
    const ImageOrientation orientation = applyOrientation ? fileOrientation : ImageOrientation::Normal;
    metadata.sourceSize = orientedImageSize(hasDefaultCrop ? declaredArea.size() : metadata.sourceSize, orientation);

    // Un-demosaiced camera RAW renders the sensor mosaic itself, exactly like the headerless RAW
    // path, instead of LibRaw's already-demosaiced bitmap. Preview frames drop the mosaic so the
    // probe can promote them to a full decode that addresses the plane directly; grid thumbnails
    // keep the fast embedded preview.
    const bool developPreview = rawParameters && request.rawParameters &&
        request.purpose == DecodePurpose::Preview;

    if (request.purpose != DecodePurpose::Full && !developPreview) {
        const QSize thumbSize(processor->imgdata.thumbnail.twidth, processor->imgdata.thumbnail.theight);
        if (!request.prepareAllocation(estimatedPixelBytes(
                thumbSize.isValid() ? thumbSize : metadata.sourceSize, 48)))
            return {{}, QStringLiteral("Cancelled")};
        if (!hasDefaultCrop && (applyOrientation || fileOrientation == ImageOrientation::Normal) &&
            processor->unpack_thumb() == LIBRAW_SUCCESS) {
            int imageError = LIBRAW_SUCCESS;
            ProcessedImage processed(processor->dcraw_make_mem_thumb(&imageError));
            if (processed && imageError == LIBRAW_SUCCESS) {
                QString conversionError;
                bool hasExifTransform = false;
                QImage image = processedImageToQImage(*processed, &conversionError, request,
                                                      &hasExifTransform);
                if (!image.isNull()) {
                    EncodedColorManagement::normalizeToDisplay(image, metadata);
                    // Some embedded JPEGs are already oriented in their pixels.
                    const bool alreadyOriented = hasExifTransform ||
                        (orientedImageSize(image.size(), orientation) != image.size() &&
                         (image.width() > image.height()) ==
                             (metadata.sourceSize.width() > metadata.sourceSize.height()));
                    return frameFromImage(std::move(image), std::move(metadata),
                                          std::move(rawParameters),
                                          alreadyOriented ? ImageOrientation::Normal : orientation,
                                          request.maximumSize, {},
                                          request.purpose == DecodePurpose::Preview);
                }
            }
        }

        // A RAW without an embedded preview still remains viewable. Reopen because
        // failed thumbnail extraction can leave the processor in a progressed state.
        processor->recycle();
        const int reopened = openForDevelopment();
        if (reopened != LIBRAW_SUCCESS) {
            return {{}, QStringLiteral("LibRaw could not reopen the RAW file: %1")
                            .arg(libRawError(reopened))};
        }
        processor->imgdata.params.half_size = 1;
    }

    // Sixteen-bit output keeps the decoder's real sample depth for RAW inspection instead of
    // quantizing every value to 8-bit display units.
    processor->imgdata.params.output_bps = 16;
    processor->imgdata.params.output_color = 1;
    processor->imgdata.params.gamm[0] = 1.0 / 2.4;
    processor->imgdata.params.gamm[1] = 12.92;
    if (!applyOrientation) processor->imgdata.params.user_flip = 0;
    if (request.isCancelled()) return {{}, QStringLiteral("Cancelled")};
    const QSize rawSize(processor->imgdata.sizes.raw_width, processor->imgdata.sizes.raw_height);
    if (!request.prepareAllocation(estimatedPixelBytes(rawSize, 48)))
        return {{}, QStringLiteral("Cancelled")};
    int code = processor->unpack();
    if (code != LIBRAW_SUCCESS) {
        return {{}, QStringLiteral("LibRaw decode failed for %1: %2")
                        .arg(sourceInfo.fileName(), libRawError(code))};
    }
    // LibRaw can measure the black level from masked pixels only after unpacking.
    const QSize active(processor->imgdata.sizes.width, processor->imgdata.sizes.height);
    const QRect area = cameraImageArea(processor->imgdata);
    if (area.size() != active) metadata.sourceSize = orientedImageSize(area.size(), orientation);
    const auto nativeProcessing = rawParametersFor(*processor, area, applyOrientation);
    const bool channelBlack = hasChannelBlackCalibration(processor->imgdata.color);
    // LibRaw can apply channel-specific and repeating black corrections. Keep its developed
    // bitmap for files requiring them, with the unmodified mosaic retained for source probing.
    const bool useLibRawDevelopment = channelBlack && !request.rawParameters;
    rawParameters = nativeProcessing;
    if (rawParameters && request.rawParameters) {
        applyProcessingParameters(*rawParameters, *request.rawParameters);
    }
    std::shared_ptr<PlaneBufferSet> mosaic;
    if (rawParameters && (request.purpose == DecodePurpose::Full || developPreview)) {
        mosaic = mosaicPlanes(*processor, *rawParameters, area);
        if (mosaic) {
            rawParameters->rowStride = static_cast<qsizetype>(area.width()) * 2;
            rawParameters->chromaStride = 0;
            rawParameters->msbAligned = false;
        }
    }

    if (mosaic && rawParameters && !channelBlack && request.sourceCache && !request.isCancelled()) {
        auto source = std::make_shared<SourceFrame>();
        source->planes = std::make_shared<PlaneBufferSet>(*mosaic);
        source->parameters = rawParameters;
        // Cache the file's defaults, not the display overrides of the request
        // that happened to populate the cache. Keep the cropped geometry/CFA.
        if (source->parameters && nativeProcessing)
            applyProcessingParameters(*source->parameters, *nativeProcessing);
        source->metadata = metadata;
        request.sourceCache->put(sourceKey, std::move(source));
    }

    if (mosaic && rawParameters && !rawParameters->demosaic) {
        const QSize bounded = request.maximumSize.isEmpty()
                                  ? QSize{}
                                  : rawParameters->size.scaled(
                                        orientedImageSize(request.maximumSize, orientation),
                                        Qt::KeepAspectRatio);
        QImage falseColour = cfaMosaicImage(mosaic->storage, *rawParameters, bounded,
                                            [&request] { return request.isCancelled(); });
        if (!falseColour.isNull()) {
            // LibRaw's demosaic and colour conversion are skipped entirely: the mosaic is the
            // image, and the same parameter set drives the pixel probe.
            const bool attachMosaic = request.purpose == DecodePurpose::Full;
            return frameFromImage(std::move(falseColour), std::move(metadata),
                                  std::move(rawParameters), orientation, {},
                                  attachMosaic ? mosaic : nullptr,
                                  request.purpose != DecodePurpose::Full);
        }
    }

    if (mosaic && rawParameters && rawParameters->demosaic && !useLibRawDevelopment &&
        (request.purpose == DecodePurpose::Full || developPreview)) {
        const QSize maximum = request.maximumSize.isEmpty() && !request.requireDisplayImage ? QSize(960, 720)
                                                             : request.maximumSize;
        const QSize fallbackSize = maximum.isEmpty() ? QSize{}
            : rawParameters->size.scaled(orientedImageSize(maximum, orientation),
                                         Qt::KeepAspectRatio);
        QImage developed = renderBayerImage(mosaic->storage, *rawParameters, fallbackSize,
                                             [&request] { return request.isCancelled(); });
        if (!developed.isNull()) {
            // Keep a bounded CPU fallback, but let the full-resolution GPU path consume the
            // same sensor plane and parameters. Unlike dcraw_process(), this honors edits to
            // white balance, CCM, black/white levels, and display gamma.
            const bool attachMosaic = request.purpose == DecodePurpose::Full;
            mosaic->renderFromDisplayImage = !attachMosaic;
            return frameFromImage(std::move(developed), std::move(metadata),
                                  std::move(rawParameters), orientation, {},
                                  attachMosaic ? mosaic : nullptr,
                                  !attachMosaic, true);
        }
    }

    code = processor->dcraw_process();
    if (code != LIBRAW_SUCCESS) {
        return {{}, QStringLiteral("LibRaw decode failed for %1: %2")
                        .arg(QFileInfo(request.path).fileName(), libRawError(code))};
    }

    int imageError = LIBRAW_SUCCESS;
    ProcessedImage processed(processor->dcraw_make_mem_image(&imageError));
    if (!processed || imageError != LIBRAW_SUCCESS) {
        return {{}, QStringLiteral("LibRaw image conversion failed: %1")
                        .arg(libRawError(imageError))};
    }
    QString conversionError;
    QImage image = processedImageToQImage(*processed, &conversionError, request);
    if (image.isNull()) {
        return {{}, conversionError};
    }
    image = cropDevelopedImage(std::move(image), area, active, orientation);
    // A full decode is the final answer: the frame either carries source planes, the sensor
    // mosaic, or the decoder's own processed samples. Only bounded proxies stay pending.
    DecodeResult result = frameFromImage(std::move(image), std::move(metadata),
                                         std::move(rawParameters), ImageOrientation::Normal,
                                         request.maximumSize, mosaic,
                                         request.purpose != DecodePurpose::Full);
    return result;
}

#endif

} // namespace

bool CameraRawDecoder::isAvailable() {
#if MVPVIEW_HAS_LIBRAW
    return true;
#else
    return false;
#endif
}

QStringList CameraRawDecoder::supportedSuffixes() {
    return isAvailable() ? cameraRawSuffixes() : QStringList{};
}

QString CameraRawDecoder::cacheIdentity() const {
#if MVPVIEW_HAS_LIBRAW
    return QStringLiteral("camera-raw-v11|libraw-%1|display-%2|orientation-%3|embedded-%4")
        .arg(QString::fromLatin1(libraw_version()),
             displayColorSpaceKey(currentDisplayColorSpace()))
        .arg(QtImageDecoder::autoOrientationEnabled()).arg(EncodedColorManagement::isEnabled());
#else
    return QStringLiteral("camera-raw-disabled");
#endif
}

DecodeExecutionMode CameraRawDecoder::executionMode(const QString& path) const {
    return canDecode(path) ? DecodeExecutionMode::Serialized : DecodeExecutionMode::Parallel;
}

bool CameraRawDecoder::canDecode(const QString& path) const {
    return isAvailable() && cameraRawSuffixes().contains(QFileInfo(path).suffix().toLower());
}

DecodeResult CameraRawDecoder::decode(const DecodeRequest& request) const {
    if (!canDecode(request.path)) {
        return {{},
                QStringLiteral("Camera RAW support is unavailable or the format is unsupported")};
    }
#if MVPVIEW_HAS_LIBRAW
    // Even the reentrant LibRaw build can delegate to optional codec dependencies
    // with shared process state. Serializing this adapter also bounds peak memory
    // when thumbnail, preview, and full requests for a large RAW overlap.
    static std::mutex decoderMutex;
    const std::scoped_lock lock(decoderMutex);
    try {
        return decodeWithLibRaw(request);
    } catch (const std::bad_alloc&) {
        return {{}, QStringLiteral("LibRaw could not allocate enough memory")};
    } catch (const std::exception& exception) {
        return {{}, QStringLiteral("LibRaw exception: %1")
                        .arg(QString::fromLocal8Bit(exception.what()))};
    } catch (...) {
        return {{}, QStringLiteral("Unknown LibRaw exception")};
    }
#else
    return {{}, QStringLiteral("This build does not include LibRaw")};
#endif
}

} // namespace mvpview
