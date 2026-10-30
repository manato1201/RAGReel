# 第12章 拡張レシピ

**使い方**: 「〇〇を足したい」から該当レシピを選び、**手順 → 確認 → よくある漏れ**の順に進める。変更前に第11章の「変更の種類 → 検査」も見ること。

| # | やりたいこと | 主な変更先 |
|---|---|---|
| 1 | パイプラインに新しい段を足す | `orchestrator/` `main_cloudrag.cpp` `web/public/app.js` |
| 2 | 新しい声(ナレーションの実装)を足す | `narration/` `services/concrete_services.h` |
| 3 | 読み(発音辞書)を足す | `assets/tts_dictionary.json` `tests/pronunciation_test.cpp` |
| 4 | `metadata`/`manifest` に項目を足す | `manifest/` `tests/` `web/public/` |
| 5 | 新しいトランジションを足す | `compare/python_render/engine.py` |
| 6 | ダッシュボードにページ・部品を足す | `web/public/` `engine/CMakeLists.txt` `installer/ragreel.iss` |
| 7 | 共有ギャラリー(Worker)にルートを足す | `cloudflare/ragreel-gallery/` |
| 8 | 図を足す・直す | `tools/build_diagrams.py` |

---

## レシピ1: パイプラインに新しい段を足す

例: 動画の完成後に「要約を作る(Summarize)」段を足す。

1. `engine/src/orchestrator/job_pipeline.h`: `enum class JobStage` に `Summarize` を、`jobStageLabel`(日本語)と `jobStageKey`(`"summarize"`)に対応を足す。**3か所そろえる**。
2. `main_cloudrag.cpp`: 該当の位置で処理を呼び、前後で時間を測って `orchestrator.recordStage(JobStage::Summarize, 成功か, 秒数, エラー文)`。
3. 外部サービスを呼ぶなら、`services/interfaces.h` に `I〇〇` を足し、`concrete_services.h` に実物のアダプタ、テスト用の偽物を用意(第4章)。
4. 失敗しても動画を失いたくない段なら、**例外を握って警告にする**(クラウド送信と同じ作法)。
5. 画面: `metadata.json` の `pipeline[]` に自動で出る。`app.js` の `stepsHTML` が段の `stage`/`label` をそのまま出すか確認する。

**確認**: 該当ロジックの単体テスト → `--mock` で通しを実行 → 生成された `metadata.json` の `pipeline[]` に新しい段がある → `video.html` の「生成過程」に出る。
**よくある漏れ**: `jobStageKey` の対応忘れ / 段の記録を成功時だけ書いて失敗時に書かない / 旧い動画(新しい段が無い)で画面が壊れる。

---

## レシピ2: 新しい声(ナレーションの実装)を足す

1. `narration/` に新しいクラス(例 `my_tts_engine.{h,cpp}`)。`NarrationResult` を返す: WAV のパス・長さ・エンジン名・**文ごとの区間**(分かるなら `segmentsExact = true`)。
2. `services/concrete_services.h` に、`INarrationEngine` に合わせるアダプタを足す。
3. `makeNarrationEngineFromEnvironment()` に、環境変数 `RAGREEL_TTS_BACKEND` の新しい値と、`auto` での優先順位を足す。**失敗時の切り替え先**(`FallbackNarrationEngine`)を決める。
4. 設定が要るなら、環境変数名を決める(`RAGREEL_<名前>_URL` など)。ランチャーから渡すなら `launcher/launcher_settings.*` と `qml/SettingsTab.qml`。
5. `engine/CMakeLists.txt`(本体とテストの両方のソース一覧)。
6. `engine/tests/narration_test.cpp`(または専用のテスト): 偽のサーバーで、正常・拒否・不正な音声・到達不能。

**確認**: テスト → 実際に `RAGREEL_TTS_BACKEND=<新>` で `--mock` 実行 → ログの「どの声を使うか」の行が正しい。
**よくある漏れ**: サンプルレートの変換(第3章)/ User-Agent / 失敗時の例外にエラー文を含める / `docs/learn/06` と図(`narration-backend`)の更新。

---

## レシピ3: 読み(発音辞書)を足す

