# ナレーション音声の品質向上(VOICEVOX 対応)

作成日: 2026-10-11 / 対象: `engine/src/narration/`、`video_factory_cloudrag_poc.exe`

## 1. 何が問題だったか
- 従来の音声は Windows の SAPI5 で、この PC に入っている日本語の声は **「Microsoft Haruka Desktop」の1種類だけ**(`System.Speech` で確認)。古い方式の声で、抑揚が平坦。
- 専門用語(Houdini / Niagara など)の読みも制御できなかった。

## 2. 採った方式
- **VOICEVOX ENGINE をローカルで動かし、HTTP で合成する**(`/audio_query` → 調整 → `/synthesis`)。
  - 無料、API キー不要、回数制限なし、通信はローカルのみ。
  - 長文は文ごとに分割して合成し、間(既定 0.25 秒)を入れて連結する。
  - 出力は従来と同じ **44.1kHz / 16bit / mono の WAV**(下流のエンコーダは変更なし)。
- **エンジンが無いとき・途中で止まったときは SAPI に自動で戻る**(動画が無音にならない)。
- 専門用語は **ユーザー辞書**(`engine/assets/tts_dictionary.json`)で読みを固定する。

## 3. 使い方
### 3.1 エンジンの起動(どちらか)
| 方式 | コマンド / 手順 | メモ |
|---|---|---|
| Docker(CPU) | `docker run --rm -p 127.0.0.1:50021:50021 voicevox/voicevox_engine:cpu-latest` | 公式の例。localhost のみに公開する。**Docker Hub のタグ一覧で確認できた CPU 版(amd64)は `cpu-0.26.0-dev` などの dev 版で、`cpu-latest` が一覧に見当たらなかった**。取得できない場合は、タグ一覧(hub.docker.com/r/voicevox/voicevox_engine/tags)で安定版を確認する |
| Docker(NVIDIA GPU) | `voicevox/voicevox_engine:nvidia-latest` を `--gpus all` で | 未確認。CPU でも文単位の合成なら実用的な速度のはず(要計測) |
| インストーラ版 | 公式サイトの VOICEVOX をインストールし、起動しておく | エディタ起動時にエンジンも上がる(版により異なる) |

**注意**: このPCの `C:\Users\matuu\AppData\Local\Programs\VOICEVOX` は空のフォルダだった(未インストール)。Docker Hub の表示では、CPU 版の圧縮サイズは約 1.9GB、NVIDIA 版は約 3.0〜3.4GB(展開後はさらに大きい)。ディスクの空きを確認してから取得する。

### 3.2 環境変数
| 変数 | 既定 | 内容 |
|---|---|---|
| `RAGREEL_TTS_BACKEND` | `auto` | `auto`(VOICEVOX が応答すれば使う。**導入済みで停止中なら、自動で起動する**)/ `voicevox` / `sapi`(従来) |
| `RAGREEL_VOICEVOX_EXE` | (未設定) | VOICEVOX ENGINE の `run.exe`。未設定のときは `%LOCALAPPDATA%\Programs\VOICEVOX\vv-engine\run.exe` を探す |
| `RAGREEL_VOICEVOX_URL` | `http://127.0.0.1:50021` | エンジンの URL |
| `RAGREEL_VOICEVOX_SPEAKER` | **`春日部つむぎ`** | 話者名(部分一致)。別の声にするときだけ設定する |
| `RAGREEL_VOICEVOX_STYLE` | `ノーマル` | スタイル名 |
| `RAGREEL_VOICEVOX_SPEAKER_ID` | (未設定) | スタイル ID を直接指定(名前より優先) |
| `RAGREEL_VOICEVOX_SPEED` | `1.0` | 話速(0.5〜2.0。解説は 1.05〜1.15 が聞きやすいことが多い) |
| `RAGREEL_VOICEVOX_INTONATION` | `1.0` | 抑揚(1.0 より大きいと強くなる) |
| `RAGREEL_VOICEVOX_PITCH` | `0.0` | 音高 |
| `RAGREEL_VOICEVOX_GAP` | `0.25` | 文と文の間(秒) |
| `RAGREEL_TTS_DICTIONARY` | `<exe>/assets/tts_dictionary.json` | 読み替え・ユーザー辞書の JSON のパス(2026-10-19 から、既定で exe の隣のファイルを使う) |

