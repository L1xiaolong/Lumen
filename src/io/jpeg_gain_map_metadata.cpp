#include "io/jpeg_gain_map_metadata.h"

#include "core/image_types.h"

#include <QCoreApplication>
#include <QFile>
#include <QStringList>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <utility>

namespace mvpview {
namespace {

constexpr QByteArrayView isoSignature("urn:iso:std:iso:ts:21496:-1\0",
                                      sizeof("urn:iso:std:iso:ts:21496:-1"));
constexpr QByteArrayView mpfSignature("MPF\0", 4);
constexpr QByteArrayView startOfImage("\xff\xd8", 2);
constexpr QByteArrayView endOfImage("\xff\xd9", 2);
constexpr qint64 maximumHeaderBytes = 16 * 1024 * 1024;
constexpr uchar multiChannelFlag = 0x80;
constexpr uchar useBaselineColorSpaceFlag = 0x40;
constexpr uchar sharedDenominatorFlag = 0x08;

struct JpegHeader {
    QSize size;
    int precision = 0;
    int components = 0;
    qint64 scanOffset = 0;
    std::optional<QByteArray> isoMetadata;
    QByteArray mpIndex;
    qint64 mpIndexOffset = 0;
};

JpegHeader readHeader(QFile& file, qint64 offset, qint64 length, bool readIndex) {
    JpegHeader header;
    if (length < 4 || !file.seek(offset) || file.read(2) != startOfImage) return header;
    const qint64 limit = offset + std::min(length, maximumHeaderBytes);
    while (file.pos() < limit) {
        char markerByte = 0;
        if (!file.getChar(&markerByte) || uchar(markerByte) != 0xff) break;
        do {
            if (file.pos() >= limit || !file.getChar(&markerByte)) return header;
        } while (uchar(markerByte) == 0xff);
        const auto marker = uchar(markerByte);
        if (marker == 0x00 || marker == 0xd8 || marker == 0xd9) break;
        if (marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;

        std::array<char, 2> lengthBytes{};
        if (limit - file.pos() < 2 || file.read(lengthBytes.data(), 2) != 2) break;
        const int size = int(qFromBigEndian<quint16>(lengthBytes.data())) - 2;
        const qint64 payload = file.pos();
        if (size < 0 || size > limit - payload) break;
        if (marker == 0xda) {
            if (size >= 6 && payload + size < offset + length) header.scanOffset = payload + size;
            break;
        }
        if (marker == 0xe2) {
            const QByteArray segment = file.read(size);
            if (segment.size() != size) break;
            if (!header.isoMetadata && segment.startsWith(isoSignature)) {
                header.isoMetadata = segment.sliced(isoSignature.size());
            } else if (readIndex && header.mpIndex.isEmpty() && segment.startsWith(mpfSignature)) {
                header.mpIndex = segment.sliced(mpfSignature.size());
                header.mpIndexOffset = payload + mpfSignature.size();
            }
        } else if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 &&
                   marker != 0xc8 && marker != 0xcc && size >= 6) {
            const QByteArray frame = file.read(6);
            if (frame.size() != 6) break;
            header.precision = uchar(frame[0]);
            header.size = {qFromBigEndian<quint16>(frame.constData() + 3),
                           qFromBigEndian<quint16>(frame.constData() + 1)};
            header.components = uchar(frame[5]);
            if (size != 6 + 3 * header.components) break;
        }
        if (!file.seek(payload + size)) break;
    }
    return header;
}

struct ImageRange {
    qint64 offset;
    qint64 length;
};

QVector<ImageRange> auxiliaryImages(const JpegHeader& header, qint64 fileSize) {
    const QByteArrayView tiff(header.mpIndex);
    if (tiff.size() < 8) return {};
    const bool bigEndian = tiff.startsWith("MM");
    if (!bigEndian && !tiff.startsWith("II")) return {};
    const auto u16 = [&](qsizetype offset) {
        return bigEndian ? qFromBigEndian<quint16>(tiff.data() + offset)
                         : qFromLittleEndian<quint16>(tiff.data() + offset);
    };
    const auto u32 = [&](qsizetype offset) {
        return bigEndian ? qFromBigEndian<quint32>(tiff.data() + offset)
                         : qFromLittleEndian<quint32>(tiff.data() + offset);
    };
    const qsizetype ifd = u32(4);
    if (u16(2) != 42 || ifd < 8 || ifd > tiff.size() - 2) return {};
    const int tags = u16(ifd);
    if (tiff.size() - ifd - 2 < 12 * tags) return {};

    qsizetype entries = 0;
    qint64 entryBytes = 0;
    qint64 images = 0;
    for (int i = 0; i < tags; ++i) {
        const qsizetype tag = ifd + 2 + 12 * i;
        if (u16(tag) == 0xb001 && u16(tag + 2) == 4 && u32(tag + 4) == 1) {
            images = u32(tag + 8);
        } else if (u16(tag) == 0xb002 && u16(tag + 2) == 7) {
            entryBytes = u32(tag + 4);
            entries = u32(tag + 8);
        }
    }
    constexpr qsizetype entrySize = 16;
    if (images < 2 || entries < 8 || entries > tiff.size() ||
        entryBytes != images * entrySize || entryBytes > tiff.size() - entries ||
        u32(entries + 8) != 0) return {};

    QVector<ImageRange> ranges;
    for (qsizetype entry = entries + entrySize; entry < entries + entryBytes; entry += entrySize) {
        const qint64 length = u32(entry + 4);
        const qint64 relativeOffset = u32(entry + 8);
        const qint64 offset = header.mpIndexOffset + relativeOffset;
        // Some encoders leave the primary length stale after rewriting its headers.
        if ((u32(entry) & 0x07000000) == 0 && relativeOffset > 0 &&
            offset >= header.scanOffset && offset <= fileSize && length >= 4 && length <= fileSize - offset) {
            ranges.append({offset, length});
        }
    }
    return ranges;
}

struct Rational {
    qint64 numerator;
    quint32 denominator;

