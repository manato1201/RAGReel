// GTest-based unit tests for engine/src/ingest/script_composer.{h,cpp}
// (IMPROVEMENT_PLAN.md Phase 5). ScriptComposer is Qt/GPU non-dependent in
// the sense that matters here -- no QtQuick/QRhi/rendering -- so this
// target links only Qt6::Core (via script_composer.cpp's own QString/
// QRegularExpression/QJsonDocument usage) and GTest, no GUI/Quick
// libraries, no display or GPU device needed. That's what lets it run on
// a bare CI runner.
//
// A QCoreApplication is still required at process scope: QRegularExpression
// and other QtCore facilities used deep in script_composer.cpp assert on
// having one constructed before use in some Qt configurations. main()
// below constructs it once for the whole test binary.

#include <array>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include "ingest/script_composer.h"

TEST(SplitIntoSlides, SingleHeadinglessAnswerBecomesOneSlide) {
    const auto slides = splitIntoSlides(QStringLiteral("トピック"), QStringLiteral("本文のみ、見出しなし。"));
    ASSERT_EQ(slides.size(), 1u);
    EXPECT_EQ(slides[0].heading, QStringLiteral("トピック"));
    EXPECT_EQ(slides[0].body, QStringLiteral("本文のみ、見出しなし。"));
}

TEST(SplitIntoSlides, SplitsOnLevel2HeadingsAndKeepsIntroAsFirstSlide) {
    const QString markdown = QStringLiteral(
        "導入文です。\n\n"
        "## 見出しA\n"
        "本文A\n\n"
        "## 見出しB\n"
        "本文B\n");
    const auto slides = splitIntoSlides(QStringLiteral("トピック"), markdown);
    ASSERT_EQ(slides.size(), 3u);
    EXPECT_EQ(slides[0].heading, QStringLiteral("トピック"));
    EXPECT_EQ(slides[0].body, QStringLiteral("導入文です。"));
    EXPECT_EQ(slides[1].heading, QStringLiteral("見出しA"));
    EXPECT_EQ(slides[1].body, QStringLiteral("本文A"));
    EXPECT_EQ(slides[2].heading, QStringLiteral("見出しB"));
    EXPECT_EQ(slides[2].body, QStringLiteral("本文B"));
}

TEST(StripCitationMarkers, RemovesNumericAndDescriptiveBracketedCitations) {
    EXPECT_EQ(stripCitationMarkers(QStringLiteral("これは引用です[1][2]。")),
              QStringLiteral("これは引用です。"));
    EXPECT_EQ(stripCitationMarkers(QStringLiteral("説明的な出典[参考: 過去Q&A]も除去。")),
              QStringLiteral("説明的な出典も除去。"));
}

TEST(StripCitationMarkers, DoesNotTouchArrayIndexSyntaxOutsideItsOwnCallers) {
    // stripCitationMarkers() has no way to distinguish "array[0]" from a
    // short citation marker by itself -- callers are responsible for only
    // applying it to prose, never code (see stripMarkdownForNarration's
    // fence-substitution, which replaces whole code fences before this
    // ever runs on them). This test documents that limitation rather than
    // asserting behavior this function was never designed to have.
    EXPECT_EQ(stripCitationMarkers(QStringLiteral("array[0]")), QStringLiteral("array"));
}

TEST(HumanizeExtractionNote, RewritesTerseStatIntoFullSentence) {
    const QString markdown = QStringLiteral("## 参考\n利用率: 0%（引用 0/2 件）\n- [1] ⬜ 未引用 タイトルA（houdini21）");
    const QString result = humanizeExtractionNote(markdown);
    EXPECT_FALSE(result.contains(QStringLiteral("利用率: 0%（引用")));
    EXPECT_TRUE(result.contains(QStringLiteral("2件検索し")));
    EXPECT_TRUE(result.contains(QStringLiteral("0件を実際にチュートリアル生成で")));
}

TEST(HumanizeExtractionNote, LeavesMarkdownWithoutTheStatLineUnchanged) {
    const QString markdown = QStringLiteral("## 参考\n（参考ドキュメントなし）");
    EXPECT_EQ(humanizeExtractionNote(markdown), markdown);
}

