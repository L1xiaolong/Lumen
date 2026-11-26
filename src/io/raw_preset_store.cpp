#include "io/raw_preset_store.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSettings>
#include <QVariantMap>

#include <array>
#include <cstddef>
#include <functional>
#include <limits>

namespace mvpview {
namespace {

constexpr auto kNamedPresetsKey = "rawPresets/named";
constexpr auto kFilenameRulesKey = "rawPresets/filenameRules";

// Settings fallback used when a file has no sidecar. The key deliberately covers one directory
// and suffix instead of a single file: RawParametersController::applyToFolder() persists one
// layout for every file of that suffix in the folder. Per-file parameters always win, because
// loadForFile() prefers the sidecar written by saveSidecar().
QString keyForFile(const QString& path) {
    const QFileInfo info(path);
    const QByteArray identity =
        (info.absolutePath() + QLatin1Char('|') + info.suffix().toLower()).toUtf8();
    return QStringLiteral("rawPresets/%1")
        .arg(QString::fromLatin1(
            QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex()));
}

QVariantMap toMap(const RawImageParameters& parameters) {
    QVariantMap values;
    values.insert(QStringLiteral("width"), parameters.size.width());
    values.insert(QStringLiteral("height"), parameters.size.height());
    values.insert(QStringLiteral("format"), static_cast<int>(parameters.format));
    values.insert(QStringLiteral("offset"), parameters.headerOffset);
    values.insert(QStringLiteral("rowStride"), parameters.rowStride);
    values.insert(QStringLiteral("chromaStride"), parameters.chromaStride);
    values.insert(QStringLiteral("frameIndex"), parameters.frameIndex);
    values.insert(QStringLiteral("littleEndian"), parameters.littleEndian);
    values.insert(QStringLiteral("msbAligned"), parameters.msbAligned);
    values.insert(QStringLiteral("validBits"), parameters.validBitsOverride);
    values.insert(QStringLiteral("bayerPattern"), static_cast<int>(parameters.bayerPattern));
    values.insert(QStringLiteral("bayerSampling"), static_cast<int>(parameters.bayerSampling));
    values.insert(QStringLiteral("matrix"), static_cast<int>(parameters.yuvMatrix));
    values.insert(QStringLiteral("primaries"), static_cast<int>(parameters.yuvPrimaries));
    values.insert(QStringLiteral("transfer"), static_cast<int>(parameters.yuvTransfer));
    values.insert(QStringLiteral("chromaLocation"), static_cast<int>(parameters.chromaLocation));
    values.insert(QStringLiteral("range"), static_cast<int>(parameters.range));
    values.insert(QStringLiteral("orientation"), static_cast<int>(parameters.orientation));
    values.insert(QStringLiteral("blackLevel"), parameters.blackLevel);
    values.insert(QStringLiteral("whiteLevel"), parameters.whiteLevel);
    values.insert(QStringLiteral("demosaic"), parameters.demosaic);
    values.insert(QStringLiteral("wbRed"), parameters.whiteBalanceGains[0]);
    values.insert(QStringLiteral("wbGreen"), parameters.whiteBalanceGains[1]);
    values.insert(QStringLiteral("wbBlue"), parameters.whiteBalanceGains[2]);
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            const auto index = static_cast<std::size_t>(row * 3 + column);
            values.insert(QStringLiteral("ccm%1%2").arg(row).arg(column),
                          parameters.colorCorrectionMatrix[index]);
        }
    }
    values.insert(QStringLiteral("displayGamma"), parameters.displayGamma);
    return values;
}

