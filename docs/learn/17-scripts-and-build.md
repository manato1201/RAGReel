# 第17章 ビルド・CI・配布・運用スクリプト編

**学習目標**: RAGReel を**ビルドし、テストし、配り、公開する**ための設定とスクリプト(CMake・vcpkg・プリセット・バッチ・GitHub Actions・Inno Setup・wrangler・Node/Python のツール)を読み、直せるようになる。
各節は「何のためか → 実物の要点 → 触るときの注意 → 練習」。

```
ソース → [CMake + vcpkg + MSVC] → 実行ファイル
        → [ctest / node / unittest] → 検査
        → [Inno Setup] → インストーラー(配布)
        → [wrangler] → 共有ギャラリー(公開)
        → [GitHub Actions] → 自動のビルド+テスト
```

---

## 17.1 CMake(ビルドの設計図)

**役割**: 「どのソースから、どの実行ファイルを、どのライブラリとリンクして作るか」を書く。`engine/CMakeLists.txt` が中心(約 370 行)。

| 命令 | 意味 | 実例 |
|---|---|---|
| `add_executable(名前 ソース…)` | 実行ファイルを作る | `video_factory_cloudrag_poc`・`RAGReel`・各テスト |
| `target_link_libraries(名前 …)` | ライブラリをつなぐ | Qt6・FFmpeg・GTest |
| `target_include_directories` | `#include "…"` の探し場所 | `src/` |
| `add_test(NAME … COMMAND …)` | `ctest` に登録 | 5つのテスト |
| `add_custom_command(TARGET … POST_BUILD …)` | ビルド後に実行する処理 | 画面・アセット・Qt プラグインのコピー |
| `foreach(… )` / `function(…)` | 繰り返し・関数 | `_dashboard_file` の一覧、`deploy_qt_plugin_dir` |

**ビルド後のコピー(重要)**: 実行ファイルは**自分の隣のフォルダ**から資源を探す(`appRelativePath()` / `applicationDirPath()`)。そのため、ビルド後に次を実行ファイルの隣へコピーする。
- **`qml/CloudRagScene.qml`**、**`assets/mermaid_theme.json`**、**`assets/tts_dictionary.json`**。**ファイルを1つずつ**コピーしている(`copy_if_different`)。新しい資源(例: 別の辞書や QML)を足したら、**そのファイル用のコピー行を足す**。辞書は最初コピーを忘れて、読まれずに動いていたことがある(第13章)
- **`libssl-3-x64.dll`**(HTTPS の実行時依存。TLS プラグインが使う)と、GUI 用の **`qml/QtQuick/`**(Qt の QML モジュール。実行時に読むため自動収集されない)
- **ダッシュボードの画面**(`index.html gallery.html video.html random.html styles.css app.js hero-disc.js` → `output/`)。`copy_if_different` なので、**すでに公開済みの `manifest.json`/`videos/` を消さない**
- **Qt のプラグイン**: `platforms/`(ウィンドウ系)と `tls/`(HTTPS)。**実行時に動的に読む部品なので自動のDLL収集では拾われない**。無いと「platform plugin が見つからない」「No functional TLS backend」で起動時に落ちる。

**触るときの注意**
- 新しいソース・テスト・画面ファイルは**一覧に足す**(足し忘れが最頻の不具合。第13章)。
- `build/` が古いと、変更が効かない。**`build/` を消して `cmake --preset default` から**。
- Release 構成で作る(`CMAKE_BUILD_TYPE: Release`)。デバッグ用の巨大なライブラリを避ける。

**練習**: 新しいテスト `foo_tests`(`tests/foo_test.cpp`)を足す。最低限書く3行は。(答え: `add_executable(foo_tests tests/foo_test.cpp …)`、`target_link_libraries(foo_tests PRIVATE …)`、`add_test(NAME foo_tests COMMAND foo_tests)`)

---

## 17.2 vcpkg(C++ ライブラリの管理)

