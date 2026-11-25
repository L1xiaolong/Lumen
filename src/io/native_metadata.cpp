#include "io/native_metadata.h"
#include "io/jpeg_gain_map_metadata.h"

#include <jxl/decode.h>

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QTimeZone>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <bit>
#include <memory>
#include <limits>
#include <cmath>
#include <stdexcept>

namespace mvpview {
namespace {

using Property = ImageMetadata::Property;
using Group = ImageMetadata::PropertyGroup;
constexpr qsizetype maximumValueBytes = 64 * 1024;

QString number(float value) {
    return QString::number(value, 'g', 9);
}

QString contentText(QByteArrayView bytes) {
    auto value = QString::fromUtf8(bytes.first(std::min<qsizetype>(bytes.size(), 4096)));
    for (auto& character : value) {
        if (character.category() == QChar::Other_Control &&
            character != QLatin1Char('\n') && character != QLatin1Char('\t'))
            character = QChar::ReplacementCharacter;
    }
    if (bytes.size() > 4096) value += QChar(0x2026);
    return value;
}

QString binary(QByteArrayView bytes, qint64 size) {
    if (size == 0) return QStringLiteral("0 B");
    const auto prefix = bytes.first(std::min<qsizetype>(bytes.size(), 64)).toByteArray().toHex(' ');
    return QStringLiteral("%1 B · %2%3").arg(size).arg(QString::fromLatin1(prefix),
        size > 64 ? QStringLiteral(" …") : QString{});
}

template <typename T>
T little(QByteArrayView bytes, qsizetype offset = 0) {
    return qFromLittleEndian<T>(bytes.data() + offset);
}

float real(QByteArrayView bytes, qsizetype offset = 0) {
    return std::bit_cast<float>(little<quint32>(bytes, offset));
}

QString exrChannelValue(QByteArrayView data, QVector<Property>& channels) {
    qsizetype position = 0;
    QStringList names;
    while (position < data.size() && data[position] != 0) {
        const auto end = data.indexOf('\0', position);
        if (end < 0 || data.size() - end < 17) return {};
        const auto name = QString::fromUtf8(data.sliced(position, end - position));
        const auto offset = end + 1;
        const auto sample = little<quint32>(data, offset);
        const QString sampleType = sample == 0 ? QStringLiteral("UINT32") :
            sample == 1 ? QStringLiteral("HALF (16-bit)") :
            sample == 2 ? QStringLiteral("FLOAT (32-bit)") : QString::number(sample);
        channels.append({name, QStringLiteral("%1 · %2 × %3 · pLinear %4")
            .arg(sampleType).arg(little<qint32>(data, offset + 8))
            .arg(little<qint32>(data, offset + 12)).arg(static_cast<unsigned char>(data[offset + 4])),
            QStringLiteral("channel")});
        names.append(name);
        position = offset + 16;
    }
    return names.join(QStringLiteral(", "));
}

QString exrValue(const QByteArray& type, QByteArrayView data, qint64 bytes, QVector<Property>& channels) {
    if (type == "string") {
        auto value = contentText(data);
        if (bytes > data.size() && data.size() <= 4096) value += QChar(0x2026);
        return value.isEmpty() ? QStringLiteral("\"\"") : value;
    }
    if (type == "preview" && data.size() >= 8)
        return QStringLiteral("%1 × %2 px · RGBA8 · %3 B")
            .arg(little<quint32>(data)).arg(little<quint32>(data, 4)).arg(bytes);
    if (data.size() != bytes) return binary(data, bytes);
    if (type == "chlist") {
        const auto value = exrChannelValue(data, channels);
        if (!value.isEmpty()) return value;
    }
    if (type == "stringvector" || type == "floatvector") {
        QStringList values;
        qsizetype position = 0;
        while (data.size() - position >= 4 && values.size() < 128) {
            if (type == "floatvector") {
                values.append(number(real(data, position)));
                position += 4;
            } else {
                const auto size = little<quint32>(data, position);
                position += 4;
                if (size > data.size() - position) return binary(data, bytes);
                values.append(contentText(data.sliced(position, size)));
                position += size;
            }
        }
        auto value = values.join(QStringLiteral(", "));
        if (position < data.size()) value += QChar(0x2026);
        return value;
    }
    if (data.size() == 1) {
        const auto value = static_cast<unsigned char>(data[0]);
        static constexpr std::array compression{"NONE", "RLE", "ZIPS", "ZIP", "PIZ", "PXR24",
            "B44", "B44A", "DWAA", "DWAB", "HTJ2K256", "HTJ2K32"};
        static constexpr std::array lineOrder{"INCREASING_Y", "DECREASING_Y", "RANDOM_Y"};
        static constexpr std::array envmap{"LATLONG", "CUBE"};
        if (type == "compression" && value < compression.size()) return QString::fromLatin1(compression[value]);
        if (type == "lineOrder" && value < lineOrder.size()) return QString::fromLatin1(lineOrder[value]);
        if (type == "envmap" && value < envmap.size()) return QString::fromLatin1(envmap[value]);
        if (type == "deepImageState") {
            static constexpr std::array states{"MESSY", "SORTED", "NON_OVERLAPPING", "TIDY"};
            if (value < states.size()) return QString::fromLatin1(states[value]);
        }
    }
    if (type == "rational" && data.size() == 8)
        return QStringLiteral("%1 / %2").arg(little<qint32>(data)).arg(little<quint32>(data, 4));
    if (type == "tiledesc" && data.size() == 9) {
        const auto size = QStringLiteral("%1 × %2").arg(little<quint32>(data)).arg(little<quint32>(data, 4));
        static constexpr std::array levels{"ONE_LEVEL", "MIPMAP_LEVELS", "RIPMAP_LEVELS"};
        const auto mode = static_cast<unsigned char>(data[8]);
        if ((mode & 15) < levels.size())
            return size + QStringLiteral(" · %1 · %2").arg(QString::fromLatin1(levels[mode & 15]),
                (mode & 16) ? QStringLiteral("ROUND_UP") : QStringLiteral("ROUND_DOWN"));
    }
    if (type == "timecode" && data.size() == 8) {
        const auto code = little<quint32>(data);
        return QStringLiteral("%1:%2:%3:%4 · flags 0x%5 · user 0x%6")
            .arg((code >> 24) & 0x3f, 2, 16, QLatin1Char('0'))
            .arg((code >> 16) & 0x7f, 2, 16, QLatin1Char('0'))
            .arg((code >> 8) & 0x7f, 2, 16, QLatin1Char('0'))
            .arg(code & 0x3f, 2, 16, QLatin1Char('0'))
            .arg(code & 0xc08080c0, 8, 16, QLatin1Char('0')).arg(little<quint32>(data, 4), 8, 16, QLatin1Char('0'));
    }
    const bool integer = type == "int" || type == "box2i" || type == "v2i" || type == "v3i" || type == "keycode";
    const bool single = type == "float" || type == "box2f" || type == "v2f" || type == "v3f" ||
        type == "m33f" || type == "m44f" || type == "chromaticities";
    const bool wide = type == "double" || type == "v2d" || type == "v3d" || type == "m33d" || type == "m44d";
    const qsizetype step = wide ? 8 : 4;
    if ((integer || single || wide) && data.size() % step == 0 && data.size() / step <= 16) {
        QStringList values;
        for (qsizetype offset = 0; offset < data.size(); offset += step) {
            if (integer) values.append(QString::number(little<qint32>(data, offset)));
            else if (wide) values.append(QString::number(std::bit_cast<double>(little<quint64>(data, offset)), 'g', 17));
            else values.append(number(real(data, offset)));
        }
        if ((type == "box2i" || type == "box2f") && values.size() == 4)
            return QStringLiteral("(%1, %2) – (%3, %4)").arg(values[0], values[1], values[2], values[3]);
        if (type == "chromaticities" && values.size() == 8)
            return QStringLiteral("R (%1, %2)\nG (%3, %4)\nB (%5, %6)\nW (%7, %8)")
                .arg(values[0], values[1], values[2], values[3], values[4], values[5], values[6], values[7]);
        return values.join(QStringLiteral(", "));
    }
    return binary(data, bytes);
}

QByteArray exrName(QFile& file, int maximumLength) {
    QByteArray name;
    char c;
    while (file.getChar(&c)) {
        if (c == 0) return name;
        if (name.size() == maximumLength) break;
        name.append(c);
    }
    throw std::runtime_error("Invalid OpenEXR header field name");
}

void addExrOverview(const Group& group, ImageMetadata& metadata) {
    const auto value = [&](QLatin1StringView name) {
        const auto property = std::ranges::find_if(group.properties, [&](const auto& item) {
            return item.name == name && (item.type == QStringLiteral("string") || item.type == QStringLiteral("float"));
        });
        return property == group.properties.end() || property->value == QStringLiteral("\"\"")
            ? QString{} : property->value;
    };
    ImageMetadata::Camera camera;
    camera.make = value(QLatin1StringView("cameraMake"));
    camera.model = value(QLatin1StringView("cameraModel"));
    camera.lens = (value(QLatin1StringView("lensMake")) + QLatin1Char(' ') + value(QLatin1StringView("lensModel"))).trimmed();
    camera.exposureSeconds = value(QLatin1StringView("expTime")).toDouble();
    camera.aperture = value(QLatin1StringView("aperture")).toDouble();
    camera.focalLengthMm = value(QLatin1StringView("effectiveFocalLength")).toDouble();
    const double iso = value(QLatin1StringView("isoSpeed")).toDouble();
    if (iso > 0 && iso <= std::numeric_limits<int>::max()) camera.iso = static_cast<int>(std::lround(iso));
    camera.capturedAt = QDateTime::fromString(value(QLatin1StringView("capDate")), QStringLiteral("yyyy:MM:dd HH:mm:ss"));
    bool offsetPresent = false;
    const double offset = value(QLatin1StringView("utcOffset")).toDouble(&offsetPresent);
    if (camera.capturedAt.isValid() && offsetPresent && std::abs(offset) <= 86400)
        camera.capturedAt.setTimeZone(QTimeZone::fromSecondsAheadOfUtc(-static_cast<int>(offset)));
    metadata.camera = std::move(camera);
    const auto comments = value(QLatin1StringView("comments"));
    if (!comments.isEmpty()) {
        ImageMetadata::Descriptive descriptive;
        descriptive.description = comments;
        metadata.descriptive = std::move(descriptive);
    }
}

void readExr(QFile& file, ImageMetadata& metadata) {
    const QByteArray prefix = file.read(8);
    if (prefix.size() != 8 || little<quint32>(prefix) != 0x01312f76)
        throw std::runtime_error("Invalid OpenEXR file signature");
    const auto version = little<quint32>(prefix, 4);
    const bool multipart = (version & 0x1000) != 0;
    const int maxName = (version & 0x400) ? 255 : 31;
    Group container{QStringLiteral("exr"), QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "OpenEXR container")), {},
        {{QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "File format version")), QString::number(version & 255), {}},
         {QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "File flags")), QStringLiteral("0x%1").arg(version & ~255u, 0, 16), {}}}};
    const auto index = metadata.embeddedProperties.size();
    metadata.embeddedProperties.append(std::move(container));
    int parts = 0;
    for (;;) {
        auto name = exrName(file, maxName);
        if (multipart && name.isEmpty()) break;
        Group part{QStringLiteral("exr-part-%1").arg(parts), QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "OpenEXR header")), {}, {}, parts > 0};
        Group channels{QStringLiteral("exr-channels-%1").arg(parts), QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "OpenEXR channels")), {}, {}, parts > 0};
        QString partName;
        while (!name.isEmpty()) {
            const auto type = exrName(file, maxName);
            const auto length = file.read(4);
            if (type.isEmpty() || length.size() != 4) throw std::runtime_error("Incomplete OpenEXR attribute");
            const qint64 size = little<qint32>(length);
            if (size < 0 || size > file.size() - file.pos())
                throw std::runtime_error("Invalid OpenEXR attribute size");
            const auto count = std::min<qint64>(size, maximumValueBytes);
            const auto data = file.read(count);
            if (data.size() != count || !file.seek(file.pos() + size - count))
                throw std::runtime_error("Incomplete OpenEXR attribute value");
            auto value = exrValue(type, data, size, channels.properties);
            if (name == "name" && type == "string") partName = value;
            if (value.isEmpty()) value = QStringLiteral("0 B");
            part.properties.append({QString::fromUtf8(name), std::move(value), QString::fromUtf8(type)});
            name = exrName(file, maxName);
        }
        if (multipart) {
            part.detail = QString::number(parts + 1);
            if (!partName.isEmpty()) part.detail += QStringLiteral(" · ") + partName;
            channels.detail = part.detail;
        }
        if (parts == 0) addExrOverview(part, metadata);
        metadata.embeddedProperties.append(std::move(part));
        if (!channels.properties.isEmpty()) metadata.embeddedProperties.append(std::move(channels));
        ++parts;
        if (!multipart) break;
    }
    metadata.embeddedProperties[index].properties.append({QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "Part count")), QString::number(parts), {}});
}

