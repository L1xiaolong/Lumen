#include "qml/properties_export.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QVariantMap>
#include <QVector>

#include <cmath>

namespace mvpview {
namespace PropertiesExport {
namespace {

constexpr int kRuleWidth = 80;
constexpr int kMaxLabelColumns = 30;
constexpr int kHeaderLabelColumns = 9;

QString text(const char* source) {
    return QCoreApplication::translate("ImageProperties", source);
}

bool isWideCodePoint(char32_t code) {
    return (code >= 0x1100 && code <= 0x115F)      // Hangul Jamo
        || (code >= 0x2E80 && code <= 0x303E)      // CJK radicals, Kangxi, CJK symbols
        || (code >= 0x3041 && code <= 0x33FF)      // Kana, Bopomofo, CJK compatibility
        || (code >= 0x3400 && code <= 0x4DBF)      // CJK Extension A
        || (code >= 0x4E00 && code <= 0x9FFF)      // CJK Unified Ideographs
        || (code >= 0xA000 && code <= 0xA4CF)      // Yi Syllables
        || (code >= 0xAC00 && code <= 0xD7A3)      // Hangul Syllables
        || (code >= 0xF900 && code <= 0xFAFF)      // CJK Compatibility Ideographs
        || (code >= 0xFE30 && code <= 0xFE4F)      // CJK Compatibility Forms
        || (code >= 0xFF01 && code <= 0xFF60)      // Fullwidth Forms
        || (code >= 0xFFE0 && code <= 0xFFE6)
        || (code >= 0x20000 && code <= 0x3FFFD);   // CJK Extension B and beyond
}

// Terminal column count, counting East Asian wide characters as two columns so
// translated (e.g. Chinese) labels still align with plain spaces.
int displayWidth(const QString& value) {
    int width = 0;
    for (qsizetype index = 0; index < value.size();) {
        const QChar lead = value.at(index);
        char32_t code = lead.unicode();
        ++index;
        if (lead.isHighSurrogate() && index < value.size() && value.at(index).isLowSurrogate()) {
            code = QChar::surrogateToUcs4(lead, value.at(index));
            ++index;
        }
        width += isWideCodePoint(code) ? 2 : 1;
    }
    return width;
}

QString spaces(int count) {
    return QString(qMax(1, count), QLatin1Char(' '));
}

QString rule(QChar fill) {
    return QString(kRuleWidth, fill);
}

QString centered(const QString& value) {
    const int padding = (kRuleWidth - displayWidth(value)) / 2;
    return padding > 0 ? spaces(padding) + value : value;
}

struct FieldRow {
    QString label;
    QString value;
    QVariantList matrix;
};

QVector<FieldRow> fieldRows(const QVariantList& fields) {
    QVector<FieldRow> rows;
    rows.reserve(fields.size());
    for (const QVariant& entry : fields) {
        const QVariantMap field = entry.toMap();
        FieldRow row;
        row.label = field.value(QStringLiteral("label")).toString().trimmed();
        row.value = field.value(QStringLiteral("value")).toString();
        if (field.contains(QStringLiteral("matrix")))
            row.matrix = field.value(QStringLiteral("matrix")).toList();
        rows.append(row);
    }
    return rows;
}

void appendFieldRow(QStringList& lines, const FieldRow& row, int labelColumns) {
    const int labelWidth = displayWidth(row.label);
    const int gap = labelWidth <= labelColumns ? labelColumns - labelWidth + 2 : 2;
    if (!row.matrix.isEmpty()) {
        // Matrix values render as three continuation rows below the label,
        // mirroring the inspector's grid instead of one long " | " line.
        lines.append(spaces(2) + row.label + QLatin1Char(':'));
        for (int start = 0; start < row.matrix.size(); start += 3) {
            QStringList values;
            for (int column = 0; column < 3 && start + column < row.matrix.size(); ++column)
                values.append(row.matrix.at(start + column).toString());
            lines.append(spaces(4) + values.join(QStringLiteral(", ")));
        }
        return;
    }
    const QString prefix = spaces(2) + row.label + QLatin1Char(':') + spaces(gap);
    const QStringList valueLines = row.value.split(QLatin1Char('\n'));
    lines.append(prefix + valueLines.constFirst().trimmed());
    for (int index = 1; index < valueLines.size(); ++index)
        lines.append(spaces(4) + valueLines.at(index).trimmed());
}

void appendGroup(QStringList& lines, const QVariantMap& group) {
    const QVector<FieldRow> rows = fieldRows(group.value(QStringLiteral("fields")).toList());
    if (rows.isEmpty()) return;
    const QString title = group.value(QStringLiteral("title")).toString();
    int labelColumns = 0;
    for (const FieldRow& row : rows)
        labelColumns = qMax(labelColumns, qMin(kMaxLabelColumns, displayWidth(row.label)));
    lines.append(QString());
    lines.append(title);
    for (const FieldRow& row : rows) appendFieldRow(lines, row, labelColumns);
}

void appendSection(QStringList& lines, const QString& title, const QVariantList& groups) {
    if (groups.isEmpty()) return;
    // Only groups with at least one field row contribute; the inspector never
    // emits field-less groups, and a section of bare rules would look broken.
    bool hasFields = false;
    for (const QVariant& entry : groups)
        if (!entry.toMap().value(QStringLiteral("fields")).toList().isEmpty()) {
            hasFields = true;
            break;
        }
    if (!hasFields) return;
    lines.append(QString());
    lines.append(QString());
    lines.append(rule(QLatin1Char('-')));
    lines.append(title);
    lines.append(rule(QLatin1Char('-')));
    for (const QVariant& entry : groups) appendGroup(lines, entry.toMap());
}

// Header block rows share one label column ("Exported" is the longest label).
QString headerRow(const char* labelSource, const QString& value) {
    const QString label = text(labelSource);
    return spaces(2) + label + QLatin1Char(':')
        + spaces(kHeaderLabelColumns - displayWidth(label) + 2) + value;
}

// Mirrors PropertiesHistogram's formatStatistic: fixed decimals for integer
// sample domains, a compact precise form for floating-point (EXR) domains.
QString statisticText(const QVariant& value, int decimals, bool floatingPoint) {
    const double number = value.toDouble();
    if (!std::isfinite(number)) return {};
    if (!floatingPoint) return QString::number(number, 'f', decimals);
    if (number == std::round(number) && std::abs(number) < 1e15)
        return QString::number(number, 'f', 0);
    return QString::number(number, 'g', 6);
}

// The inspector's Histogram tab reduced to an aligned statistics table: one row
// per channel with the same Mean/Variance/Min/Max/Median values it shows.
void appendHistogramSection(QStringList& lines, const QVariantMap& histogram) {
    const QVariantList channels = histogram.value(QStringLiteral("channels")).toList();
    if (!histogram.value(QStringLiteral("valid")).toBool() || channels.isEmpty()) return;
    const bool floatingPoint = histogram.value(QStringLiteral("floatingPoint")).toBool();
    lines.append(QString());
    lines.append(QString());
    lines.append(rule(QLatin1Char('-')));
    lines.append(text(QT_TRANSLATE_NOOP("ImageProperties", "Histogram")));
    lines.append(rule(QLatin1Char('-')));
    const QString summary = histogram.value(QStringLiteral("summary")).toString().trimmed();
    if (!summary.isEmpty()) {
        lines.append(QString());
        lines.append(spaces(2) + summary);
    }
    lines.append(QString());
    QStringList header;
    header << text(QT_TRANSLATE_NOOP("ImageProperties", "Channel"))
           << text(QT_TRANSLATE_NOOP("ImageProperties", "Mean"))
           << text(QT_TRANSLATE_NOOP("ImageProperties", "Variance"))
           << text(QT_TRANSLATE_NOOP("ImageProperties", "Min"))
           << text(QT_TRANSLATE_NOOP("ImageProperties", "Max"))
           << text(QT_TRANSLATE_NOOP("ImageProperties", "Median"))
           << text(QT_TRANSLATE_NOOP("ImageProperties", "Samples"));
    QVector<QStringList> rows;
    rows.append(header);
    for (const QVariant& entry : channels) {
        const QVariantMap channel = entry.toMap();
        if (channel.isEmpty()) continue;
        rows.append(QStringList{
            channel.value(QStringLiteral("name")).toString(),
            statisticText(channel.value(QStringLiteral("mean")), 2, floatingPoint),
            statisticText(channel.value(QStringLiteral("variance")), 2, floatingPoint),
            statisticText(channel.value(QStringLiteral("min")), 0, floatingPoint),
            statisticText(channel.value(QStringLiteral("max")), 0, floatingPoint),
            statisticText(channel.value(QStringLiteral("median")), 1, floatingPoint),
            QLocale().toString(channel.value(QStringLiteral("sampleCount")).toLongLong())});
    }
    if (rows.size() <= 1) return;
    QVector<int> widths(header.size(), 0);
    for (const QStringList& row : rows)
        for (int column = 0; column < row.size(); ++column)
            widths[column] = qMax(widths[column], displayWidth(row.at(column)));
    for (const QStringList& row : rows) {
        QStringList cells;
        for (int column = 0; column < row.size(); ++column)
            cells.append(row.at(column)
                         + spaces(widths.at(column) - displayWidth(row.at(column)) + 2));
        lines.append(spaces(2) + cells.join(QString()));
    }
}

} // namespace

QString documentText(const QString& fileName, const QString& filePath,
                     const QVariantList& overviewGroups, const QVariantList& advancedGroups,
                     const QStringList& warnings, const QVariantMap& histogram,
                     const QDateTime& exportedAt) {
    QStringList lines;
    lines.append(rule(QLatin1Char('=')));
    lines.append(centered(text(QT_TRANSLATE_NOOP("ImageProperties", "Lumen Image Properties"))));
    lines.append(rule(QLatin1Char('=')));
    lines.append(headerRow(QT_TRANSLATE_NOOP("ImageProperties", "File"), fileName));
    lines.append(headerRow(QT_TRANSLATE_NOOP("ImageProperties", "Path"),
                           QDir::toNativeSeparators(filePath)));
    lines.append(headerRow(QT_TRANSLATE_NOOP("ImageProperties", "Exported"),
                           QLocale().toString(exportedAt, QLocale::ShortFormat)));
    appendSection(lines, text(QT_TRANSLATE_NOOP("ImageProperties", "Overview")), overviewGroups);
    appendSection(lines, text(QT_TRANSLATE_NOOP("ImageProperties", "Advanced")), advancedGroups);
    appendHistogramSection(lines, histogram);
    if (!warnings.isEmpty()) {
        lines.append(QString());
        lines.append(QString());
        lines.append(rule(QLatin1Char('-')));
        lines.append(text(QT_TRANSLATE_NOOP("ImageProperties", "Warnings")));
        lines.append(rule(QLatin1Char('-')));
        for (const QString& warning : warnings) {
            if (warning.trimmed().isEmpty()) continue;
            lines.append(spaces(2) + QLatin1String("- ") + warning.trimmed());
        }
    }
    // Field rows pad labels with spaces; drop any trailing padding per line.
    for (QString& line : lines)
        while (line.endsWith(QLatin1Char(' '))) line.chop(1);
    QString document = lines.join(QLatin1Char('\n'));
    document.append(QLatin1Char('\n'));
    // No trailing blank lines: the file ends with exactly one newline.
    while (document.endsWith(QStringLiteral("\n\n"))) document.chop(1);
    return document;
}

QString suggestedFileName(const QString& sourcePath, const QDateTime& timestamp) {
    QString base = QFileInfo(sourcePath).completeBaseName().trimmed();
    if (base.isEmpty()) base = QStringLiteral("image");
    return base + QStringLiteral("_properties_")
        + timestamp.toString(QStringLiteral("yyyyMMddhhmm"));
}

} // namespace PropertiesExport
} // namespace mvpview
