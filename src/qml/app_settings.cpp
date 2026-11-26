#include "diagnostics/diagnostics.h"
#include "qml/app_settings.h"
#include "core/display_color_space.h"
#include "io/encoded_color_management.h"
#include "io/qt_image_decoder.h"
#ifdef MVPVIEW_HAS_SPARKLE
#include "qml/mac_update_driver.h"
#endif

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSettings>
#include <QStandardPaths>
#include <QStyleHints>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextList>
#include <QTimer>
#include <QVersionNumber>

#include <utility>

#ifndef MVPVIEW_GITHUB_REPOSITORY
#define MVPVIEW_GITHUB_REPOSITORY ""
#endif

namespace mvpview {
namespace {
constexpr auto kDefaultRepositorySlug = "L1xiaolong/Lumen";
constexpr auto kLanguageKey = "general/language";
constexpr auto kThemeKey = "appearance/theme";
constexpr auto kRestoreLastDirectoryKey = "general/restoreLastDirectory";
constexpr auto kConfirmTrashKey = "general/confirmTrash";
constexpr auto kAutomaticUpdateChecksKey = "updates/automaticChecks";
constexpr auto kApplyEmbeddedColorProfilesKey = "color/applyEmbeddedProfiles";
constexpr auto kPreserveHighBitDepthKey = "color/preserveHighBitDepth";
constexpr auto kDisplayColorSpaceKey = "color/displayColorSpace";
constexpr auto kHonorExifOrientationKey = "display/honorExifOrientation";
constexpr auto kCanvasBackgroundKey = "display/canvasBackground";
constexpr auto kThumbnailPresentationKey = "display/thumbnailPresentation";
constexpr auto kSmoothDisplayKey = "display/smoothDisplay";
constexpr auto kLastUpdateCheckKey = "updates/lastCheckUtc";
constexpr auto kIgnoredUpdateVersionKey = "updates/ignoredVersion";
constexpr auto kPendingUpdateVersionKey = "updates/pendingVersion";
constexpr auto kPendingReleaseUrlKey = "updates/pendingReleaseUrl";

bool gSoftwareCacheDeletionRequested = false;
QString settingsFileToDelete;
QStringList appDirectoriesToDelete;

QStringList localAppDirectories() {
    QStringList paths;
    const auto append = [&paths](QStandardPaths::StandardLocation location) {
        const QString path = QDir::cleanPath(QStandardPaths::writableLocation(location));
        if (!path.isEmpty() && QDir::isAbsolutePath(path) && path != QDir::rootPath()
            && path != QDir::homePath() && !paths.contains(path)) {
            paths.append(path);
        }
    };
    append(QStandardPaths::CacheLocation);
    append(QStandardPaths::AppDataLocation);
    append(QStandardPaths::AppLocalDataLocation);
    return paths;
}

QString platformInstallerSuffix() {
#if defined(Q_OS_WIN)
    return QStringLiteral("-windows-x64-setup.exe");
#elif defined(Q_OS_MACOS) && defined(Q_PROCESSOR_ARM_64)
    return QStringLiteral("-macos-arm64.dmg");
#else
    return {};
#endif
}

QString formatReleaseNotes(const QString& markdown, const QString& version) {
    QTextDocument notes;
    notes.setMarkdown(markdown, QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub) | QTextDocument::MarkdownNoHTML);
    const QTextBlock first = notes.begin();
    const QString title = first.text().trimmed();
    if (first.blockFormat().headingLevel() > 0 &&
        (title.compare(QStringLiteral("Lumen v") + version, Qt::CaseInsensitive) == 0 ||
         title.compare(QStringLiteral("Lumen ") + version, Qt::CaseInsensitive) == 0)) {
        QTextCursor cursor(first);
        cursor.select(QTextCursor::BlockUnderCursor);
        cursor.removeSelectedText();
        cursor.deleteChar();
    }
    if (notes.toPlainText().trimmed().isEmpty())
        return {};

    for (QTextBlock block = notes.begin(); block.isValid(); block = block.next()) {
        QTextBlockFormat format = block.blockFormat();
        const bool heading = format.headingLevel() > 0;
        const bool listItem = block.textList();
        format.setTopMargin(block == notes.begin() ? 0 : heading ? 18 : listItem ? 4 : 8);
        format.setBottomMargin(heading ? 6 : 4);
        format.setLineHeight(120, QTextBlockFormat::ProportionalHeight);
        QTextCursor cursor(block);
        cursor.setBlockFormat(format);
        if (heading) {
            QTextCharFormat style;
            style.setProperty(QTextFormat::FontSizeAdjustment, 1);
            style.setFontWeight(QFont::DemiBold);
            cursor.select(QTextCursor::BlockUnderCursor);
            cursor.mergeCharFormat(style);
        }
    }
    // The popup supplies its own font and colors.
    const QString html = notes.toHtml();
    const qsizetype body = html.indexOf(QStringLiteral("<body"));
    return html.mid(html.indexOf(QLatin1Char('>'), body) + 1).section(QStringLiteral("</body>"), 0, 0).trimmed();
}

#ifdef Q_OS_WIN
bool isInstalledWindowsCopy() {
    const QSettings registration(
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\MVPImageViewer"),
        QSettings::Registry32Format);
    const QString folder = registration.value(QStringLiteral("InstallLocation")).toString();
    return !folder.isEmpty() &&
        QFileInfo(QDir(folder).filePath(QStringLiteral("Lumen.exe"))) ==
            QFileInfo(QCoreApplication::applicationFilePath());
}
#endif

struct ShortcutDefinition {
    const char* id;
    const char* defaultSequence;
};

constexpr ShortcutDefinition kShortcutDefinitions[] = {
    {"openFolder", "Ctrl+O"},      {"find", "Ctrl+F"}, {"settings", "Ctrl+,"},
    {"toggleNavigator", "Ctrl+B"}, {"compare", "C"},   {"rename", "F2"},
    {"newFolder", "Ctrl+Shift+N"},
};

// Include platform-specific StandardKey alternatives and chord prefixes: a custom
// multi-stroke binding must not swallow the first stroke of an existing action.
bool shortcutsOverlap(const QString& first, const QString& second) {
    const auto a = QKeySequence::fromString(first, QKeySequence::PortableText);
    const auto b = QKeySequence::fromString(second, QKeySequence::PortableText);
    return a.matches(b) != QKeySequence::NoMatch || b.matches(a) != QKeySequence::NoMatch;
}

bool isReservedShortcut(const QString& sequence) {
    for (const auto key : {QKeySequence::Copy, QKeySequence::Cut, QKeySequence::Paste,
                           QKeySequence::SelectAll, QKeySequence::Back,
                           QKeySequence::Forward, QKeySequence::Refresh}) {
        for (const auto& binding : QKeySequence::keyBindings(key)) {
            if (shortcutsOverlap(sequence, binding.toString(QKeySequence::PortableText)))
                return true;
        }
    }
    for (const auto* binding : {"F", "1", "B", "Esc", "Tab", "Backtab", "Return", "Enter",
                                "Space", "Left", "Right", "Up", "Down", "Home", "End",
                                "PgUp", "PgDown", "Delete", "Alt+Up", "Alt+Return"}) {
        if (shortcutsOverlap(sequence, QLatin1String(binding)))
            return true;
    }
#ifdef Q_OS_MACOS
    for (const auto* binding : {"Backspace", "Ctrl+Backspace", "Ctrl+Delete"}) {
        if (shortcutsOverlap(sequence, QLatin1String(binding)))
            return true;
    }
#endif
    return false;
}

const ShortcutDefinition* shortcutDefinition(const QString& action) {
    for (const auto& definition : kShortcutDefinitions) {
        if (action == QLatin1String(definition.id))
            return &definition;
    }
    return nullptr;
}
} // namespace

