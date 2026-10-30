# 第15章 言語とスクリプトの基礎(C++/Qt・QML・JavaScript・Python・シェル)

**学習目標**: RAGReel を読み書きするのに要る言語の基礎を、**このリポジトリの実コード**で身につける。「この書き方は何で、なぜここで使うか」を説明できる。
各節は **①要点 → ②リポジトリの実例 → ③読み方のコツ → ④練習** の順。すでに知っている言語は飛ばしてよい。

> 先に第2章(プロセス・HTTP・JSON・ビルド)を読んでおくと楽。

---

## 15.1 C++ の基礎(エンジンの言語)

### ① 要点
- **ヘッダ(`.h`)と実装(`.cpp`)に分ける**。ヘッダは「何ができるか(宣言)」、実装は「どうやるか」。`#pragma once` は同じヘッダを2回読まない印。
- **型は静的**。`int`、`double`、`bool`、`QString`(Qt の文字列)、`std::vector<T>`(可変長の配列)。
- **`const`** は「変更しない」約束。引数を `const QString&`(参照)で受けると、コピーせず読むだけにできる。
- **`constexpr`** はコンパイル時に決まる定数(例: `constexpr int kFps = 15;`)。
- **`enum class`** は名前つきの選択肢(例: `JobStage::Ingest`)。
- **例外**(`throw` / `try-catch`)で失敗を伝える。

### ② 実例
`orchestrator/job_pipeline.h`(列挙型と、文字列を返す小さな関数):

```cpp
enum class JobStage { Ingest, Compose, Narrate, Assemble, Render, Encode, Publish };

inline const char* jobStageKey(JobStage stage) {
    switch (stage) {
        case JobStage::Ingest: return "ingest";
        // ...
    }
    return "";
}
```
`switch` で全ての値を並べると、**値を足したときにコンパイラが警告してくれる**(対応の書き忘れに気づける)。

`main_cloudrag.cpp` の引数処理(参照で受けて、見つけたら取り除く):

```cpp
QString takeFlagValue(QStringList& args, const QString& flag) {
    const int idx = args.indexOf(flag);
    if (idx < 0 || idx + 1 >= args.size()) return QString();   // 無い/値が無い → 空
    const QString value = args.at(idx + 1);
    args.removeAt(idx + 1);
    args.removeAt(idx);
    return value;
}
```
`QStringList& args`(`const` なし)は「この関数が**書き換える**」印。呼ぶ側は `takeFlagValue(args, "--houdini-md")` で、`args` から該当の2語が消える。

### ③ 読み方のコツ
`型 名前(引数) { … }` が関数。まず**戻り値と引数の型**で何をするかを推測し、中身は後で読む。

---

## 15.2 C++ の「所有」と RAII(最重要の考え方)

### ① 要点
- **RAII**: 資源(メモリ・ファイル・ロック・GPU の札)の**確保をコンストラクタ、解放をデストラクタ**に結びつける。スコープを抜ければ、例外が出ても自動で解放される。
- **`std::unique_ptr<T>`**: 「このオブジェクトの持ち主は1人」を表す。持ち主が消えると中身も消える。コピーできず、**ムーブ(持ち主の移動)**だけできる。
- **ムーブ**: 中身を「盗む」ことで、コピーより安く渡す。盗まれた側は空になる。

### ② 実例
`orchestrator/resource_budget_manager.cpp`(GPU の貸出札):

```cpp
GpuLease::GpuLease(GpuLease&& other) noexcept
    : manager_(std::exchange(other.manager_, nullptr)), owner_(other.owner_) {}

GpuLease::~GpuLease() {
    if (manager_) manager_->release();     // 持っている間だけ解放する責任がある
}
```
- `GpuLease&&` は「ムーブ元」。`std::exchange(a, 新)` は「a の今の値を返し、a を新に置き換える」。**盗まれた札は `nullptr` になり、二重に解放されない**。
- コピーは `= delete` で禁止(同じ札が2枚あってはいけない)。

`encode/video_encoder.h`(FFmpeg の C 資源を安全に包む):

