#pragma once

#include <QSettings>

namespace mvpview {

// Keep the completion marker in the legacy store so clearing Lumen's settings
// cannot resurrect the old preferences on the next launch.
inline bool migrateLegacySettings(QSettings& legacy, QSettings& current) {
    const QString marker = QStringLiteral("migration/lumenComplete");
    legacy.setFallbacksEnabled(false);
    current.setFallbacksEnabled(false);
    if (legacy.value(marker, false).toBool() || legacy.allKeys().isEmpty())
        return true;
    for (const auto& key : legacy.allKeys()) {
        if (key != marker && !current.contains(key))
            current.setValue(key, legacy.value(key));
    }
    current.sync();
    if (current.status() != QSettings::NoError)
        return false;
    legacy.setValue(marker, true);
    legacy.sync();
    return legacy.status() == QSettings::NoError;
}

inline bool migrateLegacySettings() {
    // Historical storage identifiers, not the displayed product name.
    QSettings legacy(QSettings::UserScope, QStringLiteral("MvpView"),
                     QStringLiteral("MVP Image Viewer"));
    QSettings current;
    return migrateLegacySettings(legacy, current);
}

} // namespace mvpview
