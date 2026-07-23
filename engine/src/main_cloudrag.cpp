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

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QUrl>
#include <QVariantList>

#include <rhi/qrhi.h>

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

#include "encode/video_encoder.h"
#include "manifest/manifest_writer.h"
#include "narration/narration_engine.h"
#include "ragclient/cloud_rag_client.h"

namespace {

constexpr int kFrameWidth = 1280;
constexpr int kFrameHeight = 720;
constexpr int kFps = 30;
constexpr double kMinDurationSeconds = 4.0;
// Keeps the scroll from ending exactly as narration stops, and covers the
// silent tail while the visual reveal catches up if TTS failed/was skipped.
constexpr double kTailPaddingSeconds = 1.5;
// Digest pacing target: a slide's body should read as one punchy screenful,
// not a scroll-through-everything wall of text (see splitLongTextSlides).
constexpr int kMaxCharsPerSlide = 200;

void logLine(const QString& msg) {
    std::fprintf(stderr, "%s\n", msg.toUtf8().constData());
    std::fflush(stderr);
}

// Extracts and removes a "--flag value" pair from `args` (both tokens),
// e.g. for options like --houdini-md that take a following path argument.
// Returns an empty string if the flag isn't present or has no value after
// it, leaving `args` untouched in that case.
// Rough LOCAL estimate of token consumption from character counts. The
// Cloud RAG GAS backend tracks real usage server-side (recordTokenUsage_ in
// gas_cloud_rag.js) but its query response never returns that figure to the
// caller, and extending the shared backend that Unity/Houdini clients also
// depend on is out of scope here -- so this is a heuristic, not a measured
// value, and must stay labeled "推定" (estimated) everywhere it's surfaced.
// ~1.8 characters per token is a reasonable middle estimate for mixed
// Japanese/English text with a Gemini-family tokenizer.
int estimateTokens(const QString& text) {
    return static_cast<int>(std::round(text.size() / 1.8));
}

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

// RAG source citations -- numeric ("[1]", "[4]") or, as seen with real
// houdini21 answers, descriptive ("[参考: 過去Q&A]") -- read fine as body
// text, but once embedded into a Mermaid diagram-generation prompt they get
// interpreted as Mermaid node-shape syntax ("[...]" = rectangle node),
// producing stray garbage nodes in the rendered diagram; they also get read
// aloud verbatim by narration/bullets if left in. Strip any bracketed
// citation-shaped span (bounded length so this can't runaway-match across a
// whole paragraph). Only ever applied to prose (narration/bullets/diagram
// prompts), never to code fences, so it can't corrupt array-index syntax
// like `array[0]`.
QString stripCitationMarkers(QString text) {
    text.remove(QRegularExpression(QStringLiteral("\\[[^\\[\\]]{1,60}\\]")));
    return text;
}

// Cloud RAG answers are markdown (headings/bold/code fences/bullets), which
// reads great on screen (CloudRagScene.qml uses Text.MarkdownText) but
// terribly out loud ("hash hash VEX for loop..."). This strips the syntax
// down to prose for narration.
//
// Diagram fences don't need special handling here: fetchDiagramAndCodeCaptions
// (see below) already prepends a real spoken explanation as plain prose
// immediately before the "## 図解" section's mermaid fence, so by the time
// narration reaches the fence itself, the explanation has already been
// spoken -- the fence just needs a short connective, not a duplicate
// explanation.
//
// Code fences don't get that free ride (they appear inline, mid-section,
// with no guaranteed lead-in sentence explaining that specific snippet), so
// `codeCaptions` supplies a real explanation per fence, consumed in the
// order the fences appear in the document; any fence beyond the fetched
// captions falls back to a generic phrase (e.g. under --mock, or if the
// caption follow-up failed/returned fewer captions than code blocks).
QString stripMarkdownForNarration(QString text, const QStringList& codeCaptions = {}) {
    static const QRegularExpression mermaidFence(QStringLiteral("```mermaid\\n[\\s\\S]*?```"));
    text.replace(mermaidFence, QStringLiteral("(図解をご覧ください。)"));

    static const QRegularExpression codeFence(
        QStringLiteral("```[a-zA-Z0-9]*\\n[\\s\\S]*?```"));
    {
        QString result;
        int cursor = 0;
        int captionIndex = 0;
        QRegularExpressionMatchIterator it = codeFence.globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            result += text.mid(cursor, m.capturedStart() - cursor);
            result += (captionIndex < codeCaptions.size())
                ? codeCaptions.at(captionIndex)
                : QStringLiteral("(コード例は画面をご覧ください。)");
            ++captionIndex;
            cursor = m.capturedEnd();
        }
        result += text.mid(cursor);
        text = result;
    }

    text.replace(QRegularExpression(QStringLiteral("^#{1,6}\\s*"),
                                     QRegularExpression::MultilineOption),
                 QString());
    text.replace(QRegularExpression(QStringLiteral("\\*\\*(.*?)\\*\\*")), QStringLiteral("\\1"));
    text.remove(QLatin1Char('`'));
    text.replace(QRegularExpression(QStringLiteral("^[-*]\\s+"),
                                     QRegularExpression::MultilineOption),
                 QString());
    text = stripCitationMarkers(text);
    text.replace(QRegularExpression(QStringLiteral("\\n{2,}")), QStringLiteral("\n"));
    return text.trimmed();
}

