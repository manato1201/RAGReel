#include "ingest/script_composer.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcess>
#include <QRegularExpression>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "common/app_utils.h"
#include "ragclient/cloud_rag_client.h"

int estimateTokens(const QString& text) {
    return static_cast<int>(std::round(text.size() / 1.8));
}

QString stripCitationMarkers(QString text) {
    text.remove(QRegularExpression(QStringLiteral("\\[[^\\[\\]]{1,60}\\]")));
    return text;
}

namespace {

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

// Renders one Mermaid diagram source to a PNG via mermaid-cli (mmdc).
// Shelled out to rather than embedded, since mmdc already exists, is
// well-tested, and re-implementing a Mermaid layout engine in C++ would be
// a large, unnecessary undertaking. Throws std::runtime_error if mmdc is
// missing or fails.
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
        QStringLiteral("-c"), appRelativePath(QStringLiteral("assets/mermaid_theme.json")),
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

// Parses tutorial_agent.py's "## 参考" source-list lines (source_lines in
// _assemble_markdown), e.g.:
//   - [1] ⬜ 未引用 KineFXプロシージャルアニメーションとフルボディダイナミクス（houdini21）
//   - [2] ✅ 引用済み VEX ループ・条件文と関数定義（houdini21）
// into structured {title, db, cited} entries for the dedicated reference-
// list visual (see Slide::referenceItems). Returns an empty list for the
// "（参考ドキュメントなし）" no-sources fallback line, or any body that
// doesn't match this exact format.
QVariantList parseHoudiniReferenceItems(const QString& body) {
    QVariantList items;
    static const QRegularExpression lineRe(QStringLiteral(
        "(?m)^-\\s*\\[\\d+\\]\\s*(✅|⬜)\\S*\\s*(.+?)(?:（([^）]*)）)?\\s*$"));
    QRegularExpressionMatchIterator it = lineRe.globalMatch(body);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        QVariantMap item;
        item[QStringLiteral("title")] = m.captured(2).trimmed();
        item[QStringLiteral("db")] = m.captured(3).trimmed();
        item[QStringLiteral("cited")] = (m.captured(1) == QStringLiteral("✅"));
        items << item;
    }
    return items;
}

}  // namespace