QString AppSettings::repositorySlug() {
    const QString configured = QString::fromUtf8(MVPVIEW_GITHUB_REPOSITORY).trimmed();
    return configured.isEmpty() ? QString::fromLatin1(kDefaultRepositorySlug) : configured;
}

QUrl AppSettings::repositoryUrl(const QString& suffix) {
    return QUrl(QStringLiteral("https://github.com/%1%2").arg(repositorySlug(), suffix));
}

QUrl AppSettings::latestReleaseApiUrl() {
    return QUrl(
        QStringLiteral("https://api.github.com/repos/%1/releases/latest").arg(repositorySlug()));
}

AppSettings::AppSettings(QGuiApplication* application, QObject* parent)
    : QObject(parent), application_(application) {
    QSettings settings;
    loggingEnabled_ = settings.value(QStringLiteral("diagnostics/loggingEnabled"), true).toBool();
    crashReportingEnabled_ = settings.value(QStringLiteral("diagnostics/crashReportingEnabled"), true).toBool();
    logLevel_ = diagnostics::levelName(diagnostics::parseLevel(settings.value(QStringLiteral("diagnostics/logLevel"), QStringLiteral("Info")).toString()));
    language_ = normalizedLanguage(
        settings.value(QLatin1String(kLanguageKey), QStringLiteral("system")).toString());
    theme_ = normalizedTheme(
        settings.value(QLatin1String(kThemeKey), QStringLiteral("system")).toString());
    if (qEnvironmentVariableIsSet("MVPVIEW_LANGUAGE_OVERRIDE")) {
        language_ = normalizedLanguage(qEnvironmentVariable("MVPVIEW_LANGUAGE_OVERRIDE"));
    }
    if (qEnvironmentVariableIsSet("MVPVIEW_THEME_OVERRIDE")) {
        theme_ = normalizedTheme(qEnvironmentVariable("MVPVIEW_THEME_OVERRIDE"));
    }
    restoreLastDirectory_ = settings.value(QLatin1String(kRestoreLastDirectoryKey), true).toBool();
    confirmTrash_ = settings.value(QLatin1String(kConfirmTrashKey), true).toBool();
    automaticUpdateChecks_ =
        settings.value(QLatin1String(kAutomaticUpdateChecksKey), true).toBool();
    ignoredUpdateVersion_ = settings.value(QLatin1String(kIgnoredUpdateVersionKey)).toString();
    applyEmbeddedColorProfiles_ =
        settings.value(QLatin1String(kApplyEmbeddedColorProfilesKey), true).toBool();
    preserveHighBitDepth_ = settings.value(QLatin1String(kPreserveHighBitDepthKey), true).toBool();
    displayColorSpace_ = normalizedDisplayColorSpace(
        settings.value(QLatin1String(kDisplayColorSpaceKey), QStringLiteral("auto")).toString());
    honorExifOrientation_ = settings.value(QLatin1String(kHonorExifOrientationKey), true).toBool();
    canvasBackground_ = normalizedCanvasBackground(
        settings.value(QLatin1String(kCanvasBackgroundKey), QStringLiteral("neutral")).toString());
    thumbnailPresentation_ = settings.value(QLatin1String(kThumbnailPresentationKey)).toString() == QStringLiteral("fill")
                                 ? QStringLiteral("fill") : QStringLiteral("fit");
    smoothDisplay_ = settings.value(QLatin1String(kSmoothDisplayKey), true).toBool();
    EncodedColorManagement::setEnabled(applyEmbeddedColorProfiles_);
    applyDisplayColorSpace();
    QtImageDecoder::setPreserveHighBitDepth(preserveHighBitDepth_);
    QtImageDecoder::setAutoOrientationEnabled(honorExifOrientation_);
    for (const auto& definition : kShortcutDefinitions) {
        const QString action = QLatin1String(definition.id);
        const QString stored = settings
                                   .value(QStringLiteral("shortcuts/") + action,
                                          QLatin1String(definition.defaultSequence))
                                   .toString();
        const QString normalized = normalizedShortcut(stored);
        shortcuts_.insert(action, (normalized.isEmpty() || isReservedShortcut(normalized))
                                      ? QLatin1String(definition.defaultSequence) : normalized);
    }

    // Repair older saved assignments too. Reset conflicting custom bindings until
    // the entire map is valid, including collisions with a restored default.
    bool repaired = true;
    while (repaired) {
        repaired = false;
        for (const auto& first : kShortcutDefinitions) {
            for (const auto& second : kShortcutDefinitions) {
                if (QString::fromLatin1(first.id) >= QString::fromLatin1(second.id) ||
                    !shortcutsOverlap(shortcuts_.value(QLatin1String(first.id)),
                                      shortcuts_.value(QLatin1String(second.id))))
                    continue;
                for (const auto* definition : {&first, &second}) {
                    const QString action = QLatin1String(definition->id);
                    const QString fallback = QLatin1String(definition->defaultSequence);
                    if (shortcuts_.value(action) != fallback) {
                        shortcuts_.insert(action, fallback);
                        repaired = true;
                    }
                }
            }
        }
    }
    for (const auto& definition : kShortcutDefinitions) {
        const QString action = QLatin1String(definition.id);
        const QString key = QStringLiteral("shortcuts/") + action;
        if (settings.contains(key) && settings.value(key).toString() != shortcuts_.value(action))
            settings.setValue(key, shortcuts_.value(action));
    }

    applyLanguage();
    const QString pendingVersion = settings.value(QLatin1String(kPendingUpdateVersionKey)).toString();
    if (!pendingVersion.isEmpty()) {
        if (QVersionNumber::compare(QVersionNumber::fromString(applicationVersion()),
                                    QVersionNumber::fromString(pendingVersion)) >= 0) {
            installedUpdateVersion_ = applicationVersion();
        } else {
            updateState_ = QStringLiteral("error");
            latestVersion_ = pendingVersion;
            releaseUrl_ = settings.value(QLatin1String(kPendingReleaseUrlKey)).toUrl();
            updateError_ = QCoreApplication::translate("UpdateErrors", "The new version did not start. Try updating again.");
        }
        settings.remove(QLatin1String(kPendingUpdateVersionKey));
        settings.remove(QLatin1String(kPendingReleaseUrlKey));
    }
    if (application_ && application_->styleHints()) {
        connect(application_->styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
            if (theme_ == QStringLiteral("system"))
                emit themeChanged();
        });
    }
}

