#include "ingest/houdini_script.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <algorithm>

namespace {

constexpr int kMaxPoints = 2;

QStringList readPoints(const QJsonValue& v) {
    QStringList out;
    for (const QJsonValue& p : v.toArray()) {
        const QString s = p.toString().trimmed();
        if (!s.isEmpty() && !looksLikeCode(s)) out << s;
        if (out.size() >= kMaxPoints) break;
    }
    return out;
}

// Returns the narration, or an empty string (and a warning) if it is missing or code-like.
QString readNarration(const QJsonObject& o, const QString& what, QStringList* warnings) {
    const QString text = o.value(QStringLiteral("narration")).toString().trimmed();
    if (text.isEmpty()) return {};
    if (looksLikeCode(text)) {
        if (warnings) *warnings << QStringLiteral("%1: ナレーションがコードのように見えるため使いません").arg(what);
        return {};
    }
    return text;
}

bool startsWithHeading(const QString& heading, const QString& base) {
    return heading == base || heading.startsWith(base + QStringLiteral("（続き"));
}

}  // namespace

bool looksLikeCode(const QString& text) {
    if (text.contains(QStringLiteral("```"))) return true;
    static const QRegularExpression codePatterns(
        QStringLiteral(R"((@[A-Za-z_][A-Za-z0-9_]*\s*[-+*/]?=)|(\b(float|int|vector|string|matrix)\s+[A-Za-z_]\w*\s*=)|(\bfor\s*\()|(\b[a-z_]\w*\([^)]{0,60}\)\s*;)|(\}\s*$))"));
    if (codePatterns.match(text).hasMatch()) return true;
    int symbols = 0;
    for (const QChar c : text) {
        if (c == QLatin1Char('{') || c == QLatin1Char('}') || c == QLatin1Char(';') || c == QLatin1Char('=') ||
            c == QLatin1Char('[') || c == QLatin1Char(']') || c == QLatin1Char('<') || c == QLatin1Char('>')) {
            ++symbols;
        }
    }
    return symbols >= 4 && symbols * 12 >= text.size();
}

HoudiniScript parseHoudiniScript(const QByteArray& json) {
    HoudiniScript s;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) return s;
    const QJsonObject root = doc.object();
    s.source = root.value(QStringLiteral("source")).toString();

    const QJsonObject intro = root.value(QStringLiteral("intro")).toObject();
    s.introNarration = readNarration(intro, QStringLiteral("導入"), &s.warnings);
    s.introPoints = readPoints(intro.value(QStringLiteral("points")));

    const QJsonObject structure = root.value(QStringLiteral("structure")).toObject();
    s.structureNarration = readNarration(structure, QStringLiteral("全体の構成"), &s.warnings);
    s.structurePoints = readPoints(structure.value(QStringLiteral("points")));

    const QJsonObject outro = root.value(QStringLiteral("outro")).toObject();
    s.outroNarration = readNarration(outro, QStringLiteral("まとめ"), &s.warnings);
    s.outroPoints = readPoints(outro.value(QStringLiteral("points")));

    for (const QJsonValue& v : root.value(QStringLiteral("sections")).toArray()) {
        const QJsonObject o = v.toObject();
        HoudiniScriptSection sec;
        sec.match = o.value(QStringLiteral("match")).toString().trimmed();
        if (sec.match.isEmpty()) continue;
        sec.heading = o.value(QStringLiteral("heading")).toString().trimmed();
        if (looksLikeCode(sec.heading)) sec.heading.clear();
        sec.narration = readNarration(o, QStringLiteral("節「%1」").arg(sec.match), &s.warnings);
        sec.points = readPoints(o.value(QStringLiteral("points")));
        if (!sec.narration.isEmpty()) s.sections.push_back(sec);
    }

    for (const QJsonValue& v : root.value(QStringLiteral("steps")).toArray()) {
        const QJsonObject o = v.toObject();
        if (!o.contains(QStringLiteral("step"))) continue;
        HoudiniScriptStep step;
        step.step = o.value(QStringLiteral("step")).toInt(-1);
        if (step.step < 0) continue;
        step.heading = o.value(QStringLiteral("heading")).toString().trimmed();
        if (looksLikeCode(step.heading)) step.heading.clear();
        step.narration = readNarration(o, QStringLiteral("手順%1").arg(step.step), &s.warnings);
        step.points = readPoints(o.value(QStringLiteral("points")));
        s.steps[step.step] = step;
    }
    // Usable when at least the steps or the intro carry real text.
    s.valid = !s.introNarration.isEmpty() ||
              std::any_of(s.steps.begin(), s.steps.end(), [](const auto& kv) { return !kv.second.narration.isEmpty(); });
    return s;
}

HoudiniScript loadHoudiniScript(const QString& path) {
    if (path.isEmpty()) return {};
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return parseHoudiniScript(f.readAll());
}