bool parametersAreValid(const RawImageParameters& result) {
    const int format = static_cast<int>(result.format);
    const int pattern = static_cast<int>(result.bayerPattern);
    const int sampling = static_cast<int>(result.bayerSampling);
    const int matrix = static_cast<int>(result.yuvMatrix);
    const int range = static_cast<int>(result.range);
    return format >= static_cast<int>(RawPixelFormat::NV12) &&
           format <= static_cast<int>(RawPixelFormat::Raw16) &&
           pattern >= static_cast<int>(BayerPattern::RGGB) &&
           pattern <= static_cast<int>(BayerPattern::BGGR) &&
           sampling >= static_cast<int>(BayerSampling::Standard2x2) &&
           sampling <= static_cast<int>(BayerSampling::QuadBayer4x4) &&
           matrix >= static_cast<int>(YuvMatrix::BT601) &&
           matrix <= static_cast<int>(YuvMatrix::BT2020) &&
           range >= static_cast<int>(QuantizationRange::Full) &&
           range <= static_cast<int>(QuantizationRange::Limited) && !result.size.isEmpty() &&
           result.headerOffset >= 0 && result.rowStride >= 0 && result.chromaStride >= 0 &&
           result.frameIndex >= 0 && result.hasValidBitLayout() && result.blackLevel >= 0 &&
           result.blackLevel < result.maximumSampleValue() &&
           (result.whiteLevel == 0 || (result.whiteLevel > result.blackLevel &&
                                       result.whiteLevel <= result.maximumSampleValue())) &&
           result.hasValidDisplayTransform() && result.hasValidOrientation() &&
           result.hasValidBayerSampling() && result.hasValidYuvColorDescription() &&
           frameByteSize(result) > 0;
}

std::optional<RawImageParameters> fromMap(const QVariantMap& values) {
    if (values.isEmpty()) {
        return std::nullopt;
    }
    RawImageParameters result;
    result.size = {values.value(QStringLiteral("width")).toInt(),
                   values.value(QStringLiteral("height")).toInt()};
    result.format = static_cast<RawPixelFormat>(values.value(QStringLiteral("format")).toInt());
    result.headerOffset = values.value(QStringLiteral("offset")).toLongLong();
    result.rowStride = values.value(QStringLiteral("rowStride")).toLongLong();
    result.chromaStride = values.value(QStringLiteral("chromaStride")).toLongLong();
    result.frameIndex = values.value(QStringLiteral("frameIndex")).toInt();
    result.littleEndian = values.value(QStringLiteral("littleEndian"), true).toBool();
    result.msbAligned = values.value(QStringLiteral("msbAligned"), false).toBool();
    result.validBitsOverride = values.value(QStringLiteral("validBits"), 0).toInt();
    result.bayerPattern =
        static_cast<BayerPattern>(values.value(QStringLiteral("bayerPattern")).toInt());
    result.bayerSampling = static_cast<BayerSampling>(
        values.value(QStringLiteral("bayerSampling"), 0).toInt());
    result.yuvMatrix = static_cast<YuvMatrix>(values.value(QStringLiteral("matrix"), 1).toInt());
    result.yuvPrimaries =
        static_cast<YuvPrimaries>(values.value(QStringLiteral("primaries"), 0).toInt());
    result.yuvTransfer =
        static_cast<YuvTransfer>(values.value(QStringLiteral("transfer"), 0).toInt());
    result.chromaLocation = static_cast<ChromaLocation>(
        values.value(QStringLiteral("chromaLocation"), 0).toInt());
    result.range = static_cast<QuantizationRange>(values.value(QStringLiteral("range"), 1).toInt());
    result.orientation =
        static_cast<ImageOrientation>(values.value(QStringLiteral("orientation"), 0).toInt());
    result.blackLevel = values.value(QStringLiteral("blackLevel")).toInt();
    result.whiteLevel = values.value(QStringLiteral("whiteLevel")).toInt();
    result.demosaic = values.value(QStringLiteral("demosaic"), false).toBool();
    result.whiteBalanceGains = {
        values.value(QStringLiteral("wbRed"), 1.0).toDouble(),
        values.value(QStringLiteral("wbGreen"), 1.0).toDouble(),
        values.value(QStringLiteral("wbBlue"), 1.0).toDouble(),
    };
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            const auto index = static_cast<std::size_t>(row * 3 + column);
            const double identity = row == column ? 1.0 : 0.0;
            result.colorCorrectionMatrix[index] =
                values.value(QStringLiteral("ccm%1%2").arg(row).arg(column), identity).toDouble();
        }
    }
    result.displayGamma = values.value(QStringLiteral("displayGamma"), 2.2).toDouble();
    if (!parametersAreValid(result)) {
        return std::nullopt;
    }
    return result;
}