QString jxlChannelType(JxlExtraChannelType type) {
    switch (type) {
    case JXL_CHANNEL_ALPHA: return QStringLiteral("Alpha");
    case JXL_CHANNEL_DEPTH: return QStringLiteral("Depth");
    case JXL_CHANNEL_SPOT_COLOR: return QStringLiteral("Spot color");
    case JXL_CHANNEL_SELECTION_MASK: return QStringLiteral("Selection mask");
    case JXL_CHANNEL_BLACK: return QStringLiteral("Black");
    case JXL_CHANNEL_CFA: return QStringLiteral("CFA");
    case JXL_CHANNEL_THERMAL: return QStringLiteral("Thermal");
    default: return QString::number(type);
    }
}

void addJxlChannels(const JxlDecoder* decoder, const JxlBasicInfo& info, ImageMetadata& metadata) {
    Group group{QStringLiteral("jxl-extra"), QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "JPEG XL extra channels")), {}, {}};
    for (uint32_t i = 0; i < info.num_extra_channels; ++i) {
        JxlExtraChannelInfo channel{};
        if (JxlDecoderGetExtraChannelInfo(decoder, i, &channel) != JXL_DEC_SUCCESS) continue;
        const auto type = jxlChannelType(channel.type);
        auto value = QStringLiteral("%1 · %2 bit · exponent %3 · dim_shift %4")
            .arg(type).arg(channel.bits_per_sample).arg(channel.exponent_bits_per_sample).arg(channel.dim_shift);
        if (channel.type == JXL_CHANNEL_ALPHA)
            value += QStringLiteral(" · alpha_premultiplied %1").arg(channel.alpha_premultiplied);
        if (channel.type == JXL_CHANNEL_CFA)
            value += QStringLiteral(" · cfa_channel %1").arg(channel.cfa_channel);
        if (channel.type == JXL_CHANNEL_SPOT_COLOR)
            value += QStringLiteral(" · RGBA (%1, %2, %3, %4)").arg(number(channel.spot_color[0]),
                number(channel.spot_color[1]), number(channel.spot_color[2]), number(channel.spot_color[3]));
        QString name = QString::number(i + 1);
        if (channel.name_length > 0 && channel.name_length < maximumValueBytes) {
            QByteArray bytes(channel.name_length + 1, '\0');
            if (JxlDecoderGetExtraChannelName(decoder, i, bytes.data(), static_cast<size_t>(bytes.size())) == JXL_DEC_SUCCESS)
                name += QStringLiteral(" · ") + contentText(QByteArrayView(bytes).first(channel.name_length));
        }
        group.properties.append({std::move(name), std::move(value), {}});
    }
    if (!group.properties.isEmpty()) metadata.embeddedProperties.append(std::move(group));
}