**既定の声は「春日部つむぎ」(スタイル: ノーマル)**。別の声にする場合は `RAGREEL_VOICEVOX_SPEAKER` か `RAGREEL_VOICEVOX_SPEAKER_ID` を設定する。エンジンに該当の声が無い場合は、エラーにして SAPI に戻る(別の声に黙って切り替えない)。

### 3.3 声を選ぶ(聴き比べ)
```bash
python tools/tts_ab_test.py --names 春日部つむぎ --speed 1.05          # つむぎだけ
python tools/tts_ab_test.py --names 春日部つむぎ 四国めたん --speed 1.05  # 比較したいとき
```
`tts_samples/` に、専門用語・数字・長文の3種類の WAV ができる。SAPI の声との比較にも使う。

### 3.4 辞書
`engine/assets/tts_dictionary.json` は `{"surface","pronunciation","accent_type"}` の配列。
- `pronunciation` は全角カタカナ。`accent_type` はアクセント核の位置(0 = 平板)。
- **同梱のアクセント型は暫定値(多くは 0)**。聴いて不自然なものは直す。
- エンジン側にすでにある単語(同じ表記)は追加しない。

### 3.5 春日部つむぎを使うときの表記
動画・サイトの説明やフッターに **「VOICEVOX:春日部つむぎ」** を表示する(書式・条件は、春日部つむぎの公式の利用規約で確認する。この資料では規約の本文までは確認していない)。

## 4. ライセンスとクレジット(重要)
- エンジンは LGPL。**声(キャラクター)ごとに別の利用規約**がある。
- 動画の公開時は、**「VOICEVOX:<キャラクター名>」のクレジット表記**を動画の説明などに入れる(各キャラクターの規約で書式を確認する)。
- この資料では、各キャラクターの規約の詳細までは確認していない。公開(身内向けを超える場合)の前に、使う声の公式規約を確認する。

## 5. 検証した内容と、未検証の内容
| 項目 | 状態 |
|---|---|
| 文分割・正規化・話者名の解決・クエリ調整・WAV の読み書きとリサンプル | **単体テスト 11 件が通過**(`narration_tests`) |
| エンジン本体・エンドポイント(`/audio_query` `/synthesis` `/speakers` `/user_dict_word`) | 公式の仕様と検索結果に基づく実装。**実エンジンでの動作は未確認**(この PC にエンジンが無いため) |
| 聴感の品質 | 未確認。`tools/tts_ab_test.py` で聴き比べて決める |
| `outputSamplingRate` を無視する版 | コード側で 44.1kHz へリサンプルするため影響しない |

## 6. 次の候補(必要になったら)
| 候補 | 狙い | 注意 |
|---|---|---|
| AivisSpeech(VOICEVOX 互換 API) | より自然な日本語を狙う | 声のモデルごとに規約が異なる。コード変更は URL の切り替え程度の見込み(要確認) |
| Style-Bert-VITS2 JP-Extra | 自然性が高いとの評価がある | AGPL。モデルごとの規約。導入・速度は未確認 |
| 音声後処理(ラウドネス統一・BGM とのバランス) | 聞き取りやすさ | 今回は対象外 |

## 7. 2026-10-19 の追記: 「音声が良くなった実感が無い」の原因と対策

