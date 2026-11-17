#pragma once

// 「前に描いた画面と、見た目が同じになるフレーム」を見分ける(Qt にも GPU にも依存しない部分)。
//
// 動画の大半は静止したスライドで、見た目が変わるのは次のものだけ(engine/qml/CloudRagScene.qml):
//   - スライドが変わる(slideIndex・図/クリップの画像 diagramSource)
//   - 入場・退場の薄れ(slideOpacity。スライドの最初と最後の 12%、フレームごとに値が変わる)
//   - コードのスクロール(contentY が slideProgress に比例。コードが枠に収まらないスライドだけ)
//   - 下の進捗バー(幅 = 1184 設計単位 × progress。出力の画素に直すと、整数の画素ごとにしか見た目は変わらない)
// これらが前に描いたフレームと同じなら、描画(QML)・GPU の読み戻し・RGBA→YUV の変換を省いて、
// 前の画面をそのまま使える。
//
// 進捗バーは「幅が整数の画素になる progress」に丸めて描く(丸めた分のずれは 1 画素未満で、
// バーの縁の位置は変わらない。グラデーションの色が 8 ビットの最小の段で動く程度)。

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace frame_reuse {

// CloudRagScene.qml の timeline の幅(設計単位): stage.width(1280) - 左右の余白 48 x 2
constexpr double kDesignWidth = 1280.0;
constexpr double kTimelineDesignWidth = 1184.0;

// 出力の画素に直した進捗バーの全幅(1920 幅なら 1776 画素)
inline double progressBarPixels(int frameWidth) {
    return kTimelineDesignWidth * static_cast<double>(frameWidth) / kDesignWidth;
}

// progress(0..1)のときに、バーを塗る画素の列の数。GPU は画素の中心が図形の中に入った列だけを塗る
// (アンチエイリアスなしの Rectangle)ので、中心 k+0.5 が幅 w より小さい列の数 = ceil(w - 0.5)。
// 頂点の位置は GPU が画素の 1/256 に丸める(Direct3D 11 で確認: 丸めないと、幅の小数部が .5 のすぐ近くのとき、
// 実際の列の数と 1 つずれる)ので、同じように丸めてから数える。
inline int progressBarColumns(double progress, int frameWidth) {
    const double total = progressBarPixels(frameWidth);
    const double w = std::clamp(progress, 0.0, 1.0) * total;
    const double snapped = std::round(w * 256.0) / 256.0;
    return std::max(0, static_cast<int>(std::ceil(snapped - 0.5)));
}

// バーの幅が画素の整数になる progress(描画に渡す値)。progressBarColumns の列数を、そのまま幅にする。
inline double quantizedProgress(double progress, int frameWidth) {
    const double total = progressBarPixels(frameWidth);
    if (total <= 0.0) return progress;
    return static_cast<double>(progressBarColumns(progress, frameWidth)) / total;
}

// QML の slideOpacity と同じ式。入場・退場の薄れの度合い(0..1)。
inline double slideOpacity(double slideProgress) {
    const double fadeWindow = 0.12;
    return std::max(0.0, std::min(1.0, std::min(slideProgress / fadeWindow, (1.0 - slideProgress) / fadeWindow)));
}

// 見た目を決める値の組。operator== が真なら、描いた画素は同じ。
struct FrameKey {
    int slideIndex = -1;
    std::string diagramSource;  // 図・クリップの画像の URL(空なら画像なし)
    double opacity = 0.0;       // slideOpacity
    double scroll = 0.0;        // コードのスクロールの度合い(slideProgress)。スクロールしないスライドでは常に 0
    int barColumns = 0;         // 進捗バーの列の数

    bool operator==(const FrameKey& o) const {
        return slideIndex == o.slideIndex && barColumns == o.barColumns && opacity == o.opacity &&
               scroll == o.scroll && diagramSource == o.diagramSource;
    }
    bool operator!=(const FrameKey& o) const { return !(*this == o); }
};

// codeScrolls: そのスライドのコードが枠に収まらず、slideProgress に応じてスクロールするか。
// 収まる(スクロールしない)スライドでは slideProgress を見た目に使わないので、キーに入れない。
inline FrameKey makeKey(int slideIndex, const std::string& diagramSource, double slideProgress,
                        double progress, int frameWidth, bool codeScrolls) {
    FrameKey key;
    key.slideIndex = slideIndex;
    key.diagramSource = diagramSource;
    key.opacity = slideOpacity(slideProgress);
    key.scroll = codeScrolls ? slideProgress : 0.0;
    key.barColumns = progressBarColumns(progress, frameWidth);
    return key;
}

// 「前に描いた画面を使い回せるか」を決める。描いたフレームのキーだけを覚える。
class ReusePolicy {
public:
    explicit ReusePolicy(bool enabled = true) : enabled_(enabled) {}

    // true なら、前に描いた画面をそのまま使う(描かない)。false なら、描く(描いたら commit を呼ぶ)。
    bool canReuse(const FrameKey& key) const { return enabled_ && haveLast_ && key == last_; }

    // 描いたフレームのキーを覚える
    void commit(const FrameKey& key) {
        last_ = key;
        haveLast_ = true;
    }

    bool enabled() const { return enabled_; }

private:
    bool enabled_;
    bool haveLast_ = false;
    FrameKey last_;
};

}  // namespace frame_reuse