QString jxlTransferName(JxlTransferFunction transfer) {
    switch (transfer) {
    case JXL_TRANSFER_FUNCTION_709: return QStringLiteral("BT.709");
    case JXL_TRANSFER_FUNCTION_LINEAR: return QStringLiteral("Linear");
    case JXL_TRANSFER_FUNCTION_SRGB: return QStringLiteral("sRGB");
    case JXL_TRANSFER_FUNCTION_PQ: return QStringLiteral("PQ (ST 2084)");
    case JXL_TRANSFER_FUNCTION_DCI: return QStringLiteral("DCI");
    case JXL_TRANSFER_FUNCTION_HLG: return QStringLiteral("HLG");
    case JXL_TRANSFER_FUNCTION_GAMMA: return QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "Gamma"));
    default: return QString::number(transfer);
    }
}

void addJxlColor(const JxlDecoder* decoder, ImageMetadata& metadata) {
    Group group{QStringLiteral("jxl-color"), QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "JPEG XL encoded color")), {}, {}};
    const auto add = [&](QString name, QString value) {
        group.properties.append({std::move(name), std::move(value), {}});
    };
    JxlColorEncoding encoding{};
    const bool structured = JxlDecoderGetColorAsEncodedProfile(decoder, JXL_COLOR_PROFILE_TARGET_ORIGINAL, &encoding) == JXL_DEC_SUCCESS;
    size_t size = 0;
    if (JxlDecoderGetICCProfileSize(decoder, JXL_COLOR_PROFILE_TARGET_ORIGINAL, &size) == JXL_DEC_SUCCESS)
        add(structured ? QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "ICC representation size")) : QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "Embedded ICC size")),
            QStringLiteral("%1 B").arg(size));
    if (structured) {
        const auto point = [](const double (&xy)[2]) {
            return QStringLiteral("(%1, %2)").arg(xy[0], 0, 'g', 12).arg(xy[1], 0, 'g', 12);
        };
        static constexpr std::array spaces{"RGB", "Gray", "XYB", "Unknown"};
        const auto space = static_cast<unsigned>(encoding.color_space);
        add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "Color space")), space < spaces.size()
            ? QString::fromLatin1(spaces[space]) : QString::number(space));
        add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "White point")), point(encoding.white_point_xy));
        if (encoding.color_space == JXL_COLOR_SPACE_RGB) {
            add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "Red primary")), point(encoding.primaries_red_xy));
            add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "Green primary")), point(encoding.primaries_green_xy));
            add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "Blue primary")), point(encoding.primaries_blue_xy));
        }
        add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "Transfer function")), jxlTransferName(encoding.transfer_function));
        if (encoding.transfer_function == JXL_TRANSFER_FUNCTION_GAMMA)
            add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "Gamma")), QString::number(encoding.gamma, 'g', 12));
        static constexpr std::array intents{"Perceptual", "Relative colorimetric", "Saturation", "Absolute colorimetric"};
        const auto intent = static_cast<unsigned>(encoding.rendering_intent);
        add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "Rendering intent")), intent < intents.size()
            ? QString::fromLatin1(intents[intent]) : QString::number(intent));
    }
    if (!group.properties.isEmpty()) metadata.embeddedProperties.append(std::move(group));
}

