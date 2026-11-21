# Third-party notices / 第三者のソフトウェアの表記

RAGReel（動画エンジン）の配布物（インストーラ）には、次の第三者のソフトウェアが入っています。
それぞれのライセンスが、RAGReel 本体の `LICENSE`（PolyForm Noncommercial 1.0.0）とは別に適用されます。

| 名前 | 用途 | ライセンス | 入手元（ソース） |
|---|---|---|---|
| Qt 6（qtbase・qtdeclarative など） | 画面・描画 | GNU LGPL v3 | https://www.qt.io/ ・ https://code.qt.io/ |
| FFmpeg（avcodec・avformat・avfilter・avdevice・swscale・swresample） | 動画・音声の書き出し | GNU LGPL v2.1 以降（GPL・nonfree の部品は、有効にしていない） | https://ffmpeg.org/ |
| libvpx | VP8/VP9 の符号化 | BSD 3-Clause | https://www.webmproject.org/ |
| Opus | 音声の符号化 | BSD 3-Clause | https://opus-codec.org/ |
| Microsoft Visual C++ ランタイム | 実行に必要な DLL | Microsoft の再配布条件 | https://learn.microsoft.com/cpp/windows/redistributing-visual-cpp-files |

## LGPL の部品（Qt・FFmpeg）について

- これらは、**動的にリンクした DLL**として同梱しています（`{app}` フォルダの `Qt6*.dll`・`av*.dll`・`sw*.dll）。
- 利用者は、同じバージョンの互換の DLL に、差し替えられます（リンクし直さなくてよい）。
- LGPL の全文は、https://www.gnu.org/licenses/lgpl-3.0.html（Qt）、https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html（FFmpeg）にあります。
- ソースコードは、上の入手元から取れます。バージョンは、ビルド時の `vcpkg.json` と `vcpkg` のポート（FFmpeg 8.1.x など）に固定されています。

## 商標

Houdini・SideFX は、SideFX Software Inc. の商標です。RAGReel は、SideFX の製品ではなく、SideFX と提携・承認の関係にありません。
