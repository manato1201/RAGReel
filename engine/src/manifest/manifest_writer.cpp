#include "manifest_writer.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <stdexcept>

namespace {

QJsonArray readOrCreateManifestArray(const QString& path) {
    QFile file(path);
    if (!file.exists()) {
        return QJsonArray();
    }
    if (!file.open(QIODevice::ReadOnly)) {
        throw std::runtime_error("Cannot open manifest.json for reading: " + path.toStdString());
    }
    const QByteArray data = file.readAll();
    file.close();
    if (data.trimmed().isEmpty()) {
        return QJsonArray();
    }
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isArray()) {
        throw std::runtime_error("manifest.json is not a valid JSON array: " +
                                  parseError.errorString().toStdString());
    }
    return doc.array();
}

void writeJsonFile(const QString& path, const QJsonDocument& doc) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        throw std::runtime_error("Cannot write file: " + path.toStdString());
    }
    file.write(doc.toJson(QJsonDocument::Indented));
}

} // namespace

void ManifestWriter::publish(const QString& webPublicDir, const ManifestEntryInfo& entry,
                              const ManifestVideoDetail& detail, const QString& videoFilePath,
                              const QImage& thumbnail) {
    const QString videoDir = webPublicDir + QStringLiteral("/videos/") + entry.id;
    if (!QDir().mkpath(videoDir)) {
        throw std::runtime_error("Cannot create directory: " + videoDir.toStdString());
    }

    const QString destVideoPath = videoDir + QStringLiteral("/video.mp4");
    QFile::remove(destVideoPath); // QFile::copy refuses to overwrite an existing file
    if (!QFile::copy(videoFilePath, destVideoPath)) {
        throw std::runtime_error("Failed to copy video into web dashboard: " +
                                  destVideoPath.toStdString());
    }

    // PNG rather than JPEG: Qt's PNG codec is built into QtGui, while JPEG
    // is a runtime plugin (imageformats/qjpeg.dll) this project doesn't
    // deploy next to the executable -- PNG avoids that dependency entirely.
    const QString thumbPath = videoDir + QStringLiteral("/thumb.png");
    if (!thumbnail.isNull() && !thumbnail.save(thumbPath, "PNG")) {
        throw std::runtime_error("Failed to save thumbnail: " + thumbPath.toStdString());
    }

    QJsonArray sourcesArr;
    for (const CloudRagSource& s : detail.ragSources) {
        QJsonObject o;
        o["file"] = s.title;
        o["namespace"] = s.db;
        o["similarity"] = s.score;
        o["excerpt"] = QString();
        sourcesArr.append(o);
    }

    QJsonArray pipelineArr;
    for (const PipelineStageTiming& p : detail.pipeline) {
        QJsonObject o;
        o["stage"] = p.stage;
        o["label"] = p.label;
        o["status"] = QStringLiteral("done");
        o["duration_sec"] = p.durationSec;
        pipelineArr.append(o);
    }

    QJsonObject renderObj;
    renderObj["engine_version"] = QStringLiteral("video_factory_cloudrag_poc");
    renderObj["render_started_at"] = entry.createdAtIso;
    renderObj["render_duration_sec"] = entry.durationSec;

    QJsonObject qualityObj;
    qualityObj["self_reported_status"] = QStringLiteral("ok");
    qualityObj["notes"] = QString();

    QJsonObject metaObj;
    metaObj["id"] = entry.id;
    metaObj["slug"] = entry.slug;
    metaObj["title"] = entry.title;
    metaObj["narration_summary"] = detail.narrationSummary;
    metaObj["rag_sources"] = sourcesArr;
    metaObj["node_graph_path"] = QJsonValue();
    metaObj["render"] = renderObj;
    metaObj["pipeline"] = pipelineArr;
    metaObj["quality"] = qualityObj;

    writeJsonFile(videoDir + QStringLiteral("/metadata.json"), QJsonDocument(metaObj));

    QJsonObject entryObj;
    entryObj["id"] = entry.id;
    entryObj["slug"] = entry.slug;
    entryObj["title"] = entry.title;
    entryObj["created_at"] = entry.createdAtIso;
    entryObj["duration_sec"] = entry.durationSec;
    entryObj["video_path"] = QStringLiteral("videos/") + entry.id + QStringLiteral("/video.mp4");
    entryObj["thumbnail_path"] = QStringLiteral("videos/") + entry.id + QStringLiteral("/thumb.png");
    entryObj["tags"] = QJsonArray::fromStringList(entry.tags);
    entryObj["status"] = QStringLiteral("done");
    entryObj["source_tutorial"] = entry.sourceTutorial;

    const QJsonArray existing = readOrCreateManifestArray(webPublicDir + QStringLiteral("/manifest.json"));
    QJsonArray updated;
    updated.append(entryObj); // newest first
    for (const QJsonValue& v : existing) {
        if (v.toObject().value(QStringLiteral("id")).toString() != entry.id) {
            updated.append(v);
        }
    }

    writeJsonFile(webPublicDir + QStringLiteral("/manifest.json"), QJsonDocument(updated));
}