QVariantMap namedPresetMap() { return QSettings().value(kNamedPresetsKey).toMap(); }

bool applyCapturedInteger(const QRegularExpressionMatch& match, const QString& captureName,
                          qint64 minimum, qint64 maximum,
                          const std::function<void(qint64)>& apply) {
    const QString captured = match.captured(captureName);
    if (captured.isEmpty()) {
        return true;
    }
    bool valid = false;
    const qint64 value = captured.toLongLong(&valid);
    if (!valid || value < minimum || value > maximum) {
        return false;
    }
    apply(value);
    return true;
}

bool containsFrame(qint64 fileSize, const RawImageParameters& parameters) {
    return parameters.frameIndex < availableFrameCount(fileSize, parameters);
}

std::optional<RawImageParameters> uniqueMatch(const QVector<RawImageParameters>& matches) {
    if (matches.isEmpty()) return std::nullopt;
    const QString firstKey = matches.first().cacheKey();
    for (const auto& match : matches) {
        if (match.cacheKey() != firstKey) return std::nullopt;
    }
    return matches.first();
}

} // namespace

std::optional<RawImageParameters> RawPresetStore::loadForFile(const QString& path) {
    QFile sidecar(sidecarPath(path));
    if (sidecar.open(QIODevice::ReadOnly)) {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(sidecar.readAll(), &parseError);
        if (parseError.error == QJsonParseError::NoError && document.isObject()) {
            const QJsonObject root = document.object();
            if (root.value(QStringLiteral("version")).toInt() == 1) {
                if (const auto parameters = fromMap(root.toVariantMap())) {
                    return parameters;
                }
            }
        }
    }
    return fromMap(QSettings().value(keyForFile(path)).toMap());
}

void RawPresetStore::saveForFile(const QString& path, const RawImageParameters& parameters) {
    QSettings settings;
    settings.setValue(keyForFile(path), toMap(parameters));
    settings.sync();
}

bool RawPresetStore::saveSidecar(const QString& path, const RawImageParameters& parameters,
                                 QString* error) {
    QJsonObject root = QJsonObject::fromVariantMap(toMap(parameters));
    root.insert(QStringLiteral("version"), 1);
    QSaveFile file(sidecarPath(path));
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    if (file.write(QJsonDocument(root).toJson(QJsonDocument::Indented)) < 0 || !file.commit()) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    return true;
}

QString RawPresetStore::sidecarPath(const QString& path) {
    return path + QStringLiteral(".mvpview.json");
}

