#pragma once

#include <QObject>
#include <QSettings>
#include <QString>

// Persists the per-person Cloud RAG credentials the launcher needs to run
// video_factory_cloudrag_poc.exe (which itself reads them from the
// CLOUD_RAG_URL / CLOUD_RAG_API_KEY environment variables -- see
// CloudRagClient::fromEnvironment). Each teammate brings their own API key,
// entered once here and injected into the subprocess's environment by
// ProcessRunner on every run.
//
// Stored via QSettings (Windows registry, HKCU\Software\VideoFactory\
// Launcher) -- plain text, protected only by normal per-user registry ACLs.
// A real credential-manager-backed store would be more correct but is a lot
// of extra Win32 (wincred.h) plumbing for a key that's already scoped to a
// single Windows user account; flagged here rather than silently assumed.
class LauncherSettings : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString apiUrl READ apiUrl WRITE setApiUrl NOTIFY apiUrlChanged)
    Q_PROPERTY(QString apiKey READ apiKey WRITE setApiKey NOTIFY apiKeyChanged)
    Q_PROPERTY(QString lastDbKey READ lastDbKey WRITE setLastDbKey NOTIFY lastDbKeyChanged)

public:
    explicit LauncherSettings(QObject* parent = nullptr);

    QString apiUrl() const;
    void setApiUrl(const QString& value);

    QString apiKey() const;
    void setApiKey(const QString& value);

    QString lastDbKey() const;
    void setLastDbKey(const QString& value);

signals:
    void apiUrlChanged();
    void apiKeyChanged();
    void lastDbKeyChanged();

private:
    QSettings settings_;
};
