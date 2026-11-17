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
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QUrl>
#include <QHash>
#include <QVariantList>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX // windows.h's max()/min() macros collide with std::max/std::min below
#endif
#include <windows.h>
#endif

#include "common/app_utils.h"
#include "encode/frame_pipeline.h"
#include "encode/video_encoder.h"
#include "encode/video_profile.h"
#include "ingest/script_composer.h"
#include "manifest/manifest_writer.h"
#include "ingest/houdini_script.h"
#include "narration/narration_engine.h"
#include "narration/scripted_narration.h"
#include "orchestrator/orchestrator.h"
#include "ragclient/cloud_rag_client.h"
#include "scene/frame_reuse.h"
#include "scene/scene_assembler.h"
#include "services/concrete_services.h"
#include "services/service_container.h"

namespace {

// 大きさ・fps は VideoProfile(encode/video_profile.h)で決める。既定は 1920x1080・30 fps。
// 経緯: 2026-09-26 に 30→15 fps に下げた(描画+エンコードの時間はフレーム数に比例し、スライド・スクリーンショットの
// 動画では 30 fps にしても動きは増えず、生成が ~17 分と倍かかるだけだったため)。2026-11-17 の実証実験で
// 「15 fps・1280x720 では荒く、VEX など文字が多い所がつぶれる」と出たため、既定をフル HD・30 fps に戻した。
// 速さを優先したいときは --video-profile 720p15 か RAGREEL_VIDEO_PROFILE=720p15 で、前の設定にできる。
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

// 環境変数のオン・オフ("0" / "false" / "off" でオフ、空ならば既定値)
bool envFlag(const char* name, bool defaultValue) {
    const QString value = qEnvironmentVariable(name).trimmed().toLower();
    if (value.isEmpty()) return defaultValue;
    return !(value == QStringLiteral("0") || value == QStringLiteral("false") || value == QStringLiteral("off"));
}

// 描画 -> 変換(RGBA→YUV) -> VP9 エンコード の 3 段に分けて流す。段ごとにスレッド 1 本。
// 変換のスレッドに渡す 1 フレーム。repeat は「直前に渡した画面と同じ」の印(RGBA→YUV の変換を省いて、前の YUV を使う)。
// QImage は共有されるので、同じ画面を何度積んでも、メモリは増えない。
struct EncodeItem {
    QImage image;
    bool repeat = false;
};

// VP9 エンコードのスレッドに渡す 1 フレーム(変換済みの YUV。繰り返しは同じ YUV を共有する)
struct ConvertedItem {
    IVideoEncoder::ConvertedFrame yuv;
};

// 段の間のキューの長さ(フレーム数)。描画 -> 変換は RGBA(1080p で 1 枚 約 8 MB)、変換 -> エンコードは YUV420
// (1 枚 約 3 MB)。同じ画面の繰り返しは共有なので、増えない。最大でも、RGBA 4 枚 + YUV 8 枚 + 作業中の数枚(約 80 MB)。
constexpr std::size_t kEncodeQueueFrames = 4;
constexpr std::size_t kConvertedQueueFrames = 8;

// RAGREEL_VERIFY_FRAME_REUSE=1 のとき、使い回した(丸めた)画面と、毎フレーム描いた画面を比べた結果をためる。
// 違ってよいのは、進捗バーのグラデーションの色が最小の段だけずれる所(バーの帯の中)だけ。
struct FrameVerifyStats {
    long long frames = 0;
    long long framesWithDiff = 0;
    long long diffPixels = 0;
    long long diffPixelsOutsideBar = 0;
    int maxDiff = 0;
    long long diffPixelsOver8 = 0;
    long long diffPixelsOver100 = 0;
    long long firstBigDiffFrame = -1;  // 最初に大きく違ったフレームの番号と x(確認用)
    int firstBigDiffX = -1;

    void compare(const QImage& delivered, const QImage& exact, int width, int height) {
        ++frames;
        const double scale = width / 1280.0;
        const double stageHeight = height / scale;
        const int barTop = static_cast<int>(std::floor((stageHeight - 96 + 42) * scale)) - 1;
        const int barBottom = static_cast<int>(std::ceil((stageHeight - 96 + 53) * scale)) + 1;
        bool any = false;
        if (delivered.size() != exact.size() || delivered.format() != exact.format()) {
            any = true;
            ++diffPixelsOutsideBar;
            maxDiff = 255;
        } else {
            for (int y = 0; y < delivered.height(); ++y) {
                const uchar* a = delivered.constScanLine(y);
                const uchar* b = exact.constScanLine(y);
                const int bytes = delivered.width() * 4;
                if (std::memcmp(a, b, static_cast<size_t>(bytes)) == 0) continue;
                for (int x = 0; x < delivered.width(); ++x) {
                    int d = 0;
                    for (int c = 0; c < 4; ++c) d = std::max(d, std::abs(int(a[x * 4 + c]) - int(b[x * 4 + c])));
                    if (d == 0) continue;
                    any = true;
                    ++diffPixels;
                    maxDiff = std::max(maxDiff, d);
                    if (d > 8) ++diffPixelsOver8;
                    if (d > 100) {
                        ++diffPixelsOver100;
                        if (firstBigDiffFrame < 0) {
                            firstBigDiffFrame = frames - 1;
                            firstBigDiffX = x;
                        }
                    }
                    if (y < barTop || y > barBottom) ++diffPixelsOutsideBar;
                }
            }
        }
        if (any) ++framesWithDiff;
    }

