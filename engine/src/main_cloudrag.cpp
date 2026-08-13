// Phase 2 PoC: Cloud RAG query -> narration (TTS) -> headless
// QQuickRenderControl render -> FFmpeg mux (video + audio). See
// docs/architecture/video-factory-design.md §8 (Phase 2: "RAG bridge HTTP
// client + consume real content -> generate one real video") and §3
// (NarrationEngine runs CPU-only text->speech, no GPU contention with the
// renderer). localRAG/tutorials/ is still empty (houdini21 hasn't produced
// a real tutorial yet), so this PoC sources content from Cloud RAG instead
// of the local file-pair contract.
//
// Usage:
//   video_factory_cloudrag_poc [topic] [dbKey] [--mock]
// Requires CLOUD_RAG_URL and CLOUD_RAG_API_KEY in the environment unless
// --mock is passed (see docs/cloud-rag.md §6.2.6 / §8.2 -- credentials are
// never stored in this repo, matching the Unity/Houdini client policy).
//
// Note: qDebug()/qCritical() output does not reliably reach stderr when
// this console-subsystem exe is launched with redirected stdio in this
// environment (observed while debugging Phase 1), so diagnostics here use
// std::fprintf(stderr, ...) directly instead.

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QRegularExpression>
#include <QUrl>
#include <QVariantList>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX // windows.h's max()/min() macros collide with std::max/std::min below
#endif
#include <windows.h>
#endif

#include "common/app_utils.h"
#include "encode/video_encoder.h"
#include "ingest/script_composer.h"
#include "manifest/manifest_writer.h"
#include "narration/narration_engine.h"
#include "orchestrator/orchestrator.h"
#include "ragclient/cloud_rag_client.h"
#include "scene/scene_assembler.h"
#include "services/concrete_services.h"
#include "services/service_container.h"

