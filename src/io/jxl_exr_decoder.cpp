#include "io/jxl_exr_decoder.h"

#include "io/encoded_color_management.h"
#include "io/qt_image_decoder.h"

#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfIO.h>
#include <OpenEXR/ImfInputPart.h>
#include <OpenEXR/ImfMultiPartInputFile.h>
#include <OpenEXR/ImfPartType.h>
#include <OpenEXR/ImfStandardAttributes.h>
#include <OpenEXR/ImfTiledInputPart.h>
#include <OpenEXR/OpenEXRConfig.h>
#include <jxl/decode_cxx.h>

#include <QColorSpace>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>

namespace mvpview {
namespace {

namespace Exr = OPENEXR_IMF_NAMESPACE;

const QString kJxlSuffix = QStringLiteral("jxl");
const QString kExrSuffix = QStringLiteral("exr");

QString jxlVersion() {
    const auto version = JxlDecoderVersion();
    return QStringLiteral("%1.%2.%3").arg(version / 1'000'000)
        .arg(version / 1'000 % 1'000).arg(version % 1'000);
}

QSize previewSize(QSize source, QSize maximum) {
    if (maximum.isEmpty() ||
        (source.width() <= maximum.width() && source.height() <= maximum.height())) {
        return source;
    }
    const QSize scaled = source.scaled(maximum, Qt::KeepAspectRatio);
    return scaled.expandedTo({1, 1});
}

ImageFrame makeFrame(const DecodeRequest& request, QSize source, const QString& format,
                     const QString& decoder, const QString& version) {
    const QFileInfo file(request.path);
    ImageFrame frame;
    frame.metadata.path = file.absoluteFilePath();
    frame.metadata.fileName = file.fileName();
    frame.metadata.fileSize = file.size();
    frame.metadata.modifiedAt = file.lastModified();
    frame.metadata.sourceSize = source;
    frame.metadata.format = format;
    frame.metadata.decoderName = decoder;
    frame.metadata.decoderVersion = version;
    applyDisplayColor(frame.descriptor.displayColor);
    return frame;
}

DecodeResult finishFrame(const DecodeRequest& request, ImageFrame frame, QImage image,
                         int sourceBits) {
    if (request.isCancelled()) return {{}, QStringLiteral("Cancelled")};
    if (image.isNull()) return {{}, QStringLiteral("Could not allocate decoded image")};
    auto& descriptor = frame.descriptor;
    descriptor.size = image.size();
    descriptor.sampleType = image.format() == QImage::Format_RGBA32FPx4
        ? SampleType::Float : SampleType::UInt;
    descriptor.storageBits = descriptor.sampleType == SampleType::Float
        ? 32 : image.depth() / 4;
    descriptor.validBits = std::min(sourceBits, descriptor.storageBits);
    frame.storage = std::move(image);
    return {std::make_shared<ImageFrame>(std::move(frame)), {}};
}

QColorSpace jxlColorSpace(const JxlDecoder* decoder) {
    constexpr auto target = JXL_COLOR_PROFILE_TARGET_DATA;
    constexpr size_t maximumProfileSize = 64 * 1024 * 1024;
    size_t profileSize = 0;
    if (JxlDecoderGetICCProfileSize(decoder, target, &profileSize) == JXL_DEC_SUCCESS &&
        profileSize > 0 && profileSize <= maximumProfileSize) {
        QByteArray profile(static_cast<qsizetype>(profileSize), Qt::Uninitialized);
        if (JxlDecoderGetColorAsICCProfile(decoder, target,
                reinterpret_cast<uint8_t*>(profile.data()), profileSize) == JXL_DEC_SUCCESS) {
            QColorSpace space = QColorSpace::fromIccProfile(profile);
            if (space.colorModel() == QColorSpace::ColorModel::Rgb) return space;
            // The decoder expands grayscale samples into RGB. Use the encoded gray
            // transfer curve on neutral RGB when Qt can describe it.
            if (space.colorModel() == QColorSpace::ColorModel::Gray &&
                space.transferFunction() != QColorSpace::TransferFunction::Custom) {
                return {QColorSpace::Primaries::SRgb, space.transferFunction(), space.gamma()};
            }
        }
    }

    JxlColorEncoding encoding{};
    if (JxlDecoderGetColorAsEncodedProfile(decoder, target, &encoding) != JXL_DEC_SUCCESS)
        return {};
    using Transfer = QColorSpace::TransferFunction;
    Transfer transfer = Transfer::SRgb;
    float gamma = 0.0f;
    switch (encoding.transfer_function) {
    case JXL_TRANSFER_FUNCTION_LINEAR: transfer = Transfer::Linear; break;
    case JXL_TRANSFER_FUNCTION_709: transfer = Transfer::Bt2020; break;
    case JXL_TRANSFER_FUNCTION_PQ: transfer = Transfer::St2084; break;
    case JXL_TRANSFER_FUNCTION_HLG: transfer = Transfer::Hlg; break;
    case JXL_TRANSFER_FUNCTION_GAMMA:
        if (encoding.gamma <= 0.0) return {};
        transfer = Transfer::Gamma;
        gamma = static_cast<float>(1.0 / encoding.gamma);
        break;
    case JXL_TRANSFER_FUNCTION_SRGB: break;
    default: return {};
    }
    if (encoding.color_space == JXL_COLOR_SPACE_GRAY)
        return {QColorSpace::Primaries::SRgb, transfer, gamma};
    if (encoding.color_space != JXL_COLOR_SPACE_RGB) return {};
    return {{encoding.white_point_xy[0], encoding.white_point_xy[1]},
            {encoding.primaries_red_xy[0], encoding.primaries_red_xy[1]},
            {encoding.primaries_green_xy[0], encoding.primaries_green_xy[1]},
            {encoding.primaries_blue_xy[0], encoding.primaries_blue_xy[1]}, transfer, gamma};
}

DecodeResult decodeJxl(QFile& file, const DecodeRequest& request) {
    // Keep the buffer alive until the decoder is destroyed, including on early returns.
    QByteArray input(64 * 1024, Qt::Uninitialized);
    const auto decoder = JxlDecoderMake(nullptr);
    const QString invalid = QStringLiteral("Could not decode JPEG XL image");
    if (!decoder ||
        JxlDecoderSetKeepOrientation(decoder.get(), QtImageDecoder::autoOrientationEnabled()
            ? JXL_FALSE : JXL_TRUE) != JXL_DEC_SUCCESS ||
        JxlDecoderSetUnpremultiplyAlpha(decoder.get(), JXL_TRUE) != JXL_DEC_SUCCESS ||
        JxlDecoderSubscribeEvents(decoder.get(), JXL_DEC_BASIC_INFO |
            JXL_DEC_COLOR_ENCODING | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS) {
        return {{}, invalid};
    }

    JxlBasicInfo info{};
    QColorSpace colorSpace;
    QImage image;
    QSize source;
    QSize output;
    JxlPixelFormat pixels{4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
    int sourceBits = 8;
    qsizetype inputSize = 0;
    bool endOfInput = false;
    while (!request.isCancelled()) {
        switch (JxlDecoderProcessInput(decoder.get())) {
        case JXL_DEC_NEED_MORE_INPUT: {
            if (endOfInput) return {{}, invalid};
            const auto remaining = static_cast<qsizetype>(JxlDecoderReleaseInput(decoder.get()));
            if (remaining > 0)
                std::memmove(input.data(), input.constData() + inputSize - remaining,
                             static_cast<size_t>(remaining));
            const qint64 read = file.read(input.data() + remaining, input.size() - remaining);
            if (read < 0) return {{}, file.errorString()};
            inputSize = remaining + static_cast<qsizetype>(read);
            if (JxlDecoderSetInput(decoder.get(), reinterpret_cast<const uint8_t*>(input.constData()),
                                   static_cast<size_t>(inputSize)) != JXL_DEC_SUCCESS) {
                return {{}, invalid};
            }
            if (file.atEnd()) {
                JxlDecoderCloseInput(decoder.get());
                endOfInput = true;
            } else if (read == 0) {
                return {{}, invalid};
            }
            break;
        }
        case JXL_DEC_BASIC_INFO:
            if (JxlDecoderGetBasicInfo(decoder.get(), &info) != JXL_DEC_SUCCESS ||
                info.xsize == 0 || info.ysize == 0 ||
                info.xsize > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
                info.ysize > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
                return {{}, invalid};
            }
            source = {static_cast<int>(info.xsize), static_cast<int>(info.ysize)};
            output = previewSize(source, request.maximumSize);
            sourceBits = static_cast<int>(std::max(info.bits_per_sample, info.alpha_bits));
            if (QtImageDecoder::preserveHighBitDepth()) {
                if (info.exponent_bits_per_sample || info.alpha_exponent_bits || sourceBits > 16)
                    pixels.data_type = JXL_TYPE_FLOAT;
                else if (sourceBits > 8)
                    pixels.data_type = JXL_TYPE_UINT16;
            }
            // Allow for libjxl's internal float planes as well as the decoded and scaled copies.
            if (!request.prepareAllocation(addedAllocationBytes(
                    estimatedPixelBytes(source, 64), estimatedPixelBytes(output, 32)))) {
                return {{}, QStringLiteral("Cancelled")};
            }
            break;
        case JXL_DEC_COLOR_ENCODING:
            colorSpace = jxlColorSpace(decoder.get());
            break;
        case JXL_DEC_NEED_IMAGE_OUT_BUFFER: {
            const auto format = pixels.data_type == JXL_TYPE_FLOAT ? QImage::Format_RGBA32FPx4 :
                pixels.data_type == JXL_TYPE_UINT16 ? QImage::Format_RGBA64 : QImage::Format_RGBA8888;
            image = QImage(source, format);
            if (image.isNull()) return {{}, QStringLiteral("Could not allocate JPEG XL image")};
            if (JxlDecoderSetImageOutBuffer(decoder.get(), &pixels, image.bits(),
                    static_cast<size_t>(image.sizeInBytes())) != JXL_DEC_SUCCESS) {
                return {{}, invalid};
            }
            break;
        }
        case JXL_DEC_FULL_IMAGE: {
            if (request.isCancelled()) return {{}, QStringLiteral("Cancelled")};
            // A coalesced JPEG XL frame is a complete canvas, including for animations.
            image.setColorSpace(colorSpace);
            if (source != output) image = image.scaled(output, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            auto frame = makeFrame(request, source, QStringLiteral("JXL"), QStringLiteral("libjxl"),
                                   jxlVersion());
            EncodedColorManagement::normalizeToDisplay(image, frame.metadata);
            if (frame.metadata.colorProfile) {
                frame.descriptor.sourceColor.colorSpace = frame.metadata.colorProfile->sourceDescription;
                frame.descriptor.sourceColor.primaries = QStringLiteral("Defined by ICC");
                frame.descriptor.sourceColor.transferFunction = QStringLiteral("Defined by ICC");
                if (!frame.metadata.colorProfile->converted)
                    frame.descriptor.displayColor = frame.descriptor.sourceColor;
            }
            return finishFrame(request, std::move(frame), std::move(image), sourceBits);
        }
        default: return {{}, invalid};
        }
    }
    return {{}, QStringLiteral("Cancelled")};
}

class ExrStream final : public Exr::IStream {
  public:
    explicit ExrStream(QFile& file) : Exr::IStream("OpenEXR"), file_(file), size_(file.size()) {}
    bool read(char data[], int count) override {
        if (count < 0 || count > size_ - file_.pos() || file_.read(data, count) != count)
            throw std::runtime_error("Unexpected end of OpenEXR file");
        return file_.pos() < size_;
    }
    uint64_t tellg() override { return static_cast<uint64_t>(file_.pos()); }
#if OPENEXR_VERSION_MAJOR > 3 || OPENEXR_VERSION_MINOR >= 4
    int64_t size() override { return size_; }
#endif
    void seekg(uint64_t offset) override {
        if (offset > static_cast<uint64_t>(size_) || !file_.seek(static_cast<qint64>(offset)))
            throw std::runtime_error("Invalid OpenEXR file offset");
    }

  private:
    QFile& file_;
    qint64 size_;
};

struct ExrChannels {
    std::array<std::string, 4> names;
    bool grayscale;
    bool hasAlpha;
    int sourceBits = 16;
};

std::optional<ExrChannels> exrChannels(const Exr::ChannelList& channels, const std::string& prefix) {
    const bool rgb = channels.findChannel(prefix + "R") || channels.findChannel(prefix + "G") ||
                     channels.findChannel(prefix + "B");
    const bool luminance = !rgb && channels.findChannel(prefix + "Y") &&
                           !channels.findChannel(prefix + "RY") && !channels.findChannel(prefix + "BY");
    if (!rgb && !luminance) return std::nullopt;

    ExrChannels result{{prefix + (luminance ? "Y" : "R"), prefix + "G", prefix + "B", prefix + "A"},
                       luminance, channels.findChannel(prefix + "A") != nullptr};
    for (const auto& name : result.names) {
        if (const auto* channel = channels.findChannel(name)) {
            if (channel->xSampling != 1 || channel->ySampling != 1) return std::nullopt;
            if (channel->type != Exr::HALF) result.sourceBits = 32;
        }
    }
    return result;
}

std::optional<ExrChannels> exrChannels(const Exr::ChannelList& channels) {
    if (auto color = exrChannels(channels, {})) return color;
    std::set<std::string> layers;
    channels.layers(layers);
    for (const auto& layer : layers) {
        if (auto color = exrChannels(channels, layer + '.')) return color;
    }
    return std::nullopt;
}

QColorSpace exrColorSpace(const Exr::Header& header, ImageMetadata& metadata) {
    if (Exr::hasChromaticities(header)) {
        const auto& c = Exr::chromaticities(header);
        const QColorSpace space({c.white.x, c.white.y}, {c.red.x, c.red.y},
                                {c.green.x, c.green.y}, {c.blue.x, c.blue.y},
                                QColorSpace::TransferFunction::Linear);
        if (space.isValid()) return space;
        metadata.colorWarning = QStringLiteral("Invalid OpenEXR chromaticities; assuming linear sRGB");
    }
    return QColorSpace(QColorSpace::SRgbLinear);
}

DecodeResult decodeExr(QFile& file, const DecodeRequest& request) {
    ExrStream stream(file);
    // The image loader already runs several independent decode workers.
    Exr::MultiPartInputFile input(stream, 0);
    int part = 0;
    std::optional<ExrChannels> channels;
    for (; part < input.parts(); ++part) {
        const auto& header = input.header(part);
        if (header.type() == Exr::SCANLINEIMAGE || header.type() == Exr::TILEDIMAGE) {
            channels = exrChannels(header.channels());
            if (channels) break;
        }
    }
    if (!channels) return {{}, QStringLiteral("No flat RGB or luminance OpenEXR image")};

    const auto& header = input.header(part);
    const auto& window = header.dataWindow();
    const qint64 width = qint64(window.max.x) - window.min.x + 1;
    const qint64 height = qint64(window.max.y) - window.min.y + 1;
    constexpr int maximumDimension = std::numeric_limits<int>::max();
    if (width <= 0 || height <= 0 || width > maximumDimension || height > maximumDimension)
        return {{}, QStringLiteral("Invalid OpenEXR dimensions")};
    const QSize source(static_cast<int>(width), static_cast<int>(height));
    const QSize output = previewSize(source, request.maximumSize);
    if (!request.prepareAllocation(addedAllocationBytes(
            estimatedPixelBytes(source, 32), estimatedPixelBytes(output, 32)))) {
        return {{}, QStringLiteral("Cancelled")};
    }

    QImage image(source, QImage::Format_RGBA32FPx4);
    if (image.isNull()) return {{}, QStringLiteral("Could not allocate OpenEXR image")};
    Exr::FrameBuffer buffer;
    for (size_t index = 0; index < channels->names.size(); ++index) {
        buffer.insert(channels->names[index], Exr::Slice::Make(Exr::FLOAT,
            image.bits() + index * sizeof(float), window, 4 * sizeof(float),
            static_cast<size_t>(image.bytesPerLine()), 1, 1, index == 3 ? 1.0 : 0.0));
    }

    if (header.type() == Exr::TILEDIMAGE) {
        Exr::TiledInputPart imagePart(input, part);
        imagePart.setFrameBuffer(buffer);
        for (int row = 0; row < imagePart.numYTiles() && !request.isCancelled(); ++row)
            imagePart.readTiles(0, imagePart.numXTiles() - 1, row, row);
    } else {
        Exr::InputPart imagePart(input, part);
        imagePart.setFrameBuffer(buffer);
        for (qint64 row = window.min.y; row <= window.max.y && !request.isCancelled(); row += 32) {
            imagePart.readPixels(static_cast<int>(row),
                                 static_cast<int>(std::min(row + 31, qint64(window.max.y))));
        }
    }

    for (int y = 0; (channels->grayscale || channels->hasAlpha) && y < image.height() &&
                    !request.isCancelled(); ++y) {
        auto* pixel = reinterpret_cast<float*>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x, pixel += 4) {
            if (channels->grayscale) pixel[2] = pixel[1] = pixel[0];
            // OpenEXR color channels are conventionally associated with alpha.
            if (channels->hasAlpha && pixel[3] > 0.0f && pixel[3] != 1.0f) {
                pixel[0] /= pixel[3];
                pixel[1] /= pixel[3];
                pixel[2] /= pixel[3];
            }
        }
    }
    if (request.isCancelled()) return {{}, QStringLiteral("Cancelled")};

    auto frame = makeFrame(request, source, QStringLiteral("EXR"), QStringLiteral("OpenEXR"),
                           QStringLiteral(OPENEXR_VERSION_STRING));
    const QColorSpace colorSpace = exrColorSpace(header, frame.metadata);
    frame.descriptor.sourceColor.transferFunction = QStringLiteral("Linear");
    if (colorSpace != QColorSpace(QColorSpace::SRgbLinear)) {
        frame.descriptor.sourceColor.colorSpace = QStringLiteral("OpenEXR chromaticities");
        frame.descriptor.sourceColor.primaries = QStringLiteral("Defined by chromaticities");
    } else {
        frame.descriptor.sourceColor.colorSpace = QStringLiteral("Linear sRGB");
    }
    image.setColorSpace(colorSpace);
    if (source != output) image = image.scaled(output, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const auto format = QtImageDecoder::preserveHighBitDepth()
        ? QImage::Format_RGBA32FPx4 : QImage::Format_RGBA8888;
    image.convertToColorSpace(displayQColorSpace(currentDisplayColorSpace()), format);
    return finishFrame(request, std::move(frame), std::move(image), channels->sourceBits);
}

} // namespace

QStringList JxlExrDecoder::supportedSuffixes() {
    return {kJxlSuffix, kExrSuffix};
}

bool JxlExrDecoder::supportsPath(const QString& path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == kJxlSuffix || suffix == kExrSuffix;
}

QString JxlExrDecoder::cacheIdentity() const {
    return QStringLiteral("jxl-exr-v1|jxl-%1|exr-%2|%3")
        .arg(jxlVersion(), QStringLiteral(OPENEXR_VERSION_STRING), QtImageDecoder{}.cacheIdentity());
}

bool JxlExrDecoder::canDecode(const QString& path) const {
    return supportsPath(path);
}

DecodeResult JxlExrDecoder::decode(const DecodeRequest& request) const {
    if (request.isCancelled()) return {{}, QStringLiteral("Cancelled")};
    if (!canDecode(request.path)) return {{}, QStringLiteral("Unsupported image format")};
    if (request.purpose == DecodePurpose::Region)
        return {{}, QStringLiteral("Regional decoding is unavailable for this format")};
    QFile file(request.path);
    if (!file.open(QIODevice::ReadOnly)) return {{}, file.errorString()};
    if (QFileInfo(request.path).suffix().compare(kJxlSuffix, Qt::CaseInsensitive) == 0)
        return decodeJxl(file, request);
    try {
        return decodeExr(file, request);
    } catch (const std::exception& error) {
        return {{}, QStringLiteral("Could not decode OpenEXR image: %1").arg(QString::fromUtf8(error.what()))};
    }
}

} // namespace mvpview
