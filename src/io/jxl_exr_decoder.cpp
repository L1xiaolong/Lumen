#include "io/jxl_exr_decoder.h"

#include "core/histogram_sampling.h"
#include "io/encoded_color_management.h"
#include "io/qt_image_decoder.h"

#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfChromaticities.h>
#include <OpenEXR/ImfIntAttribute.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfIO.h>
#include <OpenEXR/ImfInputPart.h>
#include <OpenEXR/ImfMultiPartInputFile.h>
#include <OpenEXR/ImfPartType.h>
#include <OpenEXR/ImfStandardAttributes.h>
#include <OpenEXR/ImfStringAttribute.h>
#include <OpenEXR/ImfTiledInputPart.h>
#include <OpenEXR/OpenEXRConfig.h>
#include <jxl/decode_cxx.h>

#include <QColorSpace>
#include <QFile>
#include <QFileInfo>
#include <QTransform>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace mvpview {
namespace {

namespace Exr = OPENEXR_IMF_NAMESPACE;

const QString kJxlSuffix = QStringLiteral("jxl");
const QString kExrSuffix = QStringLiteral("exr");
// The viewer's current window targets are SDR. This is a reference white, not a measurement
// of the user's monitor or an assertion that the window has HDR output.
constexpr float kSdrReferenceWhite = 100.0f;

float sdrHighlightScale(float luminance) {
    if (!std::isfinite(luminance) || luminance <= 0.75f) return 1.0f;
    const float excess = luminance - 0.75f;
    return (0.75f + 0.25f * (excess / (excess + 0.25f))) / luminance;
}

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

QSize outputSize(QSize samples, QSize presentation, const DecodeRequest& request) {
    return previewSize(request.purpose == DecodePurpose::Thumbnail ? presentation : samples,
                       request.maximumSize);
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

QColorSpace jxlColorSpace(const JxlColorEncoding& encoding) {
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

QColorSpace jxlColorSpace(const JxlDecoder* decoder, const JxlColorEncoding& encoding) {
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

    return jxlColorSpace(encoding);
}

Exr::Chromaticities targetChromaticities(DisplayColorSpace target);

float pqLuminance(float value) {
    constexpr float m1 = 2610.0f / 16384.0f, m2 = 2523.0f / 32.0f;
    constexpr float c1 = 3424.0f / 4096.0f, c2 = 2413.0f / 128.0f, c3 = 2392.0f / 128.0f;
    const float p = std::pow(value, 1.0f / m2);
    return std::pow(std::max(0.0f, p - c1) / (c2 - c3 * p), 1.0f / m1) * (10000.0f / kSdrReferenceWhite);
}

float hlgSceneLight(float value) {
    return value <= 0.5f ? value * value / 3.0f
                         : (std::exp((value - 0.55991073f) / 0.17883277f) + 0.28466892f) / 12.0f;
}

void applyJxlHdr(QImage& image, const QColorSpace& sourceSpace, bool hlg, float intensityTarget,
                 ImageFrame& frame, const DecodeRequest& request) {
    constexpr int steps = 4096;
    std::array<float, steps + 1> linear{};
    for (int i = 0; i <= steps; ++i) {
        const float encoded = static_cast<float>(i) / steps;
        linear[i] = hlg ? hlgSceneLight(encoded) : pqLuminance(encoded);
    }
    for (int y = 0; y < image.height() && !request.isCancelled(); ++y) {
        auto* pixel = reinterpret_cast<float*>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x, pixel += 4) {
            for (int channel = 0; channel < 3; ++channel) {
                const float value = pixel[channel];
                const float index = std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) * steps : 0.0f;
                const int lower = std::min(static_cast<int>(index), steps - 1);
                pixel[channel] = std::lerp(linear[lower], linear[lower + 1], index - lower);
            }
        }
    }
    if (request.isCancelled()) return;
    const auto target = currentDisplayColorSpace();
    const auto display = displayQColorSpace(target);
    const auto targetLinear = display.withTransferFunction(QColorSpace::TransferFunction::Linear);
    image.setColorSpace(sourceSpace.withTransferFunction(QColorSpace::TransferFunction::Linear));
    image.convertToColorSpace(targetLinear);
    const auto targetToXyz = Exr::RGBtoXYZ(targetChromaticities(target), 1.0f);
    const float peak = std::isfinite(intensityTarget) && intensityTarget > 0.0f ? intensityTarget : 1000.0f;
    const float hlgGamma = std::clamp(1.2f + 0.42f * std::log10(peak / 1000.0f), 1.0f, 1.5f);
    for (int y = 0; y < image.height() && !request.isCancelled(); ++y) {
        auto* pixel = reinterpret_cast<float*>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x, pixel += 4) {
            const float luminance = (Imath::V3f(pixel[0], pixel[1], pixel[2]) * targetToXyz).y;
            const float exposure = hlg ? std::pow(std::max(0.0f, luminance), hlgGamma - 1.0f) *
                                         (peak / kSdrReferenceWhite) : 1.0f;
            const float scale = exposure * sdrHighlightScale(luminance * exposure);
            for (int channel = 0; channel < 3; ++channel) pixel[channel] *= scale;
        }
    }
    if (request.isCancelled()) return;
    image.convertToColorSpace(display, QtImageDecoder::preserveHighBitDepth() ? QImage::Format_RGBA32FPx4
                                                                              : QImage::Format_RGBA8888);
    frame.descriptor.sourceColor.colorSpace = sourceSpace.description();
    frame.descriptor.sourceColor.transferFunction = hlg ? QStringLiteral("HLG") : QStringLiteral("ST 2084 (PQ)");
}

