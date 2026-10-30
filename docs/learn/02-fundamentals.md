# 第2章 基礎知識

**学習目標**: RAGReel を読み書きするのに要る土台(プロセス・環境変数・HTTP・JSON・パス・ビルド・テスト)を、このプロジェクトの具体例で理解する。
すでに知っている節は飛ばしてよい。

## 2.1 プロセスと引数

プログラムが動いている1つの実体を**プロセス**と呼ぶ。プロセスは別のプロセスを**子プロセス**として起動できる。

- RAGReel.exe(GUI)は、エンジン(`video_factory_cloudrag_poc.exe`)を子として起動する(`engine/src/launcher/process_runner.cpp`)。
- 起動時に渡す文字列を**引数**という。エンジンは `--mock` や `--houdini-md <パス>` を受け取る(`engine/src/main_cloudrag.cpp` の `takeFlagValue`)。
- 子の標準出力(ログ)を親が読み、ランチャーの「実行ログ」と進捗(%)に表示する。ログの文言を変えると進捗表示が壊れうる(第13章)。

> 学び: **GUI と処理本体を分ける**と、GUI なしの CLI でテストでき、GUI が固まっても処理は止まらない。

## 2.2 環境変数

プロセスごとに持つ「設定の名前と値」。コードや引数に秘密(APIキー)を書かずに渡せる。

| 変数 | 意味 |
|---|---|
| `CLOUD_RAG_URL` / `CLOUD_RAG_API_KEY` | Cloud RAG の Worker の URL とキー |
| `RAGREEL_CLOUD_TTS_URL` / `_KEY` / `_VOICE` / `_RATE` | クラウド音声の送り先・声・速さ(無ければ上の2つを使う) |
| `RAGREEL_TTS_BACKEND` | `auto`(既定)/ `cloud` / `voicevox` / `sapi` |
| `GALLERY_UPLOAD_URL` / `GALLERY_UPLOAD_TOKEN` | 共有ギャラリーへの送り先(無ければ送らない) |

ランチャーは「設定」タブの値を `QSettings`(Windows ではレジストリ)に保存し、エンジンを起動するとき**環境変数に詰めて**渡す(`launcher_settings.cpp`)。コードの読み取りは `qEnvironmentVariable("名前", 既定値)`。

## 2.3 HTTP と JSON

クラウドとの通信はすべて **HTTPS の POST/GET + JSON**。

```
POST {CLOUD_RAG_URL}/query
Authorization: Bearer <APIキー>
Content-Type: application/json

{"query": "...", "dbKey": "houdini21"}     →   {"answer": "...", "sources": [ {...} ]}
```

- **Bearer トークン**: `Authorization: Bearer <文字列>` ヘッダで「私はこの鍵の持ち主です」と示す方式。共有ギャラリーのアップロード/削除も同じ(第9章)。
- **ステータスコード**: 200 成功 / 206 部分(Range)/ 400 要求が不正 / 401 認証失敗 / 404 無い。
- **JSON**: `{"キー": 値}` の形式。C++ は `QJsonDocument`、JavaScript は `JSON.parse` で読む。**`metadata.json` の形はC++とJavaScriptの契約**で、片方だけ変えると壊れる(第8章)。

> 実例: Cloud TTS は 1 文ずつ POST して WAV を受け取る。通信失敗・上限超過は、HTTP の状態と Worker のエラー文を含む例外にして、上位でローカル音声へ切り替える(第6章)。

## 2.4 ファイルパスと「出力先」

- Windows のパスは `C:\...` と `\`。Web の URL は `/`。混ぜて書くと壊れるので、C++ では `QDir` / `QFileInfo` や `QStringLiteral("/")` で組む。
- 出力先は `QCoreApplication::applicationDirPath() + "/output"`(`launcher/video_history.cpp` の `outputDir()`)。**インストール先ごとに1つ**で、共有の場所には書かない。
- Windows のパス長は約260文字が上限。深いインストール先でコピーが失敗する(第13章)。

## 2.3 と 2.4 の確認問題

- 共有ギャラリーに動画を送るのに必要な設定はどれか(答え: `GALLERY_UPLOAD_URL` と `GALLERY_UPLOAD_TOKEN`。無ければ**送らずにローカルだけ**で完了する)。

## 2.5 ビルド(CMake / vcpkg / MSVC)

```powershell
cmake --preset default        # 設定(依存を vcpkg が取得。初回は数十分)
cmake --build --preset default
```

- **CMake**: ビルド手順を書く言語。`engine/CMakeLists.txt` に、実行ファイルとテストの一覧がある。
- **vcpkg**: C++ のライブラリ管理。`vcpkg.json` に Qt6・FFmpeg・GTest を宣言(manifest モード)。
- **MSVC**: Windows の C++ コンパイラ。Visual Studio 2022 が要る。
- **新しいファイルを足したら、`CMakeLists.txt` の一覧に足す**。足し忘れるとビルドに含まれない(このプロジェクトで実際に起きた)。QML・アセット・Web のファイルは、インストーラー(`installer/ragreel.iss`)の一覧にも要る。

## 2.6 テスト

- **単体テスト**: 小さな関数を、入力と期待値で確かめる。`engine/tests/*.cpp`(GoogleTest。一部はフレームワーク不使用)、`compare/tests/`(Python `unittest`)。
- **E2E(通し)**: 実際に動かして確かめる。時間がかかり環境に依存する。
- **検査ツール**: `tools/check_contrast.js`(色)、`tools/check_gallery_layout.js`(レイアウト)。

> 最重要の習慣: **「確認した」ことと「確認していない」ことを分けて書く**(第11章)。単体テストが通っても、通しで動くとは限らない。

## 2.7 Git の使い方(このリポジトリの慣習)

- コミットメッセージは「平易な英語の1行 + 必要なら箇条書き」。
- 大きなバイナリ(動画 `*.mp4` `*.webm`)や生成物(`build/`、`compare/out/`)は追跡しない(`.gitignore`)。
- **公開リポジトリ**なので、鍵・トークン・アカウント情報(`.wrangler/` など)を入れない。入れてしまうと履歴に残る。

## 演習

1. `RAGREEL_TTS_BACKEND=sapi` を設定して動画を作ると、クラウド音声は呼ばれるか。
2. エンジンのログの1行を書き換えたら、ランチャーの進捗表示が止まった。なぜ起こりうるか。
3. 新しい `.cpp` を `engine/src/` に足した。ほかにどこを変えるか。

## 解答(例)

1. 呼ばれない。`sapi` 指定は、クラウド・VOICEVOX を経由せずに SAPI を使う(第6章の図)。
2. Houdini パネルの `video_progress.py` などが、ログの特定の文言(`Rendered frame N / M` など)から%を計算しているため。
3. `engine/CMakeLists.txt` の対象実行ファイルのソース一覧。テストで使うなら、テスト側のソース一覧にも。