QString stripMarkdownForNarration(QString text, const QStringList& codeCaptions) {
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

std::vector<Slide> splitLongTextSlides(const std::vector<Slide>& input, int maxCharsPerSlide) {
    // A code fence has no blank-line "paragraphs" for this function's
    // paragraph/sentence splitter to align with, so treating a code-bearing
    // slide as splittable would very likely tear a ```fence``` in half
    // across two slides -- corrupting it for enrichSlidesForDisplay's later
    // code-block extraction. Exempt any slide containing a code fence from
    // length-based splitting; its code panel has its own scroll for
    // overflow (CloudRagScene.qml).
    static const QRegularExpression anyCodeFence(QStringLiteral("```[a-zA-Z0-9]*\\n[\\s\\S]*?```"));

    std::vector<Slide> result;
    for (const Slide& s : input) {
        // Same reasoning as the code-fence exemption above: a "参考" slide
        // tagged with referenceItems (see assignHoudiniReferenceItems) is a
        // structured source list, not prose -- splitting it would scatter
        // sources across multiple "参考（続き）" slides that nothing re-tags.
        if (!s.diagramImagePath.isEmpty() || !s.referenceItems.isEmpty() ||
            s.body.size() <= maxCharsPerSlide || anyCodeFence.match(s.body).hasMatch()) {
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
            Slide chunkSlide;
            chunkSlide.heading = heading;
            chunkSlide.body = chunks.at(i);
            // Preserve houdiniStepNumber across continuation slides (mostly
            // moot in practice: buildHoudiniStepSlidesFromScreenshots's
            // slides always have diagramImagePath set already, which makes
            // this function's early-return skip them entirely -- kept for
            // robustness in case that ever changes).
            chunkSlide.houdiniStepNumber = s.houdiniStepNumber;
            result.push_back(chunkSlide);
        }
    }
    if (result.empty()) {
        result.push_back({QString(), QString(), QString()});
    }
    return result;
}

int enrichSlidesForDisplay(std::vector<Slide>& slides, const QString& dbKey,
                            const QString& runId, bool useMock) {
    static const QRegularExpression codeFence(
        QStringLiteral("```([a-zA-Z0-9]*)\\n([\\s\\S]*?)```"));
    static const QRegularExpression mermaidCheck(QStringLiteral("```mermaid\\n([\\s\\S]*?)```"));
    int perSlideDiagramCounter = 0;
    int estimatedTokens = 0;

    for (size_t i = 0; i < slides.size(); ++i) {
        Slide& s = slides[i];
        if (s.houdiniStepNumber >= 0) {
            // buildHoudiniStepSlidesFromScreenshots already put exactly the
            // one fact worth showing in s.body (a tool-call result string
            // like "stairs_geo/set_stair_positions.snippet = float rise =
            // 0.2; ..."). extractBullets()'s sentence-splitter chops on any
            // "." followed by whitespace, which false-positives on the
            // decimal points and dotted node paths these strings are full
            // of (confirmed on real output: "0.2" got split into two
            // separate bullets at the decimal point) -- skip it here and
            // show the fact as-is.
            s.bullet1 = s.body.size() > 120 ? s.body.left(120) + QStringLiteral("…") : s.body;
            s.bullet2.clear();
        } else {
            const QStringList bullets = extractBullets(s.body);
            s.bullet1 = bullets.value(0);
            s.bullet2 = bullets.value(1);
        }

        if (!s.diagramImagePath.isEmpty()) {
            continue; // already a diagram slide from expandDiagramSlides
        }
        if (!s.referenceItems.isEmpty()) {
            continue; // "参考" slide already has its own source-card visual
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

QString humanizeExtractionNote(QString markdown) {
    static const QRegularExpression noteRe(QStringLiteral(
        "利用率:\\s*(\\d+)%\\s*（引用\\s*(\\d+)/(\\d+)\\s*件）"));
    const QRegularExpressionMatch m = noteRe.match(markdown);
    if (!m.hasMatch()) {
        return markdown;
    }
    const QString replacement = QStringLiteral(
        "参考ドキュメントは%1件検索し、そのうち%2件を実際にチュートリアル生成で"
        "活用しました（利用率%3%）。")
            .arg(m.captured(3), m.captured(2), m.captured(1));
    markdown.replace(m.capturedStart(), m.capturedLength(), replacement);
    return markdown;
}

void assignHoudiniReferenceItems(std::vector<Slide>& slides) {
    for (Slide& s : slides) {
        if (s.heading != QStringLiteral("参考")) {
            continue;
        }
        s.referenceItems = parseHoudiniReferenceItems(s.body);
    }
}

std::vector<HoudiniStepScreenshot> loadHoudiniScreenshotManifest(const QString& jsonPath) {
    std::vector<HoudiniStepScreenshot> result;
    if (jsonPath.isEmpty()) {
        return result;
    }
    QFile file(jsonPath);
    if (!file.open(QIODevice::ReadOnly)) {
        return result;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isArray()) {
        return result;
    }
    for (const QJsonValue& v : doc.array()) {
        const QJsonObject obj = v.toObject();
        HoudiniStepScreenshot shot;
        shot.step = obj.value(QStringLiteral("step")).toInt();
        shot.tool = obj.value(QStringLiteral("tool")).toString();
        shot.result = obj.value(QStringLiteral("result")).toString();
        shot.viewportPath = obj.value(QStringLiteral("viewport")).toString();
        shot.networkPath = obj.value(QStringLiteral("network")).toString();
        shot.viewportClipFps = obj.value(QStringLiteral("viewport_clip_fps")).toInt();
        for (const QJsonValue& frame : obj.value(QStringLiteral("viewport_clip_frames")).toArray()) {
            shot.viewportClipFrames << frame.toString();
        }
        result.push_back(shot);
    }
    return result;
}

std::vector<Slide> buildHoudiniStepSlidesFromScreenshots(
        const std::vector<HoudiniStepScreenshot>& shots) {
    std::vector<Slide> result;
    for (const HoudiniStepScreenshot& shot : shots) {
        const bool hasViewport = !shot.viewportPath.isEmpty() && QFile::exists(shot.viewportPath);
        const bool hasNetwork = !shot.networkPath.isEmpty() && QFile::exists(shot.networkPath);
        if ((!hasNetwork && !hasViewport) || shot.result.trimmed().isEmpty()) {
            continue; // no usable image or nothing to say about it
        }
        Slide s;
        s.heading = QStringLiteral("手順 %1").arg(shot.step);
        s.body = shot.result;
        s.houdiniStepNumber = shot.step;
        const bool preferViewport = (shot.tool == QStringLiteral("cook_node"));
        if (preferViewport) {
            s.diagramImagePath = hasViewport ? shot.viewportPath : shot.networkPath;
        } else {
            s.diagramImagePath = hasNetwork ? shot.networkPath : shot.viewportPath;
        }
        // cook_node steps with a usable clip play that back instead of the
        // still viewport frame -- sim-heavy nodes (fire, pyro, clouds) show
        // their actual time evolution rather than one frozen frame. Verify
        // every listed frame file actually exists before trusting the clip;
        // diagramImagePath (set above) still stays populated as the
        // fallback for anything that only looks at a single image (web
        // thumbnail extraction, assignHoudiniFinalGraphScreenshot's "already
        // has an image" check, etc).
        if (preferViewport && shot.viewportClipFps > 0 && !shot.viewportClipFrames.isEmpty()) {
            bool allFramesExist = true;
            for (const QString& framePath : shot.viewportClipFrames) {
                if (!QFile::exists(framePath)) {
                    allFramesExist = false;
                    break;
                }
            }
            if (allFramesExist) {
                s.clipFramePaths = shot.viewportClipFrames;
                s.clipFps = shot.viewportClipFps;
                s.diagramImagePath = shot.viewportClipFrames.first();
            }
        }
        result.push_back(s);
    }
    return result;
}

void assignHoudiniFinalGraphScreenshot(std::vector<Slide>& slides,
                                        const std::vector<HoudiniStepScreenshot>& shots) {
    for (Slide& s : slides) {
        if (!s.diagramImagePath.isEmpty()) {
            continue; // already a diagram slide, or one of the per-step slides above
        }
        if (!s.heading.contains(QStringLiteral("ノード")) &&
            !s.heading.contains(QStringLiteral("コード"))) {
            continue;
        }
        for (auto rit = shots.rbegin(); rit != shots.rend(); ++rit) {
            if (!rit->networkPath.isEmpty() && QFile::exists(rit->networkPath)) {
                s.diagramImagePath = rit->networkPath;
                break;
            }
            if (!rit->viewportPath.isEmpty() && QFile::exists(rit->viewportPath)) {
                s.diagramImagePath = rit->viewportPath;
                break;
            }
        }
    }
}

ShotList toShotList(const std::vector<Slide>& slides) {
    ShotList shots;
    for (const Slide& s : slides) {
        if (!s.referenceItems.isEmpty()) {
            shots.referenceCards.push_back(ReferenceCardsShot{s.referenceItems});
            shots.order.push_back(ShotKind::ReferenceCards);
        } else if (s.houdiniStepNumber >= 0 && !s.clipFramePaths.isEmpty()) {
            shots.houdiniStepClips.push_back(
                HoudiniStepClipShot{s.houdiniStepNumber, s.clipFramePaths, s.clipFps});
            shots.order.push_back(ShotKind::HoudiniStepClip);
        } else if (s.houdiniStepNumber >= 0) {
            shots.houdiniStepStills.push_back(
                HoudiniStepStillShot{s.houdiniStepNumber, s.diagramImagePath, s.body});
            shots.order.push_back(ShotKind::HoudiniStepStill);
        } else if (!s.diagramImagePath.isEmpty()) {
            shots.diagramImages.push_back(DiagramImageShot{s.heading, s.diagramImagePath});
            shots.order.push_back(ShotKind::DiagramImage);
        } else if (!s.codeBlock.isEmpty()) {
            shots.codeBlocks.push_back(CodeBlockShot{s.heading, s.codeBlock});
            shots.order.push_back(ShotKind::CodeBlock);
        } else {
            shots.textDigests.push_back(TextDigestShot{s.heading, s.bullet1, s.bullet2});
            shots.order.push_back(ShotKind::TextDigest);
        }
    }
    return shots;
}