QImage orientJxl(QImage image, JxlOrientation orientation) {
    switch (orientation) {
    case JXL_ORIENT_FLIP_HORIZONTAL: return image.flipped(Qt::Horizontal);
    case JXL_ORIENT_ROTATE_180: return image.flipped(Qt::Horizontal | Qt::Vertical);
    case JXL_ORIENT_FLIP_VERTICAL: return image.flipped(Qt::Vertical);
    case JXL_ORIENT_TRANSPOSE: return image.flipped(Qt::Horizontal).transformed(QTransform().rotate(270));
    case JXL_ORIENT_ROTATE_90_CW: return image.transformed(QTransform().rotate(90));
    case JXL_ORIENT_ANTI_TRANSPOSE: return image.flipped(Qt::Horizontal).transformed(QTransform().rotate(90));
    case JXL_ORIENT_ROTATE_90_CCW: return image.transformed(QTransform().rotate(270));
    default: return image;
    }
}

DecodeResult decodeJxl(QFile& file, const DecodeRequest& request) {
    // Keep the buffer alive until the decoder is destroyed, including on early returns.
    QByteArray input(64 * 1024, Qt::Uninitialized);
    const auto decoder = JxlDecoderMake(nullptr);
    const QString invalid = QStringLiteral("Could not decode JPEG XL image");
    if (!decoder ||
        JxlDecoderSetKeepOrientation(decoder.get(), JXL_TRUE) != JXL_DEC_SUCCESS ||
        JxlDecoderSetUnpremultiplyAlpha(decoder.get(), JXL_TRUE) != JXL_DEC_SUCCESS ||
        JxlDecoderSubscribeEvents(decoder.get(), JXL_DEC_BASIC_INFO |
            JXL_DEC_COLOR_ENCODING | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS) {
        return {{}, invalid};
    }

    JxlBasicInfo info{};
    QColorSpace colorSpace;
    JxlColorEncoding encoding{};
    bool hdr = false;
    QImage image;
    QSize encoded;
    QSize source;
    QSize presentation;
    QSize output;
    JxlPixelFormat pixels{4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
    int sourceBits = 8;
    qsizetype inputSize = 0;
    bool endOfInput = false;
    const bool applyOrientation = QtImageDecoder::autoOrientationEnabled();
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
            encoded = {static_cast<int>(info.xsize), static_cast<int>(info.ysize)};
            source = encoded;
            presentation = encoded;
            if (info.intrinsic_xsize > 0 && info.intrinsic_ysize > 0 &&
                info.intrinsic_xsize <= static_cast<uint32_t>(std::numeric_limits<int>::max()) &&
                info.intrinsic_ysize <= static_cast<uint32_t>(std::numeric_limits<int>::max())) {
                presentation = {static_cast<int>(info.intrinsic_xsize), static_cast<int>(info.intrinsic_ysize)};
            }
            if (applyOrientation && info.orientation >= JXL_ORIENT_TRANSPOSE) {
                source.transpose();
                presentation.transpose();
            }
            output = outputSize(source, presentation, request);
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
        case JXL_DEC_COLOR_ENCODING: {
            if (JxlDecoderGetColorAsEncodedProfile(decoder.get(), JXL_COLOR_PROFILE_TARGET_DATA,
                                                   &encoding) != JXL_DEC_SUCCESS) encoding = {};
            if (encoding.transfer_function == JXL_TRANSFER_FUNCTION_PQ ||
                encoding.transfer_function == JXL_TRANSFER_FUNCTION_HLG) {
                colorSpace = jxlColorSpace(encoding);
            }
            if (!colorSpace.isValid()) colorSpace = jxlColorSpace(decoder.get(), encoding);
            using Transfer = QColorSpace::TransferFunction;
            const auto transfer = colorSpace.transferFunction();
            hdr = EncodedColorManagement::isEnabled() && colorSpace.isValid() &&
                  (transfer == Transfer::St2084 || transfer == Transfer::Hlg);
            if (hdr) pixels.data_type = JXL_TYPE_FLOAT;
            break;
        }
        case JXL_DEC_NEED_IMAGE_OUT_BUFFER: {
            const auto format = pixels.data_type == JXL_TYPE_FLOAT ? QImage::Format_RGBA32FPx4 :
                pixels.data_type == JXL_TYPE_UINT16 ? QImage::Format_RGBA64 : QImage::Format_RGBA8888;
            image = QImage(encoded, format);
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
            auto frame = makeFrame(request, source, QStringLiteral("JXL"), QStringLiteral("libjxl"),
                                   jxlVersion());
            if (presentation != source) frame.descriptor.presentationSize = presentation;
            if (hdr) applyJxlHdr(image, colorSpace, colorSpace.transferFunction() == QColorSpace::TransferFunction::Hlg,
                                 info.intensity_target, frame, request);
            else image.setColorSpace(colorSpace);
            if (request.isCancelled()) return {{}, QStringLiteral("Cancelled")};
            if (applyOrientation) image = orientJxl(std::move(image), info.orientation);
            if (source != output) image = image.scaled(output, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            if (!hdr) EncodedColorManagement::normalizeToDisplay(image, frame.metadata);
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

struct ExrImagePart {
    int index;
    ExrChannels channels;
};

std::optional<ExrImagePart> findExrImage(Exr::MultiPartInputFile& input) {
    for (int index = 0; index < input.parts(); ++index) {
        const auto& header = input.header(index);
        if (header.type() == Exr::SCANLINEIMAGE || header.type() == Exr::TILEDIMAGE) {
            if (auto channels = exrChannels(header.channels()))
                return ExrImagePart{index, std::move(*channels)};
        }
    }
    return std::nullopt;
}

using Chromaticities = Exr::Chromaticities;

std::optional<Chromaticities> standardExrChromaticities(const std::string& id) {
    // ASWF Color Interop standard linear spaces. AP0 has a negative blue y coordinate
    // and therefore needs the EXR/XYZ matrix path even on Qt versions that reject it.
    const Imath::V2f d65(0.3127f, 0.3290f);
    const Imath::V2f d60(0.32168f, 0.33767f);
    if (id == "lin_rec709_scene") return Chromaticities{};
    if (id == "lin_ap0_scene")
        return Chromaticities({0.7347f, 0.2653f}, {0.0f, 1.0f}, {0.0001f, -0.0770f}, d60);
    if (id == "lin_ap1_scene")
        return Chromaticities({0.713f, 0.293f}, {0.165f, 0.830f}, {0.128f, 0.044f}, d60);
    if (id == "lin_p3d65_scene")
        return Chromaticities({0.680f, 0.320f}, {0.265f, 0.690f}, {0.150f, 0.060f}, d65);
    if (id == "lin_rec2020_scene")
        return Chromaticities({0.708f, 0.292f}, {0.170f, 0.797f}, {0.131f, 0.046f}, d65);
    if (id == "lin_adobergb_scene")
        return Chromaticities({0.640f, 0.330f}, {0.210f, 0.710f}, {0.150f, 0.060f}, d65);
    return std::nullopt;
}

Chromaticities targetChromaticities(DisplayColorSpace target) {
    switch (target) {
    case DisplayColorSpace::DisplayP3: return *standardExrChromaticities("lin_p3d65_scene");
    case DisplayColorSpace::AdobeRgb: return *standardExrChromaticities("lin_adobergb_scene");
    case DisplayColorSpace::Bt2020: return *standardExrChromaticities("lin_rec2020_scene");
    case DisplayColorSpace::Srgb: return {};
    }
    return {};
}

template <typename Attribute>
const Attribute* exrAttribute(const Exr::Header& part, const Exr::Header& first, const char* name) {
    if (const auto* value = part.findTypedAttribute<Attribute>(name)) return value;
    return first.findTypedAttribute<Attribute>(name);
}

bool validXy(const Imath::V2f& xy) {
    return std::isfinite(xy.x) && std::isfinite(xy.y) && std::abs(xy.y) > 1e-6f &&
           std::abs(xy.x) <= 2.0f && std::abs(xy.y) <= 2.0f;
}

struct ExrColor {
    Chromaticities chroma;
    Imath::V2f neutral = chroma.white;
    float whiteScale = 1.0f;
    QString label = QStringLiteral("Linear sRGB");
    bool data = false;
};

ExrColor exrColor(const Exr::Header& part, const Exr::Header& first, ImageMetadata& metadata,
                  bool useEmbeddedColor) {
    ExrColor result;
    const auto* id = exrAttribute<Exr::StringAttribute>(part, first, "colorInteropID");
    const auto* firstId = first.findTypedAttribute<Exr::StringAttribute>("colorInteropID");
    const auto* aces = exrAttribute<Exr::IntAttribute>(part, first, "acesImageContainerFlag");
    const bool acesContainer = aces && aces->value() == 1;
    if (!acesContainer && ((id && id->value() == "data") || (firstId && firstId->value() == "data"))) {
        result.data = true;
        result.label = QStringLiteral("OpenEXR data channels");
        return result;
    }
    if (!useEmbeddedColor) return result;

    if (acesContainer) {
        result.chroma = *standardExrChromaticities("lin_ap0_scene");
        result.label = QStringLiteral("ACES2065-1");
    } else if (const auto standard = id ? standardExrChromaticities(id->value()) : std::nullopt) {
        result.chroma = *standard;
        result.label = QString::fromStdString(id->value());
    } else {
        if (id && id->value() != "unknown")
            metadata.colorWarning = QStringLiteral("Unrecognized OpenEXR colorInteropID; using available chromaticities or linear sRGB");
        if (const Exr::Header* tagged = Exr::hasChromaticities(part) ? &part :
                                       Exr::hasChromaticities(first) ? &first : nullptr) {
            const auto& chroma = Exr::chromaticities(*tagged);
            if (validXy(chroma.red) && validXy(chroma.green) && validXy(chroma.blue) && validXy(chroma.white)) {
                result.chroma = chroma;
                result.label = QStringLiteral("OpenEXR chromaticities");
            } else {
                metadata.colorWarning = QStringLiteral("Invalid OpenEXR chromaticities; assuming linear sRGB");
            }
        }
    }
    result.neutral = result.chroma.white;
    if (const auto* tagged = Exr::hasAdoptedNeutral(part) ? &part : Exr::hasAdoptedNeutral(first) ? &first : nullptr) {
        const auto& neutral = Exr::adoptedNeutral(*tagged);
        if (validXy(neutral) && neutral.y > 0.0f) result.neutral = neutral;
    }
    if (const auto* tagged = Exr::hasWhiteLuminance(part) ? &part : Exr::hasWhiteLuminance(first) ? &first : nullptr) {
        const float white = Exr::whiteLuminance(*tagged);
        if (std::isfinite(white) && white > 0.0f) result.whiteScale = white / kSdrReferenceWhite;
    }
    return result;
}

Imath::V3f whiteXyz(Imath::V2f xy) {
    return {xy.x / xy.y, 1.0f, (1.0f - xy.x - xy.y) / xy.y};
}

Imath::M44f adaptWhite(Imath::V2f source, Imath::V2f target) {
    if (source == target) return {};
    // Bradford transform in Imath's row-vector convention.
    const Imath::M44f bradford(
         0.8951f, -0.7502f,  0.0389f, 0.0f,
         0.2664f,  1.7135f, -0.0685f, 0.0f,
        -0.1614f,  0.0367f,  1.0296f, 0.0f,
         0.0f,     0.0f,     0.0f,    1.0f);
    const auto sourceCone = whiteXyz(source) * bradford;
    const auto targetCone = whiteXyz(target) * bradford;
    for (int i = 0; i < 3; ++i) {
        if (sourceCone[i] <= 1e-6f) return {};
    }
    Imath::M44f scale;
    scale.setScale(targetCone / sourceCone);
    return bradford * scale * bradford.inverse();
}

bool validMatrix(const Imath::M44f& matrix) {
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            if (!std::isfinite(matrix[row][column]) || std::abs(matrix[row][column]) > 100.0f) return false;
    return true;
}

void applyExrColor(QImage& image, ExrColor color, const ExrChannels& channels,
                   ImageFrame& frame, DisplayColorSpace target, const DecodeRequest& request) {
    const auto targetSpace = displayQColorSpace(target);
    const auto targetChroma = targetChromaticities(target);
    Imath::M44f transform;
    bool convert = !color.data && (color.chroma != targetChroma ||
                                  color.neutral != targetChroma.white || color.whiteScale != 1.0f);
    const auto targetToXyz = Exr::RGBtoXYZ(targetChroma, 1.0f);
    if (convert) {
        const auto toTarget = Exr::XYZtoRGB(targetChroma, 1.0f);
        try {
            transform = Exr::RGBtoXYZ(color.chroma, 1.0f) * adaptWhite(color.neutral, targetChroma.white) * toTarget;
        } catch (const std::exception&) {
            transform[0][0] = std::numeric_limits<float>::quiet_NaN();
        }
        if (!validMatrix(transform)) {
            color = {};
            transform = Exr::RGBtoXYZ(color.chroma, 1.0f) *
                        adaptWhite(color.neutral, targetChroma.white) * toTarget;
            frame.metadata.colorWarning = QStringLiteral("Invalid OpenEXR color transform; assuming linear sRGB");
        }
        convert = color.chroma != targetChroma || color.neutral != targetChroma.white || color.whiteScale != 1.0f;
    }
    const bool unassociate = channels.hasAlpha && !color.data;
    for (int y = 0; (channels.grayscale || unassociate || convert) && y < image.height() && !request.isCancelled(); ++y) {
        auto* pixel = reinterpret_cast<float*>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x, pixel += 4) {
            if (channels.grayscale) pixel[2] = pixel[1] = pixel[0];
            if (unassociate && pixel[3] > 0.0f && pixel[3] != 1.0f) {
                pixel[0] /= pixel[3]; pixel[1] /= pixel[3]; pixel[2] /= pixel[3];
            }
            if (!convert) continue;
            auto rgb = (Imath::V3f(pixel[0], pixel[1], pixel[2]) * transform) * color.whiteScale;
            if (color.whiteScale > 1.0f) {
                rgb *= sdrHighlightScale((rgb * targetToXyz).y);
            }
            pixel[0] = rgb.x; pixel[1] = rgb.y; pixel[2] = rgb.z;
        }
    }
    auto& source = frame.descriptor.sourceColor;
    source.colorSpace = color.label;
    source.transferFunction = color.data ? QString{} : QStringLiteral("Linear");
    source.primaries = color.data ? QString{} : color.label;
    image.setColorSpace(color.data ? targetSpace : targetSpace.withTransferFunction(QColorSpace::TransferFunction::Linear));
}

QSize exrSize(const Imath::Box2i& window) {
    const qint64 width = qint64(window.max.x) - window.min.x + 1;
    const qint64 height = qint64(window.max.y) - window.min.y + 1;
    constexpr auto limit = std::numeric_limits<int>::max();
    if (width <= 0 || height <= 0 || width > limit || height > limit) return {};
    return {static_cast<int>(width), static_cast<int>(height)};
}

QImage exrDisplayWindow(const QImage& data, const Imath::Box2i& dataWindow,
                       const Imath::Box2i& displayWindow, const DecodeRequest& request) {
    if (dataWindow == displayWindow) return data;
    QImage output(exrSize(displayWindow), data.format());
    if (output.isNull()) return {};
    output.fill(Qt::transparent);
    const int left = std::max(dataWindow.min.x, displayWindow.min.x);
    const int top = std::max(dataWindow.min.y, displayWindow.min.y);
    const int right = std::min(dataWindow.max.x, displayWindow.max.x);
    const int bottom = std::min(dataWindow.max.y, displayWindow.max.y);
    if (right < left || bottom < top) return output;
    constexpr qsizetype bytesPerPixel = 4 * sizeof(float);
    const auto copyBytes = static_cast<size_t>((qint64(right) - left + 1) * bytesPerPixel);
    for (qint64 y = top; y <= bottom && !request.isCancelled(); ++y) {
        std::memcpy(output.scanLine(static_cast<int>(y - displayWindow.min.y)) +
                        (qint64(left) - displayWindow.min.x) * bytesPerPixel,
                    data.constScanLine(static_cast<int>(y - dataWindow.min.y)) +
                        (qint64(left) - dataWindow.min.x) * bytesPerPixel, copyBytes);
    }
    return output;
}

DecodeResult decodeExr(QFile& file, const DecodeRequest& request) {
    ExrStream stream(file);
    // The image loader already runs several independent decode workers.
    Exr::MultiPartInputFile input(stream, 0);
    const auto part = findExrImage(input);
    if (!part) return {{}, QStringLiteral("No flat RGB or luminance OpenEXR image")};
    const auto& channels = part->channels;

    const auto& header = input.header(part->index);
    const auto& window = header.dataWindow();
    const auto& displayWindow = header.displayWindow();
    const QSize data = exrSize(window);
    const QSize source = exrSize(displayWindow);
    if (data.isEmpty() || source.isEmpty()) return {{}, QStringLiteral("Invalid OpenEXR dimensions")};
    QSize presentation = source;
    const double width = double(source.width()) * header.pixelAspectRatio();
    if (std::isfinite(width) && width > 0 && width <= std::numeric_limits<int>::max()) {
        presentation.setWidth(std::max(1, qRound(width)));
    }
    const QSize output = outputSize(source, presentation, request);
    const auto displayBytes = estimatedPixelBytes(source, 32);
    const auto workingBytes = window == displayWindow ? displayBytes :
        addedAllocationBytes(estimatedPixelBytes(data, 16), displayBytes);
    if (!request.prepareAllocation(addedAllocationBytes(workingBytes, estimatedPixelBytes(output, 32)))) {
        return {{}, QStringLiteral("Cancelled")};
    }

    QImage image(data, QImage::Format_RGBA32FPx4);
    if (image.isNull()) return {{}, QStringLiteral("Could not allocate OpenEXR image")};
    Exr::FrameBuffer buffer;
    for (size_t index = 0; index < channels.names.size(); ++index) {
        buffer.insert(channels.names[index], Exr::Slice::Make(Exr::FLOAT,
            image.bits() + index * sizeof(float), window, 4 * sizeof(float),
            static_cast<size_t>(image.bytesPerLine()), 1, 1, index == 3 ? 1.0 : 0.0));
    }

    if (header.type() == Exr::TILEDIMAGE) {
        Exr::TiledInputPart imagePart(input, part->index);
        imagePart.setFrameBuffer(buffer);
        for (int row = 0; row < imagePart.numYTiles() && !request.isCancelled(); ++row)
            imagePart.readTiles(0, imagePart.numXTiles() - 1, row, row);
    } else {
        Exr::InputPart imagePart(input, part->index);
        imagePart.setFrameBuffer(buffer);
        for (qint64 row = window.min.y; row <= window.max.y && !request.isCancelled(); row += 32) {
            imagePart.readPixels(static_cast<int>(row),
                                 static_cast<int>(std::min(row + 31, qint64(window.max.y))));
        }
    }

    if (request.isCancelled()) return {{}, QStringLiteral("Cancelled")};
    image = exrDisplayWindow(image, window, displayWindow, request);
    if (image.isNull()) return {{}, QStringLiteral("Could not allocate OpenEXR display window")};
    auto frame = makeFrame(request, source, QStringLiteral("EXR"), QStringLiteral("OpenEXR"),
                           QStringLiteral(OPENEXR_VERSION_STRING));
    if (presentation != source) frame.descriptor.presentationSize = presentation;
    const auto target = currentDisplayColorSpace();
    const auto color = exrColor(header, input.header(0), frame.metadata, EncodedColorManagement::isEnabled());
    applyExrColor(image, color, channels, frame, target, request);
    if (image.size() != output) image = image.scaled(output, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const auto format = QtImageDecoder::preserveHighBitDepth()
        ? QImage::Format_RGBA32FPx4 : QImage::Format_RGBA8888;
    image.convertToColorSpace(displayQColorSpace(target), format);
    return finishFrame(request, std::move(frame), std::move(image), channels.sourceBits);
}

ExrHistogram measureExrSamples(std::array<std::vector<double>, 4>& samples) {
    ExrHistogram result;
    constexpr int binCount = 256;
    for (size_t i = 0; i < samples.size(); ++i) {
        auto& values = samples[i];
        if (values.empty()) continue;
        std::sort(values.begin(), values.end());
        auto& channel = result.channels[i];
        channel.bins.resize(binCount);
        channel.sampleCount = static_cast<qint64>(values.size());
        channel.minimum = channel.axisMinimum = values.front();
        channel.maximum = channel.axisMaximum = values.back();
        if (channel.minimum == channel.maximum) {
            const double margin = std::max(0.5, std::abs(channel.minimum) * 0.05);
            channel.axisMinimum -= margin;
            channel.axisMaximum += margin;
        }
        const double binScale = binCount / (channel.axisMaximum - channel.axisMinimum);
        channel.median = std::midpoint(values[(values.size() - 1) / 2], values[values.size() / 2]);
        const double count = static_cast<double>(channel.sampleCount);
        channel.mean = std::accumulate(values.begin(), values.end(), 0.0) / count;
        double squaredDifference = 0;
        for (const double value : values) {
            const int bin = std::clamp(static_cast<int>((value - channel.axisMinimum) * binScale), 0, binCount - 1);
            ++channel.bins[bin];
            const double difference = value - channel.mean;
            squaredDifference += difference * difference;
        }
        channel.variance = squaredDifference / count;
    }
    return result;
}

ExrHistogram analyzeExrPixels(QFile& file, const std::function<bool()>& cancelled) {
    ExrStream stream(file);
    Exr::MultiPartInputFile input(stream, 0);
    const auto part = findExrImage(input);
    if (!part || cancelled()) return {};
    const auto& header = input.header(part->index);
    const auto& window = header.dataWindow();
    const QSize size = exrSize(window);
    if (size.isEmpty()) return {};

    std::optional<Exr::TiledInputPart> tiled;
    std::optional<Exr::InputPart> scanline;
    QSize blockSize(size.width(), 32);
    if (header.type() == Exr::TILEDIMAGE) {
        tiled.emplace(input, part->index);
        blockSize = exrSize(tiled->dataWindowForTile(0, 0));
    } else {
        scanline.emplace(input, part->index);
    }
    if (blockSize.isEmpty()) return {};

    // Sort the bounded sample grid by file block, so each tile or group of scanlines is read once.
    const auto [columns, rows] = detail::histogramGridCounts(size.width(), size.height(),
                                                             detail::kHistogramMaximumSamples);
    std::vector<QPoint> points;
    points.reserve(static_cast<size_t>(columns) * static_cast<size_t>(rows));
    const QRect region(QPoint(window.min.x, window.min.y), QPoint(window.max.x, window.max.y));
    for (int row = 0; row < rows; ++row)
        for (int column = 0; column < columns; ++column)
            points.push_back(detail::histogramSamplePoint(region, column, row, columns, rows));
    const auto blockIndex = [&](QPoint pixel) {
        return std::pair{(pixel.y() - window.min.y) / blockSize.height(),
                         (pixel.x() - window.min.x) / blockSize.width()};
    };
    std::sort(points.begin(), points.end(), [&](QPoint a, QPoint b) { return blockIndex(a) < blockIndex(b); });

    ImageMetadata metadata;
    const auto color = exrColor(header, input.header(0), metadata, true);
    auto toXyz = Exr::RGBtoXYZ(Chromaticities{}, 1.0f);
    if (!color.data) {
        try {
            const auto nativeToXyz = Exr::RGBtoXYZ(color.chroma, 1.0f);
            if (validMatrix(nativeToXyz)) toXyz = nativeToXyz;
        } catch (const std::exception&) {
            // A malformed chromaticity header does not prevent RGB statistics.
        }
    }
    const auto& channels = part->channels;
    std::array<bool, 3> isUint{};
    for (size_t i = 0; i < isUint.size(); ++i) {
        const auto* channel = header.channels().findChannel(channels.names[i]);
        isUint[i] = channel && channel->type == Exr::UINT;
    }
    std::array<std::vector<float>, 3> floatPixels;
    std::array<std::vector<unsigned int>, 3> uintPixels;
    std::array<std::vector<double>, 4> samples;
    for (auto& channel : samples) channel.reserve(points.size());

    for (auto point = points.cbegin(); point != points.cend();) {
        if (cancelled()) return {};
        const auto [row, column] = blockIndex(*point);
        const int start = window.min.y + row * blockSize.height();
        const auto block = tiled ? tiled->dataWindowForTile(column, row) : Imath::Box2i(
            {window.min.x, start}, {window.max.x, static_cast<int>(std::min(qint64(window.max.y), qint64(start) + 31))});
        const auto dimensions = exrSize(block);
        const auto count = static_cast<size_t>(dimensions.width()) * static_cast<size_t>(dimensions.height());
        Exr::FrameBuffer buffer;
        const size_t channelCount = channels.grayscale ? 1 : 3;
        for (size_t i = 0; i < channelCount; ++i) {
            if (isUint[i]) {
                uintPixels[i].resize(count);
                buffer.insert(channels.names[i], Exr::Slice::Make(Exr::UINT, uintPixels[i].data(), block));
            } else {
                floatPixels[i].resize(count);
                buffer.insert(channels.names[i], Exr::Slice::Make(Exr::FLOAT, floatPixels[i].data(), block));
            }
        }
        if (tiled) {
            tiled->setFrameBuffer(buffer);
            tiled->readTile(column, row);
        } else {
            scanline->setFrameBuffer(buffer);
            scanline->readPixels(block.min.y, block.max.y);
        }
        for (; point != points.cend() && blockIndex(*point) == std::pair{row, column}; ++point) {
            const size_t index = static_cast<size_t>(point->y() - block.min.y) * static_cast<size_t>(dimensions.width()) +
                                 static_cast<size_t>(point->x() - block.min.x);
            const auto value = [&](size_t i) -> double {
                if (isUint[i]) return uintPixels[i][index];
                return floatPixels[i][index];
            };
            const double red = value(0);
            const double green = channels.grayscale ? red : value(1);
            const double blue = channels.grayscale ? red : value(2);
            if (std::isfinite(red)) samples[1].push_back(red);
            if (std::isfinite(green)) samples[2].push_back(green);
            if (std::isfinite(blue)) samples[3].push_back(blue);
            if (!color.data && std::isfinite(red) && std::isfinite(green) && std::isfinite(blue))
                samples[0].push_back(green + toXyz[0][1] * (red - green) + toXyz[2][1] * (blue - green));
        }
    }
    return cancelled() ? ExrHistogram{} : measureExrSamples(samples);
}

} // namespace

QStringList JxlExrDecoder::supportedSuffixes() {
    return {kJxlSuffix, kExrSuffix};
}

bool JxlExrDecoder::supportsPath(const QString& path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == kJxlSuffix || suffix == kExrSuffix;
}

ExrHistogram JxlExrDecoder::analyzeExr(const QString& path, const std::function<bool()>& cancelled) {
    QFile file(path);
    if (cancelled() || !file.open(QIODevice::ReadOnly)) return {};
    try {
        return analyzeExrPixels(file, cancelled);
    } catch (const std::exception&) {
        return {};
    }
}

QString JxlExrDecoder::cacheIdentity() const {
    return QStringLiteral("jxl-exr-v2|jxl-%1|exr-%2|%3")
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
