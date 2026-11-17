# 書き出した動画の検査(`tools/verify_export.py`)

書き出した動画そのものを**測る**(「自分で作った動画を自分で採点しない」。QUALITY_UPLIFT QL5 / Z79)。ffmpeg / ffprobe と Pillow が要る。

```powershell
python tools/verify_export.py web/public/videos/<id>                       # 1 本(video.mp4 + metadata.json + 字幕)
python tools/verify_export.py out/video.mp4 --contact-sheet sheet.png      # コンタクトシート(拍ごとの前・拍・後)も書き出す
python tools/verify_export.py web/public/videos --all                      # ギャラリーの全部
python tools/verify_export.py a.mp4 --twin b.mp4                           # 同じ入力を 2 回書き出した 2 本が、同じフレームか
python -m unittest discover -s tools/tests -v                              # 検査自体の試験(悪い動画が、実際に不合格になる)
```

| 検査 | 内容 |
|---|---|
| `av-duration` | 映像と音声の長さ。エンジンが宣言した無音の余白(`kTailPaddingSeconds` = 1.5 秒)を許し、**音声が映像より長い**・**余白を超えて映像が続く**を不合格にする |
| `colors` | 目標色(エンジンが宣言した `#10140f`)との色差 ΔE。**開始・中間・終わり近く**で測り、途中の色のずれを捕まえる |
| `black-frames` / `audio-levels` | 途中の黒いフレーム・音割れ(ピーク 0 dBFS)・ほぼ無音 |
| `streams` | 映像・音声の有無、解像度、fps(旧版の 30fps は警告) |
| `subtitles` / `chapters` | 字幕・章(`metadata.json`)の時刻が、動画の長さと整合するか(無ければ skip) |
| `contact-sheet` | 拍ごと(章の開始。無ければ ffmpeg の場面検出)の「前・拍・後」の静止画。**合否は付けず、人が見る** |
| `--twin` | 決定性(同じ入力の 2 回の書き出しで、フレームが同じか) |

基準は `tools/video_spec.json`(エンジンの宣言した値。出どころをコメントに書いてある)。終了コード: 0 = 不合格なし / 1 = 不合格あり。**書き出しのたびに自動では走らない**(エンジンの C++ には組み込んでいない)。

**ギャラリーの古い書き出し(7 月・SAPI)が長さの検査で不合格になるのは、エンジンの不具合のため**: SAPI の WAV は `data` の後ろに `EVNT` チャンクを持ち、エンジンはそれまで音声の長さに数えて、ナレーションを約 0.7% 長く報告していた(映像が 0.8〜4 秒長く、字幕も後ろほど遅れる)。**2026-11-11 に修正**(`wav_util::pcmDurationSeconds`。実機の SAPI で、報告値と実際の長さが一致することを確認)。**直った後の書き出しは、この検査を通る**。古い動画は、書き出し直すと直る。

(README の「動作確認手順」から移したもの。テスト全般は [getting-started.md](getting-started.md)、学習資料は [第11章 テスト](learn/11-testing.md)。)
