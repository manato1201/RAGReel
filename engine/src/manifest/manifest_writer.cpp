#include "manifest_writer.h"

#include "cloud_gallery_uploader.h"
#include "narration/subtitle_writer.h"

#include <cstdio>

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLockFile>
#include <QRegularExpression>
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

// Mirrors a .json file as a plain `window.<varName> = {...};` script. The
// dashboard is opened via file:// (VideoHistory::openDashboard()), and
// Chromium-based browsers refuse fetch()/XHR of local files from a file://
// document -- but a classic <script src> tag isn't subject to that
// restriction, so the JS side loads this instead of fetching the .json.
void writeJsMirror(const QString& jsPath, const QString& varName, const QJsonDocument& doc) {
    const QString content = QStringLiteral("window.") + varName + QStringLiteral(" = ") +
                             QString::fromUtf8(doc.toJson(QJsonDocument::Compact)) + QStringLiteral(";\n");
    QSaveFile file(jsPath);
    if (!file.open(QIODevice::WriteOnly)) {
        throw std::runtime_error("Cannot write file: " + jsPath.toStdString());
    }
    file.write(content.toUtf8());
    if (!file.commit()) {
        throw std::runtime_error("Cannot commit file: " + jsPath.toStdString());
    }
}

} // namespace

QVariantList parseTutorialSources(const QString& markdown) {
    // 「## 参考」から、次の「## 」の手前まで(無ければ末尾まで)
    static const QRegularExpression sectionRe(QStringLiteral("(?m)^##\\s*参考\\s*$"));
    const QRegularExpressionMatch start = sectionRe.match(markdown);
    if (!start.hasMatch()) {
        return {};
    }
    const qsizetype bodyStart = start.capturedEnd();
    static const QRegularExpression nextHeadingRe(QStringLiteral("(?m)^##\\s"));
    const QRegularExpressionMatch next = nextHeadingRe.match(markdown, bodyStart);
    // 巨大な入力(壊れた応答など)で、正規表現と重複の除去(件数の 2 乗)に時間がかからないよう、読む範囲と件数に上限を置く
    constexpr qsizetype kMaxBodyChars = 100000;
    constexpr int kMaxSources = 200;
    const QString body =
        markdown.mid(bodyStart, next.hasMatch() ? next.capturedStart() - bodyStart : -1).left(kMaxBodyChars);

    // バッジの後ろの「引用済み」「未引用」だけを読み飛ばす(\S* だと、ラベルが無い行で、題名の最初の語が消える)。
    // 空白は、改行をまたがない [ \t] だけにする(「- [1]」だけの行の次の行を、題名として読まないため)。
    // video_script.py の _SOURCE_LINE_RE と同じ形。
    static const QRegularExpression lineRe(QStringLiteral(
        "(?m)^-[ \\t]*\\[\\d+\\][ \\t]*(?:(✅|⬜)[ \\t]*(?:引用済み|未引用)?[ \\t]*)?(.+?)(?:（([^）]*)）)?[ \\t\\r]*$"));
    QVariantList items;
    QRegularExpressionMatchIterator it = lineRe.globalMatch(body);
    while (it.hasNext() && items.size() < kMaxSources) {
        const QRegularExpressionMatch m = it.next();
        QVariantMap item;
        item[QStringLiteral("title")] = m.captured(2).trimmed();
        item[QStringLiteral("db")] = m.captured(3).trimmed();
        if (!m.captured(1).isEmpty()) {
            item[QStringLiteral("cited")] = (m.captured(1) == QStringLiteral("✅"));
        }
        items << item;
    }
    return dedupeReferenceItems(items);
}

