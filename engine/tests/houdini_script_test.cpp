#include <gtest/gtest.h>

#include <QByteArray>
#include <QFile>
#include <QTemporaryDir>

#include <stdexcept>

#include "ingest/houdini_script.h"

namespace {

Slide stepSlide(int step, const QString& body) {
    Slide s;
    s.heading = QStringLiteral("手順 %1").arg(step);
    s.body = body;
    s.houdiniStepNumber = step;
    s.bullet1 = body.left(40);
    s.visualKind = VisualKind::Node;
    return s;
}

Slide textSlide(const QString& heading, const QString& body) {
    Slide s;
    s.heading = heading;
    s.body = body;
    return s;
}

const char* kScriptJson = R"({
  "version": 1, "source": "claude",
  "intro": {"narration": "この動画では、Boxを作って、色を付けます。はじめての方でも大丈夫です。",
            "points": ["Boxを作る", "色を付ける", "三つ目は使わない"]},
  "steps": [
    {"step": 3, "heading": "Boxを作る",
     "narration": "まず、Boxノードを作ります。Boxは、四角い箱の形を作る一番かんたんなノードです。",
     "points": ["Tabキーでノードを作る", "名前は box1 のまま"]},
    {"step": 5, "heading": "色を付ける",
     "narration": "float r = rand(i); for (int i = 0; i < n; i++) { addpoint(0, p); }",
     "points": ["Cdで色を付ける"]}
  ],
  "structure": {"narration": "ここまでに作ったノードの全体像です。上から下へ、順番に流れています。"},
  "outro": {"narration": "お疲れさまでした。次は、色の付け方を変えてみましょう。", "points": ["次は色を変えてみる"]}
})";

std::vector<Slide> sampleSlides() {
    std::vector<Slide> v;
    v.push_back(textSlide(QStringLiteral("概要"), QStringLiteral("概要の本文です。")));
    v.push_back(textSlide(QStringLiteral("概要（続き）"), QStringLiteral("概要の続きです。")));
    v.push_back(stepSlide(3, QStringLiteral("「Box」ノードを作り、名前を box1 にする")));
    v.push_back(stepSlide(5, QStringLiteral("box1 のパラメータを設定: Cd を 1")));
    v.push_back(textSlide(QStringLiteral("コード・ノード構成"), QStringLiteral("ノード: box1")));
    v.push_back(textSlide(QStringLiteral("参考"), QStringLiteral("利用率: 0%")));
    return v;
}

}  // namespace

TEST(LooksLikeCode, RecognisesCodeAndLeavesProseAlone) {
    EXPECT_TRUE(looksLikeCode(QStringLiteral("float rin = chf(\"inner\");")));
    EXPECT_TRUE(looksLikeCode(QStringLiteral("@P.y += sin(@P.x);")));
    EXPECT_TRUE(looksLikeCode(QStringLiteral("for (int i = 0; i < n; i++) { addpoint(0, p); }")));
    EXPECT_TRUE(looksLikeCode(QStringLiteral("```vex\nint a = 1;\n```")));
    EXPECT_FALSE(looksLikeCode(QStringLiteral("ここでは、点を円盤の形に並べます。半径が小さいほど速く回ります。")));
    EXPECT_FALSE(looksLikeCode(QStringLiteral("n = 30000 個の点を作ります。")));
    EXPECT_FALSE(looksLikeCode(QStringLiteral("Run Over を Detail にします。")));
}