// Lightweight stand-in for the ShotList concept from docs/architecture/
// video-factory-design.md §2/§8 (deferred there to a later phase) --
// splitting the Cloud RAG answer into one slide per "## heading" section
// gives the digest/title-card pacing the user asked for (ref: YouTube tutorial
// title-card style -- big headline + short body per slide, not one long
// scrolling wall of text).
struct Slide {
    QString heading;
    QString body;             // markdown text; kept only as source material for
                               // narration/bullet/code extraction, not rendered directly
    QString diagramImagePath; // set if this slide shows a Mermaid diagram
    QString codeBlock;        // set if this slide shows a code block instead (mutually
                               // exclusive with diagramImagePath)
    QString bullet1;
    QString bullet2;
};

// Pulls two short "at a glance" facts out of a slide's body -- reference
// style (ref: KISARAGI-style split-screen chapter video) shows 2 bullet
// facts per chapter instead of a wall of body text. Prefers existing
// "- "/"* " list items already common in Cloud RAG answers (zero extra API
// calls, deterministic); falls back to the first two sentences if the body
// has no list items at all.
QStringList extractBullets(const QString& markdownBody) {
    QStringList bullets;
    QStringList seenNormalized; // de-dup key: strip markers/whitespace before comparing

    auto normalize = [](QString s) {
        s.remove(QRegularExpression(QStringLiteral("^[-*]\\s+")));
        s.remove(QRegularExpression(QStringLiteral("\\s+")));
        return s;
    };
    auto tryAdd = [&](QString candidate) {
        if (bullets.size() >= 2) return;
        candidate = candidate.trimmed();
        if (candidate.isEmpty()) return;
        const QString normalized = normalize(candidate);
        // The fallback sentence pass below re-scans the same body text, so
        // without this check a list item and its own sentence-split re-scan
        // would show up as two "different" bullets with identical content
        // (one bug this fixes: a bullet appearing twice, once with a
        // leftover leading "- ").
        if (seenNormalized.contains(normalized)) return;
        bullets << candidate;
        seenNormalized << normalized;
    };

    static const QRegularExpression bulletLine(QStringLiteral("(?m)^[-*]\\s+(.+)$"));
    QRegularExpressionMatchIterator it = bulletLine.globalMatch(markdownBody);
    while (it.hasNext() && bullets.size() < 2) {
        QString b = it.next().captured(1).trimmed();
        b.remove(QLatin1Char('`'));
        b.replace(QRegularExpression(QStringLiteral("\\*\\*(.*?)\\*\\*")), QStringLiteral("\\1"));
        b = stripCitationMarkers(b);
        tryAdd(b);
    }

    // Only fall back to generic sentence-splitting when the body had NO
    // markdown list items at all. If we already found 1 (but not 2), the
    // fallback re-scans the same body text via punctuation, and its first
    // "sentence" is very often just a truncated prefix of the list item
    // already captured above (e.g. a single long "- fact A. fact B." line
    // yields a fallback candidate that's a strict prefix of it) -- which
    // isn't caught by exact-match de-dup below since the two strings aren't
    // equal, just overlapping. Showing 1 clean bullet beats showing that
    // bullet plus a truncated restatement of its own opening.
    if (bullets.isEmpty()) {
        QString plain = markdownBody;
        plain.remove(QRegularExpression(QStringLiteral("```[a-zA-Z0-9]*\\n[\\s\\S]*?```")));
        plain.remove(QRegularExpression(QStringLiteral("^#{1,6}\\s*"),
                                         QRegularExpression::MultilineOption));
        plain.remove(QRegularExpression(QStringLiteral("^[-*]\\s+"),
                                         QRegularExpression::MultilineOption));
        plain.remove(QLatin1Char('`'));
        plain.replace(QRegularExpression(QStringLiteral("\\*\\*(.*?)\\*\\*")), QStringLiteral("\\1"));
        plain = stripCitationMarkers(plain);
        // Removing the code fence above leaves behind blank-line gaps where
        // it used to be; collapse all whitespace runs (including those) to
        // a single space so a "sentence" never carries embedded blank lines
        // into the on-screen bullet.
        plain.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));

        static const QRegularExpression sentenceEnd(QStringLiteral("(?<=[。！？.!?])\\s*"));
        const QStringList sentences = plain.trimmed().split(sentenceEnd, Qt::SkipEmptyParts);
        for (const QString& s : sentences) {
            if (bullets.size() >= 2) break;
            QString trimmed = s.trimmed();
            if (trimmed.size() > 60) {
                trimmed = trimmed.left(60) + QStringLiteral("…");
            }
            tryAdd(trimmed);
        }
    }
    while (bullets.size() < 2) {
        bullets << QString();
    }
    return bullets;
}

