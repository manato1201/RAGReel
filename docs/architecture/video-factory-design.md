# RAG/DB駆動型チュートリアル動画生成ファクトリー — アーキテクチャ設計書

**対象リポジトリ:** `LearningQt`(現状コードなし。本設計に基づき `engine/`・`web/` を新設する)
**上流連携先:** `GameDevelopment\DevelopmentRAGEnvironment`(稼働中、変更しない)
**開発機GPU:** NVIDIA RTX 3070 / VRAM 8GB — §3のVRAM競合設計の前提とする
**ステータス:** 設計確定・実装着手前(Phase 0)

---

## 目次

1. [システム境界とデータフロー](#1-システム境界とデータフロー)
2. [Qt/C++ヘッドレスレンダリングエンジンの内部構造](#2-qtc-ヘッドレスレンダリングエンジンの内部構造)
3. [VRAM/スレッド競合設計](#3-vramスレッド競合設計最重要リスク)
4. [FFmpegメモリ管理](#4-ffmpegメモリ管理)
5. [Webダッシュボード設計](#5-webダッシュボード設計)
6. [自己改善型エージェント構想](#6-自己改善型エージェント構想phase-2以降)
7. [リポジトリ構成案](#7-リポジトリ構成案)
8. [フェーズロードマップ](#8-フェーズロードマップ)
9. [.gitignore方針](#9-gitignore方針)

---

## 1. システム境界とデータフロー

3つの独立デプロイ可能なシステムを、**ファイルペア契約**(md+json)と**HTTP契約**(既存の`:8766`ブリッジ)の2本で接続する。`DevelopmentRAGEnvironment`側には一切変更を加えず、純粋な上流プロデューサーとして扱う。

```
┌──────────────────────────────────────────────────────────────────┐
│ DevelopmentRAGEnvironment (既存・上流・Python)                     │
│                                                                     │
│  ChromaDB(data/chroma/) ── rag_local_bridge.py :8766               │
│                              (HTTP+JSON, X-API-Key認証)             │
│                                                                     │
│  houdini/python_panels/tutorial_agent.py                          │
│  (Claude Sonnet 4.6 エージェントループ)                             │
│         │ ユーザーが「保存」を押すと書き込み                          │
│         ▼                                                          │
│  localRAG/tutorials/<slug>_<date>.md   (チュートリアル本文+出典)     │
│  localRAG/tutorials/<slug>_<date>.json (NodeGraphAsset)            │
│         │                                                          │
│         └─ auto_index.py (watchdog) が自動的にChromaDBへ再インデックス │
│            (自己拡張ループ、既存)                                     │
└───────────────┬───────────────────────┬───────────────────────────┘
                 │ ファイルベース(pull)    │ HTTP(pull・必要時のみ)
                 │ .md/.json ペア          │ /search, /query, /api/*
                 ▼                        ▼
┌──────────────────────────────────────────────────────────────────┐
│ Qtビデオファクトリー (新規・本リポジトリ・C++/QML・ヘッドレス・単一プロセス) │
│                                                                     │
│  IngestWatcher ──pull──▶ ScriptComposer ──▶ SceneAssembler (QML)   │
│  (localRAG/tutorials/を   (md+json → ShotList     ──▶ QQuickRenderControl│
│   ポーリング/手動起動)      + ナレーション文)         + QOffscreenSurface│
│                                  │                    (画面非表示の    │
│                            NarrationEngine              GPU描画)     │
│                            (llama.cpp,                     │        │
│                             テキスト整形)              フレームバッファ  │
│                                  │                    (ゼロコピー経路) │
│                            VectorStoreClient                │       │
│                            (HTTPクライアント→:8766          ▼       │
│                             埋め込みFaissは持たない)   VideoEncoder   │
│                                                        (FFmpeg mux, │
│                                                         RAIIラッパー)│
│                                  │                           │      │
│                            Orchestrator/JobPipeline ─────────┘      │
│                            (フェーズ制御・ResourceBudgetManager所有)  │
└───────────────┬────────────────────────────────────────────────────┘
                 │ ファイルベース(push)
                 ▼  書き出し: <slug>.mp4, <slug>_thumb.jpg, manifest.jsonエントリ
┌──────────────────────────────────────────────────────────────────┐
│ Webダッシュボード (新規・本リポジトリ・静的HTML/CSS/JS・web-production-skillで構築)│
│                                                                     │
│  ギャラリー/動画詳細ページは manifest.json + 動画 + サムネイルを静的に読む│
│  (pull・バックエンド不要)。動的検索が必要な場合のみブラウザから直接       │
│  RAGブリッジ:8766を叩く(CORS有効・新規バックエンドは作らない)          │
└──────────────────────────────────────────────────────────────────┘
```

### RAGコンテンツが動画コンテンツになる変換過程(Qtファクトリー内部で完結)

`tutorial.md`(本文+出典)+ `tutorial.json`(NodeGraphAsset: nodes/edges/params/position)
→ `ScriptComposer` が両者をマージし、順序付き**ShotList**(タイトルカード・md見出しに対応したノードグラフの段階的リビール・出典カードなど)+ 各ショットのナレーション文を生成
→ `NarrationEngine`(llama.cpp)がナレーション文をTTS向けに整形(将来的な実際のTTS音声合成は別コンポーネントとして扱い、本設計では文字整形までをスコープとする)
→ `SceneAssembler` がShotList+NodeGraphAssetのジオメトリからQMLシーン状態を構築(チュートリアルごとの手書きQMLは作らず、データ駆動の汎用テンプレート1つで賄う)
→ `QQuickRenderControl`/`QOffscreenSurface` が各フレームをオフスクリーンでテクスチャ/FBOへ描画
→ そのテクスチャをCPU側コピーを挟まず(または最小限のリードバックで)`VideoEncoder`へ渡す
→ FFmpegが`.mp4`へmux、サムネイルを1枚抽出
→ `manifest.json`エントリ+動画別メタデータを書き出す

### Pull/Push整理

- Qtファクトリーが**pull**するもの: `localRAG/tutorials/`の`.md`/`.json`ペア(ファイルベース)、必要時のみRAGブリッジへのHTTP pull(`/search`, `/query`)
- Qtファクトリーは`DevelopmentRAGEnvironment`へ**Phase 1〜4では何もpushしない**。§6でPhase 5以降のオプトインpush(動画トランスクリプトの書き戻し)を、既存のファイルドロップ+watchdog方式のまま提案する(新APIは作らない)
- Webダッシュボードは Qtファクトリーの出力ディレクトリ(静的ファイル)をpullし、任意でRAGブリッジへ直接pullする。Qtファクトリー自体はサーバープロセスではなく(バッチ/CLI起動のパイプライン)、ダッシュボードから直接話しかけることはない

---

## 2. Qt/C++ヘッドレスレンダリングエンジンの内部構造

### モジュール/クラス分割

| コンポーネント | 責務 |
|---|---|
| `Orchestrator` / `JobPipeline` | 最上位ドライバ。1ジョブ(1チュートリアル)を固定フェーズ(Ingest→Compose→Narrate→Assemble→Render→Encode→Publish)で駆動し、`ResourceBudgetManager`(§3)を所有。フェーズ単位の失敗/リトライ処理、完了時のmanifestエントリ書き出しを担う。Phase 1〜3では1ジョブ=1プロセス起動とする(リソース解放の観点で最も安全・単純) |
| `IngestWatcher` | `localRAG/tutorials/`内の未処理`.md`/`.json`ペアを検出(処理済み台帳方式。常駐ファイル監視は不要で、Phase 5以降の任意強化とする) |
| `ScriptComposer` | markdown(frontmatter+見出しセクション)とNodeGraphAsset JSONを内部`ShotList`(順序付き`Shot`構造体: 種別・時間ヒント・ナレーション文・参照ノードID)へ変換。Qt/GPU非依存の純粋なデータ変換で、単体テストが容易 |
| `NarrationEngine` | llama.cpp C APIの薄いラッパー。上流Claudeエージェントが既に大筋を書いたナレーション文を、TTS向けに正規化するテキスト整形ステップ(オープンエンドな生成ではない) |
| `VectorStoreClient` | `rag_local_bridge.py:8766`へのHTTPクライアント(`/search`, `/query`, `X-API-Key`ヘッダ、既存認証をそのまま利用)。markdownに含まれない補足文脈が必要な場合のみ使用。**埋め込みFaissインデックスは持たない(理由は下記)** |
| `SceneAssembler` | `QQuickRenderControl`+オフスクリーン`QQuickWindow`+`QOffscreenSurface`を所有。ShotList+NodeGraphAssetからQMLシーン状態を手続き的に構築(チュートリアル毎の手書きQMLなし、汎用テンプレート1本をデータ駆動) |
| `VideoEncoder` | libavformat/libavcodecのRAIIラッパー(§4)。レンダリング済みフレーム(理想はGPUテクスチャ/ハードウェアフレーム)を受け取りエンコード・mux |
| `ResourceBudgetManager` | §3で詳述する横断的ゲートキーパー。`NarrationEngine`と`SceneAssembler`/`VideoEncoder`間のGPU/VRAMアクセスを調停 |
| `ManifestWriter` | 動画別メタデータJSONと集約`manifest.json`(§5のスキーマ)を書き出す |

### 設計判断: FaissをC++に埋め込まず、既存HTTPブリッジをそのまま使う

明確な結論として採用する(選択肢の列挙で終わらせない):

- 既存のベクトル検索スタック(ChromaDB+ハイブリッドBM25+日本語トークナイズ、`multilingual-e5-large`埋め込み)はPython側で既に成熟・実運用中で、UnityとHoudiniが同じHTTP+JSON+APIキーパターンで既に利用している。Qtファクトリーを3つ目のHTTPクライアントとして追加するのはリスクがほぼゼロで、一貫性があり、既存の認証/監査/PEP(ポリシー実施点)層をそのまま享受できる。
- C++でFaissを別途持つと、Python ChromaDBとの**二重インデックス**を抱え、watchdog自動インデクサとの同期問題を新たに設計・維持する必要が生じる。この負担を負う理由がない。
- Qtファクトリー側の検索ニーズは狭く低頻度(1ジョブあたり多くて数回の補助的な検索)。LLM推論や動画エンコードがジョブの実行時間を桁違いに支配するため、`localhost`へのHTTP往復(ミリ秒オーダー)がボトルネックになるレイテンシ予算は存在しない。
- 既存ブリッジは`/search`(生ベクトル検索)・`/query`(RAG+回答)・`/api/namespaces`を既に公開しており、`docs/content-generation.md`§5のnamespaceホワイトリスト/ライセンス遵守ルールも内包している。C++側で検索を再実装するとこのガバナンス層を丸ごと迂回してしまう。
- Faissが正当化されるのは、対象マシンにPythonランタイムが一切無い完全オフライン要件がある場合のみだが、これは要件として存在しない(同一マシンで既にPython RAGスタックが稼働している)。

**設計上の帰結:** `VectorStoreClient`は小さなインターフェース(`search(query, namespace) -> vector<SourceChunk>`)として定義し、実装は当面HTTPベース1つのみ。将来的に真のオフライン要件が生じた場合、この境界でFaiss実装に差し替え可能とするが、`ScriptComposer`や`Orchestrator`に影響を与えない範囲を超えて先回りして作り込まない。

### GPUテクスチャ→エンコーダ経路

正直な注記として: 真のゼロコピー(GPUテクスチャを直接ハードウェアエンコーダへ、例えばNVENCのCUDA/D3D11 interopやFFmpegの`hwframe` API経由)は達成可能だが、本設計で最もリスクが高くプラットフォーム依存の強い部分である。**Phase 2のストレッチゴール**として扱い、Phase 1のブロッカーにはしない。Phase 1のPoCは単純なCPU側リードバック(`glReadPixels`/QRhiリードバック→ソフトウェアピクセルフォーマットの`AVFrame`)を初期実装として許容し、基本パイプラインがエンドツーエンドで動くことを証明してからハードウェアフレームのゼロコピー経路を積み増す。これにより、ユーザーが本来求めている「同一プロセス・IPC排除」はGPUコピーの完全排除を待たずに既に達成される(この2つは分離可能な改善軸である)。

---

## 3. VRAM/スレッド競合設計(最重要リスク)

**本環境で確認された制約: VRAM 8GB(RTX 3070)。** これは仮定ではなく本設計の作業前提として扱う。

### 推奨: 並行リソース分割ではなく厳密な逐次フェーズ分離

`NarrationEngine`(llama.cpp GPU推論)と`SceneAssembler`/`QQuickRenderControl`(GPU描画)を**同時実行させない**。代わりに:

1. `Orchestrator`のジョブパイプラインを `Narrate`(LLM稼働、レンダラ未初期化)→ `AssembleAndRender`(レンダラ稼働、LLMは完全破棄されVRAM解放済み)→ `Encode`(mux のみ、VRAM消費は無視できる)の順で厳密に直列化する。フェーズはジョブ内で逐次実行され、`ResourceBudgetManager`が他コンポーネントのGPUアクセス前に必ずゲートを通す。
2. `ResourceBudgetManager`の役割は細かいメモリ会計(それはOS/ドライバの仕事)ではなく、**排他制御**である: `NarrationEngine`と`SceneAssembler`のいずれか一方のみが同時にGPUコンテキストの「リース」を保持することを保証し、リース解放時に実際に確保領域が解放されること(llama.cppのコンテキストとCUDA/Vulkanバックエンドが完全に破棄されること。単なるアイドル化ではない)を担保する。
3. これは「同一プロセス・ゼロコピー」という目標と矛盾しない。両者は直交する性質である: 同一プロセスは動画フレームデータパスのIPCシリアライズ/コピーオーバーヘッド排除(§2)が目的であり、無関係なサブシステムの同時GPU常駐を要求するものではない。1プロセス内でGPU負荷の高いフェーズを時分割することは、真の並行分割より安全かつ容易であり、ユーザーが求めるIPC排除のメリットを100%享受できる。

### llama.cppをGPUで動かすべきか: 推奨は「Phase 3は当面CPU推論のみ、GPUオフロードは将来のオプトイン」

根拠:
- `NarrationEngine`の役割(§2)は、上流Claudeエージェントが既に大筋を書いたナレーション文の整形・正規化であり、オープンエンドな長文生成ではない。バッチジョブ内で動画1本あたり1度きり発生する処理であり、インタラクティブ/リアルタイム経路ではないため、量子化された小型モデル(3B〜8B GGUF)のCPU推論レイテンシ(数秒)は十分許容できる。
- CPU限定にすることで、現時点で最もリスクの高いワークロードにおけるVRAM競合問題をエンジニアリングコストゼロで回避できる。この構成ではフェーズ間のリース/破棄の厳密さすら不要になる(LLMがそもそもGPUに触れないため)。
- 将来プロファイリングでCPUナレーションが許容できないボトルネックと判明した場合(小型モデル・低頻度ワークロードのため考えにくいが)、エスケープハッチとして`NarrationEngine`のみGPUオフロードを追加できる。その際も上記`ResourceBudgetManager`のフェーズゲートは維持し、排他制御を弱めるのではなくGPU加速を追加する形にする。
- VRAM 8GBという制約下では、量子化7B〜8Bモデル(4〜6GB想定)とヘッドレスQMLレンダリング+FFmpegハードウェアエンコードセッションを同時稼働させることは、それ自体がOOM/ドライバリセットの主要因になりうる。これはユーザー自身が「最重要課題」と挙げた懸念そのものであり、逐次分離設計はこの障害モードを設計上直接排除する(予算を切り詰めて回避するのではなく)。

### スレッドモデルの帰結

`Orchestrator`は1ジョブの各フェーズをメインワーカースレッド上で逐次実行する(1ジョブ内でGPUバウンドな並行ワーカープールは不要)。**複数ジョブ(異なるチュートリアル)も並列化せず、1ジョブずつ逐次処理する**。理由は同じくVRAM安全性であり、8GBカードでジョブレベル並列化のリスクを負う価値はなく、要件としても存在しない(本システムはリアルタイムサービスではなくバッチファクトリーである)。CPUバウンドな作業(markdownパース・manifest書き出し・mux)はGPUリースに触れない限りバックグラウンドスレッドで自由に実行してよい。

---

## 4. FFmpegメモリ管理

libavcodec/libavformatの生C API直接操作はリーク高リスクであることが確認済み(対になるalloc/freeが多数あり、エラーパスでの解放漏れが起きやすい)。採用パターン: **FFmpegの各Cコンストラクトに対するカスタムデリータ付き`std::unique_ptr`型エイリアス**、すなわち`AVFormatContextPtr`, `AVCodecContextPtr`, `AVFramePtr`, `AVPacketPtr`, `SwsContextPtr`, `AVBufferRefPtr`(§2のGPUゼロコピー経路を使う場合のハードウェアフレームコンテキスト用)を、それぞれ対応する`avformat_close_input`/`avformat_free_context`, `avcodec_free_context`, `av_frame_free`, `av_packet_free`, `sws_freeContext`, `av_buffer_unref`を呼ぶデリータ関数オブジェクトとペアにする。これはC APIをラップする標準的なRAIIアダプタパターンであり、独自のリソース管理フレームワークを新規開発する必要はない。

`VideoEncoder`の構造ルール:
- すべてのFFmpegハンドルは常に1つのスマートポインタが所有し、所有権境界を跨ぐ生ポインタは存在させない
- 生成は小さなファクトリ関数を経由し、populate済みスマートポインタを返すか、失敗時は例外/エラーを返す(「確保はしたが設定に失敗し、失敗パスでハンドルがリークする」という2段階パターンは避ける)
- `VideoEncoder`のデストラクタ(メンバのスマートポインタのデストラクタによる暗黙実行)が、例外アンワインド時も含めて完全な解放処理(パケット→フレーム→コーデックコンテキスト→フォーマットコンテキストの逆順)を自動的に行う。これがこのパターンの主な効果であり、手動での解放順序ミスを防ぐ
- 内部エンコードループで使う`AVPacket`/`AVFrame`は使い回し(反復ごとに`av_frame_unref`でリセット)、フレーム毎の新規確保は避ける(性能面・リーク面の双方でメリットがある)

---

## 5. Webダッシュボード設計

### 機能スコープ(v1)

- **ギャラリービュー**: 生成済み動画のグリッド表示(サムネイル・タイトル・日付・出典チュートリアルslug・タグ)
- **動画詳細ページ**: 埋め込み動画プレイヤー、チュートリアルの元になったRAG出典ドキュメント(ファイル名+関連度、可能であれば該当抜粋へのリンク)、Houdiniノードグラフの読み取り専用表示(Qtファクトリーが消費したのと同じNodeGraphAsset JSONを再利用。既存の`GraphViewer.tsx`/`graph_view.py`と配色思想を揃え、エコシステム全体での視覚的一貫性を保つ)
- **生成プロセスの振り返り表示(パイプラインビュー)**: 動画詳細ページ内に、その動画がどのフェーズを経て作られたかをカード形式で並べて見せるセクションを設ける。デザイントーンはユーザー提示の参考UI(丸みのあるカード・パステル配色・番号付きステージが左から右へ流れる「作業が流れるポップ工場」的な見た目)に寄せ、親しみやすさとフローの見やすさを重視する。**あくまで完了済みジョブの静的な振り返り表示であり、ライブ進捗のポーリングやリアルタイム更新は行わない**(Qtファクトリーはサーバーではなくバッチ実行のため、稼働中ジョブの状態をWeb側が監視する仕組みは本設計のスコープ外)。表示するステージは`Orchestrator`の実フェーズ(Ingest→Compose→Narrate→Assemble→Render→Encode→Publish、§2)に対応させ、各カードは完了状態(例のスクリーンショットで言う「DONE」)+所要時間を表示する。データは下記`metadata.json`の`pipeline`配列のみから組み立てる(新規バックエンド不要)
- **検索/フィルタ**: v1はmanifestに対するクライアントサイドフィルタ(タグ・namespace・日付)。将来トランスクリプトへの自由文検索が必要になった場合は、新規バックエンドを作らず既存RAGブリッジの`/search`をブラウザから直接叩く
- **「再生成」ボタン**: v1では**実行トリガーではなく、該当slugを再生成するための正確なCLIコマンドを表示するだけの見た目上の導線**とする。実際のトリガーを実装すると「誰がそのリクエストを受けてローカルプロセスを起動するか」というバックエンド設計が必要になり、静的サイト優先のv1のスコープ外。実トリガー化はPhase 2以降、責任範囲が明確になってからの拡張候補とする

### Qtファクトリーからのデータ契約: `manifest.json`

`ManifestWriter`(§2)が唯一のプロデューサー。ダッシュボードは読むだけ。集約インデックス1つ+動画別詳細ファイルという構成にし、ギャラリーページを軽量に保ちつつ詳細ページは遅延取得できるようにする:

```jsonc
// manifest.json — 集約インデックス、動画1件につき1エントリ
[
  {
    "id": "string",
    "slug": "string",
    "title": "string",
    "created_at": "ISO8601",
    "duration_sec": 0,
    "video_path": "videos/<id>/video.mp4",
    "thumbnail_path": "videos/<id>/thumb.jpg",
    "tags": ["string"],
    "status": "string",
    "source_tutorial": "<slug>_<date>.md"
  }
]
```

```jsonc
// videos/<id>/metadata.json — 動画別詳細
{
  "id": "string",
  "slug": "string",
  "title": "string",
  "narration_summary": "string",
  "rag_sources": [
    { "file": "string", "namespace": "string", "similarity": 0.0, "excerpt": "string" }
  ],
  "node_graph_path": "videos/<id>/graph.json",
  "render": {
    "engine_version": "string",
    "render_started_at": "ISO8601",
    "render_duration_sec": 0
  },
  "pipeline": [
    // パイプラインビュー(振り返り表示)用。Orchestratorの各フェーズが完了するたびに
    // ManifestWriterが1エントリ追記する。ジョブは既にフェーズ単位で失敗/リトライを
    // 追跡している(§2 Orchestrator)ため、新規の計測機構は不要——記録して残すだけ。
    { "stage": "ingest", "label": "取り込み", "status": "done", "duration_sec": 0 },
    { "stage": "compose", "label": "構成", "status": "done", "duration_sec": 0 },
    { "stage": "narrate", "label": "ナレーション", "status": "done", "duration_sec": 0 },
    { "stage": "assemble", "label": "シーン組立", "status": "done", "duration_sec": 0 },
    { "stage": "render", "label": "レンダリング", "status": "done", "duration_sec": 0 },
    { "stage": "encode", "label": "エンコード", "status": "done", "duration_sec": 0 },
    { "stage": "publish", "label": "公開", "status": "done", "duration_sec": 0 }
  ],
  "quality": {
    "self_reported_status": "string",
    "notes": "string"
  }
}
```

これは`localRAG/tutorials/`で既に成功しているmd+jsonペア方式を踏襲しており、ダッシュボードのデータモデルは意図的に静的JSON+メディアファイルのみに留める(`web-production-skill`の静的優先方針とも整合)。RAGブリッジのみを唯一許容される動的依存先とする。実際のHTML/CSS/JS構築(サイトマップ→デザイン→コーディング→SEO/QA)は`web-production-skill`の呼び出しとしてPhase 4に明示的に切り出す。本設計書はそのビルドが消費すべきデータ契約のみを固定する。

---

## 6. 自己改善型エージェント構想(Phase 2以降)

将来方向性として明記するのみで、**今回のスコープには含めない**。houdini21の既存前例を再利用する形とし、新メカニズムは発明しない:

- **フィードバック取り込み(既存ループの再利用)**: 動画レンダリング完了後、`localRAG/videos/<slug>_<date>.md`(トランスクリプト+実際に使用した出典)を書き出す。既に稼働中の`auto_index.py`ウォッチャーが変更なしでこれを拾う——Qtファクトリーはhoudini21が確立した自己拡張インデクシングループの2つ目のプロデューサーになるだけで、並行メカニズムを作らない
- **namespaceガバナンス**: `docs/content-generation.md`§5(権利/ライセンス取り扱い)に従い、新設する`videos` namespaceもhoudini21DBと同様のホワイトリスト化+逐語コピー検出(n-gram一致率チェック)を経てから引用可能なRAGソースとして信頼する。独自のnamespaceルールを発明してこのガバナンスを迂回しない
- **理解度スコア連携**: 既存`/api/score`エンドポイント(ユーザー別理解度トラッキング)は「動画視聴が実際に理解の助けになったか」のシグナル取得先として妥当だが、「動画に対するスコアリングイベントとは何か」という製品判断が必要であり本設計のスコープ外。統合ポイントとしてのみ記録する
- **品質/再生成ループ**: 視聴者フィードバック(高評価/低評価、視聴完了率など、§5 `metadata.json`の`quality`フィールドに書き込む)が将来の再生成優先度キューを駆動する可能性があるが、スコアリング/ランキングロジック自体は本設計書では設計しない
- **今回の非目標の明示**: 上記はPhase 0〜4のいずれもブロックしない。§5のmanifestスキーマは`quality`フィールドを既に予約しており、スキーマ移行なしで後から追加できる

---

## 7. リポジトリ構成案

```
LearningQt/
├── README.md
├── .gitignore
├── CMakeLists.txt                  # トップレベル、下記サブディレクトリを追加
├── docs/
│   └── architecture/
│       └── video-factory-design.md # 本ドキュメント
├── engine/                         # Qt/C++ヘッドレス動画ファクトリー本体
│   ├── CMakeLists.txt
│   ├── src/
│   │   ├── orchestrator/           # Orchestrator, JobPipeline, ResourceBudgetManager
│   │   ├── ingest/                 # IngestWatcher, ScriptComposer(md/jsonパース)
│   │   ├── narration/              # NarrationEngine(llama.cppラッパー)
│   │   ├── ragclient/              # VectorStoreClient(:8766へのHTTPクライアント)
│   │   ├── scene/                  # SceneAssembler, QQuickRenderControlセットアップ
│   │   ├── encode/                 # VideoEncoder, FFmpeg RAIIラッパー(§4)
│   │   └── manifest/               # ManifestWriter
│   ├── qml/                        # データ駆動の汎用「チュートリアルシーン」QMLテンプレート
│   └── tests/                      # モジュール単位のユニットテスト(ScriptComposerパースなど)
├── web/                             # ダッシュボード(Phase 4でweb-production-skillにより構築)
│   ├── (サイトソース、web-production-skillの規約に従う)
│   └── public/                     # manifest.json + videos/<id>/... (実行時に消費)
└── thirdparty/                     # またはCMake FetchContent設定。llama.cpp/FFmpeg/Qt
    └── (ベンダリング戦略はPhase 0/1で決定、本書では固定しない)
```

各ディレクトリの役割: `docs/` 設計記録、`engine/` ヘッドレスC++ファクトリー本体(C++/QMLはここにのみ置く)、`web/` 静的ダッシュボード(HTML/CSS/JSはここにのみ置く)、`thirdparty/` サードパーティネイティブ依存管理(ファーストパーティコードと独立して`.gitignore`/ベンダリングできるよう分離)。

---

## 8. フェーズロードマップ

各フェーズは単独で動作確認可能(後続フェーズの未完成部分に依存しない)。

| Phase | 目標 | 動作確認できる成果 |
|---|---|---|
| **0** | 設計文書+リポジトリスケルトン+`.gitignore`修正(§9)。エンジンコードはまだ書かない | 本ドキュメントが`docs/architecture/`に存在。`engine/`/`web/`の空スケルトン+CMakeスタブ。`web-production-skill/`がリポジトリから確実に除外される |
| **1** | 静的コンテンツのみでのヘッドレスQMLレンダリング→FFmpeg muxのPoC(LLM/RAGなし) | 単一のハードコードされたQMLシーンを`QQuickRenderControl`/`QOffscreenSurface`でヘッドレス描画し、RAII FFmpegラッパー経由で正常な`.mp4`にエンコード。ヘッドレスレンダリング→ファイル出力という中核パイプラインがそもそも動くことを証明する |
| **2** | RAGブリッジHTTPクライアント実装+実在するhoudini21 `.md`/`.json`ペア1件から実動画1本を生成(LLMナレーション整形はまだ、markdownテキストをそのまま使用) | `localRAG/tutorials/`の実チュートリアル1件(houdini21が最低1件生成した時点)がノードグラフ可視化付きの実動画になる。§2のGPUテクスチャゼロコピーエンコーダ経路もここで試みる |
| **3** | llama.cppナレーション統合(§3の推奨通りCPU推論)+`ResourceBudgetManager`のフェーズゲート実装(CPU限定では厳密には不要だが、将来のGPUオフロードが設定切り替えだけで済むよう今のうちに構築) | ナレーション文がllama.cppで整形されてからシーンに使われる。フェーズ分離ジョブパイプラインが概念だけでなく実際に稼働する |
| **4** | `web-production-skill`によるWebダッシュボードv1構築(§5のmanifestスキーマを消費) | Phase 2/3の生成結果で構成された、ローカルで閲覧可能な静的ギャラリー+動画詳細ページ |
| **5** | 自己改善フィードバックループ(§6): 動画トランスクリプトの`localRAG/`への書き戻し、manifestの`quality`フィールド配線、理解度スコア連携の検討 | 生成動画のトランスクリプトが後続の`/search`呼び出しで検索可能なRAGソースとして現れる——houdini21の前例と揃う形でループが閉じる |

---

## 9. .gitignore方針

現状の`.gitignore`(汎用Node+Python+Windowsテンプレート)には`web-production-skill/`やC++/Qtツールチェーンに関する認識がなかったため、以下を追加済み(Phase 0の一部として本設計と同時に実施):

1. **参照スキルフォルダの完全除外**(最優先): `web-production-skill/`、および内部に存在する`__MACOSX/`(macOS zip展開時のゴミ、本フォルダに限らず一般的に有用な除外)
2. **C++/Qt/CMake用の除外ルール**(従来皆無だったため新規追加): `/build/`, `/out/`のビルド出力ディレクトリ、コンパイル成果物(`*.o`, `*.obj`, `*.a`, `*.lib`, `*.dll`, `*.exe`, `*.pdb`)、Qt生成物(`moc_*.cpp`, `ui_*.h`, `qrc_*.cpp`, `*.moc`)、CMake状態ファイル(`CMakeCache.txt`, `CMakeFiles/`, `cmake_install.cmake`, `CTestTestfile.cmake`, `Testing/`, `CMakeUserPresets.json`)、IDE/ツールチェーン状態(`.qtc_clangd/`, `*.pro.user*`)
3. `thirdparty/`のベンダリング除外パターンは、Phase 0/1で依存関係の取り込み戦略(CMake FetchContent vs. ベンダリング)が確定してから追加する(本書では固定しない)

---

## 実装時の参照ファイル(変更しないが設計の前提とする)

- `DevelopmentRAGEnvironment/scripts/rag_local_bridge.py` — `VectorStoreClient`が話すべき正確なHTTP契約(エンドポイント・認証ヘッダ・レスポンス形状)の定義元
- `DevelopmentRAGEnvironment/docs/content-generation.md` — 上流`.md`/`.json`成果物契約と、`ScriptComposer`及び将来の§6フィードバックループが尊重すべきnamespace/ライセンスガバナンス(§5)の定義元
- `GameDevelopment\Graduation\Node-Management\types\nodeGraph.ts` — `SceneAssembler`とダッシュボードのグラフビューアの両方がパースすべき正準NodeGraphAssetスキーマ
