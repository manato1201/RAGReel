#include "namespace_lister.h"
#include "launcher_settings.h"

#include "ragclient/cloud_rag_client.h"

#include <stdexcept>

NamespaceLister::NamespaceLister(LauncherSettings* settings, QObject* parent)
    : QObject(parent), settings_(settings) {}

QStringList NamespaceLister::namespaces() const {
    return namespaces_;
}

QString NamespaceLister::errorMessage() const {
    return errorMessage_;
}

QString NamespaceLister::connectionState() const {
    return connectionState_;
}

void NamespaceLister::refresh() {
    if (settings_->apiUrl().isEmpty() || settings_->apiKey().isEmpty()) {
        namespaces_.clear();
        errorMessage_ = QStringLiteral("先に「設定」タブでURL/APIキーを入力してください");
        connectionState_ = QStringLiteral("unconfigured");
        emit namespacesChanged();
        emit errorMessageChanged();
        emit connectionStateChanged();
        return;
    }

    // Emitted before the (blocking) network call so the lamp can flash
    // "checking" -- CloudRagClient's request pumps its own nested
    // QEventLoop, which processes paint events too, so this property change
    // does get a chance to render before the response arrives.
    connectionState_ = QStringLiteral("checking");
    emit connectionStateChanged();

    try {
        CloudRagClient client(settings_->apiUrl(), settings_->apiKey());
        namespaces_ = client.listAllowedNamespaces();
        errorMessage_.clear();
        connectionState_ = QStringLiteral("ok");
    } catch (const std::exception& e) {
        namespaces_.clear();
        errorMessage_ = QStringLiteral("取得に失敗しました: %1").arg(QString::fromUtf8(e.what()));
        connectionState_ = QStringLiteral("error");
    }
    emit namespacesChanged();
    emit errorMessageChanged();
    emit connectionStateChanged();
}
