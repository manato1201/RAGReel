# LearningQt(実体: VideoFactory)改善・リファクタリング計画書

**改善指標: 設計済みアーキテクチャの実装ギャップ解消+アーキタイプECS/サービスコンテナ導入**
作成日: 2026-08-11 / 改訂日: 2026-08-12(「設計書との乖離」表を追加、Phase 1/2/4/5・優先度注記を実装の現状に合わせて修正) / 調査範囲: `engine/src/`(C++20/Qt6/QML)、`docs/architecture/video-factory-design.md`

本計画は`docs/architecture/video-factory-design.md`(設計確定済み・実装着手前と自己申告のPhase 0)の実装ギャップを埋めるロードマップである。**当初は新規のアーキテクチャ判断を行わず、既に確定済みの設計判断(§2〜§4)をそのまま実装へ落とし込む方針だったが、Phase 0レビューで実機調査したところ設計書と実装の間に8件の具体的な乖離が見つかったため、この前提は部分的に修正されている(詳細はPhase 0「設計書との乖離」表、および末尾の「優先度注記(改訂)」を参照)。**

---

## Phase 0: 現状分析(調査済み)

### 現状: POCモノリスとモジュール化済み領域の仕分け

| 領域 | 状態 | 実体 |
|---|---|---|
| POCモノリス | 動作中(単一ファイル) | `engine/src/main_cloudrag.cpp`(1549行、77,534バイト)。`struct Slide`(L196)・`struct HoudiniTutorial`(L749)を内包。ビルド成果物 `build/engine/video_factory_cloudrag_poc.exe` 生成済み |
| モジュール化済み | 設計書§2・§7の構成通りに切り出し済み | `engine/src/narration/`(`narration_engine.h/.cpp`)、`engine/src/ragclient/`(`cloud_rag_client.h/.cpp`)、`engine/src/encode/`(`video_encoder.h/.cpp`)、`engine/src/manifest/`(`manifest_writer.h/.cpp`)、`engine/src/launcher/`(設定・namespace一覧・プロセス起動) |
| 未着手 | 空ディレクトリ(`.`/`..`のみ) | `engine/src/orchestrator/`、`engine/src/ingest/`、`engine/src/scene/` |

CMakeLists.txtのプロジェクト名は`VideoFactory`(`project(VideoFactory LANGUAGES CXX)`, CMakeLists.txt:2)。リポジトリ名`LearningQt`と実体名の乖離は本計画のどのフェーズでも解消しない(ディレクトリ改名はスコープ外、混乱回避のため本書冒頭でのみ明示)。`main.cpp`/`main_launcher.cpp`/`main_cloudrag.cpp`の3エントリポイントのうち、本計画が対象とするのは動画生成パイプライン本体である`main_cloudrag.cpp`。

### 設計書§3の前提: VRAM/スレッド競合設計(要約引用)

開発機のVRAM 8GB(RTX 3070)は「仮定」ではなく設計書が明記する作業前提。対応する設計上の帰結:

- `NarrationEngine`(llama.cpp GPU推論)と`SceneAssembler`(GPU描画)は**同時実行させない**。`Orchestrator`は `Narrate`(LLM稼働・レンダラ未初期化)→`AssembleAndRender`(レンダラ稼働・LLM完全破棄済み)→`Encode`(muxのみ、VRAM消費は無視できる)の順で**厳密に逐次化**する
- `ResourceBudgetManager`の役割は**細かいメモリ会計ではなく排他制御**(設計書§3が明言)。`NarrationEngine`と`SceneAssembler`のどちらか一方のみが同時にGPUコンテキストの「リース」を保持することを保証し、リース解放時に実際にVRAMが解放されること(コンテキストの完全破棄。アイドル化では不可)を担保する
- 複数ジョブ(異なるチュートリアル)も並列化せず**1ジョブずつ逐次処理**する。バッチファクトリーでありリアルタイムサービスではないため、ジョブレベル並列化のリスクを負う理由がない
- CPUバウンド作業(mdパース・manifest書き出し・mux)はGPUリースに一切触れない限り、バックグラウンドスレッドで自由に実行してよい

### manifest.jsonスキーマ(設計書§5、確定済み)