```cpp
struct AVFormatContextDeleter { void operator()(AVFormatContext* ctx) const; };
using AVFormatContextPtr = std::unique_ptr<AVFormatContext, AVFormatContextDeleter>;
```
`unique_ptr` に**専用の解放関数**を持たせる。FFmpeg の `avformat_free_context` を呼び忘れる(メモリリーク)ことが無くなる。

### ③ 読み方のコツ
`new` / `delete` を直接書いている箇所は少ない。**`unique_ptr`・`std::vector`・スコープで自動解放**が基本。`[[nodiscard]]`(`acquireGpuLease`)は「戻り値を捨てるな」(札を受け取らないと即返却されてしまう)。

### ④ 練習
1. `GpuLease` のコピーを禁止するのはなぜか。
2. `std::exchange(other.manager_, nullptr)` を書かずに単に `manager_ = other.manager_` にすると何が起きるか。

---

## 15.3 C++ の並行処理(mutex と condition_variable)

### ① 要点
- **mutex**: 同時に1つのスレッドだけが入れる「鍵」。`std::unique_lock` / `std::lock_guard` で、スコープに結びつけて使う。
- **condition_variable**: 「条件が成り立つまで寝て待つ」仕組み。**必ず条件(述語)とセットで使う**(目が覚めても条件が偽なら再び寝る。偽の起床対策)。

### ② 実例
```cpp
GpuLease ResourceBudgetManager::acquireGpuLease(GpuLeaseOwner owner) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return !leaseHeld_; });   // 誰も持っていなくなるまで待つ
    leaseHeld_ = true;
    return GpuLease(this, owner);
}

void ResourceBudgetManager::release() {
    { std::lock_guard<std::mutex> lock(mutex_); leaseHeld_ = false; }
    cv_.notify_one();                                  // 待っている1人を起こす
}
```
- `[this] { return !leaseHeld_; }` は**ラムダ式**(その場で書く小さな関数)。`[this]` は「自分のメンバを使う」。
- 通知は**ロックを外してから**(`{ … }` のブロックを閉じた後)。起きた側がすぐロックを取れる。

### ④ 練習
1. 述語なしの `cv_.wait(lock)` だと何が危ないか。
2. 札を2枚同時に取ろうとする(同じスレッドで2回 `acquire`)とどうなるか。

---

## 15.4 Qt の基礎

### ① 要点
- **`QString`/`QStringList`/`QByteArray`**: Qt の文字列・文字列の列・バイト列。`QStringLiteral("…")` は文字列リテラルを効率よく `QString` にする。
- **イベントループ**: Qt は「イベント(通信の完了・タイマー・クリック)を待つ」中心の仕組み。`QCoreApplication` が必要(通信や正規表現でも要求されることがある)。
- **シグナルとスロット**: 「何かが起きた」(シグナル)に、処理(スロット)をつなぐ。GUI の `ProcessRunner` は子プロセスの出力が来るたびにシグナルでログを足す。
- **`QProcess`**(子プロセス)、**`QSettings`**(設定の保存)、**`QJsonDocument`**(JSON)、**`QNetworkAccessManager`**(HTTP)、**`QRegularExpression`**(正規表現)。

### ② 実例
`launcher/process_runner.cpp`:

```cpp
const QRegularExpression& renderProgressRegex() {
    static const QRegularExpression re(QStringLiteral("^Rendered frame (\\d+) / (\\d+)$"));
    return re;
}
```
- `static const` の関数内変数は**最初に呼ばれたとき1回だけ作る**(毎回コンパイルしない)。
- C++ の文字列の中で `\d` を表すには `\\d`(バックスラッシュを2つ)。**正規表現のエスケープは二重になる**。
- このログ行 `Rendered frame 120 / 3367` を読んで進捗(%)を計算する。**ログの文言と正規表現が契約**(第13章)。

Windows 固有: `CreateJobObjectW` + `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` で、親が落ちても子(ffmpeg など)が**孤児として残らない**ようにしている(`#ifdef Q_OS_WIN` で Windows だけ)。

### ④ 練習
1. 正規表現 `^Rendered frame (\d+) / (\d+)$` で、何がマッチし、`(\d+)` の2つは何を取り出すか。
2. ログの出力を `Rendered frame 120/3367`(スペース無し)に変えると何が起きるか。

