# compare/ — 同じ入力を3つの方式で動画にする比較実装

`IMPROVEMENT_PLAN_2026-10.md` の V1〜V7 の実装。このリポジトリの目的は「Qtの学習兼**各言語の比較**」なので、
本番のQt/QML版に加えて、同じ入力(チュートリアル手順書 + スクリーンショット)から動画を作る方式を2つ並べた。

| 方式 | 場所 | 中身 |
|---|---|---|
| Qt/QML(本番) | `engine/` | C++/QML。ヘッドレス描画 + FFmpeg |
| HyperFrames | `compare/hyperframes/` | HTML + GSAP を書き、`npx hyperframes render` で MP4 化(V1) |
| Python | `compare/python_render/` | NumPy + Pillow でフレームを合成し ffmpeg に流す。GPU・Qt・ブラウザ不要(V2) |

共通部分: `common/tutorial_input.py`(入力) / `common/narration.py`(音声と読み上げ時刻) /
`common/subtitles.py`(字幕と、ずれの測定) / `common/beats.py`(ビート検出)。

## 使い方

```powershell
cd compare
$T = "<tutorial>"            # 例: ...\localRAG\tutorials\chashu-pork-block_20261004
python -m python_render.render --md "$T.md" --shots "${T}_screenshots.json" --out out/python.mp4 --bgm-bpm 100
python hyperframes/build_project.py --md "$T.md" --shots "${T}_screenshots.json" --narr-dir out/py_work --out out/hf_project
cd out/hf_project; npx hyperframes@0.8.147 render -o ../hyperframes.mp4 -f 30 -q standard
# Qt版(既存): video_factory_cloudrag_poc.exe "<題名>" tutorials --houdini-md "$T.md" --houdini-screenshots "${T}_screenshots.json"
python metrics.py --qt-dir out/qt_run --qt-log out/qt_run/qt.log --qt-wall 33 --py-wall <秒> --hf-wall <秒>
python eval_search.py --shots "${T}_screenshots.json" --st --out out/v6_eval.json     # V6
python publish_compare.py                                                              # web/public/compare/ へ
python -m unittest discover -s tests -v                                                # 18件
```

HyperFrames版は、Python版が作った音声・時刻(`out/py_work`)をそのまま使う。3方式で**音声が同一**になるので、
字幕のずれや生成時間の差が音声合成の違いに由来しない。

## 結果(1回の計測。入力 = チャーシュー10ステップ)

| | Qt/QML | HyperFrames | Python |
|---|---|---|---|
| 生成時間 | **33 秒** | 170〜187 秒 | 269〜337 秒 |
| 出力の長さ | 3:04(手順書の全章を読む) | 2:06 | 2:04 |
| 出力1秒あたりの生成時間 | **0.18 秒** | 1.3〜1.5 秒 | 2.2〜2.7 秒 |
| 解像度 / fps | 1280×720 / 15 | 1280×720 / 30 | 1280×720 / 30 |
| ファイルサイズ | 8.3 MiB | 10.7 MiB | 4.8 MiB |
| 字幕のずれ(平均 / 最大) | 0.26 / 0.49 秒(概算) | 0.06 / 0.18 秒 | 0.07 / 0.24 秒 |
| 実装の規模 | 約5,100行(エンジン全体) | 約180行(生成スクリプト) | 約310行 |

読み方と注意:
- **出力の長さが揃っていない**。Qt版は手順書の全章(コード・ノード構成を含む)を読み上げるため長い。比較には「出力1秒あたり」を使う。
- **実装の規模は同列に比べられない**。Qt版はランチャー・マニフェスト・キュー等を含むエンジン全体。描画部分だけならQML/C++ともっと小さい。
- 字幕のずれは、各動画の**音声そのもの**から発話の開始を検出し、その動画の字幕の開始時刻と突き合わせた値(`metrics.py`)。
  Qt版は現状 SAPI5 のため字幕時刻が概算(文字数比例)。VOICEVOX では実時刻になる(下記V7)。
- 生成時間は同じ構成で2回測った範囲(HyperFrames・Python)。Qt版は1回(33秒)。PCの負荷で数割動く。HyperFrames は初回 `npx` の取得時間を含まないが、起動・コンパイルに約1分かかる。