TEST(HoudiniScriptParse, ReadsTextsLimitsPointsAndDropsCodeLikeNarration) {
    const HoudiniScript s = parseHoudiniScript(QByteArray(kScriptJson));
    ASSERT_TRUE(s.valid);
    EXPECT_EQ(s.source, QStringLiteral("claude"));
    EXPECT_TRUE(s.introNarration.startsWith(QStringLiteral("この動画では")));
    EXPECT_EQ(s.introPoints.size(), 2);  // at most two on-screen lines
    ASSERT_EQ(s.steps.count(3), 1u);
    EXPECT_EQ(s.steps.at(3).heading, QStringLiteral("Boxを作る"));
    EXPECT_FALSE(s.steps.at(3).narration.isEmpty());
    // Step 5's narration is code: dropped, with a warning, but the step entry (heading/points) stays.
    ASSERT_EQ(s.steps.count(5), 1u);
    EXPECT_TRUE(s.steps.at(5).narration.isEmpty());
    EXPECT_FALSE(s.warnings.isEmpty());
    EXPECT_FALSE(s.structureNarration.isEmpty());
    EXPECT_FALSE(s.outroNarration.isEmpty());
}

TEST(HoudiniScriptParse, MalformedOrEmptyInputIsInvalid) {
    EXPECT_FALSE(parseHoudiniScript(QByteArray("not json")).valid);
    EXPECT_FALSE(parseHoudiniScript(QByteArray("[]")).valid);
    EXPECT_FALSE(parseHoudiniScript(QByteArray(R"({"version":1,"steps":[]})")).valid);
    EXPECT_FALSE(loadHoudiniScript(QStringLiteral("Z:/no/such/script.json")).valid);
    EXPECT_FALSE(loadHoudiniScript(QString()).valid);
}

TEST(HoudiniScriptApply, StepSlidesGetHeadingCaptionAndNarration) {
    std::vector<Slide> slides = sampleSlides();
    applyHoudiniScript(slides, parseHoudiniScript(QByteArray(kScriptJson)));

    const auto find = [&](int step) -> const Slide& {
        for (const Slide& s : slides) if (s.houdiniStepNumber == step) return s;
        throw std::runtime_error("slide not found");
    };
    const Slide& s3 = find(3);
    EXPECT_EQ(s3.heading, QStringLiteral("手順 1　Boxを作る"));  // numbered in video order, with the script's title
    EXPECT_EQ(s3.bullet1, QStringLiteral("Tabキーでノードを作る"));
    EXPECT_EQ(s3.bullet2, QStringLiteral("名前は box1 のまま"));
    EXPECT_TRUE(s3.narration.contains(QStringLiteral("一番かんたん")));
    const Slide& s5 = find(5);
    EXPECT_EQ(s5.heading, QStringLiteral("手順 2　色を付ける"));
    EXPECT_TRUE(s5.narration.isEmpty());  // the code-like narration was dropped
}

TEST(HoudiniScriptApply, IntroStructureOutroAndReferenceSlides) {
    std::vector<Slide> slides = sampleSlides();
    applyHoudiniScript(slides, parseHoudiniScript(QByteArray(kScriptJson)));

    // "概要（続き）" is removed: the intro text covers it.
    for (const Slide& s : slides) EXPECT_NE(s.heading, QStringLiteral("概要（続き）"));
    ASSERT_GE(slides.size(), 6u);
    EXPECT_EQ(slides[0].heading, QStringLiteral("この動画でやること"));
    EXPECT_TRUE(slides[0].narration.startsWith(QStringLiteral("この動画では")));
    EXPECT_EQ(slides[0].bullet1, QStringLiteral("Boxを作る"));

    // "まとめ" sits right before "参考", which keeps a short lead-in instead of reading its source list.
    ASSERT_GE(slides.size(), 2u);
    EXPECT_EQ(slides[slides.size() - 2].heading, QStringLiteral("まとめ"));
    EXPECT_EQ(slides.back().heading, QStringLiteral("参考"));
    EXPECT_TRUE(slides.back().narration.contains(QStringLiteral("参考")));
    EXPECT_FALSE(slides.back().narration.contains(QStringLiteral("利用率")));

    bool structureFound = false;
    for (const Slide& s : slides) {
        if (s.heading == QStringLiteral("コード・ノード構成")) {
            structureFound = true;
            EXPECT_TRUE(s.narration.contains(QStringLiteral("全体像")));
        }
    }
    EXPECT_TRUE(structureFound);
}