`vcpkg.json`(**manifest モード**: プロジェクトが必要な依存を宣言し、vcpkg が取得・ビルドする)

```json
{ "dependencies": [ "qtbase", "qtdeclarative",
    { "name": "ffmpeg", "default-features": true, "features": ["vpx", "opus"] },
    "gtest" ] }
```
- **機能(features)**: ffmpeg の `vpx`(VP9)と `opus` を入れないと、エンコーダが見つからず実行時に失敗する(第13章)。
- **トリプレット** `x64-windows-release`(64bit・Windows・Release のみ): デバッグ版を作らないのでビルドが速い。
- 初回は Qt/FFmpeg をソースからビルドするため数十分。以降はキャッシュ。
- **未固定**: `builtin-baseline`(ライブラリの版の固定)が無く、時間がたつと取得される版が変わりうる。CI は「毎回その時点の vcpkg」。固定するなら `vcpkg.json` に baseline を足す(再現性と更新のトレードオフ)。

---

## 17.3 CMake プリセット(`CMakePresets.json`)

設定を名前で呼べるようにする。
- `default`: Ninja・`build/`・`C:/vcpkg` のツールチェーン・Release。**手元の開発用**。
- `ci`: `default` を継承し、ツールチェーンの場所だけ**環境変数 `VCPKG_ROOT`** から取る(CI のランナーには `C:/vcpkg` が無い)。

```powershell
cmake --preset default        # 設定
cmake --build --preset default
```
**継承(`inherits`)**で、差分だけを書く。同じ設定を2か所に書くと、片方だけ直して食い違う。

---

## 17.4 開発用バッチ(`tools/build_dev.cmd`)

```bat
@echo off
setlocal
set "VCVARS=...\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" ( echo vcvars64.bat not found & exit /b 1 )
call "%VCVARS%" >nul
cd /d "%~dp0.."
if "%~1"=="" ( cmake --build build ) else ( cmake --build build --target %* )
exit /b %ERRORLEVEL%
```
- **なぜ要るか**: MSVC でビルドするには、先に**コンパイラの環境(PATH・ライブラリ)**を整える `vcvars64.bat` が要る。毎回手で打たないために包む。
- `%~dp0` はこのバッチのあるフォルダ。`%*` は残りの引数全部。`exit /b %ERRORLEVEL%` で**ビルドの成否を呼び出し元に伝える**。
- **ASCII のみ**で書く(cmd はシステムのコードページで読み、日本語は誤動作する)。
- Git Bash から呼ぶときは `cmd.exe //c tools\build_dev.cmd narration_tests`(`/c` が変換されるため)。

使い方: `tools\build_dev.cmd`(全部)/ `tools\build_dev.cmd narration_tests`(特定のターゲットだけ)。

---

## 17.5 GitHub Actions(自動のビルド+テスト)

`.github/workflows/build.yml`(`main` への push と PR で動く)

| 手順 | 中身 | 理由 |
|---|---|---|
| `actions/checkout@v4` | ソースを取得 | |
| `ilammy/msvc-dev-cmd@v1` | MSVC の環境を整える | `vcvars64.bat` の CI 版 |
| Bootstrap vcpkg | vcpkg を取得し `VCPKG_ROOT` を設定 | ランナーに無い |
| `cmake --preset ci` → `--build --preset ci` | 設定とビルド | |
| `ctest --output-on-failure` | **GPU・画面を要さない**テストだけ実行 | GitHub のランナーに GPU は無い |

`.github/workflows/lint.yml`: `clang-format --dry-run --Werror`(書式)と `clang-tidy`(静的解析)。**`continue-on-error: true`**(失敗しても全体を赤にしない=参考情報)。

**CI でできないこと**(正直に): 実際の動画生成(GPU)、QML の描画、GUI。これらは手元で確かめる(第11章)。**CI が緑でも「動画が作れる」とは言えない**。