void addJxlBoxes(QFile& file, ImageMetadata& metadata) {
    Group group{QStringLiteral("jxl-boxes"), QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "JPEG XL container boxes")), {}, {}, true};
    file.seek(0);
    int index = 0;
    while (file.size() - file.pos() >= 8) {
        const auto offset = file.pos();
        const auto header = file.read(8);
        quint64 size = qFromBigEndian<quint32>(header.constData());
        qint64 headerSize = 8;
        if (size == 1) {
            const auto extended = file.read(8);
            if (extended.size() != 8) break;
            size = qFromBigEndian<quint64>(extended.constData());
            headerSize = 16;
        } else if (size == 0) {
            size = static_cast<quint64>(file.size() - offset);
        }
        if (size < static_cast<quint64>(headerSize) || size > static_cast<quint64>(file.size() - offset)) break;
        QString type = contentText(QByteArrayView(header).sliced(4));
        if (type == QStringLiteral("brob") && size >= static_cast<quint64>(headerSize + 4))
            type += QStringLiteral(" · ") + contentText(file.read(4));
        group.properties.append({QStringLiteral("%1 · %2").arg(++index).arg(type),
            QStringLiteral("%1 B · @ %2").arg(size).arg(offset), {}});
        if (!file.seek(offset + static_cast<qint64>(size))) break;
    }
    if (!group.properties.isEmpty()) metadata.embeddedProperties.append(std::move(group));
}