TEST(AssignHoudiniReferenceItems, ParsesCitedAndUncitedSourceLines) {
    std::vector<Slide> slides;
    Slide referenceSlide;
    referenceSlide.heading = QStringLiteral("参考");
    referenceSlide.body = QStringLiteral(
        "- [1] ⬜ 未引用 KineFXプロシージャルアニメーション（houdini21）\n"
        "- [2] ✅ 引用済み VEXループ・条件文（houdini21）");
    slides.push_back(referenceSlide);

    Slide otherSlide;
    otherSlide.heading = QStringLiteral("概要");
    otherSlide.body = QStringLiteral("無関係な本文");
    slides.push_back(otherSlide);

    assignHoudiniReferenceItems(slides);

    ASSERT_EQ(slides[0].referenceItems.size(), 2);
    const QVariantMap first = slides[0].referenceItems.at(0).toMap();
    EXPECT_EQ(first.value(QStringLiteral("title")).toString(),
              QStringLiteral("KineFXプロシージャルアニメーション"));
    EXPECT_EQ(first.value(QStringLiteral("db")).toString(), QStringLiteral("houdini21"));
    EXPECT_FALSE(first.value(QStringLiteral("cited")).toBool());

    const QVariantMap second = slides[0].referenceItems.at(1).toMap();
    EXPECT_TRUE(second.value(QStringLiteral("cited")).toBool());

    EXPECT_TRUE(slides[1].referenceItems.isEmpty());
}

TEST(SplitLongTextSlides, LeavesReferenceCardSlideIntact) {
    // Regression test for the bug this exemption was added to prevent
    // (see splitLongTextSlides's comment): a "参考" slide with several
    // sources easily exceeds a small maxCharsPerSlide, and used to get
    // scattered into untagged "参考（続き）" continuation slides.
    Slide referenceSlide;
    referenceSlide.heading = QStringLiteral("参考");
    referenceSlide.body = QString(300, QLatin1Char('x'));  // longer than maxCharsPerSlide below
    QVariantMap item;
    item[QStringLiteral("title")] = QStringLiteral("t");
    referenceSlide.referenceItems << item;

    const std::vector<Slide> input{referenceSlide};
    const auto result = splitLongTextSlides(input, /*maxCharsPerSlide=*/50);

    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].heading, QStringLiteral("参考"));
    EXPECT_EQ(result[0].referenceItems.size(), 1);
}

TEST(ToShotList, ClassifiesEachSlideKindAndPreservesOrder) {
    std::vector<Slide> slides;

    Slide text;
    text.heading = QStringLiteral("テキスト");
    text.bullet1 = QStringLiteral("箇条書き1");
    slides.push_back(text);

    Slide diagram;
    diagram.heading = QStringLiteral("図解");
    diagram.diagramImagePath = QStringLiteral("diagram.png");
    slides.push_back(diagram);

    Slide code;
    code.heading = QStringLiteral("コード");
    code.codeBlock = QStringLiteral("int main() {}");
    slides.push_back(code);

    Slide houdiniStill;
    houdiniStill.heading = QStringLiteral("手順 1");
    houdiniStill.houdiniStepNumber = 1;
    houdiniStill.diagramImagePath = QStringLiteral("step1.png");
    slides.push_back(houdiniStill);

    Slide houdiniClip;
    houdiniClip.heading = QStringLiteral("手順 2");
    houdiniClip.houdiniStepNumber = 2;
    houdiniClip.clipFramePaths = {QStringLiteral("f1.png"), QStringLiteral("f2.png")};
    houdiniClip.clipFps = 12;
    slides.push_back(houdiniClip);

    Slide references;
    references.heading = QStringLiteral("参考");
    QVariantMap item;
    item[QStringLiteral("title")] = QStringLiteral("t");
    references.referenceItems << item;
    slides.push_back(references);

    const ShotList shots = toShotList(slides);

    ASSERT_EQ(shots.order.size(), slides.size());
    EXPECT_EQ(shots.order[0], ShotKind::TextDigest);
    EXPECT_EQ(shots.order[1], ShotKind::DiagramImage);
    EXPECT_EQ(shots.order[2], ShotKind::CodeBlock);
    EXPECT_EQ(shots.order[3], ShotKind::HoudiniStepStill);
    EXPECT_EQ(shots.order[4], ShotKind::HoudiniStepClip);
    EXPECT_EQ(shots.order[5], ShotKind::ReferenceCards);

    ASSERT_EQ(shots.textDigests.size(), 1u);
    EXPECT_EQ(shots.textDigests[0].bullet1, QStringLiteral("箇条書き1"));
    ASSERT_EQ(shots.houdiniStepClips.size(), 1u);
    EXPECT_EQ(shots.houdiniStepClips[0].clipFps, 12);
    ASSERT_EQ(shots.referenceCards.size(), 1u);
    EXPECT_EQ(shots.referenceCards[0].items.size(), 1);
}