AppSettings::~AppSettings() {
    updateCheckThread_.quit();
    updateCheckThread_.wait();
}

QString AppSettings::language() const { return language_; }

QString AppSettings::effectiveLanguage() const {
    if (language_ != QStringLiteral("system"))
        return language_;
    return QLocale::system().language() == QLocale::Chinese ? QStringLiteral("zh_CN")
                                                            : QStringLiteral("en");
}

QString AppSettings::theme() const { return theme_; }

bool AppSettings::darkTheme() const {
    if (theme_ == QStringLiteral("dark"))
        return true;
    if (theme_ == QStringLiteral("light") || !application_ || !application_->styleHints())
        return false;
    return application_->styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}

bool AppSettings::restoreLastDirectory() const { return restoreLastDirectory_; }

bool AppSettings::confirmTrash() const { return confirmTrash_; }

bool AppSettings::automaticUpdateChecks() const { return automaticUpdateChecks_; }
bool AppSettings::applyEmbeddedColorProfiles() const { return applyEmbeddedColorProfiles_; }
bool AppSettings::preserveHighBitDepth() const { return preserveHighBitDepth_; }
QString AppSettings::displayColorSpace() const { return displayColorSpace_; }

QString AppSettings::resolvedDisplayColorSpace() const {
    return displayColorSpaceName(currentDisplayColorSpace());
}

// "auto" follows the window surface, which is what the compositor will assume for the canvas
// buffer. Everything else pins one of the spaces the viewer can encode.
void AppSettings::applyDisplayColorSpace() {
    const DisplayColorSpace resolved =
        displayColorSpace_ == QStringLiteral("auto")
            ? displayColorSpaceForSurface(surfaceColorSpace_, DisplayColorSpace::Srgb)
            : displayColorSpaceFromKey(displayColorSpace_);
    if (resolved == appliedDisplayColorSpace_ && appliedOnce_) {
        return;
    }
    appliedDisplayColorSpace_ = resolved;
    appliedOnce_ = true;
    setCurrentDisplayColorSpace(resolved);
    // "auto" before the platform window reports a surface would only ever log the placeholder
    // fallback, so the event waits for the real report.
    if (displayColorSpace_ != QStringLiteral("auto") || surfaceColorSpace_.isValid()) {
        diagnostics::event(diagnostics::Level::Info, diagnostics::settings(),
                           QStringLiteral("settings.display_color_space"),
                           {{"preference", displayColorSpace_},
                            {"resolved", displayColorSpaceKey(resolved)},
                            {"surface", surfaceColorSpace_.isValid()
                                            ? surfaceColorSpace_.description()
                                            : QStringLiteral("unknown")}},
                           true);
    }
    emit colorDisplayChanged();
}

void AppSettings::setSurfaceColorSpace(const QColorSpace& surface) {
    if (surface == surfaceColorSpace_)
        return;
    surfaceColorSpace_ = surface;
    if (displayColorSpace_ == QStringLiteral("auto")) {
        applyDisplayColorSpace();
    }
}
bool AppSettings::honorExifOrientation() const { return honorExifOrientation_; }
QString AppSettings::canvasBackground() const { return canvasBackground_; }
QString AppSettings::thumbnailPresentation() const { return thumbnailPresentation_; }
bool AppSettings::smoothDisplay() const { return smoothDisplay_; }
bool AppSettings::colorManagementAvailable() const { return EncodedColorManagement::isAvailable(); }

QString AppSettings::updateState() const { return updateState_; }

QString AppSettings::latestVersion() const { return latestVersion_; }

QUrl AppSettings::releaseUrl() const { return releaseUrl_; }

int AppSettings::updateDownloadProgress() const { return updateDownloadProgress_; }

QString AppSettings::updateError() const { return updateError_; }

QString AppSettings::downloadedUpdatePath() const { return downloadedUpdatePath_; }

QVariantList AppSettings::shortcutEntries() const {
    QVariantList entries;
    for (const auto& definition : kShortcutDefinitions) {
        const QString action = QLatin1String(definition.id);
        const QString sequence = shortcutFor(action);
        entries.push_back(QVariantMap{
            {QStringLiteral("id"), action},
            {QStringLiteral("sequence"), sequence},
            {QStringLiteral("nativeSequence"),
             QKeySequence::fromString(sequence, QKeySequence::PortableText)
                 .toString(QKeySequence::NativeText)},
        });
    }
    return entries;
}

