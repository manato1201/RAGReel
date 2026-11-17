# 動かし方(環境・ビルド・動作確認)

README のクイックスタートの詳細版。READMEから移したもので、内容は変えていない。

## 必要環境

- Windows 11 + Visual Studio 2022(MSVC)
- CMake 3.24+ / Ninja
- [vcpkg](https://github.com/microsoft/vcpkg)(manifestモード。`vcpkg.json`からQt6・FFmpeg・GTestを自動取得。既定では`C:/vcpkg`を想定 — 別の場所にある場合は`CMakePresets.json`の`default`プリセットの`CMAKE_TOOLCHAIN_FILE`を書き換えるか、`VCPKG_ROOT`環境変数を設定した上で`ci`プリセットを使う)
- `mermaid-cli`(`npm install -g @mermaid-js/mermaid-cli`。動画中のMermaid図解レンダリングに使用。無くてもビルド・`--mock`実行は可能で、図解はコードブロック表示にフォールバックする)

## ビルド

```powershell
cmake --preset default
cmake --build --preset default
```

初回はvcpkgがQt6/FFmpeg/GTestをソースからビルドするため、時間がかかります(環境によっては数十分)。

## 動作確認手順

### 1. 一番手軽な確認: CLIを`--mock`で動かす(APIキー不要)

```powershell
cd build\engine
.\video_factory_cloudrag_poc.exe "テストトピック" houdini21 --mock
```

- 実行が終わると同じフォルダの`output/`に動画とダッシュボードが生成されます。`output/index.html`をブラウザで直接開くか、`python -m http.server`等で配信して確認してください
- `--mock-plain`は見出しの無いプレーンな回答での動作確認用フィクスチャです
- Houdiniチュートリアルの取り込みモードも`--mock`と併用でき、ネットワーク接続なしに動作確認できます:
  ```powershell
  .\video_factory_cloudrag_poc.exe --houdini-md "<チュートリアル.mdへのパス>" --mock
  ```
  (同じ場所に同名の`.json`/`_screenshots.json`があれば自動的に使われます。無くても動きます)

### 2. GUIランチャー(RAGReel)を動かす

```powershell
cd build\engine
.\RAGReel.exe
```

起動すると「はじめに」タブが最初に表示されます。実際に動画を生成するには「設定」タブでCloud RAG URL・APIキーを入力してから「Cloud RAGクエリ」または「Houdiniチュートリアル」タブを使ってください(APIキーが無い場合はCLI側の`--mock`で確認するのが手早いです)。

### 3. 実際にCloud RAGへ接続して生成する(要APIキー)

```powershell
$env:CLOUD_RAG_URL = "https://rag-poc.xxxx.workers.dev"
$env:CLOUD_RAG_API_KEY = "..."

cd build\engine
.\video_factory_cloudrag_poc.exe "<質問文>" "<dbKey>"
```

### 4. 単体テストを動かす

```powershell
cd build
ctest --output-on-failure
```

`resource_budget_manager_test`(GPUリース排他制御)、`script_composer_tests`(スライド分割・Houdini解析)、`narration_tests`(読み・字幕・クラウド音声)、`manifest_writer_tests`(公開物の書き出し)、`cloud_rag_client_tests`(Cloud RAG クライアント)の5スイートが登録されています。どれもGPU/表示デバイス不要です。

Webダッシュボードと比較実装の検査(Node / Python のみ、ビルド不要):

```powershell
node tools/check_contrast.js
node tools/check_gallery_layout.js
cd compare; python -m unittest discover -s tests
```
