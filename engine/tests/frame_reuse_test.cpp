#include <gtest/gtest.h>

#include "scene/frame_reuse.h"

using namespace frame_reuse;

TEST(FrameReuseTest, ProgressBarWidthMatchesTheScene) {
    // CloudRagScene.qml: timeline の幅は 1184 設計単位、出力の幅 / 1280 倍に拡大される
    EXPECT_DOUBLE_EQ(progressBarPixels(1280), 1184.0);
    EXPECT_DOUBLE_EQ(progressBarPixels(1920), 1776.0);
}

TEST(FrameReuseTest, BarColumnsAreTheCountOfPixelCentersInsideTheBar) {
    EXPECT_EQ(progressBarColumns(0.0, 1920), 0);
    EXPECT_EQ(progressBarColumns(1.0, 1920), 1776);
    // 幅 2.4 画素: 中心 0.5 と 1.5 が入る(2.5 は入らない) = 2 列
    EXPECT_EQ(progressBarColumns(2.4 / 1776.0, 1920), 2);
    // 幅 2.5 ちょうど: 中心 2.5 は「幅より小さい」を満たさない = 2 列
    EXPECT_EQ(progressBarColumns(2.5 / 1776.0, 1920), 2);
    // 幅 2.6: 3 列
    EXPECT_EQ(progressBarColumns(2.6 / 1776.0, 1920), 3);
    // 範囲の外は丸める
    EXPECT_EQ(progressBarColumns(-0.5, 1920), 0);
    EXPECT_EQ(progressBarColumns(2.0, 1920), 1776);
}

TEST(FrameReuseTest, QuantizedProgressGivesAWholeNumberOfPixelsAndKeepsTheColumnCount) {
    for (int width : {1280, 1920}) {
        const double total = progressBarPixels(width);
        for (int i = 0; i <= 20000; ++i) {
            const double p = i / 20000.0;
            const double q = quantizedProgress(p, width);
            const int cols = progressBarColumns(p, width);
            // 丸めた進捗の幅は整数の画素
            EXPECT_NEAR(q * total, static_cast<double>(cols), 1e-9);
            // 丸めても、塗る列の数は変わらない(縁の位置が同じ)
            EXPECT_EQ(progressBarColumns(q, width), cols);
            // ずれは 1 画素の半分(+ 頂点を 1/256 画素に丸める分)まで
            EXPECT_LE(std::abs(q * total - p * total), 0.5 + 1.0 / 256.0 + 1e-9);
        }
    }
}

TEST(FrameReuseTest, SlideOpacityMatchesTheQmlFormula) {
    EXPECT_DOUBLE_EQ(slideOpacity(0.0), 0.0);
    EXPECT_DOUBLE_EQ(slideOpacity(0.06), 0.5);
    EXPECT_DOUBLE_EQ(slideOpacity(0.12), 1.0);
    EXPECT_DOUBLE_EQ(slideOpacity(0.5), 1.0);
    EXPECT_DOUBLE_EQ(slideOpacity(0.94), 0.5);
    EXPECT_DOUBLE_EQ(slideOpacity(1.0), 0.0);
}

TEST(FrameReuseTest, FirstFrameIsNeverReusedAndDisabledPolicyNeverReuses) {
    ReusePolicy policy(true);
    const FrameKey key = makeKey(0, "", 0.5, 0.1, 1920, false);
    EXPECT_FALSE(policy.canReuse(key));
    policy.commit(key);
    EXPECT_TRUE(policy.canReuse(key));

    ReusePolicy off(false);
    off.commit(key);
    EXPECT_FALSE(off.canReuse(key));
}