TEST(HoudiniScriptApply, OutroIsAppendedWhenThereIsNoReferenceSlide) {
    std::vector<Slide> slides = {textSlide(QStringLiteral("概要"), QStringLiteral("本文")), stepSlide(3, QStringLiteral("x"))};
    applyHoudiniScript(slides, parseHoudiniScript(QByteArray(kScriptJson)));
    EXPECT_EQ(slides.back().heading, QStringLiteral("まとめ"));
}

TEST(HoudiniScriptApply, InvalidScriptLeavesSlidesUntouched) {
    std::vector<Slide> slides = sampleSlides();
    const std::vector<Slide> before = slides;
    applyHoudiniScript(slides, HoudiniScript{});
    ASSERT_EQ(slides.size(), before.size());
    for (size_t i = 0; i < slides.size(); ++i) {
        EXPECT_EQ(slides[i].heading, before[i].heading);
        EXPECT_TRUE(slides[i].narration.isEmpty());
    }
}

TEST(HoudiniScriptBlocks, UseScriptTextAndNeverReadCodeAloud) {
    std::vector<Slide> slides = sampleSlides();
    // A step the script knows nothing about, whose "result" is a code dump.
    slides.push_back(stepSlide(9, QStringLiteral("float r = rand(i);\nfor (int i = 0; i < n; i++) {}")));
    // And one whose first line is a plain operation sentence.
    slides.push_back(stepSlide(10, QStringLiteral("sphere1 の出力を、merge1 の入力につなぐ\nおまけの行")));
    applyHoudiniScript(slides, parseHoudiniScript(QByteArray(kScriptJson)));
    const QStringList blocks = slideNarrationBlocks(slides);
    ASSERT_EQ(blocks.size(), static_cast<int>(slides.size()));
    EXPECT_TRUE(blocks[0].startsWith(QStringLiteral("この動画では")));
    for (const QString& b : blocks) {
        EXPECT_FALSE(looksLikeCode(b)) << b.toStdString();
        EXPECT_FALSE(b.contains(QStringLiteral("float r")));
    }
    EXPECT_EQ(blocks.back(), QStringLiteral("sphere1 の出力を、merge1 の入力につなぐ"));  // first line only
    EXPECT_TRUE(blocks[blocks.size() - 2].isEmpty());  // code dump -> silent hold, not read aloud
    // Step 5 had code-like narration in the script: its own operation sentence is said instead.
    EXPECT_EQ(blocks[2], QStringLiteral("box1 のパラメータを設定: Cd を 1"));
}

namespace {
const char* kSectionsJson = R"({
  "version": 1,
  "intro": {"narration": "この動画では、箱を作ります。はじめての方でも大丈夫です。"},
  "sections": [
    {"match": "ハマりポイント", "heading": "つまずきやすいところ",
     "narration": "数字を入れるときに、型が合わないとエラーになることがあります。エラーの文を読むと、直し方が分かります。",
     "points": ["エラーの文を読む"]},
    {"match": "応用・発展のヒント", "heading": "試してみよう",
     "narration": "float r = rand(i); pow(u, 3); { }", "points": []}
  ]
})";
}  // namespace