namespace {

std::optional<RawImageParameters> matchFilenameRule(const QFileInfo& file,
                                                   const QVariantMap& presets) {
    const QString fileName = file.fileName();
    for (const RawFilenameRule& rule : RawPresetStore::filenameRules()) {
        if (!rule.enabled) {
            continue;
        }
        const QRegularExpression expression(QRegularExpression::anchoredPattern(rule.pattern));
        const auto match = expression.match(fileName);
        if (!expression.isValid() || !match.hasMatch()) {
            continue;
        }
        const auto preset = fromMap(presets.value(rule.presetName).toMap());
        if (!preset) continue;
        RawImageParameters result = *preset;
        const bool capturesValid =
            applyCapturedInteger(
                match, QStringLiteral("width"), 1, std::numeric_limits<int>::max(),
                [&result](qint64 value) { result.size.setWidth(static_cast<int>(value)); }) &&
            applyCapturedInteger(
                match, QStringLiteral("height"), 1, std::numeric_limits<int>::max(),
                [&result](qint64 value) { result.size.setHeight(static_cast<int>(value)); }) &&
            applyCapturedInteger(match, QStringLiteral("row_stride"), 0,
                                 std::numeric_limits<qsizetype>::max(),
                                 [&result](qint64 value) { result.rowStride = value; }) &&
            applyCapturedInteger(match, QStringLiteral("chroma_stride"), 0,
                                 std::numeric_limits<qsizetype>::max(),
                                 [&result](qint64 value) { result.chromaStride = value; }) &&
            applyCapturedInteger(match, QStringLiteral("offset"), 0,
                                 std::numeric_limits<qsizetype>::max(),
                                 [&result](qint64 value) { result.headerOffset = value; }) &&
            applyCapturedInteger(
                match, QStringLiteral("frame"), 0, std::numeric_limits<int>::max(),
                [&result](qint64 value) { result.frameIndex = static_cast<int>(value); }) &&
            applyCapturedInteger(
                match, QStringLiteral("valid_bits"), 0, 16,
                [&result](qint64 value) { result.validBitsOverride = static_cast<int>(value); });
        if (capturesValid && parametersAreValid(result) && containsFrame(file.size(), result)) {
            return result;
        }
    }
    return std::nullopt;
}

struct FilenameHints {
    RawImageParameters parameters;
    bool formatSpecified = true;
    bool patternSpecified = true;
};

FilenameHints filenameHints(const QFileInfo& file) {
    const QString name = file.completeBaseName().toLower();
    FilenameHints hints;
    auto& result = hints.parameters;
    static const QRegularExpression explicitSize(QStringLiteral(R"((?<!\d)(\d{1,6})[x×](\d{1,6})(?!\d))"));
    static const QRegularExpression underscoreSize(QStringLiteral(R"((?<!\d)(\d{2,6})_(\d{2,6})(?!\d))"));
    auto matches = explicitSize.globalMatch(name);
    if (!matches.hasNext()) matches = underscoreSize.globalMatch(name);
    while (matches.hasNext()) {
        const auto match = matches.next();
        result.size = {match.captured(1).toInt(), match.captured(2).toInt()};
    }
    if (name.contains(QStringLiteral("nv12"))) {
        result.format = RawPixelFormat::NV12;
    } else if (name.contains(QStringLiteral("nv21"))) {
        result.format = RawPixelFormat::NV21;
    } else if (name.contains(QStringLiteral("i420")) || name.contains(QStringLiteral("yuv420p"))) {
        result.format = RawPixelFormat::I420;
    } else if (name.contains(QStringLiteral("p010"))) {
        result.format = RawPixelFormat::P010;
        result.msbAligned = true;
    } else if (name.contains(QStringLiteral("raw14"))) {
        result.format = RawPixelFormat::Raw16;
        result.validBitsOverride = 14;
    } else if (name.contains(QStringLiteral("raw10"))) {
        result.format = RawPixelFormat::MipiRaw10;
    } else if (name.contains(QStringLiteral("raw12"))) {
        result.format = RawPixelFormat::MipiRaw12;
    } else if (name.contains(QStringLiteral("raw16"))) {
        result.format = RawPixelFormat::Raw16;
    } else {
        hints.formatSpecified = false;
        result.format = file.suffix().compare(QStringLiteral("raw"), Qt::CaseInsensitive) == 0
            ? RawPixelFormat::Raw16 : RawPixelFormat::NV12;
    }
    // Names such as "raw_8000_6000_10bits.raw" describe unpacked samples in a
    // 16-bit container, not MIPI RAW10 packing. Explicit raw10/raw12 markers above
    // keep their packed interpretation.
    if (result.format == RawPixelFormat::Raw16) {
        for (int bits : {10, 12, 14, 16}) {
            if (name.contains(QStringLiteral("%1bits").arg(bits)) ||
                name.contains(QStringLiteral("%1bit").arg(bits))) {
                result.format = RawPixelFormat::Raw16;
                result.validBitsOverride = bits;
                hints.formatSpecified = true;
                break;
            }
        }
    }
    if (name.contains(QStringLiteral("bggr"))) {
        result.bayerPattern = BayerPattern::BGGR;
    } else if (name.contains(QStringLiteral("gbrg"))) {
        result.bayerPattern = BayerPattern::GBRG;
    } else if (name.contains(QStringLiteral("grbg"))) {
        result.bayerPattern = BayerPattern::GRBG;
    } else {
        hints.patternSpecified = name.contains(QStringLiteral("rggb"));
    }
    if (name.contains(QStringLiteral("quadbayer")) ||
        name.contains(QStringLiteral("quad_bayer")) ||
        name.contains(QStringLiteral("4x4bin")) ||
        name.contains(QStringLiteral("4x4_bin"))) {
        result.bayerSampling = BayerSampling::QuadBayer4x4;
    }
    return hints;
}

bool matchesHints(const RawImageParameters& parameters, const FilenameHints& hints) {
    const auto& expected = hints.parameters;
    return parameters.isYuv() == expected.isYuv() &&
           (!hints.formatSpecified || parameters.format == expected.format) &&
           (expected.size.isEmpty() || parameters.size == expected.size) &&
           (expected.validBitsOverride == 0 || parameters.validBits() == expected.validBits()) &&
           (!hints.patternSpecified || parameters.bayerPattern == expected.bayerPattern) &&
           (expected.bayerSampling != BayerSampling::QuadBayer4x4 ||
            parameters.bayerSampling == expected.bayerSampling);
}

} // namespace