---

## 15.5 QML の基礎(動画の見た目を書く言語)

### ① 要点
- **宣言的**: 「どう描くか」でなく「**何がどこにあるか**」を書く。`Rectangle { width: 1280; color: "#0c0f0d" }`。
- **プロパティと束縛(バインディング)**: `property real progress: 0.0` は値。他の式が `progress` を使うと、**値が変わると自動で再計算**される(手で更新しない)。
- **C++ から値を渡す**: ルートオブジェクトの `setProperty("progress", 0.37)` で、見た目が変わる。RAGReel は**1フレームごとに値を渡して描く**(第7章)。

### ② 実例
`engine/qml/CloudRagScene.qml` の冒頭:

```qml
Rectangle {
    id: root
    width: 1280
    height: 720
    property real progress: 0.0        // 動画全体の進み具合(下部タイムラインの塗り)
    property string slideHeading: ""   // 今のスライドの見出し
    // 見出しの Text は slideHeading を参照するだけ。値が変われば自動で描き直される
}
```

### ③ 読み方のコツ
QML の先頭のコメントに、**C++ から渡す値の一覧と意味**が書いてある(静的な値 / フレームごとの値)。QML を変えるときは、その表と `SceneAssembler` の `StaticProperties`/`FrameProperties` を一緒に見る。

### ④ 練習
1. QML 側で `Timer` を使って時間を進めない理由は(第7章を踏まえて)。
2. 新しい要点 `slideBullet3` を足すには、どこを変えるか。

---

## 15.6 JavaScript の基礎(ダッシュボードと Worker の言語)

### ① 要点
- **`const` / `let`**(`var` は避ける)。**アロー関数** `(x) => x + 1`。**テンプレートリテラル** `` `${v.title}` ``(文字列に式を埋め込む)。
- **非同期**: ネットやファイルの待ちは **`Promise`**。**`async` / `await`** で「待つ」を普通の順序で書く。
- **DOM**: `document.getElementById`、`element.innerHTML`(HTML を入れる)、`addEventListener`(イベント)。
- **classic script と module の違い**: `<script src>` はグローバルに関数が増える。`import` を使う module は `file://` で読めない(第8章)。**このプロジェクトは classic だけ**。
- **`localStorage`**: ブラウザに小さな設定を保存(`vf_theme` など)。**読み書きは `try-catch`**(禁止環境では例外が出る)。

### ② 実例
`web/public/app.js`:

```js
function loadScript(src) {                       // <script> を足して、読み終わるまで待つ Promise を返す
  return new Promise((resolve, reject) => {
    const el = document.createElement("script");
    el.src = src;
    el.onload = () => resolve();
    el.onerror = () => reject(new Error(`Failed to load ${src}`));
    document.head.appendChild(el);
  });
}

async function loadManifest() {
  if (!IS_LOCAL_FILE) {                          // https: 普通に fetch
    const res = await fetch("manifest.json", { cache: "no-store" });
    if (!res.ok) throw new Error(`Failed to load manifest.json: ${res.status}`);
    return res.json();
  }
  try { await loadScript("manifest.js"); }       // file://: script タグで読む
  catch (e) { return []; }                       // 初回(まだ無い)は空
  return window.__VF_MANIFEST__;
}
```
読み方: `await` は「Promise が終わるまでここで待つ(他は動く)」。`throw` した例外は、呼び出し側の `try/catch` か `.catch()` で受ける。

安全に HTML を作る(`escapeHTML`):

```js
card.innerHTML = `<h3>${escapeHTML(v.title)}</h3>`;   // タイトルは他人が書いたものかもしれない
```
`innerHTML` に生の文字列を入れると、`<img onerror=…>` で**スクリプトが動く**(XSS)。必ず `escapeHTML` を通す。

Worker(`cloudflare/ragreel-gallery/src/index.js`)も同じ JavaScript だが、実行場所が違う:
- ブラウザ: `window`・`document` がある。
- Worker: `Request`/`Response`/`env`(バインディング: R2 など)を受け取る関数 `fetch(request, env, ctx)`。`document` は無い。