1. `engine/assets/tts_dictionary.json` に追加: `{ "surface": "Karma", "pronunciation": "カーマ", "accent_type": 0 }`(読みは全角カタカナ)。
2. `engine/tests/pronunciation_test.cpp` に1ケース(置換される・単語の一部には当たらない)。
3. `ctest` の `narration_tests`。

**確認**: 実際に読ませて聞く(SAPI かクラウドで)。字幕は元の綴りのままか。
**よくある漏れ**: 長い語と短い語の衝突(長い語が優先される仕様を踏まえる)/ 大文字小文字 / **新しい資源ファイルは、`engine/CMakeLists.txt` のビルド後コピー(1ファイルずつ)と、インストーラーの `[Files]` に足す**(辞書はコピー漏れで、読まれずに動いていたことがある)。

---

## レシピ4: `metadata`/`manifest` に項目を足す

例: 動画の言語 `language` を足す。

1. `manifest/manifest_writer.h`: `ManifestVideoDetail`(または `ManifestEntryInfo`)に項目を足す。**既定値は「無い」を表す値**(0・空)。
2. `manifest_writer.cpp`: 値があるときだけ JSON に書く(「0を書かない」)。`.js` ミラーは同じ JSON から作られるので自動。
3. `main_cloudrag.cpp`: 値を詰める。
4. `engine/tests/manifest_writer_test.cpp`: **項目あり・無しの2ケース**。
5. JS: 表示する側で「無ければ省略」。`escapeHTML` を通す。
6. 共有ギャラリーに送る分(`entry`/`metadata` は同じ JSON)は、Worker 側の変更不要(中身を解釈しない)。
7. `docs/technical-reference.md` の §10.2(スキーマ)と、この資料の第8章を更新する。

**確認**: テスト → 新しい動画で表示 → **旧い動画**(項目なし)で壊れない → https(共有ギャラリー)でも。
**よくある漏れ**: 旧い動画の考慮 / JS 側だけ先に変える(契約は同時に)/ 数値の単位(秒かミリ秒か)をドキュメントに書かない。

---

## レシピ5: 新しいトランジションを足す

(第10章の要約の詳細版)

1. `compare/python_render/engine.py`:
   ```python
   @lru_cache(maxsize=2)
   def spiral_thresholds(w: int, h: int) -> np.ndarray:
       """渦巻き状に開く順番(0..1)。"""
       ...
       return arr.astype(np.float32)
   ```
2. `SUPPORTED_TRANSITIONS` に名前、`_THRESHOLD_KINDS` に `"spiral": (spiral_thresholds, 0.08)`(ぼかし幅)。
3. `compare/tests/test_pipeline.py`: 端点(`p=0` で A、`p=1` で B と一致)・途中で混ざる・同じ `p` で同じ結果・**既存の「3種以内」のテストが壊れていないか**。
4. `hyperframes/build_project.py`: `clip-path` で書けるなら `HF_SUPPORTED` に追加。書けないなら**拒否のまま**(理由を残す)。
5. `compare/README.md` と、技術リファレンス §26 の表を更新。

**確認**: `python -m unittest discover -s tests`、p=0.25/0.5/0.75 を並べて目視。
**よくある漏れ**: 端点の丸め誤差(254)/ しきい値が 0〜1 に収まっていない / ぼかし幅 0 で割り算。

---

## レシピ6: ダッシュボードにページ・部品を足す

### 新しいページ(例: `stats.html`)
1. `web/public/stats.html` を作る。`<script src="app.js">` を読み、`mountTopbar()` で共通の上部を出す。データは `loadManifest()`(`file://` でも動く)。
2. **クラシックスクリプトのみ**(モジュール不可。第8章)。
3. **配布に載せる**: `engine/CMakeLists.txt` の `foreach(_dashboard_file ...)` に追加、`installer/ragreel.iss` の `[Files]` に追加。**足し忘れると「共有ギャラリーにはあるが、アプリが開く画面には無い」になる**。
4. 共有ギャラリーは `web/public/` が丸ごと上がるので、Worker の変更は不要(動的なルートは別。レシピ7)。
5. ローカルのアプリでは使えないページなら、リンクを `HAS_COMPARE` のように**共有ギャラリーのときだけ**出す。