TEST(HoudiniScriptSections, ExpertSectionsGetTheBeginnerTextAndLoseTheirContinuations) {
    const HoudiniScript script = parseHoudiniScript(QByteArray(kSectionsJson));
    ASSERT_TRUE(script.valid);
    // Only the section with usable (non-code) narration is kept; the code-like one is dropped with a warning.
    ASSERT_EQ(script.sections.size(), 1u);
    EXPECT_EQ(script.sections[0].match, QStringLiteral("ハマりポイント"));
    EXPECT_FALSE(script.warnings.isEmpty());

    std::vector<Slide> slides;
    slides.push_back(textSlide(QStringLiteral("概要"), QStringLiteral("本文")));
    slides.push_back(textSlide(QStringLiteral("ハマりポイント"), QStringLiteral("setpointattrib に float 変数を渡すと Ambiguous call")));
    slides.push_back(textSlide(QStringLiteral("ハマりポイント（続き）"), QStringLiteral("続きの専門的な文章")));
    slides.push_back(textSlide(QStringLiteral("応用・発展のヒント"), QStringLiteral("pow(u,3) に変えると")));
    applyHoudiniScript(slides, script);

    ASSERT_EQ(slides.size(), 3u);  // the "（続き）" part is gone
    EXPECT_EQ(slides[1].heading, QStringLiteral("つまずきやすいところ"));
    EXPECT_TRUE(slides[1].narration.contains(QStringLiteral("エラーの文")));
    EXPECT_EQ(slides[1].bullet1, QStringLiteral("エラーの文を読む"));
    // The section the script did not (usably) cover keeps its original text.
    EXPECT_EQ(slides[2].heading, QStringLiteral("応用・発展のヒント"));
    EXPECT_TRUE(slides[2].narration.isEmpty());
}

TEST(HoudiniScriptFinalImage, IntroAndClosingShowTheLastViewportPicture) {
    QTemporaryDir dir;
    const QString first = dir.filePath("step_001_viewport.png");
    const QString last = dir.filePath("step_009_viewport.png");
    for (const QString& p : {first, last}) {
        QFile f(p);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("png");
    }
    HoudiniStepScreenshot a;
    a.step = 1;
    a.viewportPath = first;
    HoudiniStepScreenshot b;
    b.step = 9;
    b.viewportPath = last;
    HoudiniStepScreenshot c;  // the very last step has no picture on disk: the one before it is used
    c.step = 10;
    c.viewportPath = dir.filePath("missing.png");

    std::vector<Slide> slides = sampleSlides();
    applyHoudiniScript(slides, parseHoudiniScript(QByteArray(kScriptJson)));
    assignFinalResultImage(slides, {a, b, c});

    for (const Slide& s : slides) {
        if (s.heading == QStringLiteral("この動画でやること") || s.heading == QStringLiteral("まとめ")) {
            EXPECT_EQ(s.diagramImagePath, last) << s.heading.toStdString();
        } else if (s.houdiniStepNumber < 0) {
            EXPECT_TRUE(s.diagramImagePath.isEmpty()) << s.heading.toStdString();  // other slides untouched
        }
    }
}

TEST(HoudiniScriptFinalImage, NoPictureOnDiskChangesNothing) {
    std::vector<Slide> slides = sampleSlides();
    applyHoudiniScript(slides, parseHoudiniScript(QByteArray(kScriptJson)));
    HoudiniStepScreenshot a;
    a.viewportPath = QStringLiteral("Z:/no/such.png");
    assignFinalResultImage(slides, {a});
    for (const Slide& s : slides) EXPECT_TRUE(s.diagramImagePath.isEmpty());
}

namespace {
QString touch(const QTemporaryDir& dir, const QString& name) {
    const QString path = dir.filePath(name);
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("png");
    return path;
}

std::vector<Slide> lessonSlides() {
    std::vector<Slide> v;
    v.push_back(textSlide(QStringLiteral("この動画で身につくこと"), QStringLiteral("a")));
    v.push_back(textSlide(QStringLiteral("先に、考え方を知ろう"), QStringLiteral("b")));
    v.push_back(stepSlide(3, QStringLiteral("x")));
    v.push_back(textSlide(QStringLiteral("数字を動かして確かめよう"), QStringLiteral("c")));
    v.push_back(textSlide(QStringLiteral("仕上げ"), QStringLiteral("d")));
    v.push_back(textSlide(QStringLiteral("つまずきやすいところ"), QStringLiteral("e")));
    Slide code = textSlide(QStringLiteral("コード・ノード構成"), QStringLiteral("f"));
    code.codeBlock = QStringLiteral("box1");
    v.push_back(code);
    Slide shot = textSlide(QStringLiteral("参考"), QStringLiteral("g"));
    shot.diagramImagePath = QStringLiteral("already.png");
    v.push_back(shot);
    return v;
}
}  // namespace