std::vector<Slide> splitIntoSlides(const QString& topic, const QString& markdown) {
    static const QRegularExpression headingRe(QStringLiteral("(?m)^##\\s+(.+)$"));

    QList<QRegularExpressionMatch> matches;
    QRegularExpressionMatchIterator it = headingRe.globalMatch(markdown);
    while (it.hasNext()) {
        matches.append(it.next());
    }

    std::vector<Slide> slides;
    if (matches.isEmpty()) {
        slides.push_back({topic, markdown.trimmed()});
        return slides;
    }

    const QString intro = markdown.left(matches.first().capturedStart()).trimmed();
    if (!intro.isEmpty()) {
        slides.push_back({topic, intro});
    }

    for (int i = 0; i < matches.size(); ++i) {
        const QRegularExpressionMatch& m = matches.at(i);
        const int bodyStart = m.capturedEnd();
        const int bodyEnd =
            (i + 1 < matches.size()) ? matches.at(i + 1).capturedStart() : markdown.size();
        slides.push_back({m.captured(1).trimmed(), markdown.mid(bodyStart, bodyEnd - bodyStart).trimmed()});
    }

    if (slides.empty()) {
        slides.push_back({topic, markdown.trimmed()});
    }
    return slides;
}

// Renders one Mermaid diagram source to a PNG via mermaid-cli (mmdc), which
// the user asked to add so generated videos can show real diagrams instead
// of walls of text. Shelled out to rather than embedded, since mmdc already
// exists, is well-tested, and re-implementing a Mermaid layout engine in
// C++/QML would be a large, unnecessary undertaking for this PoC stage.
// Throws std::runtime_error if mmdc is missing or fails.
QString renderMermaidToPng(const QString& mermaidSource, const QString& baseName) {
    const QString mmdPath = baseName + QStringLiteral(".mmd");
    const QString pngPath = baseName + QStringLiteral(".png");

    QFile mmdFile(mmdPath);
    if (!mmdFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        throw std::runtime_error("Cannot write mermaid source file: " + mmdPath.toStdString());
    }
    mmdFile.write(mermaidSource.toUtf8());
    mmdFile.close();

    // Invoked via cmd.exe /c rather than starting "mmdc"/"mmdc.cmd" directly:
    // npm installs mmdc as a .cmd shim on Windows, and QProcess's native
    // CreateProcess-based start() does not reliably resolve/execute .cmd
    // files the way a shell does.
    QProcess process;
    process.setProgram(QStringLiteral("cmd.exe"));
    process.setArguments({
        QStringLiteral("/c"), QStringLiteral("mmdc"),
        QStringLiteral("-i"), mmdPath,
        QStringLiteral("-o"), pngPath,
        QStringLiteral("-b"), QStringLiteral("transparent"),
        QStringLiteral("-c"), QStringLiteral(MERMAID_THEME_CONFIG_PATH),
        QStringLiteral("-w"), QStringLiteral("1000"),
        QStringLiteral("-H"), QStringLiteral("560"),
    });
    process.start();
    if (!process.waitForStarted(10000)) {
        throw std::runtime_error("Failed to start mmdc (is mermaid-cli installed and on PATH?)");
    }
    process.waitForFinished(30000);
    if (process.exitCode() != 0) {
        throw std::runtime_error("mmdc failed: " +
                                  QString::fromUtf8(process.readAllStandardError()).toStdString());
    }
    if (!QFile::exists(pngPath)) {
        throw std::runtime_error("mmdc reported success but produced no PNG: " + pngPath.toStdString());
    }
    return pngPath;
}

// Splits any "```mermaid ... ```" fences out of each slide's body into their
// own dedicated diagram slides (same heading, no competing text), rendering
// each via renderMermaidToPng. A render failure degrades that one diagram
// back to a plain code-block slide rather than failing the whole video.
// `runId` (see main()) prefixes the rendered PNG/mmd filenames so successive
// runs never share or overwrite each other's diagram files.
std::vector<Slide> expandDiagramSlides(const std::vector<Slide>& input, const QString& runId) {
    static const QRegularExpression mermaidFence(
        QStringLiteral("```mermaid\\n([\\s\\S]*?)```"));

    std::vector<Slide> result;
    int diagramCounter = 0;
    for (const Slide& original : input) {
        QList<QRegularExpressionMatch> matches;
        QRegularExpressionMatchIterator it = mermaidFence.globalMatch(original.body);
        while (it.hasNext()) {
            matches.append(it.next());
        }
        if (matches.isEmpty()) {
            result.push_back(original);
            continue;
        }

        int cursor = 0;
        for (const QRegularExpressionMatch& m : matches) {
            const QString textChunk = original.body.mid(cursor, m.capturedStart() - cursor).trimmed();
            if (!textChunk.isEmpty()) {
                result.push_back({original.heading, textChunk, QString()});
            }

            const QString mermaidSource = m.captured(1).trimmed();
            try {
                const QString pngPath = renderMermaidToPng(
                    mermaidSource,
                    QStringLiteral("mermaid_%1_diagram_%2").arg(runId).arg(++diagramCounter));
                result.push_back({original.heading, QString(), pngPath});
            } catch (const std::exception& e) {
                logLine(QStringLiteral("WARNING: mermaid render failed, showing as code instead: %1")
                            .arg(QString::fromUtf8(e.what())));
                result.push_back(
                    {original.heading, QStringLiteral("```\n%1\n```").arg(mermaidSource), QString()});
            }
            cursor = m.capturedEnd();
        }
        const QString tailText = original.body.mid(cursor).trimmed();
        if (!tailText.isEmpty()) {
            result.push_back({original.heading, tailText, QString()});
        }
    }
    if (result.empty()) {
        result.push_back({QString(), QString(), QString()});
    }
    return result;
}

