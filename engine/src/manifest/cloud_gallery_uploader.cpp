#include "cloud_gallery_uploader.h"

#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QBuffer>
#include <QHttpMultiPart>
#include <QHttpPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <stdexcept>

namespace {

QHttpPart textPart(const QString& fieldName, const QByteArray& value) {
    QHttpPart part;
    part.setHeader(QNetworkRequest::ContentDispositionHeader,
                    QStringLiteral("form-data; name=\"%1\"").arg(fieldName));
    part.setBody(value);
    return part;
}

QHttpPart filePart(const QString& fieldName, const QString& filePath, const QString& contentType,
                    const QString& fileName) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        throw std::runtime_error("Cannot open file for cloud gallery upload: " + filePath.toStdString());
    }
    QHttpPart part;
    part.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
    part.setHeader(QNetworkRequest::ContentDispositionHeader,
                    QStringLiteral("form-data; name=\"%1\"; filename=\"%2\"").arg(fieldName, fileName));
    part.setBody(file.readAll());
    return part;
}

// 1 回の POST で送れる大きさ(Cloudflare の本文の上限は 100MB で、Worker のメモリは 128MB。1080p30 の長い動画は、これを超え、
// 「Payload Too Large」になった = 2026-11-18)。これより大きい動画は、ギャラリーの「分けて送る」経路(R2 のマルチパート)で送る。
constexpr qint64 kSingleUploadLimit = 90LL * 1024 * 1024;
constexpr qint64 kPartSize = 32LL * 1024 * 1024;
constexpr qint64 kMinPartSize = 5LL * 1024 * 1024;   // R2: 最後以外のパートの最小
constexpr qint64 kMaxPartSize = 50LL * 1024 * 1024;  // Worker が 1 パートで受ける上限
constexpr int kPartAttempts = 3;
constexpr int kRequestTimeoutMs = 300000;            // 1 回の要求(1 パートは 32MB)

struct HttpResult {
    int status = 0;
    QByteArray body;
    QString error;  // 通信の失敗(状態が取れないとき)
};

// 1 回の HTTP 要求(POST / PUT / DELETE)を、同期で行う。HTTP/2 は、アップロードの途中で切れる既知の問題があるので、使わない。
HttpResult sendRequest(const QByteArray& verb, const QUrl& url, const QString& token, const QByteArray& body,
                       const QByteArray& contentType) {
    QNetworkRequest request{url};
    request.setRawHeader("Authorization", ("Bearer " + token).toUtf8());
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    if (!contentType.isEmpty()) {
        request.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
    }
    QNetworkAccessManager manager;
    QEventLoop loop;
    QBuffer payload;
    payload.setData(body);
    payload.open(QIODevice::ReadOnly);
    QNetworkReply* reply = manager.sendCustomRequest(request, verb, &payload);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeoutTimer.start(kRequestTimeoutMs);
    loop.exec();

    HttpResult result;
    if (!timeoutTimer.isActive()) {
        QObject::disconnect(reply, nullptr, nullptr, nullptr);
        reply->abort();
        reply->deleteLater();
        result.error = QStringLiteral("timed out");
        return result;
    }
    result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    result.body = reply->readAll();
    if (result.status == 0) {
        result.error = reply->errorString();
    }
    reply->deleteLater();
    return result;
}

std::string describeFailure(const QString& what, const HttpResult& r) {
    const QString detail = r.status == 0 ? r.error : QStringLiteral("HTTP %1 %2").arg(r.status).arg(QString::fromUtf8(r.body.left(200)));
    return (what + QStringLiteral(": ") + detail).toStdString();
}

// relative は "api/videos/uploads/up1/parts/2?id=x" のように、クエリを含んでよい。
// setPath に '?' を渡すと %3F に直されて、クエリがパスの一部になる(テストで見つけた)ので、分けて入れる。
QUrl apiUrl(const QString& baseUrl, const QString& relative) {
    QUrl url(baseUrl);
    QString path = url.path();
    if (!path.endsWith(QLatin1Char('/'))) {
        path += QLatin1Char('/');
    }
    const int question = relative.indexOf(QLatin1Char('?'));
    url.setPath(path + (question >= 0 ? relative.left(question) : relative), QUrl::TolerantMode);
    if (question >= 0) {
        url.setQuery(relative.mid(question + 1), QUrl::TolerantMode);
    }
    return url;
}

} // namespace