bool readJxl(QFile& file, ImageMetadata& metadata) {
    const std::unique_ptr<JxlDecoder, decltype(&JxlDecoderDestroy)> decoder(JxlDecoderCreate(nullptr), JxlDecoderDestroy);
    if (!decoder || JxlDecoderSetKeepOrientation(decoder.get(), JXL_TRUE) != JXL_DEC_SUCCESS ||
        JxlDecoderSubscribeEvents(decoder.get(), JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING) != JXL_DEC_SUCCESS)
        throw std::runtime_error("Could not initialize JPEG XL metadata reader");
    QByteArray input;
    bool container = false;
    bool eof = false;
    for (;;) {
        const auto status = JxlDecoderProcessInput(decoder.get());
        if (status == JXL_DEC_NEED_MORE_INPUT && !eof) {
            const auto remaining = static_cast<qsizetype>(JxlDecoderReleaseInput(decoder.get()));
            if (remaining > 16 * 1024 * 1024)
                throw std::runtime_error("JPEG XL header exceeds the metadata read limit");
            input = input.right(remaining) + file.read(maximumValueBytes);
            if (JxlDecoderSetInput(decoder.get(), reinterpret_cast<const uint8_t*>(input.constData()),
                                   static_cast<size_t>(input.size())) != JXL_DEC_SUCCESS)
                throw std::runtime_error("Could not read JPEG XL input");
            eof = file.atEnd();
            if (eof) JxlDecoderCloseInput(decoder.get());
        } else if (status == JXL_DEC_BASIC_INFO) {
            JxlBasicInfo info{};
            if (JxlDecoderGetBasicInfo(decoder.get(), &info) != JXL_DEC_SUCCESS)
                throw std::runtime_error("Invalid JPEG XL basic info");
            if (info.orientation >= 1 && info.orientation <= 8)
                metadata.sourceOrientation = static_cast<ImageMetadata::Orientation>(info.orientation);
            container = info.have_container;
            Group group{QStringLiteral("jxl"), QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "JPEG XL codestream")), {}, {}};
            const auto add = [&](QString name, QString value) {
                group.properties.append({std::move(name), std::move(value), {}});
            };
            add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "Storage")), container ? QStringLiteral("Container") : QStringLiteral("Codestream"));
            add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "Encoded dimensions")), QStringLiteral("%1 × %2").arg(info.xsize).arg(info.ysize));
            add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "Bit depth")), QString::number(info.bits_per_sample));
            if (info.exponent_bits_per_sample) add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "Exponent bits")), QString::number(info.exponent_bits_per_sample));
            add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "Color channels")), QString::number(info.num_color_channels));
            add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "Extra channels")), QString::number(info.num_extra_channels));
            add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "Orientation")), QString::number(info.orientation));
            add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "Original profile used")), QString::number(info.uses_original_profile));
            if (info.alpha_bits) {
                add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "Alpha bit depth")), QString::number(info.alpha_bits));
                if (info.alpha_exponent_bits)
                    add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "Alpha exponent bits")), QString::number(info.alpha_exponent_bits));
            }
            add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "Intensity target")), QStringLiteral("%1 cd/m²").arg(number(info.intensity_target)));
            add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "Minimum luminance")), QStringLiteral("%1 cd/m²").arg(number(info.min_nits)));
            add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "Linear below")), number(info.linear_below));
            add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "Relative to peak luminance")), QString::number(info.relative_to_max_display));
            if (info.intrinsic_xsize && info.intrinsic_ysize)
                add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "Intrinsic dimensions")), QStringLiteral("%1 × %2").arg(info.intrinsic_xsize).arg(info.intrinsic_ysize));
            if (info.have_preview)
                add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "Preview dimensions")), QStringLiteral("%1 × %2").arg(info.preview.xsize).arg(info.preview.ysize));
            if (info.have_animation) {
                add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "Animation time base")), QStringLiteral("%1 / %2 Hz").arg(info.animation.tps_numerator).arg(info.animation.tps_denominator));
                add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "Animation loop count")), info.animation.num_loops ? QString::number(info.animation.num_loops) : QStringLiteral("∞"));
                add(QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "Animation time codes")), QString::number(info.animation.have_timecodes));
            }
            metadata.embeddedProperties.append(std::move(group));
            addJxlChannels(decoder.get(), info, metadata);
        } else if (status == JXL_DEC_COLOR_ENCODING) {
            addJxlColor(decoder.get(), metadata);
            if (container) addJxlBoxes(file, metadata);
            return container;
        } else {
            throw std::runtime_error("Incomplete JPEG XL header");
        }
    }
}

} // namespace

bool readNativeMetadata(const QString& path, ImageMetadata& metadata) {
    const auto suffix = QFileInfo(path).suffix().toLower();
    const bool exr = suffix == QStringLiteral("exr");
    const bool jpeg = suffix == QStringLiteral("jpg") || suffix == QStringLiteral("jpeg") ||
                      suffix == QStringLiteral("jpe") || suffix == QStringLiteral("jfif");
    if (!exr && !jpeg && suffix != QStringLiteral("jxl")) return true;
    QFile file(path);
    try {
        if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Could not open image metadata");
        if (jpeg) {
            readJpegGainMapMetadata(file, metadata);
            return true;
        }
        if (!exr) return readJxl(file, metadata);
        readExr(file, metadata);
    } catch (const std::exception& error) {
        metadata.metadataWarning = QString::fromUtf8(error.what());
        return !exr;
    }
    return false;
}

} // namespace mvpview