### 新しい部品・色・アイコン
- アイコン: `app.js` の `ICONS` に、**同じ線幅(2・丸端)**で足す。絵文字は使わない。
- 色: **役割以外に使わない**。新しい色トークンは先に `docs/ui-rules.md` の表に書き、`styles.css` のライト・ダーク**両方**(`[data-theme="dark"]` と `prefers-color-scheme` の2か所)に足す。`check_contrast.js` の `PAIRS` に対比の組を足す。
- 動き: 1画面2種類まで。`prefers-reduced-motion` で止める。`docs/ui-rules.md` §2 の表に足す。

**確認**: `check_contrast.js` / `check_gallery_layout.js` / `node --check` / `file://` で開く / ダーク / 「動きを減らす」 / 旧い動画・空の状態。

---

## レシピ7: 共有ギャラリー(Worker)にルートを足す

例: `GET /api/stats`(本数と総再生時間)。

1. `cloudflare/ragreel-gallery/src/index.js`: `fetch` のルート分岐に追加。**書き込みなら `isAuthorized`**、読み取りなら認証なし。入力の検証(`isValidId` など)を忘れない。
2. `wrangler.jsonc` の `run_worker_first` に、パス(`/api/*` に含まれるなら不要)を足す。
3. エンジン側から呼ぶなら、`manifest/` に呼び出しを足し、**失敗しても本処理を止めない**。
4. `wrangler dev` でローカル確認(ローカルの R2 が使える)→ `npx wrangler deploy` → HTTP で確認(認証あり/なし・不正な入力)。
5. 第9章のルート表・技術リファレンス §24・図(`cloud-gallery`)を更新。

**よくある漏れ**: `run_worker_first` / 認証 / `cache-control`(最新が要る JSON は `no-store`、不変のファイルは長く)/ CORS(別のオリジンから呼ぶ場合)/ 公開リポジトリに秘密を書く。

---

## レシピ8: 図を足す・直す

図は `tools/build_diagrams.py` が `docs/diagrams/*.svg` を作る。部品は `tools/diagram_kit.py`。

```python
s = Svg(1200, 600, "図のタイトル", "読み上げ用の説明")
s.group(40, 110, 400, 400, "利用者のPC", GRAY, sub="Windows")      # 枠
s.tile(160, 260, "gear", "動画ファクトリー", "video_factory…", ORANGE[0])   # アイコンタイル
s.box(600, 260, 200, 60, ["クラウド音声", "POST /tts"], BLUE)       # 箱
s.diamond(600, 400, 240, 90, ["成功?"], YELLOW)                      # 分岐
s.arrow([(192, 260), (500, 260)])                                    # 実線(主な流れ)
s.arrow([(192, 300), (500, 300)], "dash")                            # 破線(補助)
s.badge(340, 260, 1)                                                 # 番号
legend(s, 60, 520, [(1, "手順の説明"), (2, "…")])                    # 番号の凡例
s.save(os.path.join(OUT, "my-figure.svg"))
```

**作法(参照図の様式)**
- **グループ枠 + ラベル**で「どこにあるか」(PC / Cloudflare / ブラウザ)を示す。
- **アイコンタイル**: 太字の名前 + 灰色の等幅の補足(ファイル名・URL)。
- **番号バッジ** + 下の凡例で、時系列を説明する(図の中に長い文章を置かない)。
- **実線=主な流れ、破線=補助**。色は役割(オレンジ=主操作、ミント=完了、ピンク=失敗、青=情報、黄=注意、紫=RAG)。
- 矢印と文字が重ならないように、**ラベルの下にある矢印は文字の下端から始める**。

**確認**: `python tools/build_diagrams.py` → SVG をブラウザで開いて目視(文字の重なり・はみ出し・矢印の向き)。文書には、画像の記法(感嘆符 + `[説明](../diagrams/<名前>.svg)`)で入れる。

---

## 共通のチェックリスト(どのレシピでも)

- [ ] 変更に対応する**テスト**を足した/更新した
- [ ] 契約(manifest/metadata)に触れたなら **C++・JS・テスト・ドキュメントを同時に**変えた
- [ ] **旧い動画・空の状態**で壊れない
- [ ] 新しいファイルを **CMake / インストーラー** の一覧に足した
- [ ] **ドキュメントと図**を更新した(この資料・技術リファレンス・計画書の状況)
- [ ] 「確認した/未確認」を分けて書いた(第11章)
- [ ] 公開リポジトリに**秘密を入れていない**