int AppSettings::shortcutsRevision() const { return shortcutsRevision_; }

QString AppSettings::applicationVersion() const { return QCoreApplication::applicationVersion(); }

void AppSettings::setLanguage(const QString& language) {
    const QString normalized = normalizedLanguage(language);
    if (language_ == normalized)
        return;
    language_ = normalized;
    QSettings().setValue(QLatin1String(kLanguageKey), language_);
    applyLanguage();
    emit languageChanged();
}

void AppSettings::setTheme(const QString& theme) {
    const QString normalized = normalizedTheme(theme);
    if (theme_ == normalized)
        return;
    theme_ = normalized;
    diagnostics::event(diagnostics::Level::Info, diagnostics::settings(), QStringLiteral("settings.theme"), {{"theme", normalized}}, true);
    QSettings().setValue(QLatin1String(kThemeKey), theme_);
    emit themeChanged();
}

void AppSettings::setRestoreLastDirectory(bool restore) {
    if (restoreLastDirectory_ == restore)
        return;
    restoreLastDirectory_ = restore;
    QSettings().setValue(QLatin1String(kRestoreLastDirectoryKey), restore);
    emit restoreLastDirectoryChanged();
}

void AppSettings::setConfirmTrash(bool confirm) {
    if (confirmTrash_ == confirm)
        return;
    confirmTrash_ = confirm;
    QSettings().setValue(QLatin1String(kConfirmTrashKey), confirm);
    emit confirmTrashChanged();
}

void AppSettings::setAutomaticUpdateChecks(bool enabled) {
    if (automaticUpdateChecks_ == enabled)
        return;
    automaticUpdateChecks_ = enabled;
    QSettings().setValue(QLatin1String(kAutomaticUpdateChecksKey), enabled);
    emit automaticUpdateChecksChanged();
}

void AppSettings::setApplyEmbeddedColorProfiles(bool enabled) {
    if (applyEmbeddedColorProfiles_ == enabled)
        return;
    applyEmbeddedColorProfiles_ = enabled;
    EncodedColorManagement::setEnabled(enabled);
    QSettings().setValue(QLatin1String(kApplyEmbeddedColorProfilesKey), enabled);
    emit colorDisplayChanged();
}

void AppSettings::setPreserveHighBitDepth(bool enabled) {
    if (preserveHighBitDepth_ == enabled)
        return;
    preserveHighBitDepth_ = enabled;
    QtImageDecoder::setPreserveHighBitDepth(enabled);
    QSettings().setValue(QLatin1String(kPreserveHighBitDepthKey), enabled);
    emit colorDisplayChanged();
}

void AppSettings::setDisplayColorSpace(const QString& space) {
    const QString normalized = normalizedDisplayColorSpace(space);
    if (displayColorSpace_ == normalized)
        return;
    displayColorSpace_ = normalized;
    QSettings().setValue(QLatin1String(kDisplayColorSpaceKey), normalized);
    applyDisplayColorSpace();
}

void AppSettings::setHonorExifOrientation(bool enabled) {
    if (honorExifOrientation_ == enabled)
        return;
    honorExifOrientation_ = enabled;
    QtImageDecoder::setAutoOrientationEnabled(enabled);
    QSettings().setValue(QLatin1String(kHonorExifOrientationKey), enabled);
    emit colorDisplayChanged();
}

void AppSettings::setCanvasBackground(const QString& background) {
    const QString normalized = normalizedCanvasBackground(background);
    if (canvasBackground_ == normalized)
        return;
    canvasBackground_ = normalized;
    QSettings().setValue(QLatin1String(kCanvasBackgroundKey), normalized);
    emit colorDisplayChanged();
}

void AppSettings::setThumbnailPresentation(const QString& mode) {
    const QString normalized = mode == QStringLiteral("fill") ? mode : QStringLiteral("fit");
    if (thumbnailPresentation_ == normalized) return;
    thumbnailPresentation_ = normalized;
    QSettings().setValue(QLatin1String(kThumbnailPresentationKey), normalized);
    emit thumbnailPresentationChanged();
}

void AppSettings::setSmoothDisplay(bool enabled) {
    if (smoothDisplay_ == enabled)
        return;
    smoothDisplay_ = enabled;
    diagnostics::event(diagnostics::Level::Info, diagnostics::settings(), QStringLiteral("settings.smoothing"), {{"enabled", enabled}}, true);
    QSettings().setValue(QLatin1String(kSmoothDisplayKey), enabled);
    emit smoothDisplayChanged();
}

QString AppSettings::shortcutFor(const QString& action) const {
    if (const auto it = shortcuts_.constFind(action); it != shortcuts_.cend())
        return it.value();
    if (const auto* definition = shortcutDefinition(action))
        return QLatin1String(definition->defaultSequence);
    return {};
}

QString AppSettings::nativeShortcutText(const QString& sequence) const {
    return QKeySequence::fromString(sequence, QKeySequence::PortableText)
        .toString(QKeySequence::NativeText);
}

QString AppSettings::standardShortcutText(int standardKey) const {
    QStringList labels;
    for (const auto& binding : QKeySequence::keyBindings(static_cast<QKeySequence::StandardKey>(standardKey))) {
        const QString label = binding.toString(QKeySequence::NativeText);
        if (!label.isEmpty() && !labels.contains(label))
            labels.push_back(label);
    }
    return labels.join(QStringLiteral(" / "));
}

QVariantMap AppSettings::shortcutSequenceForKey(int key, int modifiers) const {
    if (key <= 0 || key == Qt::Key_unknown || key == Qt::Key_Escape ||
        key == Qt::Key_Shift || key == Qt::Key_Control || key == Qt::Key_Meta ||
        key == Qt::Key_Alt || key == Qt::Key_AltGr)
        return {};
    const auto keyboardModifiers = Qt::KeyboardModifiers(modifiers) &
        (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier | Qt::KeypadModifier);
    const QKeySequence sequence(QKeyCombination(keyboardModifiers, static_cast<Qt::Key>(key)));
    return {{QStringLiteral("sequence"), sequence.toString(QKeySequence::PortableText)},
            {QStringLiteral("nativeSequence"), sequence.toString(QKeySequence::NativeText)}};
}