---

## 17.6 インストーラー(Inno Setup、`installer/ragreel.iss`)

| 設定 | 意味 |
|---|---|
| `PrivilegesRequired=lowest` + `…OverridesAllowed=dialog` | 管理者権限なしで入る(既定は `%LOCALAPPDATA%\Programs\RAGReel`)。「すべてのユーザー」を選べるが、`Program Files` では**動画の保存で権限エラー**になる |
| `[Files]` | 入れるファイルの一覧(exe・DLL・Qt プラグイン・`assets`・画面一式) |
| `Flags: ignoreversion` | 常に上書き(画面は常に最新に) |
| `manifest.json` の `onlyifdoesntexist` | **すでにある一覧を消さない**(再インストールで利用者の動画を失わない) |
| VC ランタイムの同梱(`VcCrtDir`) | 入っていない PC でも起動できる(app-local 配布は許可されている)。場所は環境変数 `RAGREEL_VC_CRT_DIR` で変えられる |
| `AppId`(GUID) | **変えない**(アップグレード時に同じアプリと認識させる) |

**触るときの注意**
- 新しい画面ファイルは、CMake の一覧と **`[Files]` の両方**へ(第13章)。
- バージョンは `MyAppVersion`(現行 1.1.0)。出力は `installer/Output/RAGReel-Setup-1.1.0.exe`(Git 管理外)。
- `{app}` のパスが長いと(約 250 文字)コピーが失敗する。

---

## 17.7 wrangler(Cloudflare Worker の設定と操作)

`cloudflare/ragreel-gallery/wrangler.jsonc`

```jsonc
{ "name": "ragreel-gallery", "main": "src/index.js", "compatibility_date": "2025-01-01",
  "assets": { "directory": "../../web/public", "binding": "ASSETS", "not_found_handling": "404-page",
              "run_worker_first": ["/api/*", "/manifest.json", "/videos/*/metadata.json",
                                   "/videos/*/video.webm", "/videos/*/thumb.png"] },
  "r2_buckets": [{ "binding": "GALLERY_BUCKET", "bucket_name": "ragreel-gallery" }] }
```

| 項目 | 意味 |
|---|---|
| `main` | Worker の入口 |
| `compatibility_date` | 実行環境の挙動の版。**未来は不可** |
| `assets.directory` | 静的アセットとして上げるフォルダ(`web/public`) |
| `run_worker_first` | これらのパスだけ、静的アセットより先に Worker が受ける(第9章) |
| `r2_buckets` | R2 を `env.GALLERY_BUCKET` として使えるようにする |

`web/public/.assetsignore`: アセットに**上げないもの**(`videos/`・`manifest.json`・`manifest.js`)。**利用者の動画ライブラリを誤って静的アセットとして公開しない**ための防波堤。

**よく使うコマンド**
```powershell
cd cloudflare\ragreel-gallery
npx wrangler dev                      # ローカルで動かす(ローカルの R2 付き)
npx wrangler deploy                   # 本番へ(要確認の操作)
npx wrangler secret put UPLOAD_TOKEN  # 秘密を入力(画面にも履歴にも残さない)
```
- **`deploy` は本番に反映される**。手順: ローカル確認 → デプロイ → HTTP で確認(第9章)。
- 秘密は `secret put` で。`wrangler.jsonc` や環境変数のファイルに**値を書かない**(公開リポジトリ)。

---

## 17.8 Node / Python の検査ツール

| ツール | 場所 | 中身 |
|---|---|---|
| `check_contrast.js` | `tools/` | CSS を読んで、トークンの色からコントラスト比を計算(第16章 16.11)。ライト/ダークの**2か所の値のずれ(DRIFT)**も検出 |
| `check_gallery_layout.js` | `tools/` | `pickColumns` を 1〜80本 × 5つの幅で総当たりして、最終段が1枚だけにならないことを確認 |
| `build_diagrams.py` | `tools/` | `docs/diagrams/*.svg` を生成(第12章 レシピ8) |
| `compare/tests` | `compare/` | `python -m unittest discover -s tests`(25件) |