### ④ 練習
1. `loadScript` の `onerror` で `reject` するのはなぜか。呼ぶ側はどう受けるか。
2. 次の文字列を `innerHTML` に直接入れると何が起きるか: `<img src=x onerror=alert(1)>`。
3. Worker のコードに `document.getElementById` を書くとどうなるか。

---

## 15.7 CSS の基礎(見た目のルール)

- **カスタムプロパティ(変数)** `--orange: #ff7a45;` を `var(--orange)` で使う。**色の役割(第8章)はここで一元管理**する。
- **テーマ**: ライトは `:root`、ダークは `:root[data-theme="dark"]` と `@media (prefers-color-scheme: dark)`(`:root:not([data-theme="light"])`)の**2か所**。値がずれないよう `check_contrast.js` が検出する。
- **`@media (prefers-reduced-motion: reduce)`**: 「動きを減らす」設定の人にアニメーションを止める。全体に `animation: none !important` をかけている。
- **`font-variant-numeric: tabular-nums`**: 数字の幅をそろえる(桁が揃って見える)。

---

## 15.8 Python の基礎(比較実装とツール)

### ① 要点
- **インデントが構文**。型は動的(ヒントは書ける)。`unittest` は標準のテスト。
- **numpy**: 配列を**一括(ベクトル化)**で計算する。for ループで画素を回すより桁違いに速い。
- **`functools.lru_cache`**: 同じ引数の結果を覚えておく(画素ごとの地図を1回だけ作る)。

### ② 実例
`compare/python_render/engine.py`:

```python
@lru_cache(maxsize=2)
def mask_thresholds(w: int, h: int) -> np.ndarray:
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)           # 各画素の座標の2次元配列
    return (0.7 * x / max(1, w - 1) + 0.3 * y / max(1, h - 1)).astype(np.float32)

def _reveal(a, b, thr, p, feather):
    if p <= 0: return a
    if p >= 1: return b
    wgt = np.clip((p * (1.0 + feather) - thr) / feather, 0.0, 1.0)[:, :, None]
    return np.rint(a.astype(np.float32) * (1 - wgt) + b.astype(np.float32) * wgt).astype(np.uint8)
```
- `np.mgrid[0:h, 0:w]` は座標の格子。`x / (w-1)` は**配列全体に一度に**割り算。
- `[:, :, None]` は形を `(h, w)` → `(h, w, 1)` に(RGB の3チャネルに**ブロードキャスト**するため)。
- `np.rint` で丸めて `uint8`(0〜255)に。端点を早期 return にして**厳密**にしている(第10章)。

### ③ 読み方のコツ
配列の**形(shape)**を追う。`(h, w)` の地図と `(h, w, 3)` の絵が、どの時点で何の形かを紙に書くと読める。

### ④ 練習
1. `lru_cache` の `maxsize=2` は何を意味し、解像度が3種類交互に来ると何が起きるか。
2. `wgt` を `[:, :, None]` にしないと、どんなエラーになるか(予想せよ)。

---

## 15.9 JSON・正規表現の基礎

### JSON
- 値は**文字列・数値・真偽・null・配列 `[]`・オブジェクト `{}`**だけ。コメントは書けない。末尾のカンマも不可。
- **C++ と JavaScript の契約**になる(第8章)。キーの綴り・単位(秒か ms か)を揃える。

### 正規表現(このプロジェクトで使うもの)
| 目的 | 式 | 読み方 |
|---|---|---|
| `##` 見出しを探す | `(?m)^##\s+(.+)$` | `(?m)`=行ごとに `^`/`$`。`##` + 空白 + 見出し文字(キャプチャ) |
| 空行(段落)で割る | `\n{2,}` | 改行が2つ以上 |
| コードフェンス | `` ```[a-zA-Z0-9]*\n[\s\S]*?``` `` | ` ``` ` + 言語名 + 改行 + **最短一致**の中身 + ` ``` `。`[\s\S]` は改行も含む任意の文字、`*?` は最短 |
| 進捗のログ | `^Rendered frame (\d+) / (\d+)$` | 数字2つを取り出す |

