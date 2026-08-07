#include "launcher_settings.h"

LauncherSettings::LauncherSettings(QObject* parent)
    : QObject(parent), settings_(QStringLiteral("RAGReel"), QStringLiteral("RAGReel")) {}

QString LauncherSettings::apiUrl() const {
    return settings_.value(QStringLiteral("apiUrl")).toString();
}

void LauncherSettings::setApiUrl(const QString& value) {
    if (value == apiUrl()) {
        return;
    }
    settings_.setValue(QStringLiteral("apiUrl"), value);
    emit apiUrlChanged();
}

QString LauncherSettings::apiKey() const {
    return settings_.value(QStringLiteral("apiKey")).toString();
}

void LauncherSettings::setApiKey(const QString& value) {
    if (value == apiKey()) {
        return;
    }
    settings_.setValue(QStringLiteral("apiKey"), value);
    emit apiKeyChanged();
}

QString LauncherSettings::lastDbKey() const {
    const QString stored = settings_.value(QStringLiteral("lastDbKey")).toString();
    return stored.isEmpty() ? QStringLiteral("houdini21") : stored;
}

void LauncherSettings::setLastDbKey(const QString& value) {
    if (value == lastDbKey()) {
        return;
    }
    settings_.setValue(QStringLiteral("lastDbKey"), value);
    emit lastDbKeyChanged();
}
