#include "cloud_rag_client.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QTimer>
#include <QUrl>

#include <stdexcept>

std::optional<CloudRagClient> CloudRagClient::fromEnvironment() {
    const auto env = QProcessEnvironment::systemEnvironment();
    const QString url = env.value(QStringLiteral("CLOUD_RAG_URL"));
    const QString apiKey = env.value(QStringLiteral("CLOUD_RAG_API_KEY"));
    if (url.isEmpty() || apiKey.isEmpty()) {
        return std::nullopt;
    }
    return CloudRagClient(url, apiKey);
}

CloudRagClient::CloudRagClient(QString gasWebAppUrl, QString apiKey)
    : gasWebAppUrl_(std::move(gasWebAppUrl)), apiKey_(std::move(apiKey)) {}

CloudRagResponse CloudRagClient::query(const QString& queryText, const QString& dbKey) {
    QJsonObject body;
    body["query"] = queryText;
    body["apiKey"] = apiKey_;
    body["dbKey"] = dbKey;
    body["history"] = QJsonArray{};

    QNetworkRequest request{QUrl(gasWebAppUrl_)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    QNetworkAccessManager manager;
    QEventLoop loop;
    QNetworkReply* reply = manager.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

    // GAS WebApp cold starts can take several seconds; give it a generous
    // timeout rather than hanging forever on a dropped connection.
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeoutTimer.start(30000);

    loop.exec();

    if (!timeoutTimer.isActive()) {
        reply->deleteLater();
        throw std::runtime_error("Cloud RAG request timed out after 30s");
    }

    if (reply->error() != QNetworkReply::NoError) {
        const QString errStr = reply->errorString();
        reply->deleteLater();
        throw std::runtime_error("Cloud RAG HTTP request failed: " + errStr.toStdString());
    }

    const QByteArray responseBytes = reply->readAll();
    reply->deleteLater();

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(responseBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        throw std::runtime_error("Cloud RAG response was not valid JSON: " +
                                  parseError.errorString().toStdString());
    }

    const QJsonObject obj = doc.object();
    const QString status = obj.value("status").toString();
    if (status != QStringLiteral("ok")) {
        throw std::runtime_error("Cloud RAG returned status=" + status.toStdString() +
                                  " (expected auth_error/forbidden mean bad URL, API key, "
                                  "or namespace permission)");
    }

    CloudRagResponse result;
    result.answer = obj.value("answer").toString();
    result.memoryId = obj.value("memoryId").toString();
    for (const QJsonValue& ns : obj.value("allowedNamespaces").toArray()) {
        result.allowedNamespaces << ns.toString();
    }
    for (const QJsonValue& srcVal : obj.value("sources").toArray()) {
        const QJsonObject srcObj = srcVal.toObject();
        CloudRagSource source;
        source.title = srcObj.value("title").toString();
        source.db = srcObj.value("db").toString();
        source.score = srcObj.value("score").toDouble();
        result.sources.push_back(source);
    }
    return result;
}