    QString summary(int width, int height) const {
        Q_UNUSED(width);
        Q_UNUSED(height);
        return QStringLiteral("Frame reuse verification: %1 frames compared, %2 differ, max channel difference %3/255, "
                              "%4 differing pixels (%5 outside the progress bar; %6 differ by more than 8, %7 by more than 100)")
            .arg(frames).arg(framesWithDiff).arg(maxDiff).arg(diffPixels).arg(diffPixelsOutsideBar)
            .arg(diffPixelsOver8).arg(diffPixelsOver100)
            + (firstBigDiffFrame >= 0 ? QStringLiteral(" [first big difference: frame %1, x=%2]").arg(firstBigDiffFrame).arg(firstBigDiffX) : QString());
    }
};

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
    // Beginner-friendly script written by the Houdini side (houdini_script.h). Optional: without it the
    // tutorial text is narrated as before.
    const QString houdiniScriptPath = takeFlagValue(args, QStringLiteral("--houdini-script"));
    const bool useHoudiniTutorial = !houdiniMdPath.isEmpty();
    // 動画の大きさ・fps(コマンドライン > 環境変数 > 既定の 1080p30)
    QString videoProfileName = takeFlagValue(args, QStringLiteral("--video-profile"));
    if (videoProfileName.isEmpty()) {
        videoProfileName = qEnvironmentVariable("RAGREEL_VIDEO_PROFILE");
    }
    std::string videoProfileNote;
    const VideoProfile videoProfile = video_profile::resolve(videoProfileName.toStdString(), &videoProfileNote);
    const int kFrameWidth = videoProfile.width;
    const int kFrameHeight = videoProfile.height;
    const int kFps = videoProfile.fps;

    const bool useMockPlain = args.removeAll(QStringLiteral("--mock-plain")) > 0;
    const bool useMock = args.removeAll(QStringLiteral("--mock")) > 0 || useMockPlain;

    QString topic = args.size() > 0
        ? args.at(0)
        : QStringLiteral("Houdini21のVEXでforループを使う基本的な方法を教えて");
    const QString dbKey = args.size() > 1 ? args.at(1) : QStringLiteral("houdini21");

    // Every run gets a unique output basename (timestamp + short random
    // suffix), so re-running with a different topic can never appear to
    // silently reuse/overwrite a previous run's video, narration WAV, or
    // Mermaid PNGs -- each run's artifacts are fully independent files.
    // Second-granularity alone is not enough: a batch/queue driver that
    // launches this exe multiple times back-to-back (e.g. processing
    // several Houdini tutorials) can start two runs within the same
    // wall-clock second, which previously produced the same runId and made
    // ManifestWriter::publish() silently overwrite the first run's entry
    // and files with the second's.
    const QString runId = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmsszzz")) +
        QStringLiteral("_%1").arg(QRandomGenerator::global()->bounded(0x10000), 4, 16, QLatin1Char('0'));
    const QString createdAtIso = QDateTime::currentDateTime().toString(Qt::ISODate);
    logLine(QStringLiteral("Run ID: %1").arg(runId));
    if (!videoProfileNote.empty()) {
        logLine(QStringLiteral("WARNING: %1").arg(QString::fromStdString(videoProfileNote)));
    }
    logLine(QStringLiteral("Video profile: %1 (%2x%3, %4 fps)")
                .arg(QString::fromUtf8(videoProfile.name))
                .arg(kFrameWidth)
                .arg(kFrameHeight)
                .arg(kFps));

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
    // Narration backend: VOICEVOX when its local engine is reachable,
    // otherwise SAPI5 (RAGREEL_TTS_BACKEND=auto|voicevox|sapi; see
    // docs/narration-quality.md). Falls back per call, so a stopped engine
    // never turns into a silent video.
    QString narrationEngineNote;
    services.registerNarrationEngine(makeNarrationEngineFromEnvironment(&narrationEngineNote));
    logLine(QStringLiteral("Narration engine: %1").arg(narrationEngineNote));
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
            response.allowedNamespaces = {dbKey};  // was a hardcoded "houdini21"
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
    // does (the Cloud RAG answer prompt never asks for either -- confirmed by inspecting
    // the backend), so both the diagram feature and narration over
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
    // explanation instead of a placeholder. Kept entirely on the RAGReel
    // side (no changes to the shared backend other Unity/Houdini clients
    // also depend on).
    const QRegularExpression& mermaidCheck = mermaidFenceRegex();
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

