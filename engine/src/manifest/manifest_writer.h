#pragma once

#include <QImage>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <vector>

#include "narration/narration_engine.h"  // NarrationSegment only
#include "ragclient/cloud_rag_client.h"

// Publishes one rendered video into the web dashboard's data contract
// (docs/architecture/video-factory-design.md §5: manifest.json + per-video
// metadata.json). This is the ManifestWriter component named in §2/§7 --
// previously deferred, now implemented because generated videos need to
// actually show up in web/public without a manual copy step.

struct PipelineStageTiming {
    QString stage;  // machine key, e.g. "ingest"
    QString label;  // human label shown in the dashboard, e.g. "取り込み"
    double durationSec = 0.0;
    bool success = true;
    QString errorMessage;  // empty if success
};

struct ManifestEntryInfo {
    QString id;   // unique, also used as the videos/<id>/ directory name
    QString slug;
    QString title;
    QString createdAtIso;
    double durationSec = 0.0;
    QStringList tags;
    QString sourceTutorial;
    // Rough character-count-based estimate (see estimateTokens() in
    // main_cloudrag.cpp), NOT a real measured value -- the Cloud RAG
    // backend doesn't return actual token usage in its query response.
    // Always surfaced as "推定" (estimated) in the web dashboard.
    int estimatedTokens = 0;
};

// One slide of the rendered video, for the dashboard's chapter timeline
// (IMPROVEMENT_DESIGN_2026-10.md U1). `kind` is the slide's VisualKind
// ("node" / "viewport" / "other"), which the timeline uses to colour-code.
struct ChapterInfo {
    QString title;
    QString kind;
    double startSec = 0.0;
    double durationSec = 0.0;
    // Fixed per-chapter fields for the detail page (IMPROVEMENT_DESIGN_
    // 2026-10.md T2). Both come from data the slide already carries, and
    // both are optional: `points` = the slide's two key bullets (見るポイント),
    // `sources` = titles cited on a Houdini "参考" slide (元の出典).
    QStringList points;
    QStringList sources;
};

struct ManifestVideoDetail {
    QString narrationSummary;
    // Cloud RAG に直接聞いた動画の出典(類似度つき)。metadata.json の rag_sources に、
    // {file, namespace, similarity, excerpt} として書く。
    std::vector<CloudRagSource> ragSources;
    // Houdini のチュートリアル動画の出典。チュートリアル(.md)の「## 参考」の一覧から作る
    // (parseTutorialSources の戻り値。QVariantMap{title, db, cited?})。Houdini の動画は Cloud RAG に直接
    // 聞かないので ragSources は空になり、ギャラリーの「RAG 出典」が「出典なし」になっていた(2026-11-17)。
    // これも rag_sources に書く: {file=title, namespace=db, cited?, excerpt}。類似度(similarity)は、無いので書かない。
    // cited は、打ち切りで生成が終わった動画では「評価していない」ので、キーごと書かない。
    QVariantList tutorialSources;
    std::vector<PipelineStageTiming> pipeline;
    // Slide-level timeline and encode summary. All optional: an empty
    // `chapters` / zero size means "not known", and the dashboard simply
    // omits that part (videos published before this field existed stay
    // valid -- nothing here changes the manifest.json entry shape).
    std::vector<ChapterInfo> chapters;
    int frameWidth = 0;
    int frameHeight = 0;
    int fps = 0;
    qint64 fileSizeBytes = 0;
    // Server-computed citation coverage for the main answer (0 if the
    // backend didn't return it, e.g. under --mock or Houdini-tutorial
    // ingestion mode, neither of which calls the live query endpoint).
    double extractionRate = 0.0;
    QString extractionDetail;
    // Narration timing for subtitles (V7). Empty = no subtitles written.
    // publish() renders these to videos/<id>/subtitles.vtt and records the
    // path in metadata.json render.subtitles.
    std::vector<NarrationSegment> subtitles;
    bool subtitlesExact = false;
};

// チュートリアル(.md)の「## 参考」の一覧を、QVariantMap{title, db, cited?} にする。
// tutorial_agent.py の 2 つの書き方を読む:
//   - [1] ✅ 引用済み 題名（houdini22）   → cited=true / ⬜ 未引用 → cited=false
//   - [1] 題名（houdini22）                → 打ち切りの動画(バッジ無し)。cited のキーは付けない
// 「（参考ドキュメントなし）」の行や、「参考」の節が無いときは、空。重複は 1 件にまとめる。
QVariantList parseTutorialSources(const QString& markdown);

// 同じ資料(title と db が同じ)が重複して並ぶのを 1 件にまとめる(初めに出た位置を残す)。
// どれかで「引用済み」なら、まとめた 1 件も引用済み(cited のキーが無い項目は、キー無しのまま)。
// 空の title の項目は捨てる。動画の「参照した情報源」のスライドと、metadata.json の rag_sources の両方で使う。
QVariantList dedupeReferenceItems(const QVariantList& items);

class ManifestWriter {
public:
    // Copies videoFilePath into web/public/videos/<id>/video.webm, saves
    // thumbnail as thumb.jpg (skipped if null), writes videos/<id>/
    // metadata.json, and prepends/replaces this id's entry in
    // web/public/manifest.json (created fresh if it doesn't exist yet;
    // existing entries for other videos are preserved).
    // Also mirrors metadata.json/manifest.json as metadata.js/manifest.js
    // (window.__VF_METADATA__ / window.__VF_MANIFEST__) -- the dashboard is
    // opened via file://, where browsers block fetch() of local files, so
    // the JS side loads these via <script src> instead.
    // If GALLERY_UPLOAD_URL/GALLERY_UPLOAD_TOKEN are set (see
    // CloudGalleryUploader), also uploads to the shared Cloudflare gallery;
    // this step is best-effort and never throws out of publish() itself.
    // webPublicDir must be an absolute path to web/public.
    // Throws std::runtime_error on I/O or malformed-existing-manifest failure.
    static void publish(const QString& webPublicDir, const ManifestEntryInfo& entry,
                         const ManifestVideoDetail& detail, const QString& videoFilePath,
                         const QImage& thumbnail);
};
