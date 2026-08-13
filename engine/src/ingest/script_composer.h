#pragma once

#include <QString>
#include <QStringList>
#include <QVariantList>

#include <vector>

#include "ragclient/cloud_rag_client.h"

// Markdown/NodeGraphAsset-JSON -> Slide/ShotList conversion, per
// docs/architecture/video-factory-design.md §2's ScriptComposer and
// IMPROVEMENT_PLAN.md Phase 2. Moved out of main_cloudrag.cpp, which
// previously had all of this in its own anonymous namespace.
//
// Scope note (IMPROVEMENT_PLAN.md Phase 0 "設計書との乖離" #6): "Qt/GPU非
// 依存" here is interpreted as "no QtQuick/QML/QRhi/rendering dependency"
// (this file only pulls in QtCore types -- QString/QRegularExpression/
// QJsonDocument -- via its .cpp), not "zero Qt headers at all". Rewriting
// ~700 lines of QString/QRegularExpression-based Japanese-text processing
// to std::string/std::regex to satisfy a literal zero-QtCore reading would
// be a large, regression-prone undertaking (std::regex's Unicode handling
// differs from QRegularExpression's) for a component whose actual stated
// goal is separation from QML/GPU, not from QtCore. Its unit tests
// therefore link Qt6::Core but never Qt6::Quick/GuiPrivate/rhi.
//
// IngestWatcher (design doc §2, Phase 2's other planned component) was NOT
// implemented: it polls localRAG/tutorials/ for unprocessed .md/.json
// pairs, but neither of this engine's two actual entry paths works that
// way -- the Cloud RAG query path has no filesystem input at all, and the
// Houdini-tutorial path is invoked with an explicit --houdini-md file path
// by tutorial_view.py::_on_save (a push, not a poll). Building a poller
// with no caller would be speculative; if a future entry path needs
// directory polling, add IngestWatcher then.

// Digest pacing target: a slide's body should read as one punchy
// screenful, not a scroll-through-everything wall of text (see
// splitLongTextSlides).
constexpr int kMaxCharsPerSlide = 200;

// Lightweight stand-in for the ShotList concept from docs/architecture/
// video-factory-design.md §2/§8 -- splitting the source markdown into one
// slide per "## heading" section gives the digest/title-card pacing (ref:
// YouTube tutorial title-card style -- big headline + short body per
// slide, not one long scrolling wall of text).
struct Slide {
    QString heading;
    QString body;             // markdown text; kept only as source material for
                               // narration/bullet/code extraction, not rendered directly
    QString diagramImagePath; // set if this slide shows a Mermaid diagram
    QString codeBlock;        // set if this slide shows a code block instead (mutually
                               // exclusive with diagramImagePath)
    QString bullet1;
    QString bullet2;
    // Houdini-tutorial mode only, cook_node steps with a successful clip
    // capture: a short sequence of viewport frames (screen_capture.py's
    // capture_viewport_clip) to play back during this slide's screen time
    // instead of a single static image. Empty for every other slide, in
    // which case diagramImagePath (below) is shown as normal. When set,
    // diagramImagePath is still populated (first clip frame) as a fallback
    // for any code path that only looks at a single image.
    QStringList clipFramePaths;
    int clipFps = 0;
    // Houdini-tutorial mode only (see buildHoudiniStepSlidesFromScreenshots):
    // the manifest's tool-call step index this slide was built from, or -1
    // for slides that aren't one of those per-tool-call slides (概要/
    // コード・ノード構成/ハマりポイント/参考, or any non-Houdini video).
    // Informational only -- these slides already have diagramImagePath set
    // at construction time, so nothing re-derives an image from this field.
    int houdiniStepNumber = -1;
    // Houdini-tutorial mode only, "## 参考" slide: one entry per RAG source
    // tutorial_agent.py listed (see parseHoudiniReferenceItems in the
    // .cpp), each a QVariantMap{title, db, cited}. Empty for every other
    // slide, in which case the right panel falls back to
    // diagramImagePath/codeBlock/gradient as normal.
    QVariantList referenceItems;
};

// Houdini-tutorial ingestion mode (docs/technical-reference.md §15): rather
// than querying Cloud RAG fresh, this loads a tutorial already generated
// and saved by DevelopmentRAGEnvironment's tutorial_agent.py, which is
// itself already grounded in a houdini21 RAG search. The frontmatter block
// is metadata (title/status/tags/etc.), not narration-worthy prose, so
// it's stripped before the body is handed to the slide-split pipeline.
struct HoudiniTutorial {
    QString title;
    QString body; // frontmatter stripped
};

// One entry per tool call captured by video_factory_bridge.py's
// HoudiniToolExecutor.export_step_screenshots() (see
// loadHoudiniScreenshotManifest).
struct HoudiniStepScreenshot {
    int step = 0;
    QString tool;
    QString result;       // the tool's own human-readable Japanese result text
    QString viewportPath;
    QString networkPath;
    // cook_node only: a short viewport clip (screen_capture.py's
    // capture_viewport_clip), as an ordered list of frame image paths, plus
    // its native capture fps. Empty/0 when no clip was captured.
    QStringList viewportClipFrames;
    int viewportClipFps = 0;
};