namespace {

Slide makeSlide(VisualKind kind, int bodyChars) {
    Slide s;
    s.heading = QStringLiteral("h");
    s.body = QString(bodyChars, QLatin1Char('x'));
    s.visualKind = kind;
    return s;
}

// Frames each VisualKind ends up with, as fractions of the total.
std::array<double, 3> kindShares(const std::vector<Slide>& slides, const std::vector<int>& starts, int total) {
    std::array<double, 3> frames = {0.0, 0.0, 0.0};
    for (size_t i = 0; i < slides.size(); ++i) {
        frames[static_cast<int>(slides[i].visualKind)] += starts[i + 1] - starts[i];
    }
    for (double& f : frames) f /= total;
    return frames;
}

QString writeFile(const QDir& dir, const QString& name, const QByteArray& bytes) {
    const QString path = dir.filePath(name);
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(bytes);
    return path;
}

} // namespace

TEST(ComputeSlideStartFrames, HoudiniTutorialSplitsScreenTimeSeventyTwentyTen) {
    std::vector<Slide> slides;
    for (int i = 0; i < 20; ++i) slides.push_back(makeSlide(VisualKind::Node, 40 + i * 3));
    for (int i = 0; i < 6; ++i) slides.push_back(makeSlide(VisualKind::Viewport, 50));
    for (int i = 0; i < 8; ++i) slides.push_back(makeSlide(VisualKind::Other, 60 + i * 20));
    constexpr int kTotal = 7800;
    const auto starts = computeSlideStartFrames(slides, kTotal, 15);

    ASSERT_EQ(starts.size(), slides.size() + 1);
    EXPECT_EQ(starts.front(), 0);
    EXPECT_EQ(starts.back(), kTotal);
    for (size_t i = 0; i < slides.size(); ++i) {
        EXPECT_GE(starts[i + 1] - starts[i], 37) << "slide " << i << " is shorter than the 2.5s minimum";
    }
    const auto share = kindShares(slides, starts, kTotal);
    EXPECT_NEAR(share[static_cast<int>(VisualKind::Node)], 0.7, 0.01);
    EXPECT_NEAR(share[static_cast<int>(VisualKind::Viewport)], 0.2, 0.01);
    EXPECT_NEAR(share[static_cast<int>(VisualKind::Other)], 0.1, 0.01);
}

TEST(ComputeSlideStartFrames, MissingKindGivesItsShareBackProportionally) {
    // No viewport slides: node 0.7 : other 0.1 renormalises to 87.5% : 12.5%.
    std::vector<Slide> slides;
    for (int i = 0; i < 10; ++i) slides.push_back(makeSlide(VisualKind::Node, 50));
    for (int i = 0; i < 4; ++i) slides.push_back(makeSlide(VisualKind::Other, 80));
    const auto starts = computeSlideStartFrames(slides, 4000, 15);
    const auto share = kindShares(slides, starts, 4000);
    EXPECT_NEAR(share[static_cast<int>(VisualKind::Node)], 0.875, 0.01);
    EXPECT_NEAR(share[static_cast<int>(VisualKind::Other)], 0.125, 0.01);
}

