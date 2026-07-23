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
        // quota_exceeded/rate_limited are new, legitimate-during-normal-
        // operation statuses (per-API-key token budgets and a rate limiter
        // shipped to gas_cloud_rag.js after this client was first written)
        // -- worth a distinct message so they don't read like a
        // misconfigured URL/key, which is what the generic message below
        // implies.
        if (status == QStringLiteral("quota_exceeded")) {
            throw std::runtime_error(
                "Cloud RAG returned status=quota_exceeded: this API key has used up its "
                "per-key token budget. Ask an admin to recharge it via the GAS admin "
                "panel's token-budget control.");
        }
        if (status == QStringLiteral("rate_limited")) {
            throw std::runtime_error(
                "Cloud RAG returned status=rate_limited: too many requests in a short "
                "window. Wait a bit before retrying.");
        }
        throw std::runtime_error("Cloud RAG returned status=" + status.toStdString() +
                                  " (expected auth_error/forbidden mean bad URL, API key, "
                                  "or namespace permission)");
    }

    CloudRagResponse result;
    result.answer = obj.value("answer").toString();
    result.memoryId = obj.value("memoryId").toString();
    result.extractionRate = obj.value("extractionRate").toDouble();
    result.extractionDetail = obj.value("extractionDetail").toString();
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