### 7.1 何が起きていたか(実機で確認)
| # | 事実 | 影響 |
|---|---|---|
| 1 | VOICEVOX は**導入済み**(`%LOCALAPPDATA%\Programs\VOICEVOX`、ENGINE 0.25.2)。ただし、**エンジンが起動していないと、黙って従来の SAPI(Haruka)に戻る**作りだった。エディタを開いていない限り、動画はいつも従来の声 | 向上が一度も効いていなかった |
| 2 | どちらの声で作ったかが、ログにも画面にも出なかった | 気づけなかった |
| 3 | `tts_dictionary.json` が **exe の隣にコピーされていなかった**。VOICEVOX のユーザー辞書も、環境変数を設定しない限り、読み込まれていなかった | 「Houdini」「VEX」などの読みが改善されていなかった |
| 4 | SAPI の経路には、**読み替えの処理が無かった** | 従来の声は、英語の用語を日本語の声のまま読み、崩れていた |

### 7.2 対策(実装済み)
- **自動起動:** `auto` のとき、VOICEVOX が応答しなければ、導入済みの ENGINE(`run.exe`)を起動して、最大90秒待つ。起動した ENGINE は、動画エンジンの標準出力・標準エラーを引き継がない(ログが埋まらない)。導入も、ダウンロードも、しない。
- **どの声かを必ず出す:** ログに `Narration engine: ...`(選択の理由)と `Narration voice: ...`(実際に使った声)を出す。Houdini パネルの進捗にも出る。SAPI になったときは「従来の音声です。VOICEVOXを起動してください」と出る。
- **辞書:** CMake が `tts_dictionary.json` を exe の隣にコピーする。VOICEVOX も、既定でそれを読む。Houdini の用語(Attribute Wrangle、VEX、SOP …)を47語追加した(全72語)。
- **SAPI にも読み替え:** SAPI で読む前に、辞書の語をカタカナに置き換え、残った大文字の略語(ABC)は、文字の読み(エービーシー)にする。字幕は、元の表記のまま。(`narration/pronunciation.{h,cpp}`)
- **手順ごとの音声:** Houdini の台本つきの動画は、スライドごとに別々に音声を作って連結する。スライドの長さは、実際の音声の長さで決まる(§technical-reference 22)。

### 7.3 確認した結果
- 単体テスト: `narration_tests` 32件(読み替え・手順ごとの音声の連結・時刻)、`script_composer_tests` 24件が通過。
- 実機: VOICEVOX ENGINE 0.25.2(春日部つむぎ)で、実際のチュートリアルから動画を生成して確認した(下記)。**聴感の評価は、人が聞いて行う**。

### 7.4 VOICEVOX が起動しているか確かめる
```powershell
curl http://127.0.0.1:50021/version      # "0.25.2" のように版が返れば起動している
```
動画のログに `Narration voice: VOICEVOX:春日部つむぎ(ノーマル)` と出ていれば、その声で作られている。`Narration voice: SAPI:...` なら、従来の声。

## 8. 2026-10-21 の追記: 導入不要のクラウド音声

配布(企業の技術者・学生)で、各自のPCに VOICEVOX を入れる手間を無くすため、**Google Cloud Text-to-Speech を、DevelopmentRAGEnvironment の Worker(`POST /tts`)経由で使う**経路を、最優先にした。

- 優先順: **クラウド音声 → VOICEVOX(あれば) → SAPI**。クラウドが失敗したときだけ、ローカルの音声を用意する。
- 声: 既定は `ja-JP-Neural2-B`。`ja-JP-Chirp3-HD-*` も使える(Worker が許可する声は、Neural2 と Chirp 3 HD だけ)。聞き比べ用に、DevelopmentRAGEnvironment の `scripts/tts_sample.py`。
- 上限: 鍵ごと・全体の1日の文字数(Worker側)。同じ文章は、キャッシュから返して数えない。
- **クレジット:** クラウド音声では、VOICEVOX のクレジット表記は要らない(VOICEVOX の声を使ったときだけ)。
- 詳しくは、`technical-reference.md` §23.1。