// ---------------------------------------------------------------------
// Archetype-ECS Shot representation (IMPROVEMENT_PLAN.md Phase 2). Built
// FROM a std::vector<Slide> via toShotList() below -- Slide remains the
// data the rest of this file's functions (and main_cloudrag.cpp's render
// loop) operate on directly; ShotList is an additional, homogeneous-by-
// kind view of the same data for a future SceneAssembler (Phase 3) to
// batch over. See IMPROVEMENT_PLAN.md Phase 0 "設計書との乖離" #6 for why
// this 6-kind taxonomy replaces the design doc's original 3-kind
// (TitleCard/NodeGraphReveal/SourceCard) sketch: those three don't
// correspond to anything in the actual Slide feature set, which grew a
// viewport-clip kind and a reference-card kind that the original sketch
// predates.
// ---------------------------------------------------------------------

enum class ShotKind {
    TextDigest,       // heading + up to 2 bullets, no diagram/code/clip/references
    DiagramImage,      // Mermaid PNG (diagramImagePath), not a Houdini step
    CodeBlock,         // codeBlock is non-empty
    HoudiniStepStill,  // houdiniStepNumber >= 0, single still image, no clip
    HoudiniStepClip,   // houdiniStepNumber >= 0 AND clipFramePaths is non-empty
    ReferenceCards,    // referenceItems is non-empty
};

struct TextDigestShot {
    QString heading, bullet1, bullet2;
};
struct DiagramImageShot {
    QString heading, imagePath;
};
struct CodeBlockShot {
    QString heading, code;
};
struct HoudiniStepStillShot {
    int stepNumber = -1;
    QString imagePath;  // network- or viewport-preferred is already decided by
                         // buildHoudiniStepSlidesFromScreenshots when Slide::
                         // diagramImagePath was populated -- this just carries it over
    QString resultText;
};
struct HoudiniStepClipShot {
    int stepNumber = -1;
    QStringList clipFramePaths;
    int clipFps = 0;
};
struct ReferenceCardsShot {
    QVariantList items;  // QVariantMap{title, db, cited} entries, see Slide::referenceItems
};

struct ShotList {
    std::vector<TextDigestShot> textDigests;
    std::vector<DiagramImageShot> diagramImages;
    std::vector<CodeBlockShot> codeBlocks;
    std::vector<HoudiniStepStillShot> houdiniStepStills;
    std::vector<HoudiniStepClipShot> houdiniStepClips;
    std::vector<ReferenceCardsShot> referenceCards;
    std::vector<ShotKind> order;  // playback order, indexing into the archive above
                                   // matching this ShotKind (e.g. the 2nd TextDigest
                                   // entry in `order` refers to textDigests[1])
};

// Converts an already-composed slide list (see splitIntoSlides et al.
// below) into its ShotList archetype view. Priority mirrors the existing
// per-frame branching in main_cloudrag.cpp's render loop and
// enrichSlidesForDisplay: houdiniStepNumber >= 0 takes precedence over a
// generic DiagramImage classification, clipFramePaths takes precedence
// over a still HoudiniStepStill, referenceItems takes precedence over
// everything else (a "参考" slide is never also a diagram/code slide in
// practice, but the check order documents the intended precedence either
// way).
ShotList toShotList(const std::vector<Slide>& slides);

// ---------------------------------------------------------------------
// Rough LOCAL estimate of token consumption from character counts. The
// Cloud RAG GAS backend tracks real usage server-side (recordTokenUsage_ in
// gas_cloud_rag.js) but its query response never returns that figure to
// the caller -- so this is a heuristic, not a measured value, and must
// stay labeled "推定" (estimated) everywhere it's surfaced. ~1.8
// characters per token is a reasonable middle estimate for mixed
// Japanese/English text with a Gemini-family tokenizer.
// ---------------------------------------------------------------------
int estimateTokens(const QString& text);

// RAG source citations -- numeric ("[1]", "[4]") or, as seen with real
// houdini21 answers, descriptive ("[参考: 過去Q&A]") -- read fine as body
// text, but once embedded into a Mermaid diagram-generation prompt they get
// interpreted as Mermaid node-shape syntax ("[...]" = rectangle node),
// producing stray garbage nodes in the rendered diagram; they also get read
// aloud verbatim by narration/bullets if left in. Strips any bracketed
// citation-shaped span (bounded length so this can't runaway-match across a
// whole paragraph). Only ever applied to prose (narration/bullets/diagram
// prompts), never to code fences, so it can't corrupt array-index syntax
// like `array[0]`.
QString stripCitationMarkers(QString text);

// Cloud RAG answers are markdown (headings/bold/code fences/bullets), which
// reads great on screen (CloudRagScene.qml uses Text.MarkdownText) but
// terribly out loud ("hash hash VEX for loop..."). This strips the syntax
// down to prose for narration. `codeCaptions` supplies a real spoken
// explanation per code fence, consumed in document order; any fence beyond
// the fetched captions falls back to a generic phrase.
QString stripMarkdownForNarration(QString text, const QStringList& codeCaptions = {});