void applyHoudiniScript(std::vector<Slide>& slides, const HoudiniScript& script) {
    if (!script.valid) return;

    // The script text covers the "（続き）" parts that splitLongTextSlides cut out of these sections.
    slides.erase(std::remove_if(slides.begin(), slides.end(),
                                [](const Slide& s) {
                                    return s.houdiniStepNumber < 0 &&
                                           (s.heading == QStringLiteral("概要（続き）") ||
                                            s.heading == QStringLiteral("コード・ノード構成（続き）"));
                                }),
                 slides.end());

    // The beginner text covers the "（続き）" parts of the other sections as well.
    slides.erase(std::remove_if(slides.begin(), slides.end(),
                                [&](const Slide& s) {
                                    if (s.houdiniStepNumber >= 0) return false;
                                    return std::any_of(script.sections.begin(), script.sections.end(),
                                                       [&](const HoudiniScriptSection& sec) {
                                                           return s.heading.startsWith(sec.match + QStringLiteral("（続き"));
                                                       });
                                }),
                 slides.end());

    int stepNo = 0;
    for (Slide& s : slides) {
        if (s.houdiniStepNumber >= 0) {
            ++stepNo;
            const auto it = script.steps.find(s.houdiniStepNumber);
            if (it == script.steps.end()) {
                s.heading = QStringLiteral("手順 %1").arg(stepNo);
                continue;
            }
            const HoudiniScriptStep& st = it->second;
            s.heading = st.heading.isEmpty() ? QStringLiteral("手順 %1").arg(stepNo)
                                             : QStringLiteral("手順 %1　%2").arg(stepNo).arg(st.heading);
            s.narration = st.narration;
            if (!st.points.isEmpty()) {
                s.bullet1 = st.points.value(0);
                s.bullet2 = st.points.value(1);
            }
        } else if (startsWithHeading(s.heading, QStringLiteral("概要"))) {
            s.heading = QStringLiteral("この動画でやること");
            s.narration = script.introNarration;
            if (!script.introPoints.isEmpty()) {
                s.bullet1 = script.introPoints.value(0);
                s.bullet2 = script.introPoints.value(1);
            }
        } else if (startsWithHeading(s.heading, QStringLiteral("コード・ノード構成"))) {
            if (!script.structureNarration.isEmpty()) s.narration = script.structureNarration;
            if (!script.structurePoints.isEmpty()) {
                s.bullet1 = script.structurePoints.value(0);
                s.bullet2 = script.structurePoints.value(1);
            }
        } else if (s.heading == QStringLiteral("参考")) {
            s.narration = QStringLiteral("この動画は、次の資料を参考にして作りました。");
        } else {
            const auto sec = std::find_if(script.sections.begin(), script.sections.end(),
                                          [&](const HoudiniScriptSection& c) { return s.heading == c.match; });
            if (sec != script.sections.end()) {
                if (!sec->heading.isEmpty()) s.heading = sec->heading;
                s.narration = sec->narration;
                if (!sec->points.isEmpty()) {
                    s.bullet1 = sec->points.value(0);
                    s.bullet2 = sec->points.value(1);
                }
            }
        }
    }

    if (!script.outroNarration.isEmpty()) {
        Slide outro;
        outro.heading = QStringLiteral("まとめ");
        outro.narration = script.outroNarration;
        outro.bullet1 = script.outroPoints.value(0);
        outro.bullet2 = script.outroPoints.value(1);
        const auto ref = std::find_if(slides.begin(), slides.end(),
                                      [](const Slide& s) { return s.heading == QStringLiteral("参考"); });
        slides.insert(ref, outro);  // before "参考", or at the end when there is none
    }
}

QStringList slideNarrationBlocks(const std::vector<Slide>& slides) {
    QStringList out;
    for (const Slide& s : slides) {
        if (!s.narration.trimmed().isEmpty()) {
            out << s.narration.trimmed();
        } else if (s.houdiniStepNumber >= 0) {
            // No script text for this step: say the operation sentence's first line, never code.
            const QString first = s.body.section(QLatin1Char('\n'), 0, 0).trimmed();
            out << (looksLikeCode(first) || first.size() > 120 ? QString() : first);
        } else {
            out << stripMarkdownForNarration(s.body);
        }
    }
    return out;
}

void assignFinalResultImage(std::vector<Slide>& slides, const std::vector<HoudiniStepScreenshot>& shots) {
    QString image;
    for (auto it = shots.rbegin(); it != shots.rend(); ++it) {
        if (!it->viewportPath.isEmpty() && QFile::exists(it->viewportPath)) {
            image = it->viewportPath;
            break;
        }
    }
    if (image.isEmpty()) return;
    for (Slide& s : slides) {
        if (s.houdiniStepNumber >= 0 || !s.diagramImagePath.isEmpty() || !s.codeBlock.isEmpty() || !s.referenceItems.isEmpty()) {
            continue;
        }
        if (s.heading == QStringLiteral("この動画でやること") || s.heading == QStringLiteral("まとめ")) {
            s.diagramImagePath = image;
        }
    }
}
