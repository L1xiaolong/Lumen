#include "io/metadata_reader.h"

#include "io/metadata_exiv2_lock.h"
#include "io/native_metadata.h"

#include <QCache>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string_view>

#if MVPVIEW_HAS_EXIV2
#include <exiv2/exiv2.hpp>
#endif

namespace mvpview {
namespace {

#if MVPVIEW_HAS_EXIV2

constexpr qsizetype kMaximumTextLength = 4096;
constexpr qsizetype kMaximumWarningLength = 512;
constexpr int kMaximumCachedMetadataKiB = 8192;

struct CachedMetadata {
    QString readerName;
    QString readerVersion;
    QString warning;
    ImageMetadata::Orientation orientation = ImageMetadata::Orientation::Unspecified;
    bool gpsPresent = false;
    std::optional<ImageMetadata::Camera> camera;
    std::optional<ImageMetadata::Descriptive> descriptive;
    QVector<ImageMetadata::PropertyGroup> properties;
};

QString cacheKey(const ImageMetadata& metadata) {
    return QStringLiteral("%1:%2:%3:%4")
        .arg(metadata.path.size())
        .arg(metadata.path)
        .arg(metadata.fileSize)
        .arg(metadata.modifiedAt.toMSecsSinceEpoch());
}

void applyCached(const CachedMetadata& cached, ImageMetadata& metadata) {
    metadata.metadataReaderName = cached.readerName;
    metadata.metadataReaderVersion = cached.readerVersion;
    metadata.metadataWarning = cached.warning;
    metadata.sourceOrientation = cached.orientation;
    metadata.gpsMetadataPresent = cached.gpsPresent;
    metadata.camera = cached.camera;
    metadata.descriptive = cached.descriptive;
    metadata.embeddedProperties = cached.properties;
}

CachedMetadata cachedFrom(const ImageMetadata& metadata) {
    return {metadata.metadataReaderName, metadata.metadataReaderVersion, metadata.metadataWarning,
            metadata.sourceOrientation,  metadata.gpsMetadataPresent,    metadata.camera,
            metadata.descriptive, metadata.embeddedProperties};
}

int cacheCost(const ImageMetadata& metadata) {
    qsizetype bytes = 1024;
    for (const auto& group : metadata.embeddedProperties) {
        for (const auto& property : group.properties)
            bytes += 128 + 2 * (property.name.size() + property.value.size() + property.type.size());
    }
    return static_cast<int>(std::min<qsizetype>((bytes + 1023) / 1024, kMaximumCachedMetadataKiB + 1));
}

QString sanitized(QString text, qsizetype maximumLength = kMaximumTextLength) {
    text.replace(QChar::Null, QChar::ReplacementCharacter);
    for (QChar& character : text) {
        if (character.category() == QChar::Other_Control && character != QLatin1Char('\n') &&
            character != QLatin1Char('\t')) {
            character = QChar::ReplacementCharacter;
        }
    }
    return text.trimmed().left(maximumLength);
}

QString metadataErrorText(const char* message) {
#ifdef Q_OS_WIN
    // Exiv2's narrow FileIo path (QFile::encodeName above) and CRT error
    // messages use the Windows local code page. Metadata tag values remain UTF-8.
    return QString::fromLocal8Bit(message);
#else
    return QString::fromUtf8(message);
#endif
}

QString textFrom(const Exiv2::Metadatum& datum) {
    try {
        return sanitized(QString::fromUtf8(datum.toString()));
    } catch (const std::exception&) {
        return {};
    }
}

template <typename Data>
void addTags(ImageMetadata& metadata, const Data& data, const QString& id, const QString& title,
             auto include) {
    ImageMetadata::PropertyGroup group{id, title, {}, {}, true};
    for (const auto& datum : data) {
        if (!include(datum)) continue;
        const auto bytes = datum.size();
        auto value = (datum.typeId() == Exiv2::undefined && bytes > 64) || bytes > 64 * 1024
            ? QStringLiteral("%1 B").arg(bytes) : textFrom(datum);
        if (value.isEmpty()) value = QStringLiteral("%1 B").arg(bytes);
        const char* type = datum.typeName();
        group.properties.append({QString::fromUtf8(datum.key()), std::move(value),
                                 type ? QString::fromLatin1(type) : QString{}});
    }
    if (!group.properties.isEmpty()) {
        std::stable_sort(group.properties.begin(), group.properties.end(), [](const auto& a, const auto& b) {
            return a.name < b.name;
        });
        metadata.embeddedProperties.append(std::move(group));
    }
}

bool isDngCalibrationTag(const Exiv2::Exifdatum& datum) {
    const std::string name = datum.tagName();
    static constexpr std::array<std::string_view, 41> names{
        "DNGVersion", "DNGBackwardVersion", "UniqueCameraModel", "LocalizedCameraModel",
        "CFARepeatPatternDim", "CFAPattern", "CFAPlaneColor", "CFALayout", "LinearizationTable",
        "BlackLevelRepeatDim", "BlackLevel", "BlackLevelDeltaH", "BlackLevelDeltaV", "WhiteLevel",
        "DefaultScale", "DefaultCropOrigin", "DefaultCropSize", "DefaultUserCrop", "ActiveArea",
        "MaskedAreas", "ColorMatrix1", "ColorMatrix2", "ColorMatrix3", "CameraCalibration1",
        "CameraCalibration2", "CameraCalibration3", "ReductionMatrix1", "ReductionMatrix2",
        "ReductionMatrix3", "AnalogBalance", "AsShotNeutral", "AsShotWhiteXY", "BaselineExposure",
        "BaselineNoise", "BaselineSharpness", "CalibrationIlluminant1", "CalibrationIlluminant2",
        "CalibrationIlluminant3", "ForwardMatrix1", "ForwardMatrix2", "ForwardMatrix3"};
    return std::ranges::find(names, std::string_view(name)) != names.end();
}

QString exifText(const Exiv2::ExifData& data, const char* key) {
    const auto position = data.findKey(Exiv2::ExifKey(key));
    return position == data.end() ? QString{} : textFrom(*position);
}

QString iptcText(const Exiv2::IptcData& data, const char* key) {
    const auto position = data.findKey(Exiv2::IptcKey(key));
    return position == data.end() ? QString{} : textFrom(*position);
}

QString xmpText(const Exiv2::XmpData& data, const char* key) {
    const auto position = data.findKey(Exiv2::XmpKey(key));
    if (position == data.end()) {
        return {};
    }
    QString value = textFrom(*position);
    if (value.startsWith(QStringLiteral("lang=\""))) {
        const qsizetype closingQuote = value.indexOf(QLatin1Char('"'), 6);
        if (closingQuote >= 0) {
            value = value.mid(closingQuote + 1).trimmed();
        }
    }
    return value;
}

template <typename Data, typename Key>
std::optional<double> optionalNumericValue(const Data& data, const Key& key) {
    const auto position = data.findKey(key);
    if (position == data.end() || position->count() == 0) {
        return std::nullopt;
    }
    try {
        const double value = position->toFloat();
        return std::isfinite(value) ? std::optional<double>(value) : std::nullopt;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

template <typename Data, typename Key> double numericValue(const Data& data, const Key& key) {
    return optionalNumericValue(data, key).value_or(0.0);
}

QDateTime capturedAt(const Exiv2::ExifData& exif, const Exiv2::XmpData& xmp) {
    QString value = exifText(exif, "Exif.Photo.DateTimeOriginal");
    if (value.isEmpty()) {
        value = exifText(exif, "Exif.Image.DateTime");
    }
    if (!value.isEmpty()) {
        const QString offset = exifText(exif, "Exif.Photo.OffsetTimeOriginal");
        static const QRegularExpression offsetPattern(QStringLiteral("^[+-]\\d{2}:\\d{2}$"));
        if (offsetPattern.match(offset).hasMatch()) {
            QString isoValue = value;
            if (isoValue.size() >= 10) {
                isoValue[4] = QLatin1Char('-');
                isoValue[7] = QLatin1Char('-');
            }
            const QDateTime result = QDateTime::fromString(isoValue + offset, Qt::ISODate);
            if (result.isValid()) {
                return result;
            }
        }
        const QDateTime result =
            QDateTime::fromString(value, QStringLiteral("yyyy:MM:dd HH:mm:ss"));
        if (result.isValid()) {
            return result;
        }
    }
    value = xmpText(xmp, "Xmp.photoshop.DateCreated");
    if (value.isEmpty()) {
        value = xmpText(xmp, "Xmp.xmp.CreateDate");
    }
    return QDateTime::fromString(value, Qt::ISODate);
}

QString firstNonEmpty(std::initializer_list<QString> candidates) {
    for (const QString& candidate : candidates) {
        if (!candidate.isEmpty()) {
            return candidate;
        }
    }
    return {};
}

QString exposureProgramText(int value) {
    switch (value) {
    case 1:
        return QStringLiteral("Manual");
    case 2:
        return QStringLiteral("Normal program");
    case 3:
        return QStringLiteral("Aperture priority");
    case 4:
        return QStringLiteral("Shutter priority");
    case 5:
        return QStringLiteral("Creative program");
    case 6:
        return QStringLiteral("Action program");
    case 7:
        return QStringLiteral("Portrait mode");
    case 8:
        return QStringLiteral("Landscape mode");
    default:
        return value == 0 ? QString{} : QString::number(value);
    }
}

QString meteringModeText(int value) {
    switch (value) {
    case 1:
        return QStringLiteral("Average");
    case 2:
        return QStringLiteral("Center-weighted average");
    case 3:
        return QStringLiteral("Spot");
    case 4:
        return QStringLiteral("Multi-spot");
    case 5:
        return QStringLiteral("Pattern");
    case 6:
        return QStringLiteral("Partial");
    case 255:
        return QStringLiteral("Other");
    default:
        return value == 0 ? QString{} : QString::number(value);
    }
}

QString flashText(int value) {
    if (value < 0) {
        return {};
    }
    QStringList parts;
    parts << ((value & 0x1) ? QStringLiteral("Fired") : QStringLiteral("Did not fire"));
    if ((value & 0x18) == 0x18) {
        parts << QStringLiteral("Auto");
    } else if ((value & 0x18) == 0x08) {
        parts << QStringLiteral("Compulsory");
    } else if ((value & 0x18) == 0x10) {
        parts << QStringLiteral("Suppressed");
    }
    if (value & 0x40) {
        parts << QStringLiteral("Red-eye reduction");
    }
    return parts.join(QStringLiteral(", "));
}

QString gpsText(const Exiv2::ExifData& exif) {
    const QString latitude = exifText(exif, "Exif.GPSInfo.GPSLatitude");
    const QString latitudeRef = exifText(exif, "Exif.GPSInfo.GPSLatitudeRef");
    const QString longitude = exifText(exif, "Exif.GPSInfo.GPSLongitude");
    const QString longitudeRef = exifText(exif, "Exif.GPSInfo.GPSLongitudeRef");
    const QString altitude = exifText(exif, "Exif.GPSInfo.GPSAltitude");
    QStringList parts;
    if (!latitude.isEmpty()) {
        parts << QStringLiteral("%1 %2").arg(latitude, latitudeRef).trimmed();
    }
    if (!longitude.isEmpty()) {
        parts << QStringLiteral("%1 %2").arg(longitude, longitudeRef).trimmed();
    }
    if (!altitude.isEmpty()) {
        parts << QStringLiteral("Altitude %1").arg(altitude);
    }
    return parts.join(QStringLiteral(", "));
}

bool hasGpsMetadata(const Exiv2::ExifData& exif, const Exiv2::XmpData& xmp) {
    const auto hasGpsPrefix = [](const auto& datum) {
        const std::string key = datum.key();
        return key.starts_with("Exif.GPSInfo.") || key.starts_with("Xmp.exif.GPS");
    };
    return std::any_of(exif.begin(), exif.end(), hasGpsPrefix) ||
           std::any_of(xmp.begin(), xmp.end(), hasGpsPrefix);
}

void mapCamera(const Exiv2::ExifData& exif, const Exiv2::XmpData& xmp, ImageMetadata& metadata) {
    ImageMetadata::Camera camera;
    camera.make = exifText(exif, "Exif.Image.Make");
    camera.model = exifText(exif, "Exif.Image.Model");
    camera.software = exifText(exif, "Exif.Image.Software");
    camera.lens =
        firstNonEmpty({exifText(exif, "Exif.Photo.LensModel"), xmpText(xmp, "Xmp.aux.Lens")});
    camera.capturedAt = capturedAt(exif, xmp);
    camera.exposureSeconds = numericValue(exif, Exiv2::ExifKey("Exif.Photo.ExposureTime"));
    camera.aperture = numericValue(exif, Exiv2::ExifKey("Exif.Photo.FNumber"));
    camera.focalLengthMm = numericValue(exif, Exiv2::ExifKey("Exif.Photo.FocalLength"));
    const double iso = numericValue(exif, Exiv2::ExifKey("Exif.Photo.ISOSpeedRatings"));
    camera.iso = iso > 0.0 && iso <= std::numeric_limits<int>::max() ? static_cast<int>(std::lround(iso)) : 0;
    camera.exposureProgram = exposureProgramText(
        static_cast<int>(numericValue(exif, Exiv2::ExifKey("Exif.Photo.ExposureProgram"))));
    camera.meteringMode = meteringModeText(
        static_cast<int>(numericValue(exif, Exiv2::ExifKey("Exif.Photo.MeteringMode"))));
    if (const auto compensation =
            optionalNumericValue(exif, Exiv2::ExifKey("Exif.Photo.ExposureBiasValue"))) {
        camera.exposureCompensation = QStringLiteral("%1 EV").arg(*compensation, 0, 'g', 4);
    }
    const auto flash = optionalNumericValue(exif, Exiv2::ExifKey("Exif.Photo.Flash"));
    camera.flash = flash ? flashText(static_cast<int>(std::lround(*flash))) : QString{};
    camera.gps = gpsText(exif);
    if (metadata.camera) {
        const auto& source = *metadata.camera;
        camera.sensorSize = source.sensorSize;
        if (camera.make.isEmpty()) camera.make = source.make;
        if (camera.model.isEmpty()) camera.model = source.model;
        if (camera.lens.isEmpty()) camera.lens = source.lens;
        if (!camera.capturedAt.isValid()) camera.capturedAt = source.capturedAt;
        if (camera.exposureSeconds <= 0.0) camera.exposureSeconds = source.exposureSeconds;
        if (camera.aperture <= 0.0) camera.aperture = source.aperture;
        if (camera.focalLengthMm <= 0.0) camera.focalLengthMm = source.focalLengthMm;
        if (camera.iso <= 0) camera.iso = source.iso;
    }
    if (!camera.make.isEmpty() || !camera.model.isEmpty() || !camera.lens.isEmpty() ||
        !camera.software.isEmpty() || camera.capturedAt.isValid() || camera.exposureSeconds > 0.0 ||
        camera.aperture > 0.0 || camera.focalLengthMm > 0.0 || camera.iso > 0 ||
        !camera.exposureProgram.isEmpty() || !camera.meteringMode.isEmpty() ||
        !camera.exposureCompensation.isEmpty() || !camera.flash.isEmpty() ||
        !camera.gps.isEmpty()) {
        metadata.camera = std::move(camera);
    }
}

void mapDescription(const Exiv2::ExifData& exif, const Exiv2::IptcData& iptc,
                    const Exiv2::XmpData& xmp, ImageMetadata& metadata) {
    ImageMetadata::Descriptive descriptive;
    descriptive.title = firstNonEmpty(
        {xmpText(xmp, "Xmp.dc.title"), iptcText(iptc, "Iptc.Application2.ObjectName")});
    descriptive.description = firstNonEmpty({xmpText(xmp, "Xmp.dc.description"),
                                             iptcText(iptc, "Iptc.Application2.Caption"),
                                             exifText(exif, "Exif.Image.ImageDescription")});
    descriptive.creator =
        firstNonEmpty({xmpText(xmp, "Xmp.dc.creator"), iptcText(iptc, "Iptc.Application2.Byline"),
                       exifText(exif, "Exif.Image.Artist")});
    descriptive.copyright =
        firstNonEmpty({xmpText(xmp, "Xmp.dc.rights"), iptcText(iptc, "Iptc.Application2.Copyright"),
                       exifText(exif, "Exif.Image.Copyright")});
    if (!descriptive.title.isEmpty() || !descriptive.description.isEmpty() ||
        !descriptive.creator.isEmpty() || !descriptive.copyright.isEmpty()) {
        metadata.descriptive = std::move(descriptive);
    }
}

#endif

} // namespace

bool MetadataReader::isAvailable() {
#if MVPVIEW_HAS_EXIV2
    return true;
#else
    return false;
#endif
}

QString MetadataReader::version() {
#if MVPVIEW_HAS_EXIV2
    return QString::fromStdString(Exiv2::versionString());
#else
    return {};
#endif
}

void MetadataReader::enrich(const QString& path, ImageMetadata& metadata) {
#if MVPVIEW_HAS_EXIV2
    // XMP has process-global initialization state. Serializing this optional adapter keeps
    // decoding deterministic until profiling justifies a narrower lock.
    static QCache<QString, CachedMetadata> cache(kMaximumCachedMetadataKiB);
    const QString key = cacheKey(metadata);
    {
        const std::scoped_lock lock(exiv2MetadataMutex());
        if (const CachedMetadata* cached = cache.object(key)) {
            applyCached(*cached, metadata);
            return;
        }
    }
    const bool readTags = readNativeMetadata(path, metadata);
    const std::scoped_lock lock(exiv2MetadataMutex());
    if (const CachedMetadata* cached = cache.object(key)) {
        applyCached(*cached, metadata);
        return;
    }
    const auto suffix = QFileInfo(path).suffix().toLower();
    if (!readTags ||
        ((suffix == QStringLiteral("raw") || suffix == QStringLiteral("yuv")) &&
         metadata.decoderName != QStringLiteral("LibRaw"))) {
        cache.insert(key, new CachedMetadata(cachedFrom(metadata)), cacheCost(metadata));
        return;
    }
    const auto nativeWarning = metadata.metadataWarning;
    metadata.metadataReaderName = QStringLiteral("Exiv2");
    metadata.metadataReaderVersion = version();
    try {
        // Exiv2 0.28 accepts a narrow file name. QFile supplies the native
        // encoding on Windows and the file-system encoding on macOS.
        const QByteArray nativePath = QFile::encodeName(path);
        Exiv2::Image::UniquePtr image;
#if defined(Q_OS_WIN)
        QByteArray encodedImage;
        if (QFile::decodeName(nativePath) != path) {
            // Some names cannot be represented in the active Windows code page.
            // Let Qt open the Unicode path and let Exiv2 parse its memory input.
            QFile source(path);
            if (!source.open(QIODevice::ReadOnly)) {
                throw std::runtime_error("could not open image for metadata");
            }
            encodedImage = source.readAll();
            image = Exiv2::ImageFactory::open(
                reinterpret_cast<const Exiv2::byte*>(encodedImage.constData()),
                static_cast<std::size_t>(encodedImage.size()));
        } else
#endif
        {
            image = Exiv2::ImageFactory::open(nativePath.toStdString());
        }
        if (!image) {
            throw std::runtime_error("unsupported metadata container");
        }
        image->readMetadata();
        const Exiv2::ExifData& exif = image->exifData();
        const Exiv2::IptcData& iptc = image->iptcData();
        const Exiv2::XmpData& xmp = image->xmpData();
        const double orientation = numericValue(exif, Exiv2::ExifKey("Exif.Image.Orientation"));
        if (metadata.sourceOrientation == ImageMetadata::Orientation::Unspecified &&
            orientation >= 1.0 && orientation <= 8.0) {
            metadata.sourceOrientation =
                static_cast<ImageMetadata::Orientation>(static_cast<int>(orientation));
        }
        metadata.gpsMetadataPresent = hasGpsMetadata(exif, xmp);
        mapCamera(exif, xmp, metadata);
        mapDescription(exif, iptc, xmp, metadata);
        const bool dng = suffix == QStringLiteral("dng");
        const auto maker = [](const Exiv2::Exifdatum& datum) { return Exiv2::ExifTags::isMakerGroup(datum.groupName()); };
        const auto all = [](const auto&) { return true; };
        if (dng) addTags(metadata, exif, QStringLiteral("dng-tags"), QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "DNG calibration")), isDngCalibrationTag);
        addTags(metadata, exif, QStringLiteral("exif-tags"), QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "EXIF tags")),
                [&](const Exiv2::Exifdatum& datum) { return !maker(datum) && !(dng && isDngCalibrationTag(datum)); });
        addTags(metadata, exif, QStringLiteral("maker-tags"), QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "Camera maker notes")), maker);
        addTags(metadata, iptc, QStringLiteral("iptc-tags"), QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "IPTC tags")), all);
        addTags(metadata, xmp, QStringLiteral("xmp-tags"), QStringLiteral(QT_TRANSLATE_NOOP("ImageProperties", "XMP tags")), all);
    } catch (const Exiv2::Error& error) {
        metadata.metadataWarning =
            sanitized(metadataErrorText(error.what()), kMaximumWarningLength);
    } catch (const std::exception& error) {
        metadata.metadataWarning =
            sanitized(metadataErrorText(error.what()), kMaximumWarningLength);
    } catch (...) {
        metadata.metadataWarning = QStringLiteral("Unknown metadata parsing error");
    }
    if (!nativeWarning.isEmpty() && !metadata.metadataWarning.startsWith(nativeWarning))
        metadata.metadataWarning.prepend(nativeWarning + QLatin1Char('\n'));
    cache.insert(key, new CachedMetadata(cachedFrom(metadata)), cacheCost(metadata));
#else
    readNativeMetadata(path, metadata);
#endif
}

} // namespace mvpview