bool RawPresetStore::isHeaderlessImage(const QString& path) {
    return path.endsWith(QStringLiteral(".raw"), Qt::CaseInsensitive) ||
           path.endsWith(QStringLiteral(".yuv"), Qt::CaseInsensitive);
}

std::optional<RawImageParameters> RawPresetStore::resolveForFile(const QFileInfo& file) {
    const QString path = file.filePath();
    if (!isHeaderlessImage(path) || file.size() <= 0) return std::nullopt;
    if (const auto saved = loadForFile(path))
        return containsFrame(file.size(), *saved) ? saved : std::nullopt;

    const QVariantMap presets = namedPresetMap();
    if (const auto rule = matchFilenameRule(file, presets)) return rule;

    const auto hints = filenameHints(file);
    QVector<RawImageParameters> matchingPresets;
    for (const auto& preset : presets) {
        const auto parameters = fromMap(preset.toMap());
        if (!parameters || !matchesHints(*parameters, hints) || !containsFrame(file.size(), *parameters))
            continue;
        const qint64 payload = file.size() - parameters->headerOffset;
        const qsizetype frameBytes = frameByteSize(*parameters);
        if (payload == frameBytes ||
            (!hints.parameters.size.isEmpty() && payload > frameBytes && payload % frameBytes == 0)) {
            matchingPresets.append(*parameters);
        }
    }
    if (!matchingPresets.isEmpty()) return uniqueMatch(matchingPresets);

    // These are storage dimensions. A bare byte count cannot tell us the orientation
    // or chroma order, so generic 8-bit .yuv keeps the existing NV12 default.
    static constexpr std::array commonSizes{
        QSize(320, 240), QSize(640, 480), QSize(720, 480), QSize(720, 576),
        QSize(800, 600), QSize(1024, 768), QSize(1280, 720), QSize(1280, 960),
        QSize(1600, 1200), QSize(1920, 1080), QSize(2048, 1536), QSize(2560, 1440),
        QSize(3840, 2160), QSize(4096, 2160), QSize(4000, 3000), QSize(8000, 6000),
    };
    const bool sizeSpecified = !hints.parameters.size.isEmpty();
    QVector<RawImageParameters> singleFrame;
    QVector<RawImageParameters> multipleFrames;
    const auto considerSize = [&](QSize size) {
        for (const auto format : {RawPixelFormat::NV12, RawPixelFormat::NV21, RawPixelFormat::I420,
                                  RawPixelFormat::P010, RawPixelFormat::MipiRaw10,
                                  RawPixelFormat::MipiRaw12, RawPixelFormat::Raw16}) {
            if (!hints.formatSpecified && (format == RawPixelFormat::NV21 || format == RawPixelFormat::I420))
                continue;
            RawImageParameters candidate = hints.parameters;
            candidate.size = size;
            candidate.format = format;
            candidate.msbAligned = format == RawPixelFormat::P010;
            if (!matchesHints(candidate, hints) || !parametersAreValid(candidate)) continue;
            const qsizetype frameBytes = frameByteSize(candidate);
            if (file.size() == frameBytes)
                singleFrame.append(candidate);
            else if (sizeSpecified && file.size() > frameBytes && file.size() % frameBytes == 0)
                multipleFrames.append(candidate);
        }
    };
    if (sizeSpecified)
        considerSize(hints.parameters.size);
    else
        for (const QSize size : commonSizes) considerSize(size);
    return uniqueMatch(singleFrame.isEmpty() ? multipleFrames : singleFrame);
}

