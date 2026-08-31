#include "manifest_writer.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLockFile>
#include <QSaveFile>

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

// QSaveFile writes to a sibling temp file and atomically renames it into
// place on commit(), instead of QFile's truncate-then-write -- a reader
// (the web dashboard's manifest.json fetch, which can race an in-progress
// publish() from a completely normal "viewing the gallery while another
// video renders" scenario) can no longer observe a partially-written file.
void writeJsonFile(const QString& path, const QJsonDocument& doc) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        throw std::runtime_error("Cannot write file: " + path.toStdString());
    }
    file.write(doc.toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        throw std::runtime_error("Cannot commit file: " + path.toStdString());
    }
}

} // namespace

void ManifestWriter::publish(const QString& webPublicDir, const ManifestEntryInfo& entry,
                              const ManifestVideoDetail& detail, const QString& videoFilePath,
                              const QImage& thumbnail) {
    const QString videoDir = webPublicDir + QStringLiteral("/videos/") + entry.id;
    if (!QDir().mkpath(videoDir)) {
        throw std::runtime_error("Cannot create directory: " + videoDir.toStdString());
    }

    const QString destVideoPath = videoDir + QStringLiteral("/video.webm");
    QFile::remove(destVideoPath); // QFile::copy refuses to overwrite an existing file
    if (!QFile::copy(videoFilePath, destVideoPath)) {
        throw std::runtime_error("Failed to copy video into web dashboard: " +
                                  destVideoPath.toStdString());
    }

    // PNG rather than JPEG: Qt's PNG codec is built into QtGui, while JPEG
    // is a runtime plugin (imageformats/qjpeg.dll) this project doesn't
    // deploy next to the executable -- PNG avoids that dependency entirely.
    const QString thumbPath = videoDir + QStringLiteral("/thumb.png");
    const bool thumbnailSaved = !thumbnail.isNull() && thumbnail.save(thumbPath, "PNG");
    if (!thumbnail.isNull() && !thumbnailSaved) {
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
        // Previously hardcoded to "done" regardless of the stage's actual
        // outcome, so a stage that threw (e.g. narration synthesis failing
        // and the video shipping without audio) still showed as DONE on
        // the dashboard's pipeline view.
        o["status"] = p.success ? QStringLiteral("done") : QStringLiteral("failed");
        o["duration_sec"] = p.durationSec;
        if (!p.success) {
            o["error"] = p.errorMessage;
        }
        pipelineArr.append(o);
    }

    QJsonObject renderObj;
    renderObj["engine_version"] = QStringLiteral("video_factory_cloudrag_poc");
    renderObj["render_started_at"] = entry.createdAtIso;
    renderObj["render_duration_sec"] = entry.durationSec;

    QJsonObject qualityObj;
    qualityObj["self_reported_status"] = QStringLiteral("ok");
    qualityObj["notes"] = QString();
    // Server-computed citation coverage of the main answer (gas_cloud_rag.js's
    // extraction pipeline) -- 0/empty when the backend didn't return it
    // (--mock, or Houdini-tutorial ingestion mode, neither of which calls
    // the live query endpoint).
    qualityObj["extraction_rate"] = detail.extractionRate;
    qualityObj["extraction_detail"] = detail.extractionDetail;

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
    metaObj["estimated_tokens"] = entry.estimatedTokens;

    writeJsonFile(videoDir + QStringLiteral("/metadata.json"), QJsonDocument(metaObj));

    QJsonObject entryObj;
    entryObj["id"] = entry.id;
    entryObj["slug"] = entry.slug;
    entryObj["title"] = entry.title;
    entryObj["created_at"] = entry.createdAtIso;
    entryObj["duration_sec"] = entry.durationSec;
    entryObj["video_path"] = QStringLiteral("videos/") + entry.id + QStringLiteral("/video.webm");
    // Previously written unconditionally even when the thumbnail was null
    // (e.g. a headless-render failure on the sampled frame) and thus never
    // saved above, leaving the dashboard referencing a thumb.png that does
    // not exist.
    entryObj["thumbnail_path"] = thumbnailSaved
        ? QStringLiteral("videos/") + entry.id + QStringLiteral("/thumb.png")
        : QJsonValue();
    entryObj["tags"] = QJsonArray::fromStringList(entry.tags);
    entryObj["status"] = QStringLiteral("done");
    entryObj["source_tutorial"] = entry.sourceTutorial;
    entryObj["estimated_tokens"] = entry.estimatedTokens;

    // Guards the manifest.json read-modify-write below: two publish() calls
    // overlapping (e.g. the launcher and a directly-invoked CLI run against
    // the same output/ dir) would otherwise both read the same N-entry
    // array, both append their own entry, and whichever writes last wins --
    // silently dropping the other run's just-published entry even though
    // its videos/<id>/ files were written successfully.
    QLockFile manifestLock(webPublicDir + QStringLiteral("/manifest.lock"));
    manifestLock.setStaleLockTime(30000);
    if (!manifestLock.lock()) {
        throw std::runtime_error("Cannot acquire manifest.lock (another publish still running?): " +
                                  webPublicDir.toStdString());
    }

    const QJsonArray existing = readOrCreateManifestArray(webPublicDir + QStringLiteral("/manifest.json"));
    QJsonArray updated;
    updated.append(entryObj); // newest first
    for (const QJsonValue& v : existing) {
        if (v.toObject().value(QStringLiteral("id")).toString() != entry.id) {
            updated.append(v);
        }
    }

    writeJsonFile(webPublicDir + QStringLiteral("/manifest.json"), QJsonDocument(updated));
    manifestLock.unlock();
}
