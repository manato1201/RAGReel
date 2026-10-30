# 第7章 描画とエンコード(Assemble / Render / Encode)

**学習目標**: ① ウィンドウ無しで QML を1コマずつ描く仕組みを説明できる ② フレームごとに QML へ渡す値と、スライドの切り替わりを追える ③ FFmpeg で映像と音声を1つにする流れを言える

## 7.1 ヘッドレス描画の考え方

普通の Qt アプリは、ウィンドウに描く。RAGReel は**ウィンドウを出さず、メモリ上の画像(テクスチャ)に描いて取り出す**。

- `QQuickRenderControl` + `QQuickWindow` + `QRhi`(Qt の GPU 抽象層)で、QML のシーンを**オフスクリーン**のテクスチャへ描く。
- 描き終えたら `QImage`(RGBA)として読み戻し、エンコーダへ渡す。
- 利点: 画面も表示デバイスも要らない(CI でも動く)。1コマずつ**決定的**に描ける(再生速度に依存しない)。

実装は `engine/src/scene/scene_assembler.{h,cpp}`。

```cpp
SceneAssembler assembler(1280, 720, "qml/CloudRagScene.qml");
assembler.initialize(staticProps, &error);          // 動画全体で変わらない値
for (int frame = 0; frame < totalFrames; ++frame) {
    FrameProperties fp = ...;                        // このコマの値
    QImage img = assembler.renderFrame(fp);          // 描いて読み戻す
    encoder->pushFrame(img.constBits());
}
```

## 7.2 QML に渡す値

| 種類 | 項目 | 意味 |
|---|---|---|
| **StaticProperties**(最初に1回) | `topic`・`brandLabel`・`slideCount`・`metadataLine`・`slideBoundaries` | タイトル・ブランド表示(例 `HOUDINI22`)・スライド数・タイムラインの境界 |
| **FrameProperties**(コマごと) | `progress` | 動画全体の進み具合 0〜1 |
| | `slideIndex` / `slideProgress` | 今のスライドと、その中の進み具合 |
| | `slideHeading` / `slideBullet1` / `slideBullet2` | 左側の見出しと要点 |
| | `slideCodeBlock` / `slideDiagramSource` / `slideReferenceItems` | 右側(コード・図・出典カード) |

QML(`engine/qml/CloudRagScene.qml`)はこれらの**プロパティを読んで見た目を決める**だけ。QML 側に時間の状態を持たせない。**時刻 → 見た目が純関数**になるので、同じ値を渡せば同じ絵になる(第10章のトランジションも同じ考え方)。

## 7.3 画面のレイアウト(分割画面チャプター形式)

- 左: 見出し + 要点2つ / 右: 図解・コードエディタ風・Houdini の画像・参考カード / 下: セグメント式のタイムライン
- コードは「macOS 風のウィンドウ + 等幅フォント」で表示する。
- 画像が無いスライドは、抽象的なグラデーション(`--mock` 時)や、ノード図・完成画像で埋める(第5章)。

## 7.4 スライドの切り替え

```text
全フレーム N、スライドの開始フレーム配列 S = computeSlideStartFrames(...)
frame f → スライド index = S の中で f 以下の最大の位置
slideProgress = (f - S[index]) / (S[index+1] - S[index])
```

つまり**配列(第5章)を引くだけ**で、描画ループは単純。

## 7.5 サムネイル

全体の **40% 地点**のフレームを PNG で保存する(`thumb.png`)。JPEG は追加プラグインの配置が要るため、PNG にしている。

## 7.6 エンコード(`encode/video_encoder.*`)

FFmpeg(C の API)を C++ の RAII で包んだ `VideoEncoder`。

| 手順 | 呼び出し | 中身 |
|---|---|---|
| ① 作る | `VideoEncoder(path, w, h, fps, wavPath)` | コンテナ(WebM)・映像ストリーム(VP9)・音声ストリーム(Opus)を用意 |
| ② 映像 | `pushFrame(rgba)` を全フレーム分 | RGBA → YUV420(`libswscale`)→ VP9 で圧縮 → 書き込み |
| ③ 音声 | `writeAudioTrack()` | WAV を読み、リサンプル(`libswresample`)して Opus で圧縮 → 書き込み |
| ④ 閉じる | `finish()` | 残りを書き出してファイルを閉じる |

- **RAII**: `std::unique_ptr` に専用の deleter(`AVFormatContextDeleter` など)を付け、例外が出ても確実に解放する。FFmpeg の確保/解放の取りこぼし(メモリリーク)を防ぐ。
- **VP9 が使えない FFmpeg ビルド**では例外にする(vcpkg の ffmpeg に `vpx` 機能が要る)。
- 映像は**ビットレート未指定**で、libvpx の品質設定(`deadline=good, cpu-used=4`)に任せる。
- Opus は**名前 `"libopus"` で選ぶ**(内蔵の実験エンコーダを避ける。第3章)。

## 7.7 失敗したとき

- レンダリング中の例外は、`recordStage(Render, false, …, エラー文)` で記録され、`metadata.json` の `pipeline[]` に失敗として出る(ダッシュボードのステップ表示で見える)。
- 公開(Publish)の失敗も別に記録される。

## 演習

1. `slideProgress` を QML でなくエンジン側で計算して渡す理由は。
2. フレームごとに `new` した画像を溜めて最後にまとめてエンコードすると何が起きるか。
3. 解像度を 1920×1080 にするには、どこを変えるか。ほかに何を確認すべきか。

## 解答(例)

1. QML に時間の状態を持たせず、「値 → 絵」を純関数にして、決定的で再現できる描画にするため。
2. 1コマ(1280×720×4 ≒ 3.7 MB)× 数千枚でメモリを大量に使う。1コマずつ描いてすぐ `pushFrame` する(ストリーミング)のが正しい。
3. `main_cloudrag.cpp` の `kFrameWidth/Height`。QML のレイアウト(固定ピクセルで書いた箇所)、生成時間(画素数が約2.25倍)、ファイルサイズ、`metadata.json` の `render.width/height` の表示を確認する。