// Breaks `text` into pieces no longer than maxChars, preferring to cut on
// sentence boundaries (。！？.!?) rather than mid-sentence. Falls back to
// returning the whole text as one piece if it has no sentence punctuation at
// all (better than cutting a run-on string at an arbitrary character).
QStringList splitBySentence(const QString& text, int maxChars) {
    static const QRegularExpression sentenceEnd(QStringLiteral("(?<=[。！？.!?])\\s*"));
    const QStringList sentences = text.split(sentenceEnd, Qt::SkipEmptyParts);

    QStringList pieces;
    QString current;
    for (const QString& sentence : sentences) {
        if (!current.isEmpty() && current.size() + sentence.size() > maxChars) {
            pieces << current;
            current.clear();
        }
        current += sentence;
    }
    if (!current.isEmpty()) {
        pieces << current;
    }
    if (pieces.isEmpty()) {
        pieces << text;
    }
    return pieces;
}

// Many real Cloud RAG answers are plain prose with no "## heading" structure
// at all, which made splitIntoSlides fall back to a single slide holding the
// entire answer -- effectively the old scroll-through-everything behavior
// the user asked to move away from. This re-chunks any slide whose body
// runs long into several same-headed slides (heading suffixed "（続き）" on
// the 2nd+ part) on paragraph, then sentence, boundaries, so the digest/
// slide pacing applies regardless of whether the source markdown has
// headings. Diagram slides pass through untouched.
std::vector<Slide> splitLongTextSlides(const std::vector<Slide>& input, int maxCharsPerSlide) {
    // A code fence has no blank-line "paragraphs" for this function's
    // paragraph/sentence splitter to align with, so treating a code-bearing
    // slide as splittable would very likely tear a ```fence``` in half
    // across two slides -- corrupting it for enrichSlidesForDisplay's later
    // code-block extraction (see main()). Exempt any slide containing a
    // code fence from length-based splitting; its code panel has its own
    // scroll for overflow (CloudRagScene.qml).
    static const QRegularExpression anyCodeFence(QStringLiteral("```[a-zA-Z0-9]*\\n[\\s\\S]*?```"));

    std::vector<Slide> result;
    for (const Slide& s : input) {
        if (!s.diagramImagePath.isEmpty() || s.body.size() <= maxCharsPerSlide ||
            anyCodeFence.match(s.body).hasMatch()) {
            result.push_back(s);
            continue;
        }

        const QStringList paragraphs =
            s.body.split(QRegularExpression(QStringLiteral("\\n{2,}")), Qt::SkipEmptyParts);

        QStringList chunks;
        QString current;
        for (const QString& paragraph : paragraphs) {
            const QStringList pieces = paragraph.size() > maxCharsPerSlide
                ? splitBySentence(paragraph, maxCharsPerSlide)
                : QStringList{paragraph};
            for (const QString& piece : pieces) {
                if (!current.isEmpty() && current.size() + piece.size() + 2 > maxCharsPerSlide) {
                    chunks << current;
                    current.clear();
                }
                if (!current.isEmpty()) {
                    current += QStringLiteral("\n\n");
                }
                current += piece;
            }
        }
        if (!current.isEmpty()) {
            chunks << current;
        }

        for (int i = 0; i < chunks.size(); ++i) {
            const QString heading = (i == 0) ? s.heading : s.heading + QStringLiteral("（続き）");
            result.push_back({heading, chunks.at(i), QString()});
        }
    }
    if (result.empty()) {
        result.push_back({QString(), QString(), QString()});
    }
    return result;
}