    // ---- Slides first (2026-10-19). With a beginner script the narration is written per slide and
    // each slide stays on screen exactly as long as its own narration, so the slides have to exist
    // before anything is synthesised. (Without a script the narration is the whole text read once,
    // as before, and the slide lengths are guessed from the text.)
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
        // 同じ資料(例: Shelf tools・houdini22shelfindex)が 2 件ずつ並ぶのを、1 件にまとめる
        for (Slide& s : rawSlides) {
            if (!s.referenceItems.isEmpty()) {
                s.referenceItems = dedupeReferenceItems(s.referenceItems);
            }
        }
    }
    std::vector<Slide> slides =
        splitLongTextSlides(expandDiagramSlides(rawSlides, runId), kMaxCharsPerSlide);
    if (useHoudiniTutorial) {
        assignHoudiniFinalGraphScreenshot(slides, houdiniShots);
    }
    estimatedTokens +=
        enrichSlidesForDisplay(slides, dbKey, runId, useMock, services.vectorStoreClient());
    logLine(QStringLiteral("Estimated tokens consumed (rough, character-based): %1").arg(estimatedTokens));

    // Beginner script: spoken text + short on-screen text per slide (after enrichSlidesForDisplay so the
    // script's captions replace the automatic bullets).
    HoudiniScript houdiniScript;
    if (useHoudiniTutorial && !houdiniScriptPath.isEmpty()) {
        houdiniScript = loadHoudiniScript(houdiniScriptPath);
        for (const QString& w : houdiniScript.warnings) {
            logLine(QStringLiteral("WARNING: beginner script: %1").arg(w));
        }
        if (houdiniScript.valid) {
            applyHoudiniScript(slides, houdiniScript);
            assignFinalResultImage(slides, houdiniShots);  // intro / closing slides show the finished result
            logLine(QStringLiteral("Beginner script applied (%1 step text(s), source=%2)")
                        .arg(houdiniScript.steps.size())
                        .arg(houdiniScript.source.isEmpty() ? QStringLiteral("?") : houdiniScript.source));
        } else {
            logLine("WARNING: --houdini-script was given but is missing or unusable; narrating the tutorial text as before");
        }
    }
    if (useHoudiniTutorial) {
        // No slide should be left with the empty gradient panel: show the node diagram or the finished result.
        assignPlaceholderImages(slides, houdiniShots, houdiniScript.nodeDiagramPath);
    }
    const double composeSec = composeTimer.elapsed() / 1000.0;
    orchestrator.recordStage(JobStage::Compose, /*success=*/true, composeSec);
    logLine(QStringLiteral("Split into %1 slides").arg(slides.size()));

    // Narration (best-effort): a missing/broken TTS voice degrades to a
    // silent video rather than failing the whole pipeline.
    QElapsedTimer narrateTimer;
    narrateTimer.start();
    const QString narrationText =
        topic + QStringLiteral("。") + stripMarkdownForNarration(response.answer, codeCaptions);
    const QString wavPath = QStringLiteral("phase2_cloudrag_%1_narration.wav").arg(runId);
    QString audioPathForEncoder;
    double narrationDurationSeconds = 0.0;
    std::vector<NarrationSegment> narrationSegments;  // for subtitles (V7)
    bool narrationSegmentsExact = false;
    QString narrationError;
    QString narrationVoice;
    bool scriptedNarrationDone = false;
    std::vector<scripted_narration::Block> narrationBlocks;  // per slide, only when scriptedNarrationDone
    {
        // Scoped so the GPU lease (and, once NarrationEngine actually holds
        // a GPU context -- see orchestrator.h's scope note; today's SAPI5
        // implementation doesn't -- that context too) releases as soon as
        // synthesis finishes, strictly before the Assemble/Render phase
        // below ever tries to acquire its own lease.
        GpuLease narrateLease = orchestrator.acquireGpuLease(GpuLeaseOwner::NarrationEngine);
        if (houdiniScript.valid) {
            try {
                const QStringList blocks = slideNarrationBlocks(slides);
                const QStringList subtitleBlocks = slideSubtitleBlocks(slides);  // real spelling, where the script has it
                qsizetype chars = 0;
                for (const QString& b : blocks) chars += b.size();
                logLine(QStringLiteral("Synthesizing scripted narration (%1 slides, %2 chars)...")
                            .arg(blocks.size())
                            .arg(chars));
                // One "Narration progress: k / n" line per spoken block: the Houdini panel turns these into
                // its progress bar (houdini/python_panels/video_progress.py).
                int spokenTotal = 0;
                for (const QString& b : blocks) {
                    if (!b.trimmed().isEmpty()) ++spokenTotal;
                }
                std::atomic<int> spokenDone{0};
                // 同時に合成する数(既定 1 = 従来どおり)。RAGREEL_TTS_PARALLEL=N で上げられるが、エンジンが
                // 複数のスレッドから呼ばれて安全かは、エンジンごとに確かめていない(SAPI は COM のスレッドの扱い、
                // クラウドは同時のリクエストと費用)。確かめた上で使うこと(docs/technical-reference.md §28)。
                scripted_narration::Options narrationOptions;
                narrationOptions.maxParallel = std::clamp(qEnvironmentVariableIntValue("RAGREEL_TTS_PARALLEL"), 1, 8);
                if (narrationOptions.maxParallel > 1) {
                    logLine(QStringLiteral("Narration: synthesizing up to %1 blocks at once (RAGREEL_TTS_PARALLEL)")
                                .arg(narrationOptions.maxParallel));
                }
                const scripted_narration::Result scripted = scripted_narration::synthesizeBlocks(
                    [&](const QString& text, const QString& path) {
                        auto result = services.narrationEngine().synthesize(text, path);
                        logLine(QStringLiteral("Narration progress: %1 / %2").arg(++spokenDone).arg(spokenTotal));
                        return result;
                    },
                    blocks, wavPath, narrationOptions, subtitleBlocks);
                audioPathForEncoder = scripted.merged.wavPath;
                narrationDurationSeconds = scripted.merged.durationSeconds;
                narrationSegments = scripted.merged.segments;
                narrationSegmentsExact = scripted.merged.segmentsExact;
                narrationVoice = scripted.merged.engineName;
                narrationBlocks = scripted.blocks;
                scriptedNarrationDone = true;
                logLine(QStringLiteral("Narration synthesized: %1s").arg(narrationDurationSeconds, 0, 'f', 1));
            } catch (const std::exception& e) {
                logLine(QStringLiteral("WARNING: scripted narration failed, narrating the tutorial text instead: %1")
                            .arg(QString::fromUtf8(e.what())));
            }
        }
        if (!scriptedNarrationDone) {
            try {
                logLine(QStringLiteral("Synthesizing narration (%1 chars)...").arg(narrationText.size()));
                const NarrationResult narration = services.narrationEngine().synthesize(narrationText, wavPath);
                audioPathForEncoder = narration.wavPath;
                narrationDurationSeconds = narration.durationSeconds;
                narrationSegments = narration.segments;
                narrationSegmentsExact = narration.segmentsExact;
                narrationVoice = narration.engineName;
                logLine(QStringLiteral("Narration synthesized: %1s").arg(narrationDurationSeconds, 0, 'f', 1));
            } catch (const std::exception& e) {
                narrationError = QString::fromUtf8(e.what());
                logLine(QStringLiteral("WARNING: narration synthesis failed, continuing without audio: %1")
                            .arg(narrationError));
            }
        }
    }
    // Which voice really spoke (the Houdini panel shows this line, so a silent fall-back to the old
    // voice is visible).
    if (!narrationVoice.isEmpty()) {
        logLine(QStringLiteral("Narration voice: %1").arg(narrationVoice));
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

    // Scripted narration: a slide is on screen exactly while its own text is spoken. Otherwise the
    // slide lengths are weighted by text length, as before.
    const std::vector<int> slideStartFrames = scriptedNarrationDone
        ? scripted_narration::startFramesFromBlocks(narrationBlocks, kFps, frameCount)
        : computeSlideStartFrames(slides, frameCount, kFps);
    {
        // Resulting on-screen time per kind (Houdini tutorials aim for
        // node 70% / viewport 20% / other 10%, see kNodeTimeShare). With scripted narration this is a
        // result of how long each step takes to explain, not a target.
        double kindFrames[3] = {0.0, 0.0, 0.0};
        for (size_t i = 0; i < slides.size(); ++i) {
            kindFrames[static_cast<int>(slides[i].visualKind)] += slideStartFrames[i + 1] - slideStartFrames[i];
        }
        const double totalF = std::max(1.0, kindFrames[0] + kindFrames[1] + kindFrames[2]);
        logLine(QStringLiteral("Screen time share: node %1% / viewport %2% / other %3%")
                    .arg(kindFrames[static_cast<int>(VisualKind::Node)] / totalF * 100.0, 0, 'f', 0)
                    .arg(kindFrames[static_cast<int>(VisualKind::Viewport)] / totalF * 100.0, 0, 'f', 0)
                    .arg(kindFrames[static_cast<int>(VisualKind::Other)] / totalF * 100.0, 0, 'f', 0));
    }
    // One line per slide: where the screen time goes (and how much is said), for tuning scripts.
    if (scriptedNarrationDone) {
        const QStringList spoken = slideNarrationBlocks(slides);
        for (size_t i = 0; i < slides.size(); ++i) {
            logLine(QStringLiteral("Slide %1: %2 s, %3 chars spoken, kind=%4, \"%5\"")
                        .arg(i + 1, 2)
                        .arg(static_cast<double>(slideStartFrames[i + 1] - slideStartFrames[i]) / kFps, 5, 'f', 1)
                        .arg(spoken.value(static_cast<int>(i)).size(), 4)
                        .arg(slides[i].visualKind == VisualKind::Node ? QStringLiteral("node")
                             : slides[i].visualKind == VisualKind::Viewport ? QStringLiteral("viewport")
                                                                            : QStringLiteral("other"))
                        .arg(slides[i].heading));
        }
    }

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

    const QString outputVideoPath = QStringLiteral("phase2_cloudrag_%1.webm").arg(runId);
    QImage thumbnailImage;  // captured partway through for the web dashboard gallery
    double renderSec = 0.0;
    bool renderFailed = false;
    std::string renderErrorMessage;
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
            outputVideoPath.toStdString(), kFrameWidth, kFrameHeight, kFps,
            audioPathForEncoder.toStdString());

        // Unlike narration synthesis/RAG queries/publishing above and
        // below, a failure here has no graceful degrade-and-continue path
        // (there is no video without a working render/encode loop), but it
        // still needs to be caught: encodeAndWrite()/pushFrame() can throw
        // (e.g. a transient libvpx encode error) from deep inside this
        // loop, and letting that propagate out of main() uncaught leads to
        // std::terminate() instead of unwinding -- so `encoder`'s
        // destructor (whose best-effort av_write_trailer is documented as
        // the fallback for exactly this case) may never run, and nothing
        // gets logged.
        // 描画とエンコードを別スレッドで流し(FramePipeline)、見た目が前と同じフレームは描かずに使い回す
        // (frame_reuse.h)。QML・RHI の描画は、この(メイン)スレッド 1 本に固定したまま。
        // 切り分け用の環境変数(既定はどちらも有効):
        //   RAGREEL_FRAME_REUSE=0        フレームの使い回しと進捗バーの丸めを止め、毎フレーム描く(従来と同じ)
        //   RAGREEL_ENCODE_THREAD=0      エンコードを描画と同じスレッドでやる(従来と同じ直列)
        //   RAGREEL_VERIFY_FRAME_REUSE=1 使い回した画面を、毎フレーム描いた画面と比べて、差をログに出す(遅い。確認用)
        //   RAGREEL_FRAME_HASH_LOG=<path> エンコーダに渡した各フレームの画素のハッシュを書き出す(確認用)
        const bool reuseEnabled = envFlag("RAGREEL_FRAME_REUSE", true);
        const bool encodeThreadEnabled = envFlag("RAGREEL_ENCODE_THREAD", true);
        const bool verifyReuse = reuseEnabled && envFlag("RAGREEL_VERIFY_FRAME_REUSE", false);
        const QString hashLogPath = qEnvironmentVariable("RAGREEL_FRAME_HASH_LOG");
        logLine(QStringLiteral("Render mode: frame reuse %1, encode thread %2")
                    .arg(reuseEnabled ? QStringLiteral("on") : QStringLiteral("off"))
                    .arg(encodeThreadEnabled ? QStringLiteral("on") : QStringLiteral("off")));

        QElapsedTimer renderTimer;
        renderTimer.start();
        qint64 sceneNs = 0;         // 描画スレッドが、描く(QML + 読み戻し)のに使った時間
        qint64 waitNs = 0;          // 描画スレッドが、後ろの段が追いつくのを待った時間(キューが満杯)
        long long renderedFrames = 0;     // 実際に QML で描いたフレーム数
        FrameVerifyStats verifyStats;
        std::ofstream hashLog;
        if (!hashLogPath.isEmpty()) {
            hashLog.open(hashLogPath.toStdString(), std::ios::trunc);
        }

        // 変換の段: 同じ画面の繰り返しは、RGBA→YUV の変換をやり直さず、前の YUV を使う。
        // lastYuv は変換の段(スレッドを使わない場合は、このスレッド)だけが触る。
        IVideoEncoder::ConvertedFrame lastYuv;
        const auto convertOne = [&](const EncodeItem& item) {
            if (!item.repeat || !lastYuv) {
                lastYuv = encoder->convertFrame(item.image.constBits());
            }
            return lastYuv;
        };
        // 段のスレッドは encoder より後に作る(壊れるのは逆順なので、スレッドを止めてから encoder を壊す)。
        // エンコードの段(vp9Pipeline)を先に作り、変換の段(pipeline)を後に作る: 壊すときは変換の段が先に止まる。
        std::unique_ptr<FramePipeline<ConvertedItem>> vp9Pipeline;
        std::unique_ptr<FramePipeline<EncodeItem>> pipeline;
        if (encodeThreadEnabled) {
            vp9Pipeline = std::make_unique<FramePipeline<ConvertedItem>>(
                kConvertedQueueFrames, [&](const ConvertedItem& item) { encoder->encodeConverted(item.yuv); });
            pipeline = std::make_unique<FramePipeline<EncodeItem>>(
                kEncodeQueueFrames,
                [&](const EncodeItem& item) { vp9Pipeline->push(ConvertedItem{convertOne(item)}); });
        }

        try {
        frame_reuse::ReusePolicy reusePolicy(reuseEnabled);
        // スライドごとの「コードがスクロールするか」(-1: まだ不明 / 0: しない / 1: する)。不明な間はするものとして扱う
        std::vector<signed char> codeScrollKnown(slides.size(), -1);
        QImage lastImage;  // 最後に描いた画面(使い回す元)
        size_t lastHash = 0;
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
            const double exactProgress = static_cast<double>(i) / (frameCount - 1);
            frameProps.progress = exactProgress;
            frameProps.slideIndex = static_cast<int>(currentSlide);
            frameProps.slideHeading = active.heading;
            frameProps.slideBullet1 = active.bullet1;
            frameProps.slideBullet2 = active.bullet2;
            frameProps.slideCodeBlock = active.codeBlock;
            frameProps.slideReferenceItems = active.referenceItems;
            frameProps.slideDiagramSource = diagramSource;
            frameProps.slideProgress = slideProgress;

            // このフレームの見た目を決める値。前に描いた画面と同じなら、描かずにその画面を使う。
            // コードのスライドは、スクロールすると slideProgress で見た目が変わる(収まるなら変わらない。初回に調べる)
            const bool hasCode = !active.codeBlock.isEmpty();
            const bool codeScrolls = hasCode && codeScrollKnown[currentSlide] != 0;
            const frame_reuse::FrameKey key = frame_reuse::makeKey(
                static_cast<int>(currentSlide), diagramSource.toStdString(), slideProgress, exactProgress,
                kFrameWidth, codeScrolls);

            QImage frameImage;
            bool repeat = false;
            if (reusePolicy.canReuse(key)) {
                frameImage = lastImage;  // QImage は共有(コピーしない)。中身は変えないので安全
                repeat = true;
            } else {
                QElapsedTimer sceneTimer;
                sceneTimer.start();
                // バーの幅が整数の画素になる進捗で描く(使い回すフレームすべてが、この画面と見た目が同じになる)
                if (reuseEnabled) {
                    frameProps.progress = frame_reuse::quantizedProgress(exactProgress, kFrameWidth);
                }
                frameImage = sceneAssembler.renderFrame(frameProps);
                sceneNs += sceneTimer.nsecsElapsed();
                ++renderedFrames;
                if (hasCode && codeScrollKnown[currentSlide] < 0) {
                    codeScrollKnown[currentSlide] = sceneAssembler.codeScrolls() ? 1 : 0;
                }
                reusePolicy.commit(key);
                lastImage = frameImage;
                if (hashLog.is_open()) {
                    lastHash = qHashBits(frameImage.constBits(), static_cast<size_t>(frameImage.sizeInBytes()), 12345);
                }
            }
            if (hashLog.is_open()) {
                hashLog << i << ' ' << lastHash << '\n';
            }
            if (verifyReuse) {
                // 使い回し・丸めなしで、同じフレームを描き直して比べる(描いたあとの状態は次のフレームで上書きされる)
                SceneAssembler::FrameProperties exactProps = frameProps;
                exactProps.progress = exactProgress;
                verifyStats.compare(frameImage, sceneAssembler.renderFrame(exactProps), kFrameWidth, kFrameHeight);
            }

            EncodeItem item{frameImage, repeat};
            if (pipeline) {
                QElapsedTimer waitTimer;
                waitTimer.start();
                pipeline->push(std::move(item));
                waitNs += waitTimer.nsecsElapsed();
            } else {
                encoder->encodeConverted(convertOne(item));
            }

            // A frame ~40% in usually lands inside real slide content rather
            // than the title/intro card, making for a more representative
            // gallery thumbnail than frame 0 would be.
            if (i == frameCount * 4 / 10) {
                thumbnailImage = frameImage.copy();
            }

            if (i % kFps == 0) {
                logLine(QStringLiteral("Rendered frame %1 / %2").arg(i).arg(frameCount));
            }
            // Where the render stage's wall-clock goes (QML scene render + GPU
            // readback vs. FFmpeg conversion/encode), so speed work targets the
            // real bottleneck. Separate line from "Rendered frame" on purpose:
            // the Houdini panel's progress poll parses that one.
            // scene = 描画スレッドが描くのに使った時間 / encode = 変換 + エンコードの時間 / wait = 描画が後ろの段を待った時間
            // (どれも 1 フレームあたり。段ごとに別スレッドなので、scene + encode = 全体の時間とは限らない)
            if (i > 0 && i % (kFps * 10) == 0) {
                const double n = static_cast<double>(i + 1);
                long long swsNs = 0, codecNs = 0;
                encoder->timings(&swsNs, &codecNs);
                logLine(QStringLiteral("Timing after %1 frames: scene %2 ms/frame, encode %3 ms/frame, "
                                       "wait %4 ms/frame (rendered %5 of %1 frames)")
                            .arg(i + 1)
                            .arg(sceneNs / 1e6 / n, 0, 'f', 1)
                            .arg((swsNs + codecNs) / 1e6 / n, 0, 'f', 1)
                            .arg(waitNs / 1e6 / n, 0, 'f', 1)
                            .arg(renderedFrames));
                // 内訳(測定用): QML の描画 / GPU の読み戻し / QImage への写し / sws_scale / VP9 のエンコード
                {
                    const SceneAssembler::Timings& st = sceneAssembler.timings();
                    logLine(QStringLiteral("Timing breakdown: qml %1 / readback %2 / copy %3 / sws %4 / vp9 %5 ms/frame")
                                .arg(st.qmlNs / 1e6 / n, 0, 'f', 2)
                                .arg(st.readbackNs / 1e6 / n, 0, 'f', 2)
                                .arg(st.copyNs / 1e6 / n, 0, 'f', 2)
                                .arg(swsNs / 1e6 / n, 0, 'f', 2)
                                .arg(codecNs / 1e6 / n, 0, 'f', 2));
                }
            }
        }
        // 積んだフレームをすべてエンコードし終えるまで、前の段から順に待つ(後ろの段の失敗は、ここで投げ直される)
        if (pipeline) {
            QElapsedTimer drainTimer;
            drainTimer.start();
            pipeline->finish();
            vp9Pipeline->finish();
            waitNs += drainTimer.nsecsElapsed();
        }
        renderSec = renderTimer.elapsed() / 1000.0;
        {
            const SceneAssembler::Timings& st = sceneAssembler.timings();
            long long swsNs = 0, codecNs = 0;
            encoder->timings(&swsNs, &codecNs);
            const double n = std::max(1, frameCount);
            logLine(QStringLiteral("Timing total (%1 frames): wall %2 s, scene %3 / encode %4 / wait %5 ms/frame; "
                                   "qml %6 / readback %7 / copy %8 / sws %9 / vp9 %10 ms/frame; rendered %11 of %1 frames")
                        .arg(frameCount)
                        .arg(renderSec, 0, 'f', 2)
                        .arg(sceneNs / 1e6 / n, 0, 'f', 2)
                        .arg((swsNs + codecNs) / 1e6 / n, 0, 'f', 2)
                        .arg(waitNs / 1e6 / n, 0, 'f', 2)
                        .arg(st.qmlNs / 1e6 / n, 0, 'f', 2)
                        .arg(st.readbackNs / 1e6 / n, 0, 'f', 2)
                        .arg(st.copyNs / 1e6 / n, 0, 'f', 2)
                        .arg(swsNs / 1e6 / n, 0, 'f', 2)
                        .arg(codecNs / 1e6 / n, 0, 'f', 2)
                        .arg(renderedFrames));
        }
        if (verifyReuse) {
            logLine(verifyStats.summary(kFrameWidth, kFrameHeight));
        }

        encoder->writeAudioTrack();
        encoder->finish();
        logLine(QStringLiteral("Wrote %1 (%2 frames, %3s)")
                    .arg(outputVideoPath)
                    .arg(frameCount)
                    .arg(durationSeconds, 0, 'f', 1));
        } catch (const std::exception& e) {
            renderFailed = true;
            renderErrorMessage = e.what();
            renderSec = renderTimer.elapsed() / 1000.0;
            logLine(QStringLiteral("ERROR: rendering/encoding failed: %1")
                        .arg(QString::fromUtf8(e.what())));
        }
    }  // end of Assemble/Render GPU-lease scope (assembleLease and sceneAssembler release here)
    orchestrator.recordStage(JobStage::Assemble, /*success=*/!renderFailed, 0.0);
    orchestrator.recordStage(JobStage::Render, /*success=*/!renderFailed, renderSec, renderErrorMessage);
    if (renderFailed) {
        return 1;
    }

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

        // Recorded before the pipeline array below (rather than after
        // manifestWriter().publish() succeeds, as JobStage's other stages
        // are) because the manifest write is itself what "publish" means
        // here -- there's no later point to record it from that isn't
        // also too late to include it in the very data being written.
        // Publish duration isn't independently measured (it's fast/local
        // disk I/O, same as before this stage existed as a typed value).
        orchestrator.recordStage(JobStage::Publish, /*success=*/true, 0.0);

        ManifestVideoDetail detail;
        detail.narrationSummary =
            (scriptedNarrationDone && !houdiniScript.introNarration.isEmpty() ? houdiniScript.introNarration
                                                                              : response.answer).left(140);
        detail.ragSources = response.sources;
        // Houdini のチュートリアル動画は、Cloud RAG に直接聞かない(response.sources が空)ので、「参考」の一覧を出典にする
        // (バッジ無しの打ち切りの動画も読めるよう、スライドではなく、チュートリアルの本文から読む)
        if (useHoudiniTutorial) {
            detail.tutorialSources = parseTutorialSources(response.answer);
        }
        detail.extractionRate = response.extractionRate;
        detail.extractionDetail = response.extractionDetail;
        // Chapter timeline + encode summary for the dashboard (IMPROVEMENT_
        // DESIGN_2026-10.md U1). slideStartFrames has slides.size()+1 entries
        // (the last is the end frame), so a chapter's span is the delta to
        // the next one -- the same numbers the renderer itself used above.
        for (size_t i = 0; i < slides.size() && i + 1 < slideStartFrames.size(); ++i) {
            ChapterInfo chapter;
            chapter.title = slides[i].heading;
            switch (slides[i].visualKind) {
            case VisualKind::Node: chapter.kind = QStringLiteral("node"); break;
            case VisualKind::Viewport: chapter.kind = QStringLiteral("viewport"); break;
            case VisualKind::Other: chapter.kind = QStringLiteral("other"); break;
            }
            chapter.startSec = static_cast<double>(slideStartFrames[i]) / kFps;
            chapter.durationSec = static_cast<double>(slideStartFrames[i + 1] - slideStartFrames[i]) / kFps;
            for (const QString& bullet : {slides[i].bullet1, slides[i].bullet2}) {
                if (!bullet.trimmed().isEmpty()) {
                    chapter.points << bullet.trimmed();
                }
            }
            for (const QVariant& item : slides[i].referenceItems) {
                const QString title = item.toMap().value(QStringLiteral("title")).toString().trimmed();
                if (!title.isEmpty()) {
                    chapter.sources << title;
                }
            }
            detail.chapters.push_back(chapter);
        }
        detail.frameWidth = kFrameWidth;
        detail.frameHeight = kFrameHeight;
        detail.fps = kFps;
        detail.fileSizeBytes = QFileInfo(outputVideoPath).size();
        detail.subtitles = narrationSegments;
        detail.subtitlesExact = narrationSegmentsExact;
        // Built from Orchestrator's own StageResult log (Phase 1/§0 gap #4:
        // this used to be a separate hand-typed {stage,label,duration}
        // list that could silently drift from what actually ran). Sorted
        // by JobStage's canonical Ingest->...->Publish order for display,
        // since recordStage() calls happen in this codebase's actual
        // execution order (Narrate currently runs before Compose -- a
        // pre-existing quirk, not introduced by this sort) rather than
        // that canonical order.
        std::vector<StageResult> orderedStages = orchestrator.stageResults();
        std::sort(orderedStages.begin(), orderedStages.end(), [](const StageResult& a, const StageResult& b) {
            return static_cast<int>(a.stage) < static_cast<int>(b.stage);
        });
        for (const StageResult& stage : orderedStages) {
            detail.pipeline.push_back({QString::fromUtf8(jobStageKey(stage.stage)),
                                        QString::fromUtf8(jobStageLabel(stage.stage)),
                                        stage.durationSec, stage.success,
                                        QString::fromUtf8(stage.errorMessage)});
        }

        // Local-only per §"RAGReel配布" decision -- every install writes to
        // its own output/ folder next to the exe, no shared/network
        // location. Each person's videos+dashboard stay on their own
        // machine; getting a video onto the public site is a separate,
        // manual admin step (see docs/technical-reference.md).
        const QString outputDir = appRelativePath(QStringLiteral("output"));
        services.manifestWriter().publish(outputDir, entry, detail, outputVideoPath, thumbnailImage);
        logLine(QStringLiteral("Published to local dashboard: %1/videos/%2/")
                    .arg(outputDir, entry.id));
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