TEST(FrameReuseTest, StaticSlideReusesTheFrameUntilTheBarGainsAPixel) {
    // 1 本の動画 10 秒・30 fps の静止スライド(スライドは 1 枚)。真ん中(薄れの外)の間は、バーの列が変わった時だけ描く
    const int frameCount = 300;
    ReusePolicy policy(true);
    int rendered = 0;
    int lastCols = -1;
    for (int i = 0; i < frameCount; ++i) {
        const double slideProgress = static_cast<double>(i) / frameCount;
        const double progress = static_cast<double>(i) / (frameCount - 1);
        const FrameKey key = makeKey(0, "", slideProgress, progress, 1920, false);
        const bool inFade = slideProgress < 0.12 || slideProgress > 0.88;
        if (policy.canReuse(key)) {
            // 使い回すのは、薄れの外で、バーの列が同じ時だけ
            EXPECT_FALSE(inFade) << "frame " << i;
            EXPECT_EQ(progressBarColumns(progress, 1920), lastCols) << "frame " << i;
        } else {
            policy.commit(key);
            ++rendered;
            lastCols = key.barColumns;
        }
    }
    // 10 秒 = 1776 列を 300 フレームで進む: 1 フレームで 5.9 列動くので、バーが毎フレーム変わり、使い回せない(短い動画では正しい)
    EXPECT_EQ(rendered, frameCount);
}

TEST(FrameReuseTest, LongStaticSlideReusesMostFrames) {
    // 17 分(30600 フレーム)のうち、1 枚の長いスライドの真ん中。バーは約 17 フレームごとに 1 列進む
    const int frameCount = 30600;
    ReusePolicy policy(true);
    int rendered = 0;
    for (int i = 0; i < frameCount; ++i) {
        const double slideProgress = static_cast<double>(i) / frameCount;
        const double progress = static_cast<double>(i) / (frameCount - 1);
        const FrameKey key = makeKey(0, "", slideProgress, progress, 1920, false);
        if (!policy.canReuse(key)) {
            policy.commit(key);
            ++rendered;
        }
    }
    // 描くのは、バーが進む回数(1776)と、薄れ(両端の 12% = 約 7344 フレーム)の分だけ
    EXPECT_LT(rendered, 1776 + 7344 + 50);
    EXPECT_GT(rendered, 7344);
}

TEST(FrameReuseTest, ChangingSlideOrImageForcesARender) {
    ReusePolicy policy(true);
    const FrameKey a = makeKey(1, "file:///a.png", 0.5, 0.2, 1920, false);
    policy.commit(a);
    EXPECT_TRUE(policy.canReuse(makeKey(1, "file:///a.png", 0.6, 0.2, 1920, false)));
    EXPECT_FALSE(policy.canReuse(makeKey(1, "file:///b.png", 0.5, 0.2, 1920, false)));  // クリップの次の画像
    EXPECT_FALSE(policy.canReuse(makeKey(2, "file:///a.png", 0.5, 0.2, 1920, false)));  // 次のスライド
}

TEST(FrameReuseTest, FadeFramesAreNeverReused) {
    ReusePolicy policy(true);
    // 入場の薄れの中: slideProgress が動くたびに opacity が変わる
    const FrameKey a = makeKey(0, "", 0.03, 0.001, 1920, false);
    policy.commit(a);
    EXPECT_FALSE(policy.canReuse(makeKey(0, "", 0.04, 0.001, 1920, false)));
    // 薄れが終わった後(opacity = 1)は、slideProgress が変わっても同じ
    const FrameKey b = makeKey(0, "", 0.20, 0.001, 1920, false);
    policy.commit(b);
    EXPECT_TRUE(policy.canReuse(makeKey(0, "", 0.30, 0.001, 1920, false)));
}

TEST(FrameReuseTest, ScrollingCodeSlideNeedsEveryFrameButFittingCodeDoesNot) {
    ReusePolicy policy(true);
    const FrameKey scrolling = makeKey(3, "", 0.40, 0.5, 1920, /*codeScrolls=*/true);
    policy.commit(scrolling);
    EXPECT_FALSE(policy.canReuse(makeKey(3, "", 0.41, 0.5, 1920, true)));

    ReusePolicy fitting(true);
    fitting.commit(makeKey(3, "", 0.40, 0.5, 1920, /*codeScrolls=*/false));
    EXPECT_TRUE(fitting.canReuse(makeKey(3, "", 0.41, 0.5, 1920, false)));
}