std::optional<CloudGalleryUploader> CloudGalleryUploader::fromEnvironment() {
    const auto env = QProcessEnvironment::systemEnvironment();
    const QString url = env.value(QStringLiteral("GALLERY_UPLOAD_URL"));
    const QString token = env.value(QStringLiteral("GALLERY_UPLOAD_TOKEN"));
    if (url.isEmpty() || token.isEmpty()) {
        return std::nullopt;
    }
    return CloudGalleryUploader(url, token);
}

CloudGalleryUploader::CloudGalleryUploader(QString baseUrl, QString token)
    : baseUrl_(std::move(baseUrl)), token_(std::move(token)) {}

void CloudGalleryUploader::upload(const QString& id, const QByteArray& entryJson,
                                   const QByteArray& metadataJson, const QString& videoFilePath,
                                   const QString& thumbnailFilePath) {
    if (QFileInfo(videoFilePath).size() > kSingleUploadLimit) {
        uploadChunked(id, entryJson, metadataJson, videoFilePath, thumbnailFilePath);
        return;
    }
    // Build every part (the file reads can throw) before allocating the
    // QHttpMultiPart, so a mid-way failure never leaves a heap object with
    // nothing to own it.
    const QHttpPart idPart = textPart(QStringLiteral("id"), id.toUtf8());
    const QHttpPart entryPart = textPart(QStringLiteral("entry"), entryJson);
    const QHttpPart metadataPart = textPart(QStringLiteral("metadata"), metadataJson);
    const QHttpPart videoPart =
        filePart(QStringLiteral("video"), videoFilePath, QStringLiteral("video/webm"), QStringLiteral("video.webm"));
    std::optional<QHttpPart> thumbnailPart;
    if (QFileInfo::exists(thumbnailFilePath)) {
        thumbnailPart = filePart(QStringLiteral("thumbnail"), thumbnailFilePath, QStringLiteral("image/png"),
                                  QStringLiteral("thumb.png"));
    }

    auto* multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    multiPart->append(idPart);
    multiPart->append(entryPart);
    multiPart->append(metadataPart);
    multiPart->append(videoPart);
    if (thumbnailPart) {
        multiPart->append(*thumbnailPart);
    }

    QUrl url(baseUrl_);
    QString path = url.path();
    if (!path.endsWith(QLatin1Char('/'))) {
        path += QLatin1Char('/');
    }
    url.setPath(path + QStringLiteral("api/videos"));

    QNetworkRequest request{url};
    request.setRawHeader("Authorization", ("Bearer " + token_).toUtf8());
    // Qt's HTTP/2 backend has a known issue where a QHttpMultiPart upload
    // stream can get torn down mid-transfer ("Upload device destroyed
    // while uploading" / "Stream is no longer needed") -- Cloudflare
    // negotiates HTTP/2 by default over TLS, so force HTTP/1.1 for this
    // request specifically rather than disabling HTTP/2 globally.
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);

    QNetworkAccessManager manager;
    QEventLoop loop;
    QNetworkReply* reply = manager.post(request, multiPart);
    multiPart->setParent(reply); // ties multiPart's lifetime to the reply

    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

    // Video files can be several MB; give this more headroom than the
    // Cloud RAG text-query timeout (CloudRagClient::postRaw's 30s).
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    QObject::connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeoutTimer.start(120000);

    loop.exec();

    if (!timeoutTimer.isActive()) {
        reply->deleteLater();
        throw std::runtime_error("Gallery upload timed out after 120s");
    }

    if (reply->error() != QNetworkReply::NoError) {
        const QString errStr = reply->errorString();
        const QByteArray body = reply->readAll();
        reply->deleteLater();
        throw std::runtime_error("Gallery upload failed: " + errStr.toStdString() +
                                  (body.isEmpty() ? std::string() : " -- " + body.toStdString()));
    }

    reply->deleteLater();
}

