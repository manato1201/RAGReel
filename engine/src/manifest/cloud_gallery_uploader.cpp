#include "cloud_gallery_uploader.h"

#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QHttpPart>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QTimer>
#include <QUrl>

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