QString AppSettings::setShortcut(const QString& action, const QString& sequence) {
    if (!shortcutDefinition(action))
        return QStringLiteral("unknown");
    const QString normalized = normalizedShortcut(sequence);
    if (normalized.isEmpty())
        return QStringLiteral("invalid");
    if (isReservedShortcut(normalized))
        return QStringLiteral("reserved");
    for (auto it = shortcuts_.cbegin(); it != shortcuts_.cend(); ++it) {
        if (it.key() != action && shortcutsOverlap(it.value(), normalized))
            return it.key();
    }
    if (shortcutFor(action) == normalized)
        return {};
    shortcuts_.insert(action, normalized);
    QSettings().setValue(QStringLiteral("shortcuts/") + action, normalized);
    ++shortcutsRevision_;
    emit shortcutsChanged();
    return {};
}

void AppSettings::resetShortcuts() {
    bool changed = false;
    QSettings settings;
    for (const auto& definition : kShortcutDefinitions) {
        const QString action = QLatin1String(definition.id);
        const QString defaultSequence = QLatin1String(definition.defaultSequence);
        changed = changed || shortcuts_.value(action) != defaultSequence;
        shortcuts_.insert(action, defaultSequence);
        settings.remove(QStringLiteral("shortcuts/") + action);
    }
    if (changed) {
        ++shortcutsRevision_;
        emit shortcutsChanged();
    }
}

void AppSettings::startAutomaticUpdateCheck() {
    if (!automaticUpdateChecks_ || startupUpdateCheckScheduled_)
        return;
    int delay = 1500;
#ifndef NDEBUG
    const bool customEndpoint = qEnvironmentVariableIsSet("MVPVIEW_UPDATE_API_URL");
    if (customEndpoint) delay = 100;
#else
    constexpr bool customEndpoint = false;
#endif
    if (!customEndpoint && repositorySlug().isEmpty())
        return;
    startupUpdateCheckScheduled_ = true;
    QTimer::singleShot(delay, this, [this] {
        // Recheck the preference after the startup delay. A manual check or
        // download started meanwhile already owns the update state.
        if (automaticUpdateChecks_ && updateState_ == QStringLiteral("idle"))
            checkForUpdates();
    });
}

bool AppSettings::automaticUpdateAvailable() const {
    return updateState_ == QStringLiteral("available") && latestVersion_ != ignoredUpdateVersion_;
}

bool AppSettings::canUseSparkle() const {
#ifdef MVPVIEW_HAS_SPARKLE
    return isAllowedUpdateUrl(macAppcastUrl_) && MacUpdateDriver::isConfigured();
#else
    return false;
#endif
}

bool AppSettings::canDownloadInstaller() const {
    return isAllowedUpdateUrl(installerUrl_) && !expectedInstallerSha256_.isEmpty();
}

bool AppSettings::canStartUpdate() const {
    return canUseSparkle() || canDownloadInstaller();
}

bool AppSettings::canInstallUpdate() const {
#ifdef Q_OS_WIN
    return canDownloadInstaller() && isInstalledWindowsCopy();
#else
    return canUseSparkle();
#endif
}

void AppSettings::checkForUpdates(bool showDetails) {
    if (updateState_ == QStringLiteral("checking")) {
        showUpdateDetails_ = showUpdateDetails_ || showDetails;
        return;
    }
    if (updateState_ == QStringLiteral("downloading") ||
        updateState_ == QStringLiteral("verifying") ||
        updateState_ == QStringLiteral("ready") ||
        updateState_ == QStringLiteral("installing"))
        return;
    showUpdateDetails_ = showDetails;
    cancelUpdateRequested_ = false;
    installerUrl_.clear();
    macAppcastUrl_.clear();
    installerAssetName_.clear();
    expectedInstallerSha256_.clear();
    installerAssetSize_ = 0;
    updateError_.clear();
    releaseNotes_.clear();
    updateDownloadProgress_ = -1;
    clearDownloadedUpdate();
    setUpdateState(QStringLiteral("checking"));
    QUrl endpoint = latestReleaseApiUrl();
#ifndef NDEBUG
    if (qEnvironmentVariableIsSet("MVPVIEW_UPDATE_API_URL"))
        endpoint = QUrl(qEnvironmentVariable("MVPVIEW_UPDATE_API_URL"));
#endif
    if (!endpoint.isValid()) {
        setUpdateState(QStringLiteral("error"));
        return;
    }
    QNetworkRequest request{endpoint};
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Lumen/%1").arg(applicationVersion()));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(30'000);
    if (!updateCheckWorker_) {
        updateCheckWorker_ = new QObject;
        updateCheckWorker_->moveToThread(&updateCheckThread_);
        connect(&updateCheckThread_, &QThread::finished, updateCheckWorker_, &QObject::deleteLater);
        updateCheckThread_.setObjectName(QStringLiteral("Release update check"));
        updateCheckThread_.start();
    }

    // Qt can resolve the system proxy synchronously when initiating an HTTP request.
    auto* worker = updateCheckWorker_;
    QMetaObject::invokeMethod(worker, [this, worker, request] {
        auto* manager = new QNetworkAccessManager(worker);
        auto* reply = manager->get(request);
        connect(reply, &QNetworkReply::finished, manager, [this, manager, reply] {
            const QByteArray releaseData = reply->error() == QNetworkReply::NoError
                ? reply->readAll() : QByteArray{};
            QMetaObject::invokeMethod(this, [this, releaseData] {
                finishUpdateCheck(releaseData);
            }, Qt::QueuedConnection);
            manager->deleteLater();
        });
    }, Qt::QueuedConnection);
}

