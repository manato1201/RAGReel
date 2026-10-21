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

namespace {

const QString kSharedPrefix = QStringLiteral("shared:");

// The Worker identifies a shared knowledge base as "shared:<name>". The engine and the launcher
// work with the bare name (it is also the brand label and a tag on the video), so a bare name is
// expanded when sending and stripped when listing. Personal namespaces ("personal:<id>") are left alone.
QString toWorkerNamespace(const QString& name) {
    return name.contains(QLatin1Char(':')) ? name : kSharedPrefix + name;
}

QString fromWorkerNamespace(const QString& ns) {
    return ns.startsWith(kSharedPrefix) ? ns.mid(kSharedPrefix.size()) : ns;
}

} // namespace

std::optional<CloudRagClient> CloudRagClient::fromEnvironment() {
    const auto env = QProcessEnvironment::systemEnvironment();
    const QString url = env.value(QStringLiteral("CLOUD_RAG_URL"));
    const QString apiKey = env.value(QStringLiteral("CLOUD_RAG_API_KEY"));
    if (url.isEmpty() || apiKey.isEmpty()) {
        return std::nullopt;
    }
    return CloudRagClient(url, apiKey);
}

CloudRagClient::CloudRagClient(QString workerUrl, QString apiKey)
    : workerUrl_(std::move(workerUrl)), apiKey_(std::move(apiKey)) {
    while (workerUrl_.endsWith(QLatin1Char('/'))) {
        workerUrl_.chop(1);
    }
}

QJsonObject CloudRagClient::postRaw(const QString& path, const QJsonObject& body) {
    QNetworkRequest request{QUrl(workerUrl_ + path)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + apiKey_.toUtf8());
    // Cloudflare rejects some default client User-Agents as bots (error 1010); name ourselves.
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("RAGReelEngine/1.0"));

    QNetworkAccessManager manager;
    QEventLoop loop;
    QNetworkReply* reply = manager.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

    // Answer generation can take a while; give it a generous timeout rather than hanging forever
    // on a dropped connection.
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeoutTimer.start(90000);

    loop.exec();

    if (!timeoutTimer.isActive()) {
        reply->deleteLater();
        throw std::runtime_error("Cloud RAG request timed out after 90s");
    }

    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray responseBytes = reply->readAll();
    const QNetworkReply::NetworkError netError = reply->error();
    const QString errStr = reply->errorString();
    reply->deleteLater();

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(responseBytes, &parseError);
    const QJsonObject obj = (parseError.error == QJsonParseError::NoError && doc.isObject()) ? doc.object()
                                                                                              : QJsonObject();

    if (httpStatus >= 400) {
        // The Worker answers errors as {"error": "<message>"} (auth, token budget, rate limit ...).
        const QString message = obj.value("error").toString();
        const QString detail = message.isEmpty() ? errStr : message;
        if (httpStatus == 429) {
            throw std::runtime_error(
                "Cloud RAG returned 429: this API key has used up its token budget, or there were too "
                "many requests in a short window. Ask an admin to recharge it, or wait a bit. (" +
                detail.toStdString() + ")");
        }
        if (httpStatus == 401 || httpStatus == 403) {
            throw std::runtime_error("Cloud RAG returned " + std::to_string(httpStatus) +
                                      ": bad URL, API key, or namespace permission. (" + detail.toStdString() + ")");
        }
        throw std::runtime_error("Cloud RAG returned " + std::to_string(httpStatus) + ": " +
                                  detail.toStdString());
    }
    if (netError != QNetworkReply::NoError) {
        throw std::runtime_error("Cloud RAG HTTP request failed: " + errStr.toStdString());
    }
    if (obj.isEmpty()) {
        throw std::runtime_error("Cloud RAG response was not valid JSON: " +
                                  parseError.errorString().toStdString());
    }
    return obj;
}

CloudRagResponse CloudRagClient::query(const QString& queryText, const QString& dbKey) {
    QJsonObject body;
    body["query"] = queryText;
    body["history"] = QJsonArray{};
    body["namespaces"] = QJsonArray{toWorkerNamespace(dbKey)};

    const QJsonObject obj = postRaw(QStringLiteral("/query"), body);
    const QString status = obj.value("status").toString();
    if (!status.isEmpty() && status != QStringLiteral("ok")) {
        throw std::runtime_error("Cloud RAG returned status=" + status.toStdString());
    }

    CloudRagResponse result;
    result.answer = obj.value("answer").toString();
    // memoryId is a number on the Worker (a string on the old backend); keep it as text.
    const QJsonValue memoryId = obj.value("memoryId");
    result.memoryId = memoryId.isString() ? memoryId.toString()
                                           : (memoryId.isDouble() ? QString::number(static_cast<qlonglong>(memoryId.toDouble()))
                                                                  : QString());
    result.extractionRate = obj.value("extractionRate").toDouble();
    result.extractionDetail = obj.value("extractionDetail").toString();
    for (const QJsonValue& ns : obj.value("namespaces").toArray()) {
        result.allowedNamespaces << fromWorkerNamespace(ns.toString());
    }
    for (const QJsonValue& srcVal : obj.value("sources").toArray()) {
        const QJsonObject srcObj = srcVal.toObject();
        CloudRagSource source;
        source.title = srcObj.value("file").toString();
        source.db = fromWorkerNamespace(srcObj.value("namespace").toString());
        source.score = srcObj.value("score").toDouble();
        result.sources.push_back(source);
    }
    return result;
}

QStringList CloudRagClient::listAllowedNamespaces() {
    // POST /me/namespaces returns the namespaces this key may use. No search and no LLM call, so
    // it costs no tokens.
    const QJsonObject obj = postRaw(QStringLiteral("/me/namespaces"), QJsonObject());

    QStringList namespaces;
    for (const QJsonValue& ns : obj.value("namespaces").toArray()) {
        namespaces << fromWorkerNamespace(ns.toString());
    }
    return namespaces;
}