// Fills in each slide's display fields (bullets always; exactly one of
// diagramImagePath/codeBlock when possible) for the split-screen chapter
// layout: left panel = heading + 2 bullets, right panel = a big visual
// (diagram / code-editor-style block / abstract gradient fallback).
//
// A slide from expandDiagramSlides already has diagramImagePath set and is
// left alone. Otherwise: if the slide's own body contains a code fence, that
// becomes the right-panel visual (code-editor mockup). Plain-text slides
// with neither get a dedicated per-slide diagram request -- best-effort,
// since real Cloud RAG answers rarely think to illustrate every section on
// their own; QML falls back to an abstract gradient if this also comes up
// empty (--mock, or the follow-up failing/being skipped).
// Returns a rough estimated-token count (see estimateTokens()) accumulated
// from any per-slide diagram queries this call actually made, so main()
// can fold it into the video's total consumption figure.
int enrichSlidesForDisplay(std::vector<Slide>& slides, const QString& dbKey,
                            const QString& runId, bool useMock) {
    static const QRegularExpression codeFence(
        QStringLiteral("```([a-zA-Z0-9]*)\\n([\\s\\S]*?)```"));
    static const QRegularExpression mermaidCheck(QStringLiteral("```mermaid\\n([\\s\\S]*?)```"));
    int perSlideDiagramCounter = 0;
    int estimatedTokens = 0;

    for (size_t i = 0; i < slides.size(); ++i) {
        Slide& s = slides[i];
        const QStringList bullets = extractBullets(s.body);
        s.bullet1 = bullets.value(0);
        s.bullet2 = bullets.value(1);

        if (!s.diagramImagePath.isEmpty()) {
            continue; // already a diagram slide from expandDiagramSlides
        }

        const QRegularExpressionMatch codeMatch = codeFence.match(s.body);
        if (codeMatch.hasMatch()) {
            s.codeBlock = codeMatch.captured(2).trimmed();
            continue;
        }

        if (useMock) {
            continue; // no live API calls under --mock/--mock-plain
        }
        auto client = CloudRagClient::fromEnvironment();
        if (!client) {
            continue;
        }
        try {
            const QString prompt = QStringLiteral(
                "次の内容を視覚的に表現するための簡潔なMermaid図(flowchartまたはmindmap)を"
                "1つ作成してください。出力は```mermaidブロックのみとし、前置きは不要です。"
                "出典番号([1]等)はノード名に含めないでください。\n\n"
                "【見出し】%1\n【内容】%2")
                    .arg(stripCitationMarkers(s.heading), stripCitationMarkers(s.body));
            const CloudRagResponse diagResp = client->query(prompt, dbKey);
            estimatedTokens += estimateTokens(prompt) + estimateTokens(diagResp.answer);
            const QRegularExpressionMatch m = mermaidCheck.match(diagResp.answer);
            if (m.hasMatch()) {
                s.diagramImagePath = renderMermaidToPng(
                    m.captured(1).trimmed(),
                    QStringLiteral("mermaid_%1_slide_%2").arg(runId).arg(++perSlideDiagramCounter));
                logLine(QStringLiteral("Per-slide diagram generated for slide %1").arg(static_cast<int>(i)));
            }
        } catch (const std::exception& e) {
            logLine(QStringLiteral("WARNING: per-slide diagram request failed for slide %1: %2")
                        .arg(static_cast<int>(i))
                        .arg(QString::fromUtf8(e.what())));
        }
    }
    return estimatedTokens;
}