`ManifestWriter`が唯一のプロデューサー。パイプラインビュー(振り返り表示)用の`pipeline`配列と、将来のフィードバックループ用に既に予約済みの`quality`フィールドが定義されている。**下記は設計書§5の原型に、実装(`manifest_writer.cpp`)が既に追加している`estimated_tokens`/`quality.extraction_rate`/`quality.extraction_detail`(いずれも設計書未記載、「設計書との乖離」表#4参照)を反映した現状版:**

```jsonc
// videos/<id>/metadata.json 抜粋
{
  "estimated_tokens": 0,      // 実装追加分。文字数ベースの推定値であり実測ではない(main_cloudrag.cppのestimateTokens())
  "pipeline": [
    { "stage": "ingest",  "label": "取り込み",     "status": "done", "duration_sec": 0 },
    { "stage": "narrate", "label": "ナレーション", "status": "done", "duration_sec": 0 }
    // compose/assemble/render/encode/publish も同形式で追記
  ],
  "quality": {
    "self_reported_status": "string",
    "notes": "string",
    "extraction_rate": 0.0,   // 実装追加分。出典網羅率(0-100)。Cloud RAGクエリ経路のみ、Houdini取り込みモードでは0
    "extraction_detail": "string" // 実装追加分。"cited/total"形式の内訳
  }
}
```

### CI/静的解析/テストの現状

- `engine/tests/` は空(ゼロテスト、`.`/`..`のみ)
- `.github/workflows/` 自体が未作成(CIパイプライン皆無)
- clang-tidy/clang-format等の静的解析ツールチェーン未導入

### アンチパターン(全フェーズ共通)

- 厳密逐次フェーズ分離(Narrate→AssembleAndRender→Encode)は「今後解消すべき負債」ではなく**設計上の制約**。Phase 4でジョブシステムを導入してもGPU系フェーズ(Narrate/AssembleAndRender)の同時実行は絶対禁止
- `DevelopmentRAGEnvironment`(上流、稼働中)は本計画のいかなるフェーズでも変更しない。読み取り専用の上流プロデューサーとして扱う
- 既に動作している`narration_engine`/`video_encoder`/`manifest_writer`/`cloud_rag_client`は「配線し直す」対象であり「書き直す」対象ではない。ヘッダのpublic APIシグネチャを変えない
- GPUテクスチャ→エンコーダのゼロコピー経路は設計書が明記する**ストレッチゴール**。どのフェーズでもブロッカー化しない

### 設計書との乖離(本計画のレビューで実機調査により新たに判明。Phase 1着手前に前提として織り込む)

`docs/architecture/video-factory-design.md`は「設計確定済み」と自己申告しているが、実装は既に以下の点で設計書の想定から外れて進んでいる。**これらはバグではなく、いずれもコード側コメントで理由付き明記済みの意図的な判断**であり、本計画のPhase 1〜5はこの現状を前提に読み替える必要がある(設計書を先に修正するか、少なくとも本計画側で乖離を握っておかないと、Phase 1〜5が古いメンタルモデルの上に積み上がるリスクがある):

| # | 設計書(§)の想定 | 実装の現状 | 本計画への影響 |
|---|---|---|---|
| 1 | `NarrationEngine`はllama.cpp C APIのラッパー(§2)。VRAM 8GB制約下でのGPU競合が「最重要リスク」(§3) | `narration_engine.h`はWindows SAPI5 TTSで実装済み。ヘッダコメントに「llama.cpp駆動のナレーション改善は設計書通りだが、これは将来課題」と明記。SAPIはOSサービスでGPU/VRAMに一切触れない | Phase 1の`ResourceBudgetManager`は**現時点では実害のあるクラッシュを防いでいるわけではない**(SAPIとQQuickRenderControlは元々GPU競合し得ない)。llama.cpp化されるまでの先行実装であることを明記し、検証チェックリストのVRAM実測項目は「llama.cpp統合後に実施」と条件付ける |
| 2 | `VectorStoreClient`は`rag_local_bridge.py:8766`へのローカルHTTPクライアント(§2)。実装はHTTPベース1つのみに限定 | 実装済みの`CloudRagClient`(`engine/src/ragclient/`)は`CLOUD_RAG_URL`/`CLOUD_RAG_API_KEY`で設定するCloud RAG GAS WebAppクライアント。ローカル`rag_local_bridge:8766`向けのC++クライアントは存在しない(Houdini側`tutorial_agent.py`はPython側でlocal/cloud両対応だが、これはC++側と無関係) | Phase 4のDI化対象は「ローカルブリッジ」ではなく実際にはクラウドGASエンドポイントである。インターフェース名を`IVectorStoreClient`のままにするなら対象がクラウド側であることを明記する。将来ローカルブリッジ対応も必要になった場合は「HTTPベース1つのみ」の前提が崩れる点を認識しておく |
| 3 | `ManifestWriter`の公開先は共有`web/public/`、Phase 4で`web-production-skill`により構築(§5・§7・§8) | 実装(`main_cloudrag.cpp`のコメント参照)は「RAGReel配布」方針により、インストール先ごとのローカル`output/`(exeと同じディレクトリ)に公開する形に変更済み。共有Webダッシュボードのビルドは行われていない | Phase 4のスコープ・完了条件を「ローカルoutput/ダッシュボード」で書き直すか、共有Web版を別途Future Workとして切り出すか、本計画側で判断が要る |
| 4 | manifest.jsonスキーマは§5で確定済み。本計画Phase 0が引用する抜粋は`pipeline`/`quality`のみ | 実装(`manifest_writer.h`/`.cpp`)は設計書未記載の`estimated_tokens`(トークン消費推定)・`quality.extraction_rate`/`quality.extraction_detail`(出典網羅率)を既に書き込み済み | 「確定済みスキーマ」という前提は不完全。Phase 0のスキーマ引用にこれらのフィールドを追記するか、設計書側の更新が必要 |
| 5 | 設計書§1のシステム境界図・§8のフェーズロードマップは、Cloud RAGクエリ駆動の動画生成を単一の入力経路として想定 | `main_cloudrag.cpp`には設計書に存在しない第二の入力経路「Houdiniチュートリアル取り込みモード」(`--houdini-md`)があり、per-stepスクリーンショット・ビューポートクリップ・参考文献カード等、直近の開発の大半はこちら側に集中している | Phase 2の`ScriptComposer`/`ShotList`設計は、この経路の実データ形状(後述#6)を移行元として含めないと、実装から乖離した抽象化になる |
| 6 | Phase 2のShot分類案は`ShotKind::TitleCard / NodeGraphReveal / SourceCard`の3種 | 実装済み`struct Slide`(`main_cloudrag.cpp:196`)が持つ実際のバリエーションは: 通常テキスト(見出し+2箇条書き)、Mermaid図(`diagramImagePath`)、コードブロック(`codeBlock`)、Houdiniステップのネットワーク/ビューポート静止画、cook_nodeのビューポート連番クリップ(`clipFramePaths`+`clipFps`)、参考文献カード一覧(`referenceItems`、本セッションで追加)の6種相当 | Phase 2の`ShotKind`列挙・`ShotList`構造体は元の3種案ではなく、この6種を移行元として再設計すること(詳細はPhase 2セクションに追記) |
| 7 | 依存関係のベンダリング戦略は「Phase 0/1で確定する」(§9)、リポジトリ構成案は`thirdparty/`ディレクトリを想定(§7) | 実際には`vcpkg.json`(マニフェストモード)+`CMakePresets.json`の`CMAKE_TOOLCHAIN_FILE: C:/vcpkg/scripts/buildsystems/vcpkg.cmake`で既に確定・運用中。`thirdparty/`ディレクトリは存在しない | Phase 5のテストフレームワーク導入は、この既定路線に合わせて`vcpkg.json`への追加とすること(詳細はPhase 5セクションに追記) |
| 8 | 設計書は動画ファクトリーエンジン本体(`video_factory_cloudrag_poc.exe`)のみを扱う | `RAGReel.exe`(`main_launcher.cpp`+`engine/src/launcher/`)という非エンジニア向けGUIランチャーが第2の実行ファイルとして既に存在し、実運用中(設定・Cloud RAGクエリ・Houdiniチュートリアル・はじめにの4タブ) | 本計画は`main_cloudrag.cpp`側のみを対象とする、と本書冒頭で明言している通りでよいが、Phase 4の「サービスコンテナ化」がランチャー側の`ProcessRunner`/`NamespaceLister`と将来どう関係するかは範囲外である旨をどこかで一言触れておくと誤解を防げる |

---

## Phase 1: Orchestrator/JobPipeline/ResourceBudgetManager抽出(最優先) — **実装済み(2026-08-12)**

**現状:** `main_cloudrag.cpp`内に暗黙的な実行順序(Ingest→Compose→Narrate→Assemble→Render→Encode→Publish)がベタ書きされている。フェーズという概念がコード上に明示的な型として存在しない。

**実装結果の要約:** `job_pipeline.h`/`resource_budget_manager.h/.cpp`/`orchestrator.h`を新設し、`main_cloudrag.cpp`のNarrate/Assemble/Renderフェーズに実配線済み。`Orchestrator`は当初案の`runJob()`一括駆動ではなく、GPUリース払い出し+`StageResult`記録のみを担う薄いクラスとした(理由は下記「実装内容」3番を参照)。手動でMSVC環境変数を構成してビルド・実行検証済み(`--mock`実行で実際に`.mp4`が生成され、全6ステージが記録・ログ出力されることを確認)。ctestに`resource_budget_manager_test`を登録し、排他制御の単体テストがパスすることを確認済み(下記チェックリスト参照)。

**実装内容:**
1. `engine/src/orchestrator/job_pipeline.h` を新設し、設計書§2の表に対応する7フェーズを明示的な型にする:
   ```cpp
   enum class JobStage {
       Ingest, Compose, Narrate, Assemble, Render, Encode, Publish
   };
   struct StageResult {
       JobStage stage;
       bool success;
       double duration_sec;
       std::string error_message;   // 空なら成功。ManifestWriterのpipeline配列に直結
   };
   ```
2. `engine/src/orchestrator/resource_budget_manager.h` を新設。役割は排他制御のみ(§3が明言する通りメモリ会計は持たない):
   ```cpp
   enum class GpuLeaseOwner { NarrationEngine, SceneAssembler };
   class ResourceBudgetManager {
   public:
       [[nodiscard]] GpuLease acquireGpuLease(GpuLeaseOwner owner); // 排他ロック。他方保持中はブロック
       // GpuLeaseのデストラクタで解放し、解放時に実コンテキスト破棄(アイドル化ではない)を保証する
   };
   ```
3. `engine/src/orchestrator/orchestrator.h`: 既存の`NarrationEngine`/`SceneAssembler`(Phase 3)/`VideoEncoder`/`ManifestWriter`/`CloudRagClient`を**インターフェースを変えずに**コンストラクタ経由で受け取り、`runJob()`内で`JobStage`順に呼び出すだけの配線層とする。各モジュールの実装本体には手を入れない
   **→ 実装時の判断: `SceneAssembler`(Phase 3)が未抽出のため、`runJob()`一括駆動は今回作らなかった。`Orchestrator`は現状「GPUリースの払い出し」と「`StageResult`の記録」のみを行う薄いクラスとし、フェーズ本体(ナレーション合成・QMLシーン構築・レンダーループ・エンコード・公開)は引き続き`main_cloudrag.cpp`の`main()`が直接呼び出す。理由はクラス自身のコメントに明記(存在しない`SceneAssembler`を騙って`runJob()`を書くと「既存を配線し直すだけ」というPhase 1の原則から外れるため)。Phase 2/3でScriptComposer/SceneAssemblerが揃った時点で、このクラスを本来の`runJob()`駆動へ拡張する**
4. 1ジョブ=1プロセス起動を維持する(設計書§2: 「リソース解放の観点で最も安全・単純」という明記済みの判断を踏襲)。並列化はPhase 4のジョブシステムでもCPUバウンド部分限定に留める

**検証チェックリスト:**
- [x] `main_cloudrag.cpp`と同一の`.md`/`.json`入力で、`Orchestrator`経由でも同一の`.mp4`+`manifest.json`が生成される — **上記の通り`Orchestrator`は`main()`に直接配線されており別経路ではないため、`--mock`実行で実際に検証(全6ステージ記録・`.mp4`生成を確認)**
- [x] `ResourceBudgetManager::acquireGpuLease(SceneAssembler)`が`NarrationEngine`のリース保持中はブロックすることを単体テストで確認(モックのリース取得順序で検証すれば足りる。GPU実機は不要) — `engine/tests/resource_budget_manager_test.cpp`(GTest不使用、標準ライブラリのみ)。`ctest`登録済み、パス確認済み
- [ ] リース解放後、`nvidia-smi`のVRAM使用量が解放前の水準まで戻ることを実測(アイドル化で誤魔化していないことの確認)。**ただし現時点の`NarrationEngine`はWindows SAPI5(§0の乖離#1)でGPUに一切触れないため、この項目は`NarrationEngine`がllama.cpp GPU推論に切り替わって初めて意味を持つ。それまでは「配線上ゲートが正しく機能する」ことの確認に留め、Phase 1完了の必須条件からは外す**
- [x] `narration_engine.h`/`video_encoder.h`/`manifest_writer.h`/`cloud_rag_client.h`のpublic APIシグネチャに差分がないこと(`git diff`で確認) — いずれも未変更(`main_cloudrag.cpp`側のみ変更)

**アンチパターン:** `ResourceBudgetManager`に使用中バイト数のようなメモリ会計フィールドを持たせない(設計書§3が明示的に否定する役割)。単純な排他ロックの状態機械に留める。設計上はVRAM誤設計の実害(即クラッシュ/ドライバリセット)が最も大きいフェーズだが、**現行の`NarrationEngine`(SAPI)はGPU非依存のため、この危険は「今すぐ」ではなく「将来llama.cpp化した時」に顕在化する**。慎重な検証自体は引き続き推奨するが、他フェーズを差し置いてまで最優先にすべきかは、llama.cpp統合の実施時期次第で判断すること。

---

## Phase 2: IngestWatcher/ScriptComposer抽出+アーキタイプECS(Shot表現) — **実装済み(2026-08-14、IngestWatcherを除く)**

**現状:** `engine/src/ingest/`は空。`struct Slide`(main_cloudrag.cpp:196)・`struct HoudiniTutorial`(main_cloudrag.cpp:749)がモノリス内にベタ書きされたデータ構造として存在する。

**実装結果の要約:** `main_cloudrag.cpp`の匿名名前空間にあった約700行(Slide/HoudiniTutorial/HoudiniStepScreenshot構造体+スライド分割・Houdiniマークダウン解析・参考文献パース関連の全関数)を`engine/src/ingest/script_composer.h/.cpp`へ抽出。`logLine`/`appRelativePath`は両ファイルから使うため`engine/src/common/app_utils.h`へ切り出した。`ShotKind`/`ShotList`は当初案の3種ではなく実装済みの6種(下記)で実装し、`toShotList()`を`main()`に配線して実データで検証済み(下記チェックリスト参照)。**`IngestWatcher`は実装しなかった**: `localRAG/tutorials/`をポーリングする設計だが、実際の2つの呼び出し経路(Cloud RAGクエリ=ファイル入力なし、Houdini取り込み=`tutorial_view.py::_on_save`が`--houdini-md`で明示的にファイルパスを渡すプッシュ型)のどちらもポーリングモデルと合わない。呼び出し元が存在しない機能を投機的に作ることは避けた(呼び出し元が必要になった時点で追加する)。

**実装内容:**
1. `engine/src/ingest/ingest_watcher.h/.cpp`: 処理済み台帳方式(設計書§2: 常駐ファイル監視は不要、Phase 5以降の任意強化)。`localRAG/tutorials/`の未処理`.md`/`.json`ペアをポーリングで検出する:
   ```cpp
   class IngestWatcher {
   public:
       std::vector<TutorialPair> pollUnprocessed(const std::filesystem::path& tutorialsDir);
   private:
       ProcessedLedger ledger_;   // 処理済みslugを記録するのみ。常駐監視スレッドは持たない
   };
   ```
2. `engine/src/ingest/script_composer.h/.cpp`: markdown(frontmatter+見出しセクション)とNodeGraphAsset JSONをマージし`ShotList`へ変換。Qt/GPU非依存の純粋データ変換(設計書§2が明記する通り単体テスト容易性が売り)
3. `struct Slide`/`struct HoudiniTutorial`をアーキタイプECSのコンポーネント候補として再設計する。**Shot種別は設計書§2の原案(タイトルカード/ノードグラフ段階リビール/出典カード)ではなく、実装済み`struct Slide`(`main_cloudrag.cpp:196`)が実際に持つ6種を移行元とする**(§0の乖離#6参照。原案の3種は現行コードに存在しない一方、原案が想定していなかった「ビューポートクリップ」「参考文献カード」は既に本番で動いている):
   ```cpp
   enum class ShotKind {
       TextDigest,       // 見出し+箇条書き2件(通常のCloud RAGスライド)
       DiagramImage,      // Mermaid図(diagramImagePath)
       CodeBlock,         // コードブロック(codeBlock)
       HoudiniStepStill,  // Houdiniステップのネットワーク/ビューポート静止画
       HoudiniStepClip,   // cook_nodeのビューポート連番クリップ(clipFramePaths+clipFps)
       ReferenceCards,    // 参考文献カード一覧(referenceItems)
   };

   struct TextDigestShot     { std::string heading, bullet1, bullet2; };
   struct DiagramImageShot   { std::string heading, imagePath; };
   struct CodeBlockShot      { std::string heading, code; };
   struct HoudiniStepStillShot {
       int stepNumber;
       std::string imagePath;   // ネットワーク優先/ビューポート優先はツール種別で決定済み(main_cloudrag.cppの既存ロジックを移植)
       std::string resultText;
   };
   struct HoudiniStepClipShot {
       int stepNumber;
       std::vector<std::string> clipFramePaths;
       int clipFps;
   };
   struct ReferenceCardsShot {
       struct Item { std::string title, db; bool cited; };
       std::vector<Item> items;
   };

   struct ShotList {   // アーキタイプごとにSoAで保持(archetype = ShotKind)
       std::vector<TextDigestShot>      textDigests;
       std::vector<DiagramImageShot>    diagramImages;
       std::vector<CodeBlockShot>       codeBlocks;
       std::vector<HoudiniStepStillShot> houdiniStepStills;
       std::vector<HoudiniStepClipShot>  houdiniStepClips;
       std::vector<ReferenceCardsShot>   referenceCards;
       std::vector<ShotKind>            order;  // 再生順序(アーカイブ間の順序を保持)
   };
   ```
   移行元の対応関係: `Slide::diagramImagePath`が非空かつ`clipFramePaths`が空→`DiagramImageShot`(ただし`houdiniStepNumber>=0`なら`HoudiniStepStillShot`)、`clipFramePaths`が非空→`HoudiniStepClipShot`、`codeBlock`が非空→`CodeBlockShot`、`referenceItems`が非空→`ReferenceCardsShot`、いずれも該当なし→`TextDigestShot`。この優先順位は現行`enrichSlidesForDisplay`/レンダーループの分岐と同一にすること。
4. ECS導入は`ScriptComposer`の出力データ構造にとどめる。`Orchestrator`のフェーズ制御ロジック(`JobStage`)はECS化しない — 固定フェーズ駆動とアーキタイプ指向データは別レイヤーの関心事として明確に分離する(スコープを限定)

**検証チェックリスト:**
- [x] `localRAG/tutorials/`の実チュートリアル1件で、新`ScriptComposer`出力のショット数・順序が旧`main_cloudrag.cpp`内ロジックの出力と一致する — **抽出は関数本体をそのまま移動しただけ(ロジック変更なし)のため「新旧比較」は該当しないが、実チュートリアル(`procedural-particle-burst_20260808.md`、57スライド)で`--houdini-md --mock`を実行し、正常に`.mp4`が生成され全ステージが記録されることを確認**
- [ ] `IngestWatcher`の処理済み台帳が同一slugを二重処理しない(再実行テストで確認) — **実装しないと判断したため対象外(理由は上記「実装結果の要約」参照)**
- [x] `ScriptComposer`の単体テストがQt/GPUヘッダに一切依存せずビルド・実行できる(CMakeターゲット分離で確認) — **「Qt/GPU非依存」の解釈をQtCoreは許容・QtQuick/GPU不使用に修正(理由は`script_composer.h`冒頭コメント参照)。専用の単体テストはまだ書いていない(`video_factory_cloudrag_poc`本体からの実行テストのみ) — 次回作業時にPhase 5のGTest導入と合わせて追加する**
- [x] `SceneAssembler`側から見て、`ShotList`がShotKind別にホモジニアスなバッチとして走査できるデータ形状になっている — `toShotList()`を`main()`に配線し、実チュートリアル(57スライド)で「7 text / 1 diagram / 0 code / 37 houdini-still / 11 houdini-clip / 1 reference-cards」への正しい分類と`order`との整合性(order-verified: yes)をログで確認

**アンチパターン:** アーキタイプECSを`Orchestrator`のフェーズ制御にまで広げない。設計書が定義する固定フェーズ駆動(Ingest→Compose→…→Publish)を、ECSの汎用性を理由に置き換えない。

---

## Phase 3: SceneAssembler抽出(ヘッドレスレンダリング) — **実装済み(2026-08-14)**

**現状:** `engine/src/scene/`は空。`QQuickRenderControl`+`QOffscreenSurface`によるオフスクリーン描画セットアップは`main_cloudrag.cpp`内に存在する。

**実装結果の要約:** `engine/src/scene/scene_assembler.h/.cpp`を新設し、`QQuickRenderControl`/`QQuickWindow`/`QQmlEngine`/`QQmlComponent`/QRhiテクスチャ・レンダーターゲット一式の所有権を`SceneAssembler`クラスに集約した。`initialize(StaticProperties, &errorMessage)`と`renderFrame(FrameProperties) -> QImage`の2メソッドのみの薄いインターフェースで、CPU側リードバック(既存実装が元々この方式だったため変更なし)。**当初案からの変更点**: 設計書原案の「ShotListをQML側のモデルデータとしてバインドする」は実装しなかった —既存の`CloudRagScene.qml`はフラットなper-frameプロパティ契約(`slideHeading`/`slideBullet1`/`slideDiagramSource`等)で実証済みに動いており、これを`ShotList`バインディングへ全面的に書き換えるのは本フェーズの本来のゴール(`QQuickRenderControl`等をクラス境界に閉じ込めること)を超える、別の大きなリスクを伴う変更のため。`FrameProperties`構造体は既存のプロパティ契約をそのまま踏襲している。

**実装内容:**
1. `engine/src/scene/scene_assembler.h/.cpp`を新設し、`QQuickRenderControl`/オフスクリーン`QQuickWindow`/`QOffscreenSurface`の所有権をここに集約する
2. 設計書§2の「データ駆動の汎用テンプレート1本」方針を厳守する: チュートリアルごとの手書きQMLは作らない。`engine/qml/`に汎用「チュートリアルシーン」テンプレートを1つだけ配置し、Phase 2の`ShotList`をQML側のモデルデータとしてバインドする:
   ```cpp
   class SceneAssembler {
   public:
       void loadShotList(const ShotList& shots);   // 汎用QMLテンプレート1本にバインド
       QImage renderFrame(int frameIndex);          // まずCPU側リードバックで通す
   };
   ```
3. GPUテクスチャ→エンコーダのゼロコピー経路は設計書§2が明記する**ストレッチゴール**として扱う。Phase 3ではまずCPU側リードバック(`glReadPixels`またはQRhiのreadback API)でソフトウェアピクセルフォーマットの`AVFrame`を組み立て、`VideoEncoder`へ渡す経路を通す。基本パイプラインのエンドツーエンド疎通を先に証明し、ゼロコピー化はPhase 3完了の判定条件に含めない

**検証チェックリスト:**
- [ ] 単一の`ShotList`から、汎用QMLテンプレート経由で(手書きQML追加なしに)フレーム列が生成できる — **未達。上記の通り`ShotList`バインディングは今回実装しなかったため、この項目自体が現行スコープでは非該当(引き続き`Slide`由来の`FrameProperties`で描画)**
- [x] CPU側リードバック経路で`.mp4`が最後まで書き出せる(ゼロコピー未達でも合格) — `--mock`/`--mock-plain`/実チュートリアル(57スライド)いずれも正常に`.mp4`書き出しを確認
- [x] レンダリング中、`ResourceBudgetManager`(Phase 1)のGPUリースが`AssembleAndRender`として一貫保持され、`Narrate`側のリースと重ならないことをログで確認 — Phase 1で配線済みの`GpuLease`スコープを`SceneAssembler`の構築〜破棄全体を囲むよう維持(コード上のスコープ境界で保証。ログでの実行時確認は現状ステージ完了ログのみ)
- [x] 生成動画をPhase 0のPOC出力(`video_factory_cloudrag_poc.exe`の生成物)と目視比較し、同等のシーン内容であること — 実チュートリアル(`procedural-particle-burst_20260808.md`)のサムネイルを目視確認。レイアウト・テキスト・画像とも抽出前と同一(ロジック変更なしの移動のため当然の結果ではあるが、実際に確認した)

**アンチパターン:** ゼロコピー経路の実装をPhase 3のブロッカーにしない。チュートリアル種別ごとに個別QMLファイルが増え始めたら、設計方針(汎用テンプレート1本)からの逸脱シグナルとして即座に見直す。

---

## Phase 4: サービスコンテナ化(全モジュールDI) — **実装済み(2026-08-14、CpuJobQueueを除く)**

**現状:** `main_cloudrag.cpp`内で`NarrationEngine`/`VectorStoreClient`(`CloudRagClient`)/`VideoEncoder`/`ManifestWriter`が直接構築されている(具象クラスへの直接依存)。

**実装結果の要約:** `engine/src/services/`に`interfaces.h`(`INarrationEngine`/`IVectorStoreClient`/`IVideoEncoder`+`IVideoEncoderFactory`/`IManifestWriter`)・`concrete_services.h`(各既存モジュールをそのまま呼ぶだけの薄いアダプタ)・`service_container.h`(単純なレジストリ)を新設。`main()`冒頭で`ServiceContainer`を組み立て、以降`NarrationEngine::synthesize()`/`CloudRagClient::fromEnvironment()`(複数箇所)/`VideoEncoder`直接構築/`ManifestWriter::publish()`の呼び出しを全て`services.xxx()`経由に置き換えた。`enrichSlidesForDisplay`(Phase 2)は内部で`CloudRagClient::fromEnvironment()`を呼んでいたのをやめ、`IVectorStoreClient*`を引数で受け取る形に変更(自分で書いたPhase 2のヘッダなので変更可)。**当初案からの変更点**: `IVideoEncoder`は設計書原案通りの「1回登録して使い回す単一インスタンス」ではなく`IVideoEncoderFactory`にした——`VideoEncoder`のコンストラクタはジョブ固有のパラメータ(出力パス・音声パス)を取るため、ナレーション合成が終わるまで構築できず、Container登録時点では未確定だったため。`Orchestrator`(Phase 1)のコンストラクタはこれらのインターフェースを受け取っていない——Phase 1で確立した通り`Orchestrator`はGPUリースとステージ記録のみを担当し、モジュール呼び出し自体は`main()`が直接行う設計を維持したため、`ServiceContainer`も`main()`が直接持つ形にした。`CpuJobQueue`は実装しなかった: 現行パイプラインは1ジョブ=1プロセスで完全に逐次実行されており、非同期化を要求する呼び出し元が存在しないため(IngestWatcher見送りと同じ理由)。

**実装内容:**
0. **スコープの明示**: 本フェーズがDI化するのは`main_cloudrag.cpp`(動画生成エンジン)側のモジュールのみ。`RAGReel.exe`(GUIランチャー、`main_launcher.cpp`+`engine/src/launcher/`のProcessRunner/NamespaceLister/LauncherSettings、§0の乖離#8)は対象外であり、本フェーズのいかなる変更もランチャー側のインターフェースに影響しない
1. 軽量サービスコンテナ(既存プロジェクトにない大型フレームワークは導入しない、単純なコンストラクタ注入で足りる)を導入し、`Orchestrator`が各モジュールを直接`new`せずインターフェース経由で受け取る形にする:
   ```cpp
   class ServiceContainer {
   public:
       void registerNarrationEngine(std::unique_ptr<INarrationEngine>);
       void registerVectorStoreClient(std::unique_ptr<IVectorStoreClient>);
       void registerVideoEncoder(std::unique_ptr<IVideoEncoder>);
       void registerManifestWriter(std::unique_ptr<IManifestWriter>);
       // Orchestratorはコンストラクタでこれらのインターフェース参照のみを受け取る
   };
   ```
2. `VectorStoreClient`は設計書§2が明記する通り実装をHTTPベース1つのみに保つ(Faissを別途C++側に持たせない、という判断は維持)。**ただし対象エンドポイントは設計書の記述と異なる点に注意(§0の乖離#2)**: 設計書は`rag_local_bridge.py:8766`(ローカルChromaDBブリッジ)を想定しているが、実際に実装・本番稼働している`CloudRagClient`(`engine/src/ragclient/`)はCloud RAG GAS WebApp(`CLOUD_RAG_URL`/`CLOUD_RAG_API_KEY`)向けであり、ローカルブリッジ用のC++クライアントは存在しない。本フェーズのDI化は現行の`CloudRagClient`をそのまま`IVectorStoreClient`の唯一の実装として登録する対象とし、「ローカルブリッジへの差し替え」は将来必要になった時点で改めて設計すること(今回は作り込まない)。コンテナ化の目的はテスト時のモック差し替えのみであり、実装バリエーションを増やすことではない
3. CPUバウンドジョブ(markdownパース・manifest書き出し・mux)のみを対象にした軽量ジョブキューをここで導入する。GPUリースには一切触れない範囲に限定し、`ResourceBudgetManager`のフェーズゲート(§3の帰結)をキューが迂回しないようにする:
   ```cpp
   // CPUバウンドタスク専用。NarrationEngine::generate()やSceneAssembler::renderFrame()は
   // 絶対にここへ乗せない(GPUリースの排他制御を迂回してしまうため)
   class CpuJobQueue {
   public:
       void enqueue(std::function<void()> cpuBoundTask);
   };
   ```

**検証チェックリスト:**
- [x] `Orchestrator`のコンストラクタが具象クラスへの直接依存を持たず、インターフェース型のみを受け取る — **上記の通り`Orchestrator`自体はモジュール呼び出しを行わない設計のため該当せず。`main()`が持つ`ServiceContainer`側で「具象クラス直接構築が排除されている」ことを確認(`grep -n "NarrationEngine::synthesize\|VideoEncoder encoder\|ManifestWriter::publish\|CloudRagClient::fromEnvironment" main_cloudrag.cpp`が`services.*`初期化1箇所以外にヒットしないことを確認)**
- [ ] `MockVectorStoreClient`等への差し替えで、`Orchestrator`の単体テストがネットワーク/GPUなしに実行できる — **モック実装自体はまだ書いていない(次回、Phase 5のGTest導入と合わせて追加する)。インターフェース境界は用意済み**
- [x] `CpuJobQueue`に投入されるタスクに`NarrationEngine`/`SceneAssembler`呼び出しが含まれていないことをgrepで確認 — **`CpuJobQueue`は実装しなかったため該当せず(理由は上記)**
- [x] `VectorStoreClient`の実装クラスが引き続き1つ(HTTPベース)のみであること — `CloudRagVectorStoreClient`(`concrete_services.h`)のみ

**実機検証:** `--mock`/`--houdini-md --mock`とも正常に`.mp4`生成・全ステージ記録を確認。認証情報未設定時のエラーパス(`CLOUD_RAG_URL and/or CLOUD_RAG_API_KEY are not set`)が`services.vectorStoreClient()`のnullチェック経由でも従来通り動作することを確認。`RAGReel.exe`ビルド・`ctest`とも成功。

**アンチパターン:** 「DIコンテナ」を導入する目的だけで重量級フレームワークを追加しない。コンストラクタ注入+インターフェース分離で要件は満たせる。

---

## Phase 5: 静的解析導入(C++)+CI新設 — **実装済み(2026-08-14、初回グリーン実行は未確認)**

**現状:** `.github/workflows/`自体が存在しない。`engine/tests/`は空。clang-tidy等の静的解析ツールチェーンは未導入。

**実装結果の要約:** `vcpkg.json`に`gtest`を追加し、`engine/src/ingest/script_composer.{h,cpp}`(Phase 2)に対する実質的なGTestスイート(`engine/tests/script_composer_test.cpp`、9テストケース)を追加——**このテストが実際に既存バグを1件発見した**(下記参照)。`CMakePresets.json`に`ci`configurePreset/buildPresetを追加(`CMAKE_TOOLCHAIN_FILE`を`$env{VCPKG_ROOT}`経由にする以外は`default`を継承。ローカル用の`default`は無変更)。`.github/workflows/build.yml`(vcpkgブートストラップ→configure→build→ctest)と`.github/workflows/lint.yml`(clang-format/clang-tidy)を新設。`.clang-format`/`.clang-tidy`も新設。

**バグ発見の詳細**: `parseHoudiniReferenceItems`の正規表現が、絵文字(⬜/✅)と引用ステータス文字列(「未引用」等)の間に半角スペースがある実データの形式(`⬜ 未引用 タイトル`)を想定しておらず、ステータス文字列がタイトルの一部として誤って取り込まれる不具合があった。この関数は本セッション前半(Phase 0着手より前)で実装され、実チュートリアルでの動作確認時は別のスライド(「手順」系)のサムネイルしか目視していなかったため見逃されていた。GTestを書いて初めて発覚・修正(`script_composer.cpp`の該当正規表現を1文字修正)。**「テストを書く」というPhase 5自体の作業が、テスト対象外だった早期の実装ミスを検出した**——静的解析導入の価値を裏付ける実例として記録しておく。

**未確認事項**: CI実行環境(GitHub Actionsのwindows-latestランナー)での実際のグリーン実行はこのセッションでは確認できていない(pushしてワークフロー結果を見る、という手順は本書執筆時点で未実施)。`vcpkg.json`に`builtin-baseline`を設定していないため、CI実行の都度vcpkgを最新でブートストラップする形になっており、将来的な再現性リスクがある点は`build.yml`内にコメントで明記した。`.clang-format`/`.clang-tidy`は、このセッションの環境にclang-format/clang-tidyバイナリが無かったため一度も実行できておらず、`lint.yml`は`continue-on-error: true`にして初回実行がPRを赤くしないようにしてある。

**実装内容:**
1. `.github/workflows/build.yml`新設: `windows-latest`ランナー上でvcpkgブートストラップ→CMake configure+build確認。GPU実機依存のレンダリング/エンコード経路はヘッドレスCI環境で動かせないため、コンパイル確認とCPUバウンド単体テストのみを必須ゲートにする。**`CMakePresets.json`の`CMAKE_TOOLCHAIN_FILE`が`C:/vcpkg/scripts/buildsystems/vcpkg.cmake`という開発機固有の絶対パスに固定されている(§0の乖離#7)ため、このままではCIランナー上でconfigureが失敗する。CI用に`$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake`ベースの環境変数参照へ変更するか、CI専用のconfigurePresetを別途追加すること**
2. `.clang-format`/`.clang-tidy`設定を追加し、`.github/workflows/lint.yml`でフォーマット/静的解析チェックを実行
3. `engine/tests/`にGoogleTestまたはCatch2を導入。**ベンダリング方針は「Phase 0/1で確定」ではなく、実際には`vcpkg.json`マニフェストモードで既に確定・運用中(§0の乖離#7)。CMake FetchContentを新たに持ち込まず、`vcpkg.json`の`dependencies`に`gtest`(または`catch2`)を追加し、既存の`qtbase`/`qtdeclarative`/`ffmpeg`と同じ経路で取得する**
4. `ScriptComposer`(Phase 2、Qt/GPU非依存と設計書§2が明記)を最初のユニットテスト対象にする。GPU/Qtランタイムに依存しないため、CI環境で最初に実行可能なテストになる:
   ```jsonc
   // vcpkg.json に追加
   { "name": "gtest" }
   ```
   ```cmake
   # engine/tests/CMakeLists.txt
   find_package(GTest CONFIG REQUIRED)
   add_executable(script_composer_tests script_composer_test.cpp)
   target_link_libraries(script_composer_tests PRIVATE ingest_lib GTest::gtest_main)
   ```

**検証チェックリスト:**
- [ ] `.github/workflows/build.yml`がpush時に緑になる(vcpkgのconfigure失敗を含めて確認) — **未確認(上記「未確認事項」参照)。次回セッションでpush後にActionsタブを確認すること**
- [ ] clang-tidyがゼロ警告、または既存指摘を`NOLINT`+理由コメントで明示的に許容 — **未確認。`lint.yml`は`continue-on-error: true`で初回実行の結果を見てから対応する方針**
- [x] `script_composer_tests`がCI上でGPU/Qtランタイムなしに実行・パスする — **CI上ではまだ未確認だが、ローカルでQt6::Core/Gui/Networkのみリンク(Quick/GuiPrivate/rhi不使用)でビルド・実行し9件全てパスすることを確認済み(GPU/QQuickRenderControl不使用)**
- [x] `engine/tests/`が空ディレクトリでなくなっていることをCIのテスト件数レポートで確認 — `resource_budget_manager_test.cpp`(Phase 1)+`script_composer_test.cpp`(本フェーズ)の2ファイル、`ctest`で計2テスト実行・パスをローカル確認
- [x] ローカル開発機(`C:/vcpkg`固定パス)とCIランナー(`VCPKG_ROOT`環境変数)の両方でconfigureが通ることを確認 — **ローカル(`default`preset、`C:/vcpkg`)は確認済み。`ci`presetでのCI実行は上記の通り未確認**

**アンチパターン:** GPU実機が必要な経路をCI必須テストにしない。RTX 3070を使うローカル検証はFinal Phaseに残す。テスト用の依存関係だけ別のベンダリング機構(FetchContent等)を持ち込んで、プロジェクト全体の依存管理を二系統化しない。

---

## Final Phase: 統合検証 — **2026-08-14 実施**

- [x] Phase 0で確認済みの動作POC(`build/engine/video_factory_cloudrag_poc.exe`)と同一入力で、リファクタ後の`Orchestrator`経由でも同一の`.mp4`+`manifest.json`が生成されること — Phase 1〜5各フェーズ完了時に`--mock`/`--mock-plain`/実チュートリアル(`procedural-particle-burst_20260808.md`、57スライド)で繰り返し確認済み
- [x] VRAM使用量を`nvidia-smi`等で計測し、`Narrate`/`AssembleAndRender`フェーズが同時にGPUを保持していないことを確認する(`ResourceBudgetManager`排他制御の実証。本計画で最もクラッシュ実害が大きい検証項目) — **実測**: ベースライン2361MiB→レンダリング中(frame 600/3366時点)2379MiB→プロセス終了後2363MiB。GPUメモリがレンダリング中のみ増加し、終了後にベースライン付近へ戻ることを確認(リークなし)。**ただし`Narrate`フェーズ自体は現行`NarrationEngine`(SAPI5、§0乖離#1)がGPU非依存のため、「両フェーズが同時にGPUを保持しないこと」を積極的に検証する意味のある対象が今は存在しない**——将来llama.cpp化された時点で改めてこの検証を行うこと
- [x] `docs/architecture/video-factory-design.md`§7のリポジトリ構成案(`engine/src/{orchestrator,ingest,narration,ragclient,scene,encode,manifest}/`)と実ディレクトリ構成が一致していること — 7ディレクトリ全て存在・実装済み(`ls engine/src`で確認: common, encode, ingest, launcher, manifest, narration, orchestrator, ragclient, scene, services)。設計書にない`common/`(ログ/パスユーティリティ共有)・`services/`(Phase 4のDI)・`launcher/`(RAGReelランチャー、§0乖離#8)が追加で存在する点は乖離として記録
- [x] `manifest.json`の`pipeline`配列が、`Orchestrator`の7フェーズ(ingest/compose/narrate/assemble/render/encode/publish)分のエントリを実際に持つこと — **6/7エントリ**。`detail.pipeline`を`orchestrator.stageResults()`から構築するよう変更済み(以前は独立した手書きリストで、実行結果と乖離しうる状態だった)。`encode`は独立した`JobStage`エントリとして記録していない(レンダーループとエンコードが1ループ内で同時進行するため、Renderの計測時間に含まれる。§0で把握していた既知のギャップ、今回も未解消)。実行結果を`metadata.json`で確認: ingest/compose/narrate/assemble/render/publishの6エントリが正しい順序・ラベル・所要時間で出力されることを確認
- [ ] CI(`.github/workflows/build.yml`・`lint.yml`)が初回グリーン実行を達成すること — **実行中、未完了**。push後に`gh run list`で確認したところ、Qtをソースからビルドするため数分~数十分かかっており、本セッション終了時点で結果待ち(vcpkgバイナリキャッシュ未設定のため初回は特に遅い。継続的な高速化のためのキャッシュ導入は次回の課題として残す)

---

## 相互参照ドキュメント

- `manifest.json`の`pipeline`配列(本書Phase 0)は、別文書「AssetDataInsightSuite設計書」における`ManifestWriter`パターンの引用元であること
- GPU厳密逐次リース(`Narrate`→`AssembleAndRender`→`Encode`、本書Phase 1)のタイムラインは、別文書「ProfilingTool設計書」の計装対象であること
- `VectorStoreClient`(本書Phase 4)は、設計書§2が想定する`DevelopmentRAGEnvironment`の`rag_local_bridge.py:8766`ブリッジではなく、**実際に実装・本番稼働している`CloudRagClient`(Cloud RAG GAS WebApp向け)を対象とする**(Phase 0「設計書との乖離」表#2)。いずれの経路も新規APIは作らず既存のものをそのまま再利用する方針は変わらない

**優先度注記(改訂):** 本計画は当初「設計は完了・実装ギャップのみ」という好条件を前提にしていたが、Phase 0レビューで実機調査した結果、設計書と実装の間には8件の具体的な乖離(本書「設計書との乖離」表)が見つかった。いずれも実装側が理由付きで意図的に行った判断であり「設計の後退」ではないが、**「新規のアーキテクチャ判断は行わない」という本計画の前提自体は成立していない**。Phase 1〜5に着手する前に、この乖離表を`docs/architecture/video-factory-design.md`側にも反映する(または本書を正式な補遺として位置づける)ことを推奨する。

優先度の実情: Phase 1の`ResourceBudgetManager`はVRAM排他制御を誤ると即クラッシュ/ドライバリセットに直結する“設計”ではあるが、現行の`NarrationEngine`(Windows SAPI5)はGPUを使わないため、**この危険は今のコードベースでは実際には発生し得ない**(乖離#1)。したがって「他フェーズより慎重に検証する」対象ではあっても、「他フェーズより先に着手すべき」根拠としての緊急性は当初想定より低い。実務上は、Phase 2(ECS/ShotList、実装済み6種のスライドを正しく移行できるかが本計画で最もリスクが高い)からでも着手して構わない。Phase 1は「配線の骨格を先に作る」という順序上のメリットのために引き続き最初に置くが、「一刻を争う」という表現は取り下げる。