TEST(ComputeSlideStartFrames, VideosWithoutHoudiniStepsKeepContentLengthWeighting) {
    std::vector<Slide> slides = {makeSlide(VisualKind::Other, 100), makeSlide(VisualKind::Other, 300)};
    const auto starts = computeSlideStartFrames(slides, 400, 30);
    // (heading 1 + body) : (1 + body) => 101 : 301
    EXPECT_NEAR(starts[1], 400 * 101.0 / 402.0, 1.0);
    EXPECT_EQ(starts.back(), 400);
}

TEST(BuildHoudiniStepSlides, CookAlwaysViewportAndChangedViewportsAreToppedUpEvenly) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QDir dir(tmp.path());

    std::vector<HoudiniStepScreenshot> shots;
    for (int step = 1; step <= 18; ++step) {
        HoudiniStepScreenshot s;
        s.step = step;
        s.tool = (step % 6 == 0) ? QStringLiteral("cook_node") : QStringLiteral("set_parameter");
        s.result = QStringLiteral("結果 %1").arg(step);
        s.networkPath = writeFile(dir, QStringLiteral("n%1.png").arg(step), QByteArray("net") + QByteArray::number(step));
        // Every step's viewport differs from the previous one.
        s.viewportPath = writeFile(dir, QStringLiteral("v%1.png").arg(step), QByteArray("vp") + QByteArray::number(step));
        shots.push_back(s);
    }
    const auto slides = buildHoudiniStepSlidesFromScreenshots(shots);
    ASSERT_EQ(slides.size(), 18u);

    int viewport = 0;
    for (size_t i = 0; i < slides.size(); ++i) {
        const bool isCook = shots[i].tool == QStringLiteral("cook_node");
        if (isCook) EXPECT_EQ(slides[i].visualKind, VisualKind::Viewport) << "cook step " << i;
        viewport += slides[i].visualKind == VisualKind::Viewport ? 1 : 0;
    }
    // 18 steps * (0.2 / 0.9) = 4 viewport slides; 3 are cook_node, one more is added.
    EXPECT_EQ(viewport, 4);
    EXPECT_EQ(slides.size() - viewport, 14u);
    for (const Slide& s : slides) {
        if (s.visualKind == VisualKind::Node) EXPECT_TRUE(s.diagramImagePath.contains(QStringLiteral("/n")));
    }
}

TEST(BuildHoudiniStepSlides, UnchangedViewportsStayOnTheNodeScreen) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QDir dir(tmp.path());

    std::vector<HoudiniStepScreenshot> shots;
    for (int step = 1; step <= 9; ++step) {
        HoudiniStepScreenshot s;
        s.step = step;
        s.tool = QStringLiteral("set_parameter");
        s.result = QStringLiteral("r");
        s.networkPath = writeFile(dir, QStringLiteral("n%1.png").arg(step), QByteArray("net") + QByteArray::number(step));
        s.viewportPath = writeFile(dir, QStringLiteral("v%1.png").arg(step), QByteArray("same"));  // never changes
        shots.push_back(s);
    }
    const auto slides = buildHoudiniStepSlidesFromScreenshots(shots);
    // Only the first step's viewport counts as "changed" (vs. nothing before it).
    int viewport = 0;
    for (const Slide& s : slides) viewport += s.visualKind == VisualKind::Viewport ? 1 : 0;
    EXPECT_LE(viewport, 2);
    EXPECT_GE(viewport, 0);
}

TEST(BuildHoudiniStepSlides, NoNetworkImagesFallsBackToViewportForEveryStep) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QDir dir(tmp.path());

    std::vector<HoudiniStepScreenshot> shots;
    for (int step = 1; step <= 4; ++step) {
        HoudiniStepScreenshot s;
        s.step = step;
        s.tool = QStringLiteral("create_node");
        s.result = QStringLiteral("r");
        s.viewportPath = writeFile(dir, QStringLiteral("v%1.png").arg(step), QByteArray::number(step));
        shots.push_back(s);
    }
    for (const Slide& s : buildHoudiniStepSlidesFromScreenshots(shots)) {
        EXPECT_EQ(s.visualKind, VisualKind::Viewport);
        EXPECT_TRUE(s.diagramImagePath.contains(QStringLiteral("/v")));
    }
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