// Splits `markdown` into one Slide per "## heading" section (intro text
// before the first heading, if any, becomes its own untitled-body slide
// under `topic`). Falls back to a single slide holding the whole answer if
// there are no "##" headings at all.
std::vector<Slide> splitIntoSlides(const QString& topic, const QString& markdown);

// Splits any "```mermaid ... ```" fences out of each slide's body into
// their own dedicated diagram slides (same heading, no competing text),
// rendering each to a PNG via mermaid-cli (mmdc). A render failure
// degrades that one diagram back to a plain code-block slide rather than
// failing the whole video. `runId` prefixes the rendered PNG/mmd filenames
// so successive runs never share/overwrite each other's diagram files.
std::vector<Slide> expandDiagramSlides(const std::vector<Slide>& input, const QString& runId);

// Many real Cloud RAG answers are plain prose with no "## heading"
// structure at all, which would otherwise leave one giant slide. Re-chunks
// any slide whose body runs longer than maxCharsPerSlide into several
// same-headed slides (heading suffixed "（続き）" on the 2nd+ part) on
// paragraph, then sentence, boundaries. Diagram, code-fence-bearing, and
// reference-card slides (see Slide::referenceItems) pass through
// untouched -- splitting any of those would corrupt or scatter content a
// later step depends on being intact in one slide.
std::vector<Slide> splitLongTextSlides(const std::vector<Slide>& input, int maxCharsPerSlide);

// Fills in each slide's display fields (bullets always; exactly one of
// diagramImagePath/codeBlock when possible) for the split-screen chapter
// layout. A slide that already has diagramImagePath or referenceItems set
// is left alone. Otherwise: a code fence in the body becomes the code
// panel; plain-text slides get a best-effort dedicated per-slide Mermaid
// diagram request against Cloud RAG (skipped entirely under --mock).
// Returns a rough estimated-token count (see estimateTokens()) accumulated
// from any such requests actually made.
int enrichSlidesForDisplay(std::vector<Slide>& slides, const QString& dbKey, const QString& runId,
                            bool useMock);

// Frame index -> slide boundary lookup table. Each slide's on-screen
// window is proportional to its content length (a longer section gets
// more time), with a floor so short slides don't flash by in a couple of
// frames.
std::vector<int> computeSlideStartFrames(const std::vector<Slide>& slides, int totalFrames);

// Loads a Houdini-tutorial markdown file (frontmatter stripped, title
// extracted from it if present).
HoudiniTutorial loadHoudiniTutorialMarkdown(const QString& mdPath);

// Reads a NodeGraphAsset .json (houdini_tools.py::export_node_graph) and
// summarizes its TOP-LEVEL nodes only into one short line of node labels
// (nested VOP/subnetwork plumbing is excluded -- see the .cpp for why).
// Best-effort: a missing/unreadable/malformed json degrades to an empty
// string, not an error.
QString summarizeHoudiniNodeGraph(const QString& jsonPath);

// tutorial_agent.py's "## コード・ノード構成" section is a raw dump of
// every node created, including deeply-nested internal plumbing that can
// span thousands of lines -- narration-unsafe. Replaces the section's body
// with the short summary from summarizeHoudiniNodeGraph() instead.
QString replaceNodeConfigSection(QString markdown, const QString& topLevelNodeSummary);

// tutorial_agent.py's "## 参考" section opens with a terse research-metric
// line ("利用率: 0%（引用 0/2 件）") that reads as a meaningless fragment
// when narrated verbatim. Rewrites it in place into a full sentence; a
// body with no such line is returned unchanged.
QString humanizeExtractionNote(QString markdown);

// Runs across every slide (Houdini-tutorial mode only) looking for the
// "参考" slide and filling in its referenceItems by parsing the raw
// source-list text in its body (tutorial_agent.py's source_lines format).
// Must run BEFORE splitLongTextSlides (a populated referenceItems is what
// exempts that slide from length-based splitting) and BEFORE
// enrichSlidesForDisplay (which skips requesting a diagram for any slide
// that already has referenceItems).
void assignHoudiniReferenceItems(std::vector<Slide>& slides);

// Loads the JSON array video_factory_bridge.py writes from
// HoudiniToolExecutor.export_step_screenshots().
std::vector<HoudiniStepScreenshot> loadHoudiniScreenshotManifest(const QString& jsonPath);

// Builds one slide PER CAPTURED TOOL CALL directly from the screenshot
// manifest, to REPLACE the "## 手順" section entirely in Houdini-tutorial
// mode -- see the .cpp for why this doesn't try to align Claude's own
// narrative step numbers with the tool-call-level screenshot indices.
std::vector<Slide> buildHoudiniStepSlidesFromScreenshots(
    const std::vector<HoudiniStepScreenshot>& shots);

// The "コード・ノード構成" slide (final graph structure, not an individual
// step) gets the LAST captured screenshot, network-preferred. Must run
// BEFORE enrichSlidesForDisplay, which skips any slide that already has a
// diagramImagePath.
void assignHoudiniFinalGraphScreenshot(std::vector<Slide>& slides,
                                        const std::vector<HoudiniStepScreenshot>& shots);