    double value() const { return double(numerator) / denominator; }
    QString display() const {
        return QStringLiteral("%1 (%2/%3)").arg(value(), 0, 'g', 10).arg(numerator).arg(denominator);
    }
};

QString translated(const char* text) {
    return QCoreApplication::translate("ImageProperties", text);
}

bool parseMetadata(const QByteArray& bytes, const JpegHeader& header,
                   ImageRange range, ImageMetadata::PropertyGroup& group) {
    if (bytes.size() < 5 || qFromBigEndian<quint16>(bytes.constData()) != 0) return false;
    const auto flags = uchar(bytes[4]);
    const int channels = (flags & multiChannelFlag) ? 3 : 1;
    // Also accept the shared-denominator layout used by earlier encoders.
    const bool sharedDenominator = (flags & sharedDenominatorFlag) != 0;
    constexpr int headroomCount = 2;
    constexpr int fieldsPerChannel = 5;
    const int rationalCount = headroomCount + channels * fieldsPerChannel;
    const int expectedSize = 5 + (sharedDenominator ? 4 * (1 + rationalCount) : 8 * rationalCount);
    if (bytes.size() < expectedSize) return false;

    qsizetype position = 5;
    const auto readWord = [&] {
        const auto word = qFromBigEndian<quint32>(bytes.constData() + position);
        position += 4;
        return word;
    };
    const auto common = sharedDenominator ? readWord() : 0u;
    const auto readRational = [&](bool signedNumerator) {
        const auto word = readWord();
        const qint64 numerator = signedNumerator ? qint64(qint32(word)) : qint64(word);
        return Rational{numerator, sharedDenominator ? common : readWord()};
    };
    std::array<Rational, headroomCount> headroom{readRational(false), readRational(false)};
    if (std::ranges::any_of(headroom, [](Rational r) { return r.denominator == 0; })) return false;
    std::array<QStringList, fieldsPerChannel> channelValues;
    for (int channel = 0; channel < channels; ++channel) {
        const std::array<Rational, fieldsPerChannel> values{
            readRational(true), readRational(true), readRational(false),
            readRational(true), readRational(true)};
        if (std::ranges::any_of(values, [](Rational r) { return r.denominator == 0; }) ||
            values[0].numerator * values[1].denominator > values[1].numerator * values[0].denominator ||
            values[2].numerator == 0) return false;
        for (size_t i = 0; i < values.size(); ++i) channelValues[i].append(values[i].display());
    }

    const auto add = [&](const char* name, QString value) {
        group.properties.append({QString::fromUtf8(name), std::move(value), {}});
    };
    add(QT_TRANSLATE_NOOP("ImageProperties", "Gain map dimensions"),
        QStringLiteral("%1 × %2 px").arg(header.size.width()).arg(header.size.height()));
    add(QT_TRANSLATE_NOOP("ImageProperties", "Gain map JPEG components"), QString::number(header.components));
    add(QT_TRANSLATE_NOOP("ImageProperties", "Bit depth"), QString::number(header.precision));
    add(QT_TRANSLATE_NOOP("ImageProperties", "Gain map file offset"), QStringLiteral("%1 B").arg(range.offset));
    add(QT_TRANSLATE_NOOP("ImageProperties", "Gain map encoded size"), QStringLiteral("%1 B").arg(range.length));
    add(QT_TRANSLATE_NOOP("ImageProperties", "Minimum reader version"), QStringLiteral("0"));
    add(QT_TRANSLATE_NOOP("ImageProperties", "Writer version"),
        QString::number(qFromBigEndian<quint16>(bytes.constData() + 2)));
    add(QT_TRANSLATE_NOOP("ImageProperties", "Gain map flags"),
        QStringLiteral("0x%1").arg(flags, 2, 16, QLatin1Char('0')));
    add(QT_TRANSLATE_NOOP("ImageProperties", "Gain map metadata channels"),
        channels == 1 ? QStringLiteral("1") : QStringLiteral("3 (RGB)"));
    add(QT_TRANSLATE_NOOP("ImageProperties", "Gain map application color space"),
        (flags & useBaselineColorSpaceFlag)
            ? translated(QT_TRANSLATE_NOOP("ImageProperties", "Baseline image"))
            : translated(QT_TRANSLATE_NOOP("ImageProperties", "Alternate image")));
    add(QT_TRANSLATE_NOOP("ImageProperties", "Baseline HDR headroom (log₂)"), headroom[0].display());
    add(QT_TRANSLATE_NOOP("ImageProperties", "Alternate HDR headroom (log₂)"), headroom[1].display());
    constexpr std::array names{
        QT_TRANSLATE_NOOP("ImageProperties", "Gain map minimum (log₂)"),
        QT_TRANSLATE_NOOP("ImageProperties", "Gain map maximum (log₂)"),
        QT_TRANSLATE_NOOP("ImageProperties", "Gain map gamma"),
        QT_TRANSLATE_NOOP("ImageProperties", "Baseline offset"),
        QT_TRANSLATE_NOOP("ImageProperties", "Alternate offset")};
    for (size_t i = 0; i < names.size(); ++i)
        add(names[i], channelValues[i].join(QStringLiteral(" · ")));
    return true;
}

} // namespace

void readJpegGainMapMetadata(QFile& file, ImageMetadata& metadata) {
    const qint64 fileSize = file.size();
    const auto baseline = readHeader(file, 0, fileSize, true);
    if (!baseline.isoMetadata) return;
    if (baseline.isoMetadata->size() >= 4 &&
        qFromBigEndian<quint16>(baseline.isoMetadata->constData()) != 0) {
        metadata.metadataWarning = translated(QT_TRANSLATE_NOOP("ImageProperties",
            "This ISO 21496-1 gain map requires a newer metadata reader."));
        return;
    }
    if (baseline.scanOffset > 0 && baseline.isoMetadata->size() >= 4) {
        for (const auto range : auxiliaryImages(baseline, fileSize)) {
            const auto map = readHeader(file, range.offset, range.length, false);
            if (!map.isoMetadata || map.scanOffset == 0 || map.size.isEmpty() ||
                (map.components != 1 && map.components != 3) || map.precision < 1 ||
                !file.seek(range.offset + range.length - 2) || file.read(2) != endOfImage) continue;

            ImageMetadata::PropertyGroup group{QStringLiteral("iso-gain-map"),
                QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "HDR gain map (ISO 21496-1)")), {}, {}};
            if (parseMetadata(*map.isoMetadata, map, range, group)) {
                metadata.embeddedProperties.append(std::move(group));
                return;
            }
        }
    }
    metadata.metadataWarning = translated(QT_TRANSLATE_NOOP("ImageProperties",
        "The embedded ISO 21496-1 gain map or its metadata could not be read."));
}

} // namespace mvpview
