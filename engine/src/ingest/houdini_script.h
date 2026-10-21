#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <map>
#include <vector>

#include "ingest/script_composer.h"

// Beginner-friendly narration script for a Houdini tutorial video (2026-10-19).
//
// Before this, a step slide showed the tool's result sentence ("emit のパラメータを設定: Run Over を 0、
// VEXpression を int n = 30000; float rin = ...") and the narration was the tutorial markdown read
// aloud -- an operator's note, not an explanation for a beginner. The Houdini side now writes
// `<slug>_video_script.json` (DevelopmentRAGEnvironment/houdini/python_panels/video_script.py):
// for every step what to do, WHY, what to look at, with terms explained. This module loads it and
// puts it on the slides: the spoken text per slide, a short on-screen caption, and a closing slide.
//
// File format (version 1), all text Japanese:
//   { "version": 1, "source": "claude" | "template",
//     "intro":     { "narration": "...", "points": ["...", "..."] },
//     "steps":     [ { "step": 3, "heading": "Attribute Wrangleを作る", "narration": "...", "points": ["...", "..."] } ],
//     "structure": { "narration": "...", "points": [...] },            // optional
//     "outro":     { "narration": "...", "points": [...] },
//     "sections":  [ { "match": "ハマりポイント", "heading": "つまずきやすいところ", "narration": "...", "points": [...] } ] }
// `sections` rewrites the tutorial's other "## ..." sections (pitfalls, ideas to try) for beginners; `match` is
// the section's heading in the markdown. Without it those sections are read as written (expert wording).
// `step` is the manifest's step number (houdini_tools.py export_step_screenshots).
struct HoudiniScriptStep {
    int step = -1;
    QString heading;      // short title of the step, e.g. "Attribute Wrangleノードを作る"
    QString narration;    // spoken text, beginner level
    QStringList points;   // on-screen text, at most 2 short lines
};

struct HoudiniScriptSection {
    QString match;      // the markdown section's heading, e.g. "ハマりポイント"
    QString heading;    // heading shown in the video (empty = keep the original)
    QString narration;  // spoken text, beginner level (empty = keep the original text)
    QStringList points;
};

struct HoudiniScript {
    bool valid = false;
    QString source;  // "claude" or "template" (how the Houdini side produced it)
    QString introNarration;
    QStringList introPoints;
    QString structureNarration;
    QStringList structurePoints;
    QString outroNarration;
    QStringList outroPoints;
    std::map<int, HoudiniScriptStep> steps;
    std::vector<HoudiniScriptSection> sections;
    // Optional picture of the node graph ("images": {"node_diagram": "<absolute path>"}), drawn by the
    // Houdini side from the saved graph JSON. Used for slides that would otherwise show the empty
    // placeholder panel. Empty when absent or when the file does not exist.
    QString nodeDiagramPath;
    QStringList warnings;  // entries that were dropped (e.g. narration that looks like code)
};

// Narration that is really code or a parameter dump ("float r = rand(i);", "{...}") -- must never be
// read aloud or shown as an explanation.
bool looksLikeCode(const QString& text);

HoudiniScript parseHoudiniScript(const QByteArray& json);
HoudiniScript loadHoudiniScript(const QString& path);  // invalid when missing/unreadable/malformed

// Puts the script on the slides (call after enrichSlidesForDisplay, so the script's on-screen text
// wins over the automatic bullets):
//  * step slides get "手順 N　<heading>", the caption lines and the spoken narration;
//  * "概要" and "コード・ノード構成" get the intro / structure text (their "（続き）" parts are removed,
//    the script text covers them);
//  * the other markdown sections listed in `sections` get the beginner text (their "（続き）" parts are removed);
//  * a "まとめ" slide with the outro is inserted before "参考" (or appended);
//  * "参考" gets a one-line spoken lead-in instead of reading the source titles.
void applyHoudiniScript(std::vector<Slide>& slides, const HoudiniScript& script);

// Shows the finished result (the last captured viewport picture) on the intro ("この動画でやること") and the
// closing ("まとめ") slides, which otherwise have an empty picture area. "This is what we will make" is the
// most useful thing a beginner can see at the start. Slides that already have a picture are left alone.
void assignFinalResultImage(std::vector<Slide>& slides, const std::vector<HoudiniStepScreenshot>& shots);

// Fills every slide that would still show the empty placeholder panel (no step number, picture, code or
// reference list) with a real picture, so no slide is a blurry gradient (2026-10-21):
//  * "how it works" slides (考え方 / 仕組み / ノード / 流れ / 構成 / 学ぶ / つまずき ...) prefer the node graph
//    diagram, then the finished result;
//  * all others (experiments, finishing touches, next steps, ...) prefer the finished result (the last
//    captured viewport picture), then the diagram.
// Files that do not exist are ignored; with neither picture nothing changes.
void assignPlaceholderImages(std::vector<Slide>& slides, const std::vector<HoudiniStepScreenshot>& shots,
                             const QString& nodeDiagramPath);

// The text spoken for each slide, in slide order. Slides without a script text fall back to their own
// text with markdown stripped (step slides: first line only, never code).
QStringList slideNarrationBlocks(const std::vector<Slide>& slides);
