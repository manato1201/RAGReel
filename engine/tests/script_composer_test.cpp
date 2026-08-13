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

#include <QCoreApplication>
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

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
