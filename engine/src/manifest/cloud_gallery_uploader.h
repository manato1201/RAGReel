#pragma once

#include <QByteArray>
#include <QString>
#include <optional>

// Optional cloud mirror of ManifestWriter::publish() -- POSTs the same
// entry/metadata/video/thumbnail to the RAGReel shared gallery Worker
// (cloudflare/ragreel-gallery), so it also shows up at the gallery's own
// URL (e.g. for viewing inside Houdini's embedded web panel), in addition
// to the always-local output/ ManifestWriter already writes.
//
// Configured via GALLERY_UPLOAD_URL / GALLERY_UPLOAD_TOKEN env vars, the
// same way CloudRagClient::fromEnvironment() reads CLOUD_RAG_URL /
// CLOUD_RAG_API_KEY -- LauncherSettings injects these via ProcessRunner. If
// unset, cloud upload is skipped entirely and nothing about the existing
// local-only flow changes.
class CloudGalleryUploader {
public:
    static std::optional<CloudGalleryUploader> fromEnvironment();

    // entryJson/metadataJson are the same compact JSON ManifestWriter
    // already writes locally into manifest.json / videos/<id>/metadata.json.
    // thumbnailFilePath may be empty/non-existent (thumbnail is optional).
    // Throws std::runtime_error on failure -- callers must treat this as
    // non-fatal: the always-local publish this rides alongside must never
    // fail because of the network.
    void upload(const QString& id, const QByteArray& entryJson, const QByteArray& metadataJson,
                const QString& videoFilePath, const QString& thumbnailFilePath);

private:
    CloudGalleryUploader(QString baseUrl, QString token);

    QString baseUrl_;
    QString token_;
};