void CloudGalleryUploader::uploadChunked(const QString& id, const QByteArray& entryJson,
                                          const QByteArray& metadataJson, const QString& videoFilePath,
                                          const QString& thumbnailFilePath) {
    QFile video(videoFilePath);
    if (!video.open(QIODevice::ReadOnly)) {
        throw std::runtime_error("Cannot open video for cloud gallery upload: " + videoFilePath.toStdString());
    }
    const QJsonDocument entryDoc = QJsonDocument::fromJson(entryJson);
    const QJsonDocument metadataDoc = QJsonDocument::fromJson(metadataJson);
    if (!entryDoc.isObject() || !metadataDoc.isObject()) {
        throw std::runtime_error("Gallery upload: entry/metadata is not a JSON object");
    }

    // 1. 開始
    const HttpResult started =
        sendRequest("POST", apiUrl(baseUrl_, QStringLiteral("api/videos/uploads")), token_,
                    QJsonDocument(QJsonObject{{QStringLiteral("id"), id}}).toJson(QJsonDocument::Compact),
                    "application/json");
    if (started.status != 200) {
        throw std::runtime_error(describeFailure(QStringLiteral("Gallery chunked upload could not start"), started));
    }
    const QJsonObject startedJson = QJsonDocument::fromJson(started.body).object();
    const QString uploadId = startedJson.value(QStringLiteral("uploadId")).toString();
    if (uploadId.isEmpty()) {
        throw std::runtime_error("Gallery chunked upload: the server did not return an uploadId");
    }
    const qint64 serverPart = static_cast<qint64>(startedJson.value(QStringLiteral("partSize")).toDouble(kPartSize));
    const qint64 partSize = std::max(kMinPartSize, std::min(serverPart, kMaxPartSize));

    const QString encodedId = QString::fromLatin1(QUrl::toPercentEncoding(id));
    const QString encodedUpload = QString::fromLatin1(QUrl::toPercentEncoding(uploadId));
    auto abortUpload = [&]() {
        // 途中の分を R2 に残さない。失敗しても、元のエラーを優先する。
        try {
            sendRequest("DELETE",
                        apiUrl(baseUrl_, QStringLiteral("api/videos/uploads/%1?id=%2").arg(encodedUpload, encodedId)),
                        token_, QByteArray(), QByteArray());
        } catch (...) {
        }
    };

    try {
        // 2. パートごとに送る(通信の失敗・5xx は、少し待って、3 回まで。4xx は、すぐ失敗)
        QJsonArray parts;
        int number = 0;
        while (!video.atEnd()) {
            const QByteArray data = video.read(partSize);
            if (data.isEmpty()) {
                break;
            }
            ++number;
            const QUrl partUrl = apiUrl(
                baseUrl_, QStringLiteral("api/videos/uploads/%1/parts/%2?id=%3").arg(encodedUpload).arg(number).arg(encodedId));
            HttpResult last;
            bool ok = false;
            for (int attempt = 0; attempt < kPartAttempts && !ok; ++attempt) {
                last = sendRequest("PUT", partUrl, token_, data, "application/octet-stream");
                if (last.status == 200) {
                    const QJsonObject part = QJsonDocument::fromJson(last.body).object();
                    if (part.value(QStringLiteral("partNumber")).toInt() == number &&
                        !part.value(QStringLiteral("etag")).toString().isEmpty()) {
                        parts.append(QJsonObject{{QStringLiteral("partNumber"), number},
                                                 {QStringLiteral("etag"), part.value(QStringLiteral("etag"))}});
                        ok = true;
                        break;
                    }
                }
                if (last.status >= 400 && last.status < 500) {
                    break;  // 認証・大きすぎ: 繰り返しても同じ
                }
                if (attempt + 1 < kPartAttempts) {
                    QThread::msleep(1500 * (attempt + 1));
                }
            }
            if (!ok) {
                throw std::runtime_error(
                    describeFailure(QStringLiteral("Gallery chunked upload failed at part %1").arg(number), last));
            }
        }
        if (parts.isEmpty()) {
            throw std::runtime_error("Gallery chunked upload: the video file is empty");
        }

        // 3. 完了(サムネイルは小さいので、base64 で同じ要求に入れる)
        QJsonObject complete{{QStringLiteral("id"), id},
                             {QStringLiteral("parts"), parts},
                             {QStringLiteral("entry"), entryDoc.object()},
                             {QStringLiteral("metadata"), metadataDoc.object()}};
        if (QFileInfo::exists(thumbnailFilePath)) {
            QFile thumb(thumbnailFilePath);
            if (thumb.open(QIODevice::ReadOnly)) {
                complete.insert(QStringLiteral("thumbnail_base64"), QString::fromLatin1(thumb.readAll().toBase64()));
            }
        }
        const HttpResult done = sendRequest(
            "POST", apiUrl(baseUrl_, QStringLiteral("api/videos/uploads/%1/complete").arg(encodedUpload)), token_,
            QJsonDocument(complete).toJson(QJsonDocument::Compact), "application/json");
        if (done.status != 200) {
            throw std::runtime_error(describeFailure(QStringLiteral("Gallery chunked upload could not complete"), done));
        }
    } catch (...) {
        abortUpload();
        throw;
    }
}