### どれを標準にするか
- **標準は Qt/QML のまま**。速度が桁違いで(0.18 vs 1.5〜2.2)、ダッシュボード・キュー・字幕メタデータまで一体で動く。
- **HyperFrames は「見た目を試す・場面テンプレートを差し替える」用途**に向く。HTML/CSS/GSAP なので変更が速く、行数も少ない。
  代償は Chrome + Node が要ることと、出力が約2倍大きいこと。
- **Python 版は「GPU・Qtなしで動く / 単体テストしやすい」用途**。最も遅いが、依存が numpy・Pillow・ffmpeg だけで、CI でも回せる。

## V3〜V7

| ID | 状態 | 内容 |
|---|---|---|
| V1 | 実装・計測済み | HyperFrames版。`hyperframes check` は0エラー(重なり警告はトランジション中の意図したもの)。日本語フォントは `@font-face` でローカルの Noto Sans JP を指定すれば描画できた |
| V2 | 実装・計測済み | Python版。レイヤーは 背景→文字→残像/影→主役→仕上げ。見出しはオーバーシュート付きで叩きつけて縮む |
| V3 | 実装・検証済み | `common/beats.py`。numpyのみ(STFT→スペクトルフラックス→自己相関)。既知BPM 90〜150の合成曲6本で誤差1.5 BPM以内、拍間隔のずれ30ms以内。場面転換は「前の発話の終了〜次の発話の開始」の間にだけ拍へ寄せる(映像と音声の対応は崩さない) |
| V4 | 実装済み | トランジションは3種(fade / slide / hex)で、章ごとに固定(intro=fade、steps=slide、outro=hex)。毎回変えない |
| V5 | 実装済み | `python_render/side_by_side.py`。1つの入力を3方式で段階表示し、「STEP n/N」「完了 n」と指標バーを出す。ここでは3方式の生成時間の比較動画に使った |
| V6 | 実装・評価済み(EmbeddingGemma 2 は未検証) | `embed_search.py` / `eval_search.py`。下表 |
| V7 | 実装済み(Python / C++) | 読み上げの時刻から字幕を生成。C++ は `NarrationResult.segments`(VOICEVOXは実時刻、SAPIは概算)→ `subtitle_writer`(SRT/VTT)→ ダッシュボードに表示 |

### V6: スクリーンショットのテキスト検索(10問の言い換えクエリ、正解=そのステップ)

| 方式 | Top-1 | Top-3 |
|---|---|---|
| 文字2-gramハッシュ × 説明文(依存ゼロ) | 80% | 90% |
| 多言語テキストモデル(clip-ViT-B-32-multilingual-v1)× 説明文 | 70% | 80% |
| CLIP 画像埋め込み(clip-ViT-B-32)× 画像そのもの | 30% | 50% |

- 偶然なら Top-1 は10%、Top-3 は30%。画像そのものは偶然よりは当たるが、**Houdiniのネットワーク画面のような
  UIスクリーンショットは画像埋め込みでは区別しにくい**。素材探しは「ステップの説明文で索引し、画像は補助」とするのが妥当。
- サンプルが10問と小さい。傾向の確認であり、順位の確定ではない。
- EmbeddingGemma 2 は、モデルIDと日本語精度を確認できていないため未評価(`STEmbedder` に model_id を渡せば同じ評価が回せる)。

### V7: 字幕

- **実装前**: 字幕は無かった。**実装後**: `NarrationEngine` が文単位の時刻(開始・終了)を返す。VOICEVOX は文ごとに合成して
  連結するため時刻は正確(前後の無音設定を引いた発話区間)。SAPI は同期APIで文ごとの時刻が取れないため、全体の長さを
  文字数で割る**概算**(`subtitles_exact: false` と明記)。
- Python 側は文ごとに合成して連結し、先頭・末尾の無音を削る。実測で字幕開始の平均ずれ 0.07 秒
  (文字数比例の概算なら 0.44 秒・最大 1.95 秒)。
- ダッシュボードは `<track src>` が `file://` で読めないため、cue を `metadata.json` に埋め込み TextTrack API で付ける。
- **VOICEVOX エンジンはこの PC に未導入**のため、VOICEVOX 経路の時刻(`VoicevoxNarrationEngine::synthesize` が作る segments)は実行していない。字幕の書式変換(SRT/VTT)と ManifestWriter は単体テスト済みだが、VOICEVOX の実時刻そのものは未確認。