TEST(HoudiniScriptPlaceholder, ConceptSlidesGetTheDiagramAndTheOthersTheFinishedResult) {
    QTemporaryDir dir;
    const QString result = touch(dir, "step_009_viewport.png");
    const QString diagram = touch(dir, "node_diagram.png");
    HoudiniStepScreenshot shot;
    shot.viewportPath = result;
    std::vector<Slide> slides = lessonSlides();

    assignPlaceholderImages(slides, {shot}, diagram);

    EXPECT_EQ(slides[0].diagramImagePath, diagram);  // 身につくこと
    EXPECT_EQ(slides[1].diagramImagePath, diagram);  // 考え方
    EXPECT_TRUE(slides[2].diagramImagePath.isEmpty());  // step slides keep their own screenshot logic
    EXPECT_EQ(slides[3].diagramImagePath, result);   // 数字を動かして確かめよう
    EXPECT_EQ(slides[4].diagramImagePath, result);   // 仕上げ
    EXPECT_EQ(slides[5].diagramImagePath, diagram);  // つまずき
    EXPECT_TRUE(slides[6].diagramImagePath.isEmpty());  // code slide untouched
    EXPECT_EQ(slides[7].diagramImagePath, QStringLiteral("already.png"));  // existing picture untouched
}

TEST(HoudiniScriptPlaceholder, FallsBackToWhicheverPictureExists) {
    QTemporaryDir dir;
    const QString diagram = touch(dir, "node_diagram.png");
    std::vector<Slide> onlyDiagram = lessonSlides();
    assignPlaceholderImages(onlyDiagram, {}, diagram);
    EXPECT_EQ(onlyDiagram[3].diagramImagePath, diagram);  // no finished result: the diagram is used for every slide
    EXPECT_EQ(onlyDiagram[4].diagramImagePath, diagram);

    const QString result = touch(dir, "v.png");
    HoudiniStepScreenshot shot;
    shot.viewportPath = result;
    std::vector<Slide> onlyResult = lessonSlides();
    assignPlaceholderImages(onlyResult, {shot}, QString());
    EXPECT_EQ(onlyResult[1].diagramImagePath, result);  // no diagram: concept slides show the result
    EXPECT_EQ(onlyResult[0].diagramImagePath, result);
}

TEST(HoudiniScriptPlaceholder, NothingOnDiskChangesNothing) {
    std::vector<Slide> slides = lessonSlides();
    HoudiniStepScreenshot shot;
    shot.viewportPath = QStringLiteral("Z:/no/such.png");
    assignPlaceholderImages(slides, {shot}, QStringLiteral("Z:/no/diagram.png"));
    EXPECT_TRUE(slides[0].diagramImagePath.isEmpty());
    EXPECT_TRUE(slides[4].diagramImagePath.isEmpty());
}

TEST(HoudiniScriptParse, ReadsTheNodeDiagramPathOnlyWhenTheFileExists) {
    QTemporaryDir dir;
    const QString diagram = touch(dir, "node_diagram.png");
    const QByteArray with = QStringLiteral(R"({"intro":{"narration":"この動画では、箱を作ります。はじめての方でも大丈夫です。"},"images":{"node_diagram":"%1"}})")
                                .arg(diagram).toUtf8();
    EXPECT_EQ(parseHoudiniScript(with).nodeDiagramPath, diagram);
    const QByteArray missing = QByteArray(R"({"intro":{"narration":"この動画では、箱を作ります。はじめての方でも大丈夫です。"},"images":{"node_diagram":"Z:/none.png"}})");
    EXPECT_TRUE(parseHoudiniScript(missing).nodeDiagramPath.isEmpty());
}