QVariantList dedupeReferenceItems(const QVariantList& items) {
    QVariantList result;
    for (const QVariant& v : items) {
        const QVariantMap m = v.toMap();
        const QString title = m.value(QStringLiteral("title")).toString().trimmed();
        if (title.isEmpty()) {
            continue;
        }
        const QString db = m.value(QStringLiteral("db")).toString().trimmed();
        const bool hasCited = m.contains(QStringLiteral("cited"));
        const bool cited = m.value(QStringLiteral("cited")).toBool();
        bool merged = false;
        for (QVariant& existing : result) {
            QVariantMap em = existing.toMap();
            if (em.value(QStringLiteral("title")).toString() == title &&
                em.value(QStringLiteral("db")).toString() == db) {
                if (hasCited && (cited || !em.contains(QStringLiteral("cited")))) {
                    em[QStringLiteral("cited")] = cited || em.value(QStringLiteral("cited")).toBool();
                    existing = em;
                }
                merged = true;
                break;
            }
        }
        if (!merged) {
            QVariantMap item;
            item[QStringLiteral("title")] = title;
            item[QStringLiteral("db")] = db;
            if (hasCited) {
                item[QStringLiteral("cited")] = cited;
            }
            result << item;
        }
    }
    return result;
}

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
    // Houdini のチュートリアル動画: 「参考」の一覧から。Cloud RAG の出典と同じ資料が重なれば、1 件にする。
    for (const QVariant& v : dedupeReferenceItems(detail.tutorialSources)) {
        const QVariantMap m = v.toMap();
        const QString title = m.value(QStringLiteral("title")).toString();
        const QString db = m.value(QStringLiteral("db")).toString();
        bool exists = false;
        for (const CloudRagSource& s : detail.ragSources) {
            if (s.title == title && s.db == db) {
                exists = true;
                break;
            }
        }
        if (exists) {
            continue;
        }
        QJsonObject o;
        o["file"] = title;
        o["namespace"] = db;
        if (m.contains(QStringLiteral("cited"))) {
            o["cited"] = m.value(QStringLiteral("cited")).toBool();
        }
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
    // Encode summary for the dashboard's "書き出し" line; written only when
    // known so older/partial callers don't emit misleading zeros.
    if (detail.frameWidth > 0 && detail.frameHeight > 0) {
        renderObj["width"] = detail.frameWidth;
        renderObj["height"] = detail.frameHeight;
    }
    if (detail.fps > 0) {
        renderObj["fps"] = detail.fps;
    }
    if (detail.fileSizeBytes > 0) {
        renderObj["file_size_bytes"] = detail.fileSizeBytes;
    }
    // Subtitles (V7): a WebVTT sidecar next to the video. The path is relative
    // to the dashboard root like video_path. `subtitles_exact` tells the page
    // whether the times came from the synthesised audio or were estimated.
    if (!detail.subtitles.empty()) {
        const QString vtt = subtitle_writer::toVtt(detail.subtitles);
        const QString vttPath = videoDir + QStringLiteral("/subtitles.vtt");
        QSaveFile vttFile(vttPath);
        if (vttFile.open(QIODevice::WriteOnly)) {
            vttFile.write(vtt.toUtf8());
            if (vttFile.commit()) {
                renderObj["subtitles"] = QStringLiteral("videos/") + entry.id + QStringLiteral("/subtitles.vtt");
                renderObj["subtitles_exact"] = detail.subtitlesExact;
            }
        }
        // The dashboard is opened via file:// where <track src> is blocked by the
        // browser, so the same cues are also inlined (same reason as the .js
        // mirrors above) and attached through the TextTrack API.
        QJsonArray cues;
        for (const NarrationSegment& s : detail.subtitles) {
            if (!(s.endSec > s.startSec) || s.text.trimmed().isEmpty()) continue;
            QJsonObject c;
            c["start"] = s.startSec;
            c["end"] = s.endSec;
            c["text"] = s.text;
            cues.append(c);
        }
        renderObj["subtitle_cues"] = cues;
    }

    QJsonArray chaptersArr;
    for (const ChapterInfo& c : detail.chapters) {
        QJsonObject o;
        o["title"] = c.title;
        o["kind"] = c.kind;
        o["start_sec"] = c.startSec;
        o["duration_sec"] = c.durationSec;
        if (!c.points.isEmpty()) {
            o["points"] = QJsonArray::fromStringList(c.points);
        }
        if (!c.sources.isEmpty()) {
            o["sources"] = QJsonArray::fromStringList(c.sources);
        }
        chaptersArr.append(o);
    }

    QJsonObject qualityObj;
    qualityObj["self_reported_status"] = QStringLiteral("ok");
    qualityObj["notes"] = QString();
    // Server-computed citation coverage of the main answer (the Cloud RAG backend's
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
    metaObj["chapters"] = chaptersArr;
    metaObj["pipeline"] = pipelineArr;
    metaObj["quality"] = qualityObj;
    metaObj["estimated_tokens"] = entry.estimatedTokens;

    const QJsonDocument metaDoc(metaObj);
    writeJsonFile(videoDir + QStringLiteral("/metadata.json"), metaDoc);
    writeJsMirror(videoDir + QStringLiteral("/metadata.js"), QStringLiteral("__VF_METADATA__"), metaDoc);

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
    // Lets the gallery sort by chapter count without loading every video's
    // metadata.json (IMPROVEMENT_DESIGN_2026-10.md T5). Omitted when the
    // chapters aren't known, same as the metadata fields.
    if (!detail.chapters.empty()) {
        entryObj["chapter_count"] = static_cast<int>(detail.chapters.size());
    }

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

    const QJsonDocument manifestDoc(updated);
    writeJsonFile(webPublicDir + QStringLiteral("/manifest.json"), manifestDoc);
    writeJsMirror(webPublicDir + QStringLiteral("/manifest.js"), QStringLiteral("__VF_MANIFEST__"), manifestDoc);
    manifestLock.unlock();

    // Optional cloud mirror (cloudflare/ragreel-gallery), configured via
    // GALLERY_UPLOAD_URL/GALLERY_UPLOAD_TOKEN. Released the local lock
    // first so a network hiccup here can never hold up another local
    // publish, and best-effort: this always-local publish already
    // succeeded above, so a failed upload must not throw out of here.
    if (auto uploader = CloudGalleryUploader::fromEnvironment()) {
        try {
            uploader->upload(entry.id, QJsonDocument(entryObj).toJson(QJsonDocument::Compact),
                              metaDoc.toJson(QJsonDocument::Compact), destVideoPath,
                              thumbnailSaved ? thumbPath : QString());
            // Houdini のパネルが、ログを読んで「ギャラリーに載せた」と伝える(失敗の行は、下の qWarning)
            std::fprintf(stderr, "Cloud gallery upload: done (%s)\n", entry.id.toUtf8().constData());
        } catch (const std::exception& e) {
            qWarning() << "Cloud gallery upload failed (video is still published locally):" << e.what();
        }
    }
}