namespace {

constexpr int kFrameWidth = 1280;
constexpr int kFrameHeight = 720;
constexpr int kFps = 30;
constexpr double kMinDurationSeconds = 4.0;
// Keeps the scroll from ending exactly as narration stops, and covers the
// silent tail while the visual reveal catches up if TTS failed/was skipped.
constexpr double kTailPaddingSeconds = 1.5;

// Extracts and removes a "--flag value" pair from `args` (both tokens),
// e.g. for options like --houdini-md that take a following path argument.
// Returns an empty string if the flag isn't present or has no value after
// it, leaving `args` untouched in that case.
QString takeFlagValue(QStringList& args, const QString& flag) {
    const int idx = args.indexOf(flag);
    if (idx < 0 || idx + 1 >= args.size()) {
        return QString();
    }
    const QString value = args.at(idx + 1);
    args.removeAt(idx + 1);
    args.removeAt(idx);
    return value;
}

CloudRagResponse mockResponse() {
    CloudRagResponse r;
    r.answer = QStringLiteral(
        "HoudiniのVEXにおける`for`ループは、C/C++に似た構文で、特定の処理を繰り返し実行するために使用されます "
        "[1][2]。\n\n"
        "## VEX forループの基本\n\n"
        "* **構文**\n"
        "  ```vex\n"
        "  for (初期化; 条件式; 更新式) {\n"
        "    // 繰り返したい処理\n"
        "  }\n"
        "  ```\n\n"
        "- `初期化`: ループ開始前に一度だけ実行されます。\n"
        "- `条件式`: 毎回のループ開始前に評価され、trueの間ループが続行されます。\n"
        "- `更新式`: 毎回のループ終了後に実行されます。\n\n"
        "## 処理フロー\n\n"
        "文章だけだとわかりづらいので、制御の流れを図にすると次のようになります。\n\n"
        "```mermaid\n"
        "flowchart LR\n"
        "    A[開始] --> B{条件式}\n"
        "    B -->|true| C[繰り返し処理]\n"
        "    C --> D[更新式]\n"
        "    D --> B\n"
        "    B -->|false| E[終了]\n"
        "```\n\n"
        "## 実践例\n\n"
        "ポイントごとに10回処理を繰り返す例:\n\n"
        "```vex\n"
        "for (int i = 0; i < 10; i++) {\n"
        "    addpoint(0, @P + {0, i * 0.1, 0});\n"
        "}\n"
        "```\n\n"
        "`foreach`構文を使うと配列やポイントの反復がより簡潔に書けます。パフォーマンスを重視する場合は、"
        "可能な限りVEXの組み込み属性ループ機能（`foreach`）を優先してください。\n\n"
        "## 応用: パーティクルの寿命制御\n\n"
        "実データで見つかった不具合(要点の重複・出典番号の混入)の回帰テスト用セクション。"
        "実際のhoudini21回答は数値引用ではなく説明的な出典表記を使うことがあり、"
        "1つの箇条書き行に複数の文が同居することもある。\n\n"
        "- パーティクルの寿命(@age / @life)に応じて、@pscale(サイズ)や@Alpha(透明度)を変化させることで、"
        "消えゆく表現が可能です[参考: 過去Q&A]。-  curlnoiseやwindを利用して、"
        "流体的な揺らぎのある軌道を作成できます[参考: 過去Q&A]。");
    r.sources = {
        {QStringLiteral("VEX ループ・条件文と関数定義"), QStringLiteral("houdini21"), 0.86},
        {QStringLiteral("VEX 言語基礎（変数・型・演算子）"), QStringLiteral("houdini21"), 0.84},
        {QStringLiteral("VEX ポイント間操作とPCLookup/nearpoint"), QStringLiteral("houdini21"), 0.83},
        {QStringLiteral("KineFXプロシージャルアニメーションとフルボディダイナミクス"), QStringLiteral("houdini21"),
         0.82},
        {QStringLiteral("Houdini デバッグテクニック"), QStringLiteral("houdini21"), 0.82},
    };
    r.allowedNamespaces = {QStringLiteral("houdini21")};
    r.memoryId = QStringLiteral("mock");
    return r;
}

// Regression fixture for splitLongTextSlides: real Cloud RAG answers are
// often plain prose with no "##" heading structure at all, which used to
// mean splitIntoSlides fell back to one giant slide (i.e. the "many videos
// end up scroll-format" bug report this fixture exists to catch).
CloudRagResponse mockResponsePlain() {
    CloudRagResponse r;
    r.answer = QStringLiteral(
        "HoudiniのVEXでノイズ関数を使ってジオメトリを歪ませるには、まずAttribute "
        "Wrangleノードをジオメトリの後段に接続し、VEXPressionの中でnoise()関数を呼び出します。"
        "noise()は座標を入力として受け取り、-1から1の範囲の擬似乱数を返す関数で、"
        "同じ入力座標に対しては常に同じ値を返すため、フレームをまたいでも安定したノイズパターンが得られます。"
        "典型的な使い方としては、@Pに対してノイズ値をスケーリングして加算し、"
        "表面をランダムに凹凸させる処理がよく使われます。ノイズの周波数を上げるには、"
        "noise()に渡す座標を事前に大きな係数で乗算しておくことで、より細かい変化を作り出せます。"
        "逆に周波数を下げてなだらかな起伏にしたい場合は係数を小さくします。"
        "また、4D版のnoise()を使えば時間軸を4つ目の引数として渡すことができ、"
        "時間経過とともに滑らかに変化するアニメーションノイズも簡単に実現できます。"
        "パフォーマンスを重視する場合は、必要以上に高い周波数のノイズを多重に重ねすぎないよう注意してください。");
    r.sources = {
        {QStringLiteral("VEX ノイズ関数リファレンス"), QStringLiteral("houdini21"), 0.81},
        {QStringLiteral("Attribute Wrangle 基礎"), QStringLiteral("houdini21"), 0.79},
    };
    r.allowedNamespaces = {QStringLiteral("houdini21")};
    r.memoryId = QStringLiteral("mock-plain");
    return r;
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // logLine() writes UTF-8 bytes straight to stderr; without this the
    // console's own (non-UTF-8) output codepage garbles every Japanese log
    // line on screen. Display-only -- argv decoding (fromLocal8Bit, below)
    // and the actual Cloud RAG query/answer are unaffected either way.
    SetConsoleOutputCP(CP_UTF8);
#endif
    QGuiApplication app(argc, argv);

    QStringList args;
    for (int i = 1; i < argc; ++i) {
        args << QString::fromLocal8Bit(argv[i]);
    }

    // Houdini-tutorial ingestion mode (docs/technical-reference.md §15/§18):
    // launched by DevelopmentRAGEnvironment's tutorial_view.py right after
    // it saves a generated tutorial's .md/.json pair, so this video is
    // built from that tutorial's own content instead of a fresh Cloud RAG
    // query. --houdini-screenshots points at a JSON manifest of per-step
    // screenshots (see loadHoudiniScreenshotManifest) captured live during
    // generation; optional -- omitted or missing entries just fall back to
    // the abstract gradient for that slide.
    const QString houdiniMdPath = takeFlagValue(args, QStringLiteral("--houdini-md"));
    const QString houdiniJsonPath = takeFlagValue(args, QStringLiteral("--houdini-json"));
    const QString houdiniScreenshotsPath = takeFlagValue(args, QStringLiteral("--houdini-screenshots"));
    const bool useHoudiniTutorial = !houdiniMdPath.isEmpty();

    const bool useMockPlain = args.removeAll(QStringLiteral("--mock-plain")) > 0;
    const bool useMock = args.removeAll(QStringLiteral("--mock")) > 0 || useMockPlain;

    QString topic = args.size() > 0
        ? args.at(0)
        : QStringLiteral("Houdini21のVEXでforループを使う基本的な方法を教えて");
    const QString dbKey = args.size() > 1 ? args.at(1) : QStringLiteral("houdini21");

    // Every run gets a unique output basename (timestamp), so re-running
    // with a different topic can never appear to silently reuse/overwrite a
    // previous run's video, narration WAV, or Mermaid PNGs -- each run's
    // artifacts are fully independent files.
    const QString runId = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
    const QString createdAtIso = QDateTime::currentDateTime().toString(Qt::ISODate);
    logLine(QStringLiteral("Run ID: %1").arg(runId));

    // Configuration root for this job (IMPROVEMENT_PLAN.md Phase 1; see
    // orchestrator.h for the current scope of what this owns/doesn't own
    // yet). Gates GPU access between the Narrate and Assemble/Render
    // phases below via acquireGpuLease() -- see resource_budget_manager.h
    // for why (design doc §3).
    Orchestrator orchestrator;

    // Service registry (IMPROVEMENT_PLAN.md Phase 4): every module below is
    // reached through an interface (services/interfaces.h) backed by a
    // thin adapter (services/concrete_services.h) around the real
    // implementation, so a future test can register mocks instead without
    // any of this file's control flow changing. vectorStoreClient stays
    // null (not an error) when CLOUD_RAG_URL/CLOUD_RAG_API_KEY aren't set
    // -- every call site below already null-checks it, same as the
    // std::optional this replaces.
    ServiceContainer services;
    services.registerNarrationEngine(std::make_unique<SapiNarrationEngine>());
    services.registerVideoEncoderFactory(std::make_unique<FfmpegVideoEncoderFactory>());
    services.registerManifestWriter(std::make_unique<LocalManifestWriter>());
    if (auto client = CloudRagClient::fromEnvironment()) {
        services.registerVectorStoreClient(
            std::make_unique<CloudRagVectorStoreClient>(std::move(*client)));
    }

    // Wall-clock timings for the web dashboard's "how this video was made"
    // retrospective pipeline view (design doc §5/§6; ManifestWriter). These
    // are real measured durations, not the placeholder values the dashboard
    // MVP shipped with.
    QElapsedTimer ingestTimer;
    ingestTimer.start();

    CloudRagResponse response;
    QString houdiniNodeSummary;
    if (useHoudiniTutorial) {
        logLine(QStringLiteral("Loading Houdini tutorial markdown: %1").arg(houdiniMdPath));
        try {
            const HoudiniTutorial tutorial = loadHoudiniTutorialMarkdown(houdiniMdPath);
            topic = tutorial.title;
            houdiniNodeSummary = summarizeHoudiniNodeGraph(houdiniJsonPath);
            response.answer = humanizeExtractionNote(
                replaceNodeConfigSection(tutorial.body, houdiniNodeSummary));
            response.allowedNamespaces = {QStringLiteral("houdini21")};
            response.memoryId = QStringLiteral("houdini-tutorial");
        } catch (const std::exception& e) {
            logLine(QStringLiteral("ERROR: Failed to load Houdini tutorial markdown: %1")
                        .arg(QString::fromUtf8(e.what())));
            return 1;
        }
    } else if (useMockPlain) {
        logLine("Using --mock-plain response (heading-less prose, no network call)");
        response = mockResponsePlain();
    } else if (useMock) {
        logLine("Using --mock response (no network call)");
        response = mockResponse();
    } else {
        IVectorStoreClient* client = services.vectorStoreClient();
        if (!client) {
            logLine("ERROR: CLOUD_RAG_URL and/or CLOUD_RAG_API_KEY are not set in the environment.");
            return 1;
        }

        logLine(QStringLiteral("Querying Cloud RAG: topic=%1 dbKey=%2").arg(topic, dbKey));
        try {
            response = client->query(topic, dbKey);
        } catch (const std::exception& e) {
            logLine(
                QStringLiteral("ERROR: Cloud RAG query failed: %1").arg(QString::fromUtf8(e.what())));
            return 1;
        }
    }
    logLine(QStringLiteral("Cloud RAG answer (%1 chars), %2 sources, allowedNamespaces=%3")
                .arg(response.answer.size())
                .arg(response.sources.size())
                .arg(response.allowedNamespaces.join(",")));

    // Running estimated-token total for this video (see estimateTokens()).
    // Houdini-tutorial mode has no query round-trip for its primary content
    // (the tutorial markdown is loaded directly), so it starts at 0 here.
    int estimatedTokens = useHoudiniTutorial ? 0 : estimateTokens(topic) + estimateTokens(response.answer);

    // Best-effort follow-up: real Cloud RAG answers essentially never
    // contain a "```mermaid" block or an explanation of what a code example
    // does (the GAS prompt never asks for either -- confirmed by inspecting
    // gas_cloud_rag.js), so both the diagram feature and narration over
    // diagrams/code previously only worked for the hand-crafted --mock
    // fixture, or fell back to a generic "please look at the screen" phrase
    // that explains nothing. Ask Cloud RAG, as a second dedicated request,
    // for (1) a diagram + a spoken-language caption of what it shows, and
    // (2) a one-sentence explanation of each code example in the answer, in
    // document order. The diagram+caption get folded into the answer text
    // as a new "## 図解" section (caption as lead-in prose, so it's narrated
    // normally when splitIntoSlides -> expandDiagramSlides turns it into a
    // slide -- no changes needed there); the code captions are threaded into
    // stripMarkdownForNarration so each code fence's narration is an actual
    // explanation instead of a placeholder. Kept entirely on the LearningQt
    // side (no changes to the shared GAS backend other Unity/Houdini clients
    // also depend on).
    static const QRegularExpression mermaidCheck(QStringLiteral("```mermaid\\n[\\s\\S]*?```"));
    QStringList codeCaptions;
    if (!useMock) {
        IVectorStoreClient* captionClient = services.vectorStoreClient();
        if (captionClient) {
            try {
                // For Houdini-tutorial mode, fold in a short node-graph
                // summary (see summarizeHoudiniNodeGraph) so this query has
                // real node names to ground its explanation in, not just
                // the markdown prose -- this is the "md・jsonの内容も取得し
                // てさらに説明補足する" requirement.
                const QString houdiniExtra = houdiniNodeSummary.isEmpty()
                    ? QString()
                    : QStringLiteral("\n\n【生成されたノード構成】\n%1").arg(houdiniNodeSummary);
                const QString captionPrompt = QStringLiteral(
                    "以下の内容の音声ナレーション原稿を補足する情報を作成してください。\n\n"
                    "1. 内容全体の理解を助ける図を1つ、Mermaid記法(flowchartまたはmindmap)で"
                    "作成し、```mermaidブロックで出力してください。その直前に「図解説明: 」で始まる"
                    "1〜2文の日本語の説明を書いてください。適切な図が作れない場合は1と2の図関連部分は"
                    "省略してください。\n\n"
                    "2. 本文中に登場するコード例それぞれについて、それが何をしているかを説明する"
                    "1〜2文の自然な日本語を「コード説明: 」で始めて、本文での登場順に列挙してください。"
                    "コード自体の引用は不要です。出典番号([1]等)は図やコードの説明に含めないで"
                    "ください。\n\n【元の内容】\n%1%2")
                        .arg(stripCitationMarkers(response.answer), houdiniExtra);
                const CloudRagResponse captionResponse = captionClient->query(captionPrompt, dbKey);
                estimatedTokens += estimateTokens(captionPrompt) + estimateTokens(captionResponse.answer);

                if (!mermaidCheck.match(response.answer).hasMatch()) {
                    const QRegularExpressionMatch diagramMatch = mermaidCheck.match(captionResponse.answer);
                    if (diagramMatch.hasMatch()) {
                        static const QRegularExpression captionRe(
                            QStringLiteral("図解説明:\\s*(.+)"));
                        const QString diagramCaption = stripCitationMarkers(
                            captionRe.match(captionResponse.answer).captured(1).trimmed());
                        const QString diagramBlock =
                            stripCitationMarkers(diagramMatch.captured(0));
                        response.answer += QStringLiteral("\n\n## 図解\n\n%1\n\n%2\n")
                                                .arg(diagramCaption, diagramBlock);
                        logLine("Diagram follow-up succeeded, appended as a new section");
                    } else {
                        logLine("WARNING: diagram follow-up did not return a mermaid block, skipping");
                    }
                }

                static const QRegularExpression codeCaptionRe(QStringLiteral("コード説明:\\s*(.+)"));
                QRegularExpressionMatchIterator it = codeCaptionRe.globalMatch(captionResponse.answer);
                while (it.hasNext()) {
                    codeCaptions << it.next().captured(1).trimmed();
                }
                logLine(QStringLiteral("Caption follow-up: %1 code caption(s) captured")
                            .arg(codeCaptions.size()));
            } catch (const std::exception& e) {
                logLine(QStringLiteral("WARNING: caption follow-up failed: %1")
                            .arg(QString::fromUtf8(e.what())));
            }
        }
    }
    const double ingestSec = ingestTimer.elapsed() / 1000.0;
    orchestrator.recordStage(JobStage::Ingest, /*success=*/true, ingestSec);

    // Narration (best-effort): a missing/broken TTS voice degrades to a
    // silent video rather than failing the whole pipeline.
    QElapsedTimer narrateTimer;
    narrateTimer.start();
    const QString narrationText =
        topic + QStringLiteral("。") + stripMarkdownForNarration(response.answer, codeCaptions);
    const QString wavPath = QStringLiteral("phase2_cloudrag_%1_narration.wav").arg(runId);
    QString audioPathForEncoder;
    double narrationDurationSeconds = 0.0;
    QString narrationError;
    {
        // Scoped so the GPU lease (and, once NarrationEngine actually holds
        // a GPU context -- see orchestrator.h's scope note; today's SAPI5
        // implementation doesn't -- that context too) releases as soon as
        // synthesis finishes, strictly before the Assemble/Render phase
        // below ever tries to acquire its own lease.
        GpuLease narrateLease = orchestrator.acquireGpuLease(GpuLeaseOwner::NarrationEngine);
        try {
            logLine(QStringLiteral("Synthesizing narration (%1 chars)...").arg(narrationText.size()));
            const NarrationResult narration = services.narrationEngine().synthesize(narrationText, wavPath);
            audioPathForEncoder = narration.wavPath;
            narrationDurationSeconds = narration.durationSeconds;
            logLine(QStringLiteral("Narration synthesized: %1s").arg(narrationDurationSeconds, 0, 'f', 1));
        } catch (const std::exception& e) {
            narrationError = QString::fromUtf8(e.what());
            logLine(QStringLiteral("WARNING: narration synthesis failed, continuing without audio: %1")
                        .arg(narrationError));
        }
    }
    const double narrateSec = narrateTimer.elapsed() / 1000.0;
    // A narration failure degrades to a silent video (see the comment
    // above) rather than aborting the job, so it's recorded as a
    // best-effort warning here, not a failed StageResult -- narrateSec
    // still reflects real elapsed time either way.
    orchestrator.recordStage(JobStage::Narrate, /*success=*/true, narrateSec,
                              narrationError.toStdString());

    const double durationSeconds =
        std::max(kMinDurationSeconds, narrationDurationSeconds + kTailPaddingSeconds);
    const int frameCount = static_cast<int>(std::round(durationSeconds * kFps));
    logLine(QStringLiteral("Video duration: %1s (%2 frames)").arg(durationSeconds, 0, 'f', 1).arg(frameCount));

    QElapsedTimer composeTimer;
    composeTimer.start();
    std::vector<Slide> rawSlides = splitIntoSlides(topic, response.answer);
    std::vector<HoudiniStepScreenshot> houdiniShots;
    if (useHoudiniTutorial) {
        houdiniShots = loadHoudiniScreenshotManifest(houdiniScreenshotsPath);
        // Replace the "## 手順" slide with one slide per captured tool
        // call, built directly from the screenshot manifest (see
        // buildHoudiniStepSlidesFromScreenshots for why: Claude's own
        // narrative step numbers don't correspond to the tool-call-level
        // screenshot indices). If capture failed entirely (empty
        // manifest/no usable images), leave "## 手順" as plain narrated
        // prose rather than silently dropping it.
        const std::vector<Slide> stepSlides = buildHoudiniStepSlidesFromScreenshots(houdiniShots);
        if (!stepSlides.empty()) {
            std::vector<Slide> replaced;
            for (const Slide& s : rawSlides) {
                if (s.heading == QStringLiteral("手順")) {
                    replaced.insert(replaced.end(), stepSlides.begin(), stepSlides.end());
                } else {
                    replaced.push_back(s);
                }
            }
            rawSlides = replaced;
        }
        // Must run on rawSlides, BEFORE splitLongTextSlides: the "参考"
        // section's body (extraction summary + one line per source) easily
        // exceeds kMaxCharsPerSlide once there are more than a couple of
        // sources, and splitLongTextSlides has no way to know a slide's
        // text is a structured source list rather than prose -- splitting
        // it would scatter the sources across several "参考（続き）" slides,
        // only the first of which this function's exact heading match would
        // ever populate. Tagging referenceItems here lets splitLongTextSlides
        // (see its own diagramImagePath-style early-return check) recognize
        // and skip splitting this slide at all.
        assignHoudiniReferenceItems(rawSlides);
    }
    std::vector<Slide> slides =
        splitLongTextSlides(expandDiagramSlides(rawSlides, runId), kMaxCharsPerSlide);
    if (useHoudiniTutorial) {
        assignHoudiniFinalGraphScreenshot(slides, houdiniShots);
    }
    estimatedTokens +=
        enrichSlidesForDisplay(slides, dbKey, runId, useMock, services.vectorStoreClient());
    logLine(QStringLiteral("Estimated tokens consumed (rough, character-based): %1").arg(estimatedTokens));
    const std::vector<int> slideStartFrames = computeSlideStartFrames(slides, frameCount);
    const double composeSec = composeTimer.elapsed() / 1000.0;
    orchestrator.recordStage(JobStage::Compose, /*success=*/true, composeSec);
    logLine(QStringLiteral("Split into %1 slides").arg(slides.size()));

    // IMPROVEMENT_PLAN.md Phase 2: prove the ShotList archetype view
    // (script_composer.h) actually classifies real Slide data correctly
    // before Phase 3's SceneAssembler is expected to consume it. The
    // render loop below still iterates `slides` directly -- switching it
    // to iterate `shotList` is Phase 3's job, once there's a SceneAssembler
    // to hand a homogeneous-by-kind batch to.
    const ShotList shotList = toShotList(slides);
    logLine(QStringLiteral("ShotList: %1 text / %2 diagram / %3 code / %4 houdini-still / "
                            "%5 houdini-clip / %6 reference-cards (%7 total, order-verified: %8)")
                .arg(shotList.textDigests.size())
                .arg(shotList.diagramImages.size())
                .arg(shotList.codeBlocks.size())
                .arg(shotList.houdiniStepStills.size())
                .arg(shotList.houdiniStepClips.size())
                .arg(shotList.referenceCards.size())
                .arg(shotList.order.size())
                .arg(shotList.order.size() == slides.size() ? QStringLiteral("yes") : QStringLiteral("NO")));

    const QString outputMp4Path = QStringLiteral("phase2_cloudrag_%1.mp4").arg(runId);
    QImage thumbnailImage;  // captured partway through for the web dashboard gallery
    double renderSec = 0.0;
    // Scoped so every GPU-owning object constructed in this block
    // (QQuickRenderControl, QQuickWindow, the QRhi texture/render-target
    // chain) is destroyed -- and its GPU context actually torn down --
    // before assembleLease releases at the closing brace. That's what
    // makes the release meaningful under ResourceBudgetManager's contract
    // (§3: real context teardown, not idling) rather than a formality.
    // Covers both the Assemble and Render phases (design doc §2): this
    // codebase doesn't yet separate "build the QML scene state" from "run
    // the per-frame render loop" into two measurable spans (that split is
    // Phase 3's SceneAssembler extraction), so both share one lease scope
    // for now.
    {
        GpuLease assembleLease = orchestrator.acquireGpuLease(GpuLeaseOwner::SceneAssembler);

        // Internal slide boundaries (excluding the implicit 0.0/1.0 ends) as
        // fractions of the whole video, for the footer's segmented timeline
        // tick marks -- slides are weighted by content length
        // (computeSlideStartFrames), so these are not evenly spaced.
        QVariantList slideBoundaries;
        for (size_t i = 1; i < slideStartFrames.size() - 1; ++i) {
            slideBoundaries << static_cast<double>(slideStartFrames[i]) / frameCount;
        }

        SceneAssembler::StaticProperties staticProps;
        staticProps.topic = topic;
        staticProps.brandLabel = dbKey.toUpper();
        staticProps.slideCount = static_cast<int>(slides.size());
        staticProps.metadataLine = QStringLiteral("%1 SEC / %2 FPS / %3 × %4 / BT.709")
                                        .arg(durationSeconds, 0, 'f', 1)
                                        .arg(kFps)
                                        .arg(kFrameWidth)
                                        .arg(kFrameHeight);
        staticProps.slideBoundaries = slideBoundaries;

        SceneAssembler sceneAssembler(kFrameWidth, kFrameHeight,
                                       appRelativePath(QStringLiteral("qml/CloudRagScene.qml")));
        QString sceneError;
        if (!sceneAssembler.initialize(staticProps, &sceneError)) {
            logLine(QStringLiteral("ERROR: %1").arg(sceneError));
            return 1;
        }

        const std::unique_ptr<IVideoEncoder> encoder = services.videoEncoderFactory().create(
            outputMp4Path.toStdString(), kFrameWidth, kFrameHeight, kFps,
            audioPathForEncoder.toStdString());

        QElapsedTimer renderTimer;
        renderTimer.start();
        size_t currentSlide = 0;
        for (int i = 0; i < frameCount; ++i) {
            while (currentSlide + 2 < slideStartFrames.size() && i >= slideStartFrames[currentSlide + 1]) {
                ++currentSlide;
            }
            const int slideStart = slideStartFrames[currentSlide];
            const int slideEnd = slideStartFrames[currentSlide + 1];
            const double slideProgress = slideEnd > slideStart
                ? static_cast<double>(i - slideStart) / (slideEnd - slideStart)
                : 0.0;

            const Slide& active = slides[currentSlide];

            // Clip slides (cook_node steps with a captured viewport clip)
            // play the clip at its own native fps from the start of the
            // slide, then hold on the last frame for the rest of the
            // slide's screen time -- that shows the simulation's actual
            // playback speed once rather than stretching/looping it to
            // fill however long the narration runs. Every other slide
            // keeps showing its single static image.
            QString diagramSource;
            if (!active.clipFramePaths.isEmpty() && active.clipFps > 0) {
                const double slideElapsedSeconds = static_cast<double>(i - slideStart) / kFps;
                int clipIndex = static_cast<int>(slideElapsedSeconds * active.clipFps);
                clipIndex = std::clamp(clipIndex, 0, static_cast<int>(active.clipFramePaths.size()) - 1);
                diagramSource = QUrl::fromLocalFile(active.clipFramePaths[clipIndex]).toString();
            } else if (!active.diagramImagePath.isEmpty()) {
                diagramSource = QUrl::fromLocalFile(active.diagramImagePath).toString();
            }

            SceneAssembler::FrameProperties frameProps;
            frameProps.progress = static_cast<double>(i) / (frameCount - 1);
            frameProps.slideIndex = static_cast<int>(currentSlide);
            frameProps.slideHeading = active.heading;
            frameProps.slideBullet1 = active.bullet1;
            frameProps.slideBullet2 = active.bullet2;
            frameProps.slideCodeBlock = active.codeBlock;
            frameProps.slideReferenceItems = active.referenceItems;
            frameProps.slideDiagramSource = diagramSource;
            frameProps.slideProgress = slideProgress;

            const QImage frameImage = sceneAssembler.renderFrame(frameProps);
            encoder->pushFrame(frameImage.constBits());

            // A frame ~40% in usually lands inside real slide content rather
            // than the title/intro card, making for a more representative
            // gallery thumbnail than frame 0 would be.
            if (i == frameCount * 4 / 10) {
                thumbnailImage = frameImage.copy();
            }

            if (i % kFps == 0) {
                logLine(QStringLiteral("Rendered frame %1 / %2").arg(i).arg(frameCount));
            }
        }
        renderSec = renderTimer.elapsed() / 1000.0;

        encoder->writeAudioTrack();
        encoder->finish();
        logLine(QStringLiteral("Wrote %1 (%2 frames, %3s)")
                    .arg(outputMp4Path)
                    .arg(frameCount)
                    .arg(durationSeconds, 0, 'f', 1));
    }  // end of Assemble/Render GPU-lease scope (assembleLease and sceneAssembler release here)
    orchestrator.recordStage(JobStage::Assemble, /*success=*/true, 0.0);
    orchestrator.recordStage(JobStage::Render, /*success=*/true, renderSec);

    // Publish into the web dashboard (design doc §5) so a generated video
    // shows up there without a manual copy step.
    try {
        ManifestEntryInfo entry;
        entry.id = QStringLiteral("cloudrag_%1").arg(runId);
        entry.slug = entry.id;
        entry.title = topic;
        entry.createdAtIso = createdAtIso;
        entry.durationSec = durationSeconds;
        entry.tags = useHoudiniTutorial ? QStringList{dbKey, QStringLiteral("houdini-tutorial")}
                                         : QStringList{dbKey, QStringLiteral("cloud-rag")};
        entry.sourceTutorial = useHoudiniTutorial
            ? QStringLiteral("houdini-tutorial:%1").arg(QFileInfo(houdiniMdPath).completeBaseName())
            : QStringLiteral("cloud-rag:%1").arg(dbKey);
        entry.estimatedTokens = estimatedTokens;

        ManifestVideoDetail detail;
        detail.narrationSummary = response.answer.left(140);
        detail.ragSources = response.sources;
        detail.extractionRate = response.extractionRate;
        detail.extractionDetail = response.extractionDetail;
        detail.pipeline = {
            {QStringLiteral("ingest"), QStringLiteral("取り込み"), ingestSec},
            {QStringLiteral("compose"), QStringLiteral("構成 (スライド分割)"), composeSec},
            {QStringLiteral("narrate"), QStringLiteral("ナレーション (SAPI TTS)"), narrateSec},
            {QStringLiteral("render"), QStringLiteral("レンダリング+エンコード"), renderSec},
            {QStringLiteral("publish"), QStringLiteral("公開"), 0.0},
        };

        // Local-only per §"RAGReel配布" decision -- every install writes to
        // its own output/ folder next to the exe, no shared/network
        // location. Each person's videos+dashboard stay on their own
        // machine; getting a video onto the public site is a separate,
        // manual admin step (see docs/technical-reference.md).
        const QString outputDir = appRelativePath(QStringLiteral("output"));
        services.manifestWriter().publish(outputDir, entry, detail, outputMp4Path, thumbnailImage);
        logLine(QStringLiteral("Published to local dashboard: %1/videos/%2/")
                    .arg(outputDir, entry.id));
        orchestrator.recordStage(JobStage::Publish, /*success=*/true, 0.0);
    } catch (const std::exception& e) {
        logLine(QStringLiteral("WARNING: failed to publish to web dashboard: %1")
                    .arg(QString::fromUtf8(e.what())));
        orchestrator.recordStage(JobStage::Publish, /*success=*/false, 0.0,
                                  e.what());
    }

    // IMPROVEMENT_PLAN.md Phase 1 checklist item: confirm every JobStage
    // actually ran and was recorded (Compose/Encode aren't independently
    // timed yet -- Encode is folded into the Render measurement above,
    // §0 gap -- but every stage still gets an entry so this list is a
    // complete, ordered record of the job).
    for (const StageResult& stage : orchestrator.stageResults()) {
        logLine(QStringLiteral("Stage %1: %2 (%3s)%4")
                    .arg(QString::fromUtf8(jobStageKey(stage.stage)))
                    .arg(stage.success ? QStringLiteral("ok") : QStringLiteral("FAILED"))
                    .arg(stage.durationSec, 0, 'f', 2)
                    .arg(stage.errorMessage.empty()
                             ? QString()
                             : QStringLiteral(" -- %1").arg(QString::fromUtf8(stage.errorMessage))));
    }

    return 0;
}