void AppSettings::finishUpdateCheck(const QByteArray& releaseData) {
    QSettings().setValue(QLatin1String(kLastUpdateCheckKey), QDateTime::currentDateTimeUtc());
    const QJsonObject release = QJsonDocument::fromJson(releaseData).object();
    const bool showDetails = std::exchange(showUpdateDetails_, false);
    const QString tag = release.value(QStringLiteral("tag_name")).toString();
    QString version = tag;
    while (version.startsWith(QLatin1Char('v'), Qt::CaseInsensitive))
        version.remove(0, 1);
    const QUrl url(release.value(QStringLiteral("html_url")).toString());
    qsizetype versionLength = 0;
    const auto releaseVersion = QVersionNumber::fromString(version, &versionLength);
    if (releaseVersion.isNull() || versionLength != version.size() || !isAllowedUpdateUrl(url)) {
        setUpdateState(QStringLiteral("error"));
        return;
    }
    const bool available =
        QVersionNumber::compare(releaseVersion,
                                QVersionNumber::fromString(applicationVersion())) > 0;
    if (!available) {
        setUpdateState(QStringLiteral("latest"), version, url);
        return;
    }

    releaseNotes_ = formatReleaseNotes(release.value(QStringLiteral("body")).toString(), version);
    const QString suffix = platformInstallerSuffix();
    const QRegularExpression sha256DigestPattern(
        QStringLiteral("^sha256:([0-9a-fA-F]{64})$"));
    const QJsonArray assets = release.value(QStringLiteral("assets")).toArray();
    for (const QJsonValue& value : assets) {
        const QJsonObject asset = value.toObject();
        const QString name = asset.value(QStringLiteral("name")).toString();
        const QUrl downloadUrl(
            asset.value(QStringLiteral("browser_download_url")).toString());
#ifdef MVPVIEW_HAS_SPARKLE
        if (name == QStringLiteral("lumen-appcast.xml") &&
            downloadUrl == repositoryUrl(QStringLiteral("/releases/download/%1/lumen-appcast.xml")
                                             .arg(tag))) {
            macAppcastUrl_ = downloadUrl;
        }
#endif
        bool matchesDownloadUrl = downloadUrl ==
            repositoryUrl(QStringLiteral("/releases/download/%1/%2").arg(tag, name));
#ifndef NDEBUG
        matchesDownloadUrl = matchesDownloadUrl || qEnvironmentVariableIsSet("MVPVIEW_UPDATE_API_URL");
#endif
        if (!suffix.isEmpty() && name.endsWith(suffix, Qt::CaseInsensitive) &&
            QFileInfo(name).fileName() == name && matchesDownloadUrl) {
            const QRegularExpressionMatch digestMatch = sha256DigestPattern.match(
                asset.value(QStringLiteral("digest")).toString());
            if (!digestMatch.hasMatch() || !isAllowedUpdateUrl(downloadUrl))
                continue;
            installerAssetName_ = name;
            installerUrl_ = downloadUrl;
            installerAssetSize_ = asset.value(QStringLiteral("size")).toInteger();
            expectedInstallerSha256_ = digestMatch.captured(1).toLower();
        }
    }
    setUpdateState(QStringLiteral("available"), version, url);
    if (showDetails) emit updateDetailsRequested();
}

void AppSettings::ignoreUpdate() {
    if (updateState_ != QStringLiteral("available")) return;
    ignoredUpdateVersion_ = latestVersion_;
    QSettings().setValue(QLatin1String(kIgnoredUpdateVersionKey), ignoredUpdateVersion_);
    emit updateStateChanged();
}

void AppSettings::startUpdate() {
    if ((updateState_ != QStringLiteral("available") && updateState_ != QStringLiteral("error")) ||
        !canStartUpdate())
        return;
    cancelUpdateRequested_ = false;
    updateError_.clear();
    updateDownloadProgress_ = -1;
    clearDownloadedUpdate();
    setUpdateState(QStringLiteral("downloading"), latestVersion_, releaseUrl_);
#ifdef MVPVIEW_HAS_SPARKLE
    if (canUseSparkle()) {
        if (!macUpdate_) macUpdate_ = std::make_unique<MacUpdateDriver>(
            [this](int progress) {
                updateDownloadProgress_ = progress;
                emit updateStateChanged();
            },
            [this](const QString& state) { setUpdateState(state, latestVersion_, releaseUrl_); },
            [this](const QString& error) {
                if (std::exchange(cancelUpdateRequested_, false)) {
                    updateDownloadProgress_ = -1;
                    setUpdateState(QStringLiteral("available"), latestVersion_, releaseUrl_);
                } else {
                    QSettings settings;
                    settings.remove(QLatin1String(kPendingUpdateVersionKey));
                    settings.remove(QLatin1String(kPendingReleaseUrlKey));
                    failUpdate(error);
                }
            });
        macUpdate_->start(macAppcastUrl_, latestVersion_);
        return;
    }
#endif
    // Start the network request after the update state change has reached QML.
    QTimer::singleShot(0, this, [this] {
        if (cancelUpdateRequested_) {
            cancelUpdateRequested_ = false;
            setUpdateState(QStringLiteral("available"), latestVersion_, releaseUrl_);
            return;
        }
        if (!networkManager_) networkManager_ = new QNetworkAccessManager(this);
        beginInstallerDownload();
    });
}

