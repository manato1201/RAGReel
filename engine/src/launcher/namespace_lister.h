#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

class LauncherSettings;

// Fetches the dbKey namespaces the current API key (Settings tab) is
// allowed to query, for the Cloud RAG tab's database picker -- see
// CloudRagClient::listAllowedNamespaces() for how this avoids costing any
// tokens. Runs synchronously (CloudRagClient's blocking call pumps its own
// nested QEventLoop, so the GUI stays responsive for the ~1-2s round trip);
// refresh() is only ever invoked from a button click, never automatically
// in a loop, so this is not a concern in practice.
class NamespaceLister : public QObject {
    Q_OBJECT
    Q_PROPERTY(QStringList namespaces READ namespaces NOTIFY namespacesChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    // One of "unknown" (never checked), "unconfigured" (no URL/key yet),
    // "checking" (request in flight), "ok", "error" -- drives the top-right
    // connection lamp in Launcher.qml.
    Q_PROPERTY(QString connectionState READ connectionState NOTIFY connectionStateChanged)

public:
    explicit NamespaceLister(LauncherSettings* settings, QObject* parent = nullptr);

    QStringList namespaces() const;
    QString errorMessage() const;
    QString connectionState() const;

    Q_INVOKABLE void refresh();

signals:
    void namespacesChanged();
    void errorMessageChanged();
    void connectionStateChanged();

private:
    LauncherSettings* settings_;
    QStringList namespaces_;
    QString errorMessage_;
    QString connectionState_ = QStringLiteral("unknown");
};