// Frame index -> slide boundary lookup table. Each slide's on-screen window
// is proportional to its content length (a longer section gets more time),
// with a floor so short slides don't flash by in a couple of frames.
std::vector<int> computeSlideStartFrames(const std::vector<Slide>& slides, int totalFrames) {
    constexpr double kMinWeight = 60.0;
    std::vector<double> weights;
    double totalWeight = 0.0;
    for (const Slide& s : slides) {
        const double w = std::max(kMinWeight, static_cast<double>(s.heading.size() + s.body.size()));
        weights.push_back(w);
        totalWeight += w;
    }

    std::vector<int> startFrames;
    double cumulative = 0.0;
    for (double w : weights) {
        startFrames.push_back(static_cast<int>(std::round(cumulative / totalWeight * totalFrames)));
        cumulative += w;
    }
    startFrames.push_back(totalFrames); // sentinel end boundary
    return startFrames;
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

// Houdini-tutorial ingestion mode (docs/technical-reference.md §15): rather
// than querying Cloud RAG fresh, this loads a tutorial already generated
// and saved by DevelopmentRAGEnvironment's tutorial_agent.py (see
// TutorialAgent._assemble_markdown), which is itself already grounded in a
// houdini21 RAG search -- re-querying would be redundant. The frontmatter
// block is metadata (title/status/tags/etc.), not narration-worthy prose,
// so it's stripped before the body is handed to the existing slide-split
// pipeline unchanged.
struct HoudiniTutorial {
    QString title;
    QString body; // frontmatter stripped
};

HoudiniTutorial loadHoudiniTutorialMarkdown(const QString& mdPath) {
    QFile file(mdPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        throw std::runtime_error("Cannot open Houdini tutorial markdown: " + mdPath.toStdString());
    }
    QString content = QString::fromUtf8(file.readAll());

    HoudiniTutorial result;
    static const QRegularExpression frontmatterRe(
        QStringLiteral("^---\\r?\\n([\\s\\S]*?)\\r?\\n---\\r?\\n"));
    const QRegularExpressionMatch fm = frontmatterRe.match(content);
    if (fm.hasMatch()) {
        static const QRegularExpression titleRe(QStringLiteral("(?m)^title:\\s*(.+)$"));
        const QRegularExpressionMatch titleMatch = titleRe.match(fm.captured(1));
        if (titleMatch.hasMatch()) {
            result.title = titleMatch.captured(1).trimmed();
        }
        content.remove(0, fm.capturedLength());
    }
    result.body = content.trimmed();
    if (result.title.isEmpty()) {
        result.title = QStringLiteral("Houdini チュートリアル");
    }
    return result;
}

// Reads a NodeGraphAsset .json (houdini_tools.py::export_node_graph) and
// summarizes it into one short line of node labels, so the diagram/code
// caption follow-up query (see main()) has real node names to ground its
// explanation in rather than only the markdown prose. Best-effort: a
// missing/unreadable/malformed json degrades to no summary, not an error,
// since the video can still be generated from the markdown alone.
//
// Only TOP-LEVEL nodes are included (id has exactly one "/", e.g.
// "geo1/terrain_grid") -- houdini_tools.py::export_node_graph walks the
// *entire* sandbox recursively, so a VOP-heavy setup's export can include
// hundreds of deeply-nested internal subnetwork/parameter/bind nodes (e.g.
// "geo1/terrain_mountain/attribvop1/unifiednoise_static1") that are
// implementation plumbing, not the user-facing steps a viewer cares about.
QString summarizeHoudiniNodeGraph(const QString& jsonPath) {
    if (jsonPath.isEmpty()) {
        return QString();
    }
    QFile file(jsonPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) {
        return QString();
    }
    const QJsonArray nodes = doc.object().value(QStringLiteral("nodes")).toArray();
    QStringList labels;
    for (const QJsonValue& v : nodes) {
        const QJsonObject obj = v.toObject();
        const QString id = obj.value(QStringLiteral("id")).toString();
        if (id.count(QLatin1Char('/')) != 1) {
            continue; // not a top-level node
        }
        const QString label = obj.value(QStringLiteral("label")).toString();
        if (!label.isEmpty()) {
            labels << label;
        }
    }
    if (labels.isEmpty()) {
        return QString();
    }
    return QStringLiteral("生成されたノード(%1個): %2")
        .arg(labels.size())
        .arg(labels.join(QStringLiteral("、")));
}

// tutorial_agent.py's "## コード・ノード構成" markdown section is a raw dump
// of EVERY node the agent created, including deeply-nested internal
// subnetwork plumbing -- for a VOP-heavy setup this can embed entire VEX
// code snippets as unfenced text spanning hundreds/thousands of lines
// (confirmed against a real saved tutorial: this one section alone was
// ~1560 of the file's 1635 lines). That's implementation detail meant for
// the paired NodeGraphAsset .json, not narration-safe prose -- feeding it
// through splitLongTextSlides/stripMarkdownForNarration unchanged would
// balloon the video into an unusable wall of mis-chunked code fragments
// read aloud as if it were Japanese prose. Replace the section's body with
// the short top-level summary from summarizeHoudiniNodeGraph() instead.
QString replaceNodeConfigSection(QString markdown, const QString& topLevelNodeSummary) {
    static const QRegularExpression sectionRe(
        QStringLiteral("(?m)^## コード・ノード構成\\s*?\\n([\\s\\S]*?)(?=\\n## |\\z)"));
    const QRegularExpressionMatch m = sectionRe.match(markdown);
    if (!m.hasMatch()) {
        return markdown;
    }
    const QString replacement = topLevelNodeSummary.isEmpty()
        ? QStringLiteral("## コード・ノード構成\n\n(ノード構成の詳細は生成されたNodeGraphAssetを参照してください。)\n")
        : QStringLiteral("## コード・ノード構成\n\n%1\n").arg(topLevelNodeSummary);
    markdown.replace(m.capturedStart(), m.capturedLength(), replacement);
    return markdown;
}

// Assigns real Houdini screenshots (captured by the Houdini-side panel at
// tutorial-save time -- see screen_capture.py in DevelopmentRAGEnvironment)
// to slides whose heading matches the tutorial markdown's own section
// naming convention (概要/手順 -> what the viewport shows; コード・ノード構成
// -> what the network editor shows). Must run BEFORE enrichSlidesForDisplay,
// since that function skips any slide that already has a diagramImagePath --
// a real screenshot should always win over a synthetic per-slide Mermaid
// diagram request for the same content.
void assignHoudiniScreenImages(std::vector<Slide>& slides, const QString& viewportPng,
                                const QString& networkPng) {
    const bool hasNetwork = !networkPng.isEmpty() && QFile::exists(networkPng);
    const bool hasViewport = !viewportPng.isEmpty() && QFile::exists(viewportPng);
    if (!hasNetwork && !hasViewport) {
        return;
    }
    for (Slide& s : slides) {
        if (!s.diagramImagePath.isEmpty()) {
            continue; // already a diagram slide from expandDiagramSlides
        }
        if (hasNetwork && (s.heading.contains(QStringLiteral("ノード")) ||
                            s.heading.contains(QStringLiteral("コード")))) {
            s.diagramImagePath = networkPng;
        } else if (hasViewport && (s.heading.contains(QStringLiteral("概要")) ||
                                    s.heading.contains(QStringLiteral("手順")))) {
            s.diagramImagePath = viewportPng;
        }
    }
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

    // Houdini-tutorial ingestion mode (docs/technical-reference.md §15):
    // launched by DevelopmentRAGEnvironment's tutorial_view.py right after
    // it saves a generated tutorial's .md/.json pair, so this video is
    // built from that tutorial's own content instead of a fresh Cloud RAG
    // query. --houdini-viewport/--houdini-network are optional; either or
    // both may be omitted if the Houdini-side capture failed for that run.
    const QString houdiniMdPath = takeFlagValue(args, QStringLiteral("--houdini-md"));
    const QString houdiniJsonPath = takeFlagValue(args, QStringLiteral("--houdini-json"));
    const QString houdiniViewportPng = takeFlagValue(args, QStringLiteral("--houdini-viewport"));
    const QString houdiniNetworkPng = takeFlagValue(args, QStringLiteral("--houdini-network"));
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
            response.answer = replaceNodeConfigSection(tutorial.body, houdiniNodeSummary);
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
        auto client = CloudRagClient::fromEnvironment();
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
        auto captionClient = CloudRagClient::fromEnvironment();
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

    // Narration (best-effort): a missing/broken TTS voice degrades to a
    // silent video rather than failing the whole pipeline.
    QElapsedTimer narrateTimer;
    narrateTimer.start();
    const QString narrationText =
        topic + QStringLiteral("。") + stripMarkdownForNarration(response.answer, codeCaptions);
    const QString wavPath = QStringLiteral("phase2_cloudrag_%1_narration.wav").arg(runId);
    QString audioPathForEncoder;
    double narrationDurationSeconds = 0.0;
    try {
        logLine(QStringLiteral("Synthesizing narration (%1 chars)...").arg(narrationText.size()));
        const NarrationResult narration = NarrationEngine::synthesize(narrationText, wavPath);
        audioPathForEncoder = narration.wavPath;
        narrationDurationSeconds = narration.durationSeconds;
        logLine(QStringLiteral("Narration synthesized: %1s").arg(narrationDurationSeconds, 0, 'f', 1));
    } catch (const std::exception& e) {
        logLine(QStringLiteral("WARNING: narration synthesis failed, continuing without audio: %1")
                    .arg(QString::fromUtf8(e.what())));
    }
    const double narrateSec = narrateTimer.elapsed() / 1000.0;

    const double durationSeconds =
        std::max(kMinDurationSeconds, narrationDurationSeconds + kTailPaddingSeconds);
    const int frameCount = static_cast<int>(std::round(durationSeconds * kFps));
    logLine(QStringLiteral("Video duration: %1s (%2 frames)").arg(durationSeconds, 0, 'f', 1).arg(frameCount));

    QElapsedTimer composeTimer;
    composeTimer.start();
    std::vector<Slide> slides = splitLongTextSlides(
        expandDiagramSlides(splitIntoSlides(topic, response.answer), runId), kMaxCharsPerSlide);
    if (useHoudiniTutorial) {
        assignHoudiniScreenImages(slides, houdiniViewportPng, houdiniNetworkPng);
    }
    estimatedTokens += enrichSlidesForDisplay(slides, dbKey, runId, useMock);
    logLine(QStringLiteral("Estimated tokens consumed (rough, character-based): %1").arg(estimatedTokens));
    const std::vector<int> slideStartFrames = computeSlideStartFrames(slides, frameCount);
    const double composeSec = composeTimer.elapsed() / 1000.0;
    logLine(QStringLiteral("Split into %1 slides").arg(slides.size()));

    QQuickRenderControl renderControl;
    QQuickWindow quickWindow(&renderControl);

    QQmlEngine qmlEngine;
    QQmlComponent component(&qmlEngine,
                             QUrl::fromLocalFile(QStringLiteral(CLOUD_RAG_SCENE_QML_PATH)));
    if (component.status() != QQmlComponent::Ready) {
        logLine(QStringLiteral("ERROR: Failed to load QML scene: %1").arg(component.errorString()));
        return 1;
    }

    std::unique_ptr<QObject> rootObject(component.create());
    auto* rootItem = qobject_cast<QQuickItem*>(rootObject.get());
    if (!rootItem) {
        logLine("ERROR: Root QML object is not a QQuickItem");
        return 1;
    }

    // Static (once-per-video) properties for the split-screen chapter layout
    // (ref: KISARAGI-style design-process reel -- brand block + chapter
    // counter + segmented footer timeline + technical metadata line).
    rootItem->setProperty("topic", topic);
    rootItem->setProperty("brandLabel", dbKey.toUpper());
    rootItem->setProperty("slideCount", static_cast<int>(slides.size()));
    rootItem->setProperty(
        "metadataLine",
        QStringLiteral("%1 SEC / %2 FPS / %3 × %4 / BT.709")
            .arg(durationSeconds, 0, 'f', 1)
            .arg(kFps)
            .arg(kFrameWidth)
            .arg(kFrameHeight));

    // Internal slide boundaries (excluding the implicit 0.0/1.0 ends) as
    // fractions of the whole video, for the footer's segmented timeline tick
    // marks -- slides are weighted by content length (computeSlideStartFrames),
    // so these are not evenly spaced.
    QVariantList slideBoundaries;
    for (size_t i = 1; i < slideStartFrames.size() - 1; ++i) {
        slideBoundaries << static_cast<double>(slideStartFrames[i]) / frameCount;
    }
    rootItem->setProperty("slideBoundaries", slideBoundaries);

    rootItem->setParentItem(quickWindow.contentItem());
    quickWindow.contentItem()->setSize(QSizeF(kFrameWidth, kFrameHeight));
    quickWindow.setGeometry(0, 0, kFrameWidth, kFrameHeight);

    if (!renderControl.initialize()) {
        logLine("ERROR: QQuickRenderControl::initialize() failed");
        return 1;
    }

    QRhi* rhi = renderControl.rhi();
    if (!rhi) {
        logLine("ERROR: No QRhi available after initialize()");
        return 1;
    }

    const QSize pixelSize(kFrameWidth, kFrameHeight);

    std::unique_ptr<QRhiTexture> texture(rhi->newTexture(
        QRhiTexture::RGBA8, pixelSize, 1,
        QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
    if (!texture->create()) {
        logLine("ERROR: Failed to create offscreen render texture");
        return 1;
    }

    std::unique_ptr<QRhiRenderBuffer> depthStencil(
        rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, pixelSize, 1));
    if (!depthStencil->create()) {
        logLine("ERROR: Failed to create depth/stencil buffer");
        return 1;
    }

    QRhiTextureRenderTargetDescription rtDesc(QRhiColorAttachment(texture.get()));
    rtDesc.setDepthStencilBuffer(depthStencil.get());
    std::unique_ptr<QRhiTextureRenderTarget> renderTarget(rhi->newTextureRenderTarget(rtDesc));
    std::unique_ptr<QRhiRenderPassDescriptor> renderPassDesc(
        renderTarget->newCompatibleRenderPassDescriptor());
    renderTarget->setRenderPassDescriptor(renderPassDesc.get());
    if (!renderTarget->create()) {
        logLine("ERROR: Failed to create QRhiTextureRenderTarget");
        return 1;
    }

    quickWindow.setRenderTarget(QQuickRenderTarget::fromRhiRenderTarget(renderTarget.get()));

    const QString outputMp4Path = QStringLiteral("phase2_cloudrag_%1.mp4").arg(runId);
    VideoEncoder encoder(outputMp4Path.toStdString(), kFrameWidth, kFrameHeight, kFps,
                          audioPathForEncoder.toStdString());

    QElapsedTimer renderTimer;
    renderTimer.start();
    QImage thumbnailImage; // captured partway through for the web dashboard gallery
    size_t currentSlide = 0;
    for (int i = 0; i < frameCount; ++i) {
        rootItem->setProperty("progress", static_cast<double>(i) / (frameCount - 1));

        while (currentSlide + 2 < slideStartFrames.size() && i >= slideStartFrames[currentSlide + 1]) {
            ++currentSlide;
        }
        const int slideStart = slideStartFrames[currentSlide];
        const int slideEnd = slideStartFrames[currentSlide + 1];
        const double slideProgress = slideEnd > slideStart
            ? static_cast<double>(i - slideStart) / (slideEnd - slideStart)
            : 0.0;

        const Slide& active = slides[currentSlide];
        rootItem->setProperty("slideIndex", static_cast<int>(currentSlide));
        rootItem->setProperty("slideHeading", active.heading);
        rootItem->setProperty("slideBullet1", active.bullet1);
        rootItem->setProperty("slideBullet2", active.bullet2);
        rootItem->setProperty("slideCodeBlock", active.codeBlock);
        rootItem->setProperty("slideProgress", slideProgress);
        rootItem->setProperty("slideDiagramSource",
                               active.diagramImagePath.isEmpty()
                                   ? QString()
                                   : QUrl::fromLocalFile(active.diagramImagePath).toString());

        renderControl.polishItems();
        renderControl.beginFrame();
        renderControl.sync();
        renderControl.render();

        QRhiReadbackResult readResult;
        QRhiResourceUpdateBatch* readbackBatch = rhi->nextResourceUpdateBatch();
        readbackBatch->readBackTexture(texture.get(), &readResult);
        renderControl.commandBuffer()->resourceUpdate(readbackBatch);

        renderControl.endFrame();

        QImage frameImage(reinterpret_cast<const uchar*>(readResult.data.constData()),
                           readResult.pixelSize.width(), readResult.pixelSize.height(),
                           QImage::Format_RGBA8888_Premultiplied);
        if (rhi->isYUpInFramebuffer()) {
            frameImage = frameImage.flipped();
        }
        frameImage = frameImage.convertToFormat(QImage::Format_RGBA8888);

        encoder.pushFrame(frameImage.constBits());

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
    const double renderSec = renderTimer.elapsed() / 1000.0;

    encoder.writeAudioTrack();
    encoder.finish();
    logLine(QStringLiteral("Wrote %1 (%2 frames, %3s)")
                .arg(outputMp4Path)
                .arg(frameCount)
                .arg(durationSeconds, 0, 'f', 1));

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

        ManifestWriter::publish(QStringLiteral(WEB_PUBLIC_DIR), entry, detail, outputMp4Path,
                                 thumbnailImage);
        logLine(QStringLiteral("Published to web dashboard: web/public/videos/%1/").arg(entry.id));
    } catch (const std::exception& e) {
        logLine(QStringLiteral("WARNING: failed to publish to web dashboard: %1")
                    .arg(QString::fromUtf8(e.what())));
    }

    return 0;
}