**注意**: 欲張り(`.*`)と最短(`.*?`)の違いで、「最初の ``` から**最後**の ``` まで」を取ってしまう事故が起きる。コードフェンスは最短一致にする。

---

## 15.10 シェルの基礎(PowerShell / Git Bash / cmd)

| | PowerShell | Git Bash(POSIX) | cmd |
|---|---|---|---|
| 環境変数を設定 | `$env:CLOUD_RAG_URL = "…"` | `export CLOUD_RAG_URL="…"` | `set CLOUD_RAG_URL=…` |
| 変数を読む | `$env:NAME` | `$NAME` | `%NAME%` |
| 連続実行 | `;` | `&&`(成功時のみ) | `&` |
| パス区切り | `\`(`/` も多くは可) | `/` | `\` |

- **Git Bash から `cmd.exe /c` を呼ぶと、`/c` が MSYS にパスと誤認されて壊れる** → `cmd.exe //c`、またはバッチファイル(`tools/build_dev.cmd`)にする(第13章)。
- **`.cmd` は ASCII のみ**にする(cmd はシステムのコードページで読むので、日本語コメントは文字化け・誤動作のもと)。
- クォート: PowerShell は `'…'`(展開しない)と `"…"`(展開する)。パスに空白があれば必ず引用符で囲む。
- **秘密を引数に直接書かない**(履歴に残る)。環境変数かプロンプト(`wrangler secret put`)を使う。

---

## 15.11 総合練習

1. `main_cloudrag.cpp` で `--mock` が指定されたかを、`takeFlagValue` ではなく `args.removeAll("--mock") > 0` で判定している。**値を取らない旗**(スイッチ)と**値を取る旗**で関数を使い分ける理由を述べよ。
2. `app.js` の `loadScript` を、`async/await` ではなく Promise の `.then/.catch` で書き直せ。
3. numpy で「画素の明るさ(0〜255)が 128 以上なら 255、未満なら 0」にする1行を書け。

## 解答(例)

**15.11 総合練習**
1. `--houdini-md <パス>` は2語で1つの設定なので「値を取り除く」関数が要る。`--mock` は有無だけなので、`removeAll` の戻り値(取り除いた個数 > 0)で足りる。
2. `loadScript("manifest.js").then(() => window.__VF_MANIFEST__).catch(() => [])`
3. `np.where(img >= 128, 255, 0).astype(np.uint8)`

**各節の練習**
- 15.2-1: 同じ札が2枚あると、片方が解放されたのに他方が「持っている」と思い込み、排他が壊れるため。
- 15.2-2: 両方のオブジェクトが解放を試み、**二重解放**になる(別の処理の札を勝手に返してしまう)。
- 15.3-1: 述語が無いと、通知が先に来たときに**永遠に寝る**/偽の起床で条件が偽のまま進む。
- 15.3-2: 自分で自分の返却を待って**永遠に止まる**(デッドロック)。
- 15.4-1: `Rendered frame` + 数字 + ` / ` + 数字 の行。2つの数字は「今のフレーム」と「全フレーム数」。
- 15.4-2: パターンに合わず、進捗が更新されない。
- 15.5-1: 時間の状態を QML に持たせると再生速度に依存して**再現できない**ため、エンジンが値を渡す。
- 15.5-2: `SceneAssembler::FrameProperties` に項目を足し、QML に `property string slideBullet3` と表示を足し、`main_cloudrag.cpp` で値を渡す(`Slide` にも項目が要る)。
- 15.6-1: 読み込み失敗を呼ぶ側に伝えるため。呼ぶ側は `try/catch`(`await` の場合)で受け、初回なら空として扱う。
- 15.6-2: スクリプトが実行される(XSS)。
- 15.6-3: `ReferenceError`(Worker に `document` は無い)。
- 15.8-1: 最後の2つの解像度の地図だけ覚える。3種を交互に使うと毎回作り直しになり、速度が落ちる。
- 15.8-2: `(h, w)` と `(h, w, 3)` の形が合わず、ブロードキャストできない(`ValueError`)。