RawImageParameters RawPresetStore::inferFromFileName(const QString& path) {
    return filenameHints(QFileInfo(path)).parameters;
}

QStringList RawPresetStore::namedPresetNames() {
    QStringList names = namedPresetMap().keys();
    names.sort(Qt::CaseInsensitive);
    return names;
}

std::optional<RawImageParameters> RawPresetStore::loadNamedPreset(const QString& name) {
    const QString normalized = name.trimmed();
    if (normalized.isEmpty()) {
        return std::nullopt;
    }
    return fromMap(namedPresetMap().value(normalized).toMap());
}

bool RawPresetStore::saveNamedPreset(const QString& name, const RawImageParameters& parameters) {
    const QString normalized = name.trimmed();
    RawImageParameters reusable = parameters;
    reusable.frameIndex = 0;
    if (normalized.isEmpty() || normalized.size() > 128 || !parametersAreValid(reusable)) {
        return false;
    }
    QVariantMap presets = namedPresetMap();
    presets.insert(normalized, toMap(reusable));
    QSettings settings;
    settings.setValue(kNamedPresetsKey, presets);
    settings.sync();
    return settings.status() == QSettings::NoError;
}

bool RawPresetStore::removeNamedPreset(const QString& name) {
    QVariantMap presets = namedPresetMap();
    if (presets.remove(name.trimmed()) == 0) {
        return false;
    }
    QSettings settings;
    settings.setValue(kNamedPresetsKey, presets);
    settings.sync();
    return settings.status() == QSettings::NoError;
}

QVector<RawFilenameRule> RawPresetStore::filenameRules() {
    QVector<RawFilenameRule> result;
    const QVariantList stored = QSettings().value(kFilenameRulesKey).toList();
    result.reserve(stored.size());
    for (const QVariant& value : stored) {
        const QVariantMap map = value.toMap();
        RawFilenameRule rule{map.value(QStringLiteral("name")).toString(),
                             map.value(QStringLiteral("pattern")).toString(),
                             map.value(QStringLiteral("preset")).toString(),
                             map.value(QStringLiteral("enabled"), true).toBool()};
        if (!rule.name.isEmpty() && !rule.pattern.isEmpty() && !rule.presetName.isEmpty()) {
            result.push_back(std::move(rule));
        }
    }
    return result;
}

bool RawPresetStore::saveFilenameRules(const QVector<RawFilenameRule>& rules) {
    const QStringList presetNames = namedPresetNames();
    const QSet<QString> presets(presetNames.cbegin(), presetNames.cend());
    QSet<QString> normalizedNames;
    QVariantList stored;
    stored.reserve(rules.size());
    for (const RawFilenameRule& source : rules) {
        RawFilenameRule rule{source.name.trimmed(), source.pattern.trimmed(),
                             source.presetName.trimmed(), source.enabled};
        const QString normalizedName = rule.name.toCaseFolded();
        const QRegularExpression expression(rule.pattern);
        if (rule.name.isEmpty() || rule.name.size() > 128 || rule.pattern.isEmpty() ||
            rule.pattern.size() > 1024 || !expression.isValid() ||
            !presets.contains(rule.presetName) || normalizedNames.contains(normalizedName)) {
            return false;
        }
        normalizedNames.insert(normalizedName);
        stored.push_back(QVariantMap{{QStringLiteral("name"), rule.name},
                                     {QStringLiteral("pattern"), rule.pattern},
                                     {QStringLiteral("preset"), rule.presetName},
                                     {QStringLiteral("enabled"), rule.enabled}});
    }
    QSettings settings;
    settings.setValue(kFilenameRulesKey, stored);
    settings.sync();
    return settings.status() == QSettings::NoError;
}

} // namespace mvpview