void AppSettings::beginInstallerDownload() {
    const QString cacheRoot = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    const QString updateDirectory = QDir(cacheRoot).filePath(QStringLiteral("updates"));
    if (cacheRoot.isEmpty() || !QDir().mkpath(updateDirectory)) {
        failUpdate(QCoreApplication::translate("UpdateErrors", "Could not create the update folder."));
        return;
    }
    const QString targetPath = QDir(updateDirectory).filePath(installerAssetName_);
    updateFile_ = std::make_unique<QSaveFile>(targetPath);
    if (!updateFile_->open(QIODevice::WriteOnly)) {
        failUpdate(QCoreApplication::translate("UpdateErrors", "Could not save the installer. Details: %1").arg(updateFile_->errorString()));
        return;
    }
    updateHash_ = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    downloadedBytes_ = 0;

    QNetworkRequest request{installerUrl_};
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Lumen/%1").arg(applicationVersion()));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(60'000);
    QNetworkReply* reply = networkManager_->get(request);
    activeUpdateReply_ = reply;
    connect(reply, &QIODevice::readyRead, this, [this, reply] {
        const QByteArray chunk = reply->readAll();
        if (chunk.isEmpty() || !updateFile_ || !updateHash_)
            return;
        if (updateFile_->write(chunk) != chunk.size()) {
            updateError_ = QCoreApplication::translate("UpdateErrors", "Could not save the installer. Details: %1").arg(updateFile_->errorString());
            reply->abort();
            return;
        }
        updateHash_->addData(chunk);
        downloadedBytes_ += chunk.size();
    });
    connect(reply, &QNetworkReply::downloadProgress, this,
            [this](qint64 received, qint64 total) {
                const qint64 expected = total > 0 ? total : installerAssetSize_;
                if (expected > 0) {
                    updateDownloadProgress_ =
                        qBound(0, static_cast<int>(received * 100 / expected), 100);
                }
                emit updateStateChanged();
            });
    connect(reply, &QNetworkReply::finished, this, [this, reply, targetPath] {
        if (activeUpdateReply_ == reply)
            activeUpdateReply_.clear();
        const auto deleteReply = qScopeGuard([reply] { reply->deleteLater(); });
        if (cancelUpdateRequested_) {
            cancelUpdateRequested_ = false;
            if (updateFile_)
                updateFile_->cancelWriting();
            updateFile_.reset();
            updateHash_.reset();
            updateDownloadProgress_ = -1;
            setUpdateState(QStringLiteral("available"), latestVersion_, releaseUrl_);
            return;
        }
        if (!updateError_.isEmpty() || reply->error() != QNetworkReply::NoError) {
            const QString error = !updateError_.isEmpty() ? updateError_ : QCoreApplication::translate("UpdateErrors", "Download failed. Check your connection and try again. Details: %1").arg(reply->errorString());
            failUpdate(error);
            return;
        }
        setUpdateState(QStringLiteral("verifying"), latestVersion_, releaseUrl_);
        if (!updateFile_ || !updateHash_ ||
            (installerAssetSize_ > 0 && downloadedBytes_ != installerAssetSize_) ||
            QString::fromLatin1(updateHash_->result().toHex()) != expectedInstallerSha256_) {
            failUpdate(QCoreApplication::translate("UpdateErrors", "The installer failed SHA-256 verification. Download it again."));
            return;
        }
        if (!updateFile_->commit()) {
            failUpdate(QCoreApplication::translate("UpdateErrors", "Could not save the installer. Details: %1").arg(updateFile_->errorString()));
            return;
        }
        updateFile_.reset();
        updateHash_.reset();
        downloadedUpdatePath_ = targetPath;
        updateDownloadProgress_ = 100;
        setUpdateState(canInstallUpdate() ? QStringLiteral("ready") : QStringLiteral("downloaded"),
                       latestVersion_, releaseUrl_);
    });
}

void AppSettings::cancelUpdateDownload() {
    if (updateState_ != QStringLiteral("downloading")) return;
    cancelUpdateRequested_ = true;
#ifdef MVPVIEW_HAS_SPARKLE
    if (canUseSparkle() && macUpdate_) {
        macUpdate_->cancel();
        return;
    }
#endif
    if (activeUpdateReply_)
        activeUpdateReply_->abort();
}

void AppSettings::openDownloadedUpdate() {
    if (updateState_ != QStringLiteral("downloaded")) return;
    const bool exists = QFileInfo(downloadedUpdatePath_).isFile();
#ifdef Q_OS_WIN
    const bool opened = exists && QProcess::startDetached(downloadedUpdatePath_, {});
#else
    const bool opened = exists && QDesktopServices::openUrl(QUrl::fromLocalFile(downloadedUpdatePath_));
#endif
    if (!opened)
        failUpdate(QCoreApplication::translate("UpdateErrors", "Could not open the downloaded installer. Try downloading it again."));
}

void AppSettings::installPreparedUpdate() {
    if (updateState_ != QStringLiteral("ready")) return;
#ifdef MVPVIEW_HAS_SPARKLE
    rememberPendingUpdate();
    setUpdateState(QStringLiteral("installing"), latestVersion_, releaseUrl_);
    macUpdate_->install();
#elif defined(Q_OS_WIN)
    if (!isInstalledWindowsCopy() || !QFileInfo(downloadedUpdatePath_).isFile() ||
        !QProcess::startDetached(downloadedUpdatePath_,
                                 {QStringLiteral("/S"),
                                  QStringLiteral("/UPDATEPID=%1").arg(QCoreApplication::applicationPid())})) {
        failUpdate(QCoreApplication::translate("UpdateErrors", "Could not start the update installer. Try again."));
        return;
    }
    rememberPendingUpdate();
    setUpdateState(QStringLiteral("installing"), latestVersion_, releaseUrl_);
    emit quitForUpdateRequested();
#endif
}

void AppSettings::rememberPendingUpdate() {
    QSettings settings;
    settings.setValue(QLatin1String(kPendingUpdateVersionKey), latestVersion_);
    settings.setValue(QLatin1String(kPendingReleaseUrlKey), releaseUrl_);
    settings.sync();
}

void AppSettings::failUpdate(const QString& error) {
    diagnostics::event(diagnostics::Level::Error, diagnostics::updates(), QStringLiteral("update.failed"), {{"reason", error}}, true);
    if (updateFile_)
        updateFile_->cancelWriting();
    updateFile_.reset();
    updateHash_.reset();
    updateDownloadProgress_ = -1;
    updateError_ = error;
    setUpdateState(QStringLiteral("error"), latestVersion_, releaseUrl_);
}

void AppSettings::clearDownloadedUpdate() {
    downloadedUpdatePath_.clear();
    updateFile_.reset();
    updateHash_.reset();
}

bool AppSettings::isAllowedUpdateUrl(const QUrl& url) const {
    if (!url.isValid() || url.host().isEmpty())
        return false;
    if (url.scheme() == QStringLiteral("https"))
        return true;
#ifndef NDEBUG
    return qEnvironmentVariableIsSet("MVPVIEW_UPDATE_API_URL") &&
           url.scheme() == QStringLiteral("http");
#else
    return false;
#endif
}

void AppSettings::openRepository() const {
    QUrl url = repositoryUrl();
    if (url.isValid())
        QDesktopServices::openUrl(url);
}