**読み方の例**(`check_contrast.js` の骨格)
1. `styles.css` を文字列として読む(`fs.readFileSync`)。
2. 正規表現で `--変数: 値;` を取り出し、ライト(`:root`)とダーク(上書き)の辞書を作る。
3. `PAIRS`(前景トークン・背景トークン・最小比)を順に計算し、基準未満なら `process.exit(1)`(失敗)。

**終了コード**: 0 = 成功、非 0 = 失敗。**CI やスクリプトは、出力でなく終了コードで成否を判定する**。

---

## 17.9 Git とリポジトリの運用

| 項目 | 内容 |
|---|---|
| ブランチ | `main` に直接(個人開発)。大きな変更はまとめて1コミットにせず、意味のある単位で |
| コミットメッセージ | 平易な英語の1行目(何をしたか)+ 必要なら箇条書き |
| `.gitignore` | `build/`・`*.mp4` `*.webm` `*.wav`・`compare/out/`・`.wrangler/`・`web-production-skill/` など(生成物・大きなファイル・秘密) |
| **公開リポジトリ** | 鍵・トークン・アカウント情報を入れない。**入れてしまったら、まず無効化・再発行**(履歴の書き換えより先) |
| 改行 | `core.autocrlf=true`: 作業フォルダは CRLF、リポジトリは LF。「LF will be replaced by CRLF」の警告は通常のメッセージ |
| 大きなファイル | 動画は Git に入れない(`.gitignore`)。必要なものはデプロイ時に静的アセットとして上げる |

---

## 17.10 運用の流れ(総まとめ)

**機能を足して配るまで**
1. ブランチ(または作業コピー)で変更 → 該当の**単体テスト**を足す(第11章)
2. `tools\build_dev.cmd <ターゲット>` でビルド → `ctest`(または `node`/`unittest`)
3. `--mock` で通しを実行 → `output/` をブラウザで確認(旧い動画・空・ダーク)
4. ドキュメントと図を更新(`python tools/build_diagrams.py`)
5. コミット → プッシュ(CI が緑か確認。ただし CI は GPU を要する部分を見ない)
6. 画面を変えたなら共有ギャラリーへ(`wrangler deploy` → HTTP で確認)
7. 配布するなら、バージョンを上げて Inno Setup でインストーラーを作る → **別の PC(または別ユーザー)で入れて起動・生成を確認**

## 演習

1. 新しいダッシュボードのファイル `stats.html` を足した。変更すべき**3か所**を言え。
2. CI が緑だったが、手元の実機で動画が作れなかった。なぜありうるか。
3. 秘密のトークンを誤ってコミットした。取るべき順序は。
4. `build_dev.cmd` に日本語のコメントを書くと、何が起きうるか。

## 解答(例)

1. `engine/CMakeLists.txt` の `_dashboard_file` の一覧、`installer/ragreel.iss` の `[Files]`、(Worker で扱う動的ルートがあれば)`wrangler.jsonc` の `run_worker_first`。共有ギャラリーだけなら Worker 側は不要(`web/public/` が丸ごと上がる)。
2. CI はビルドと GPU 不要のテストだけで、実際の描画・エンコード・音声・ネットには触れないため。VP9 の機能の有無・GPU・Qt プラグインの欠落などは手元でないと見つからない。
3. ①トークンを**無効化して再発行**(公開された時点で漏れたものとして扱う)→ ②追跡から外し `.gitignore` に追加して、新しいコミットで消す → ③必要なら履歴の書き換えを検討(影響が大きいので別の判断)。
4. cmd がシステムのコードページ(日本語 Windows では CP932)で読むため、UTF-8 の日本語が文字化けし、行が壊れてコマンドとして誤動作することがある。