void AppSettings::setLoggingEnabled(bool enabled) {
    if (loggingEnabled_ == enabled) return;
    loggingEnabled_ = enabled;
    QSettings().setValue(QStringLiteral("diagnostics/loggingEnabled"), enabled);
    emit diagnosticsChanged();
    diagnostics::event(diagnostics::Level::Info, diagnostics::settings(), QStringLiteral("settings.logging"), {{"enabled", enabled}}, true);
}
void AppSettings::setLogLevel(const QString& level) {
    const QString normalized = diagnostics::levelName(diagnostics::parseLevel(level));
    if (logLevel_ == normalized) return;
    logLevel_ = normalized;
    QSettings().setValue(QStringLiteral("diagnostics/logLevel"), normalized);
    emit diagnosticsChanged();
    diagnostics::event(diagnostics::Level::Info, diagnostics::settings(), QStringLiteral("settings.log_level"), {{"level", normalized}}, true);
}
void AppSettings::setCrashReportingEnabled(bool enabled) {
    if (crashReportingEnabled_ == enabled) return;
    crashReportingEnabled_ = enabled;
    QSettings().setValue(QStringLiteral("diagnostics/crashReportingEnabled"), enabled);
    emit diagnosticsChanged();
    diagnostics::event(diagnostics::Level::Info, diagnostics::settings(), QStringLiteral("settings.crash_capture"), {{"enabledNextStart", enabled}}, true);
}

void AppSettings::restoreDefaults() {
    setLoggingEnabled(true);
    setLogLevel(QStringLiteral("Info"));
    setCrashReportingEnabled(true);
    setLanguage(QStringLiteral("system"));
    setTheme(QStringLiteral("system"));
    setRestoreLastDirectory(true);
    setConfirmTrash(true);
    setAutomaticUpdateChecks(true);
    if (!ignoredUpdateVersion_.isEmpty()) {
        ignoredUpdateVersion_.clear();
        QSettings().remove(QLatin1String(kIgnoredUpdateVersionKey));
        emit updateStateChanged();
    }
    setApplyEmbeddedColorProfiles(true);
    setPreserveHighBitDepth(true);
    setDisplayColorSpace(QStringLiteral("auto"));
    setHonorExifOrientation(true);
    setCanvasBackground(QStringLiteral("neutral"));
    setThumbnailPresentation(QStringLiteral("fit"));
    setSmoothDisplay(true);
    resetShortcuts();
}

QString AppSettings::deleteSoftwareCache() {
    if (!application_)
        return tr("The application cannot be closed automatically.");

    QSettings settings;
    settingsFileToDelete = settings.fileName();
    settings.clear();
    settings.sync();
    if (settings.status() != QSettings::NoError)
        return tr("The application settings could not be cleared.");

    appDirectoriesToDelete = localAppDirectories();
    gSoftwareCacheDeletionRequested = true;
    // Let the confirmation dialog finish the current event before shutdown begins.
    QTimer::singleShot(0, application_, &QCoreApplication::quit);
    return {};
}

bool AppSettings::softwareCacheDeletionRequested() {
    return gSoftwareCacheDeletionRequested;
}

QString AppSettings::finishSoftwareCacheDeletion() {
    if (!softwareCacheDeletionRequested())
        return {};

    QStringList failures;
    for (const QString& path : std::as_const(appDirectoriesToDelete)) {
        QDir directory(path);
        if (directory.exists() && !directory.removeRecursively())
            failures.append(path);
    }

    // Native settings may live outside the app data directories (for example, a macOS plist).
    // Registry settings are cleared above and do not present an absolute filesystem path here.
    if (!settingsFileToDelete.isEmpty() && QFileInfo(settingsFileToDelete).isAbsolute()) {
        if (QFileInfo::exists(settingsFileToDelete) && !QFile::remove(settingsFileToDelete))
            failures.append(settingsFileToDelete);
        const QString lockFile = settingsFileToDelete + QStringLiteral(".lock");
        if (QFileInfo::exists(lockFile) && !QFile::remove(lockFile))
            failures.append(lockFile);
    }

    if (failures.isEmpty())
        return {};
    return tr("Some local application data could not be removed: %1")
        .arg(failures.join(QStringLiteral(", ")));
}

void AppSettings::applyLanguage() {
    if (!application_)
        return;
    application_->removeTranslator(&translator_);
    if (effectiveLanguage() == QStringLiteral("zh_CN") &&
        translator_.load(QStringLiteral(":/i18n/lumen_zh_CN.qm"))) {
        application_->installTranslator(&translator_);
    }
}

void AppSettings::setUpdateState(const QString& state, const QString& latestVersion,
                                 const QUrl& releaseUrl) {
    if (updateState_ == state && latestVersion_ == latestVersion && releaseUrl_ == releaseUrl)
        return;
    diagnostics::event(state == QStringLiteral("error") ? diagnostics::Level::Error : diagnostics::Level::Info,
                       diagnostics::updates(), QStringLiteral("update.state"), {{"state", state}}, true);
    updateState_ = state;
    latestVersion_ = latestVersion;
    releaseUrl_ = releaseUrl;
    emit updateStateChanged();
}

QString AppSettings::normalizedShortcut(const QString& sequence) {
    const QKeySequence parsed =
        QKeySequence::fromString(sequence.trimmed(), QKeySequence::PortableText);
    if (parsed.isEmpty())
        return {};
    return parsed.toString(QKeySequence::PortableText);
}

QString AppSettings::normalizedLanguage(const QString& language) {
    return language == QStringLiteral("zh_CN") || language == QStringLiteral("en")
               ? language
               : QStringLiteral("system");
}

QString AppSettings::normalizedTheme(const QString& theme) {
    return theme == QStringLiteral("light") || theme == QStringLiteral("dark")
               ? theme
               : QStringLiteral("system");
}

QString AppSettings::normalizedCanvasBackground(const QString& background) {
    return background == QStringLiteral("dark") || background == QStringLiteral("black") ||
                   background == QStringLiteral("white")
               ? background
               : QStringLiteral("neutral");
}

// "auto" plus the four encodings the pipeline can target; anything else falls back to "auto".
QString AppSettings::normalizedDisplayColorSpace(const QString& space) {
    const QString normalized = space.trimmed().toLower();
    if (normalized == QStringLiteral("auto")) {
        return normalized;
    }
    for (const DisplayColorSpace candidate : availableDisplayColorSpaces()) {
        if (displayColorSpaceKey(candidate) == normalized) {
            return normalized;
        }
    }
    return QStringLiteral("auto");
}

} // namespace mvpview
