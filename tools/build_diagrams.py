"""Regenerates docs/diagrams/*.svg.

    python tools/build_diagrams.py

Edit the figure functions below (text, tiles, arrows) and re-run; the SVGs are
committed so the docs render on GitHub without any build step. The look is
defined once in tools/diagram_kit.py.
"""
import os

from diagram_kit import (BLUE, GRAY, MINT, ORANGE, PINK, PURPLE, YELLOW, INK, MUTED, Svg, legend)

OUT = os.path.join(os.path.dirname(__file__), "..", "docs", "diagrams")


def system_overview():
    s = Svg(1440, 800, "RAGReel システム構成図",
            "利用者のPC(RAGReel.exe とエンジンと output/)、Cloudflare 上の Cloud RAG・音声合成・共有ギャラリー、閲覧者の関係")

    # inputs
    s.tile(130, 210, "user", "利用者", "質問文 / 設定", INK)
    s.tile(130, 350, "layers", "Houdini", "チュートリアル .md", PURPLE[0])

    # PC
    s.group(270, 110, 430, 540, "利用者のPC", GRAY, sub="Windows")
    s.tile(360, 210, "terminal", "RAGReel.exe", "GUI ランチャー", BLUE[0])
    s.tile(600, 350, "gear", "動画ファクトリー", "video_factory_cloudrag_poc", ORANGE[0])
    s.tile(600, 505, "folder", "output/", "動画 + ダッシュボード", MINT[0])
    s.tile(360, 505, "monitor", "ブラウザ", "file:// で開く", INK)

    # Cloudflare
    s.group(900, 110, 330, 540, "CLOUDFLARE", ORANGE)
    s.group(920, 165, 290, 300, "DevelopmentRAGEnvironment", PURPLE, dashed=True, sub="別リポジトリ")
    s.group(920, 490, 290, 140, "ragreel-gallery", ORANGE, dashed=True, sub="本リポジトリ cloudflare/")
    s.tile(1065, 235, "search", "Cloud RAG", "/query ・ Notion × Gemini", PURPLE[0])
    s.tile(1065, 380, "mic", "音声合成", "/tts → Google TTS", PURPLE[0])
    s.tile(1065, 545, "bucket", "共有ギャラリー", "Worker + R2", ORANGE[0])

    s.tile(1340, 545, "globe", "閲覧者", "ブラウザ / Houdiniパネル", INK)

    # flows
    s.arrow([(162, 210), (328, 210)])
    s.badge(245, 210, 1)
    s.arrow([(392, 210), (600, 210), (600, 318)])
    s.label(496, 202, "サブプロセスで起動", color=MUTED)
    s.arrow([(162, 350), (568, 350)], "dash")
    s.label(225, 342, "--houdini-md", color=MUTED, mono=True, size=11)

    s.arrow([(632, 335), (840, 335), (840, 235), (1033, 235)])
    s.badge(840, 285, 2)
    s.arrow([(632, 350), (800, 350), (800, 380), (1033, 380)])
    s.badge(800, 365, 3)
    s.arrow([(632, 365), (760, 365), (760, 545), (1033, 545)], "dash")
    s.badge(760, 455, 6)
    s.badge(632, 312, 4, "#d9591f")

    s.arrow([(600, 428), (600, 473)], "ok")
    s.badge(600, 450, 5)
    s.arrow([(568, 505), (392, 505)], "dash")
    s.badge(480, 505, 7)
    s.arrow([(1308, 545), (1097, 545)])
    s.badge(1250, 545, 7)

    legend(s, 60, 705, [
        (1, "利用者が質問文・設定を入れて RAGReel.exe を起動(Houdini からはエンジンを直接呼ぶ)"),
        (2, "エンジンが Cloud RAG に質問し、回答と出典を受け取る(HTTPS POST /query)"),
        (3, "ナレーションは文ごとに /tts へ送り、WAV を受け取る(失敗したら PC 内の音声へ)"),
        (4, "スライド分割 → 描画 → エンコードで、VP9+Opus の .webm を作る(→ pipeline-flow)"),
        (5, "ManifestWriter が output/ へ動画・サムネイル・metadata・manifest を書く"),
        (6, "(任意)共有ギャラリーへ送る。URL とトークンが無ければ送らない(→ cloud-gallery)"),
        (7, "閲覧: PC 内は file://、共有ギャラリーは https(→ dashboard-loading)"),
    ], col_w=700, size=12)
    s.save(os.path.join(OUT, "system-overview.svg"))


def pipeline_flow():
    s = Svg(1440, 660, "動画生成パイプライン",
            "取り込み・構成・ナレーション・シーン組立・レンダリング・エンコード・公開の7段と、各段の成果物・GPUリース")
    s.pill(250, 56, 380, 34, "入力: 質問文 + dbKey / Houdini の .md・台本", BLUE, 13)
    s.pill(1190, 56, 380, 34, "出力: video.webm + ダッシュボード", MINT, 13)
    s.group(30, 110, 1380, 480, "video_factory_cloudrag_poc.exe", ORANGE,
            sub="main() が段を進め、Orchestrator が GPU リースと所要時間を記録")

    stages = [
        ("search", "取り込み", "ingest", ["Cloud RAG へ質問する", "または .md・台本を読む"], "RAG 回答 + 出典"),
        ("list", "構成", "compose", ["スライド分割(3段階)", "図解 PNG・要点を付ける"], "Slide 一覧"),
        ("mic", "ナレーション", "narrate", ["文ごとに音声を作る", "実測の長さを取る"], "WAV + 文ごとの時刻"),
        ("layers", "シーン組立", "assemble", ["QML シーンを用意する", "(ヘッドレス描画)"], "QML シーン(GPU 上)"),
        ("image", "レンダリング", "render", ["フレームごとに QML へ", "値を渡して描く"], "RGBA フレーム"),
        ("film", "エンコード", "encode", ["FFmpeg で映像と音声を", "VP9 + Opus にまとめる"], "video.webm"),
        ("upload", "公開", "publish", ["ManifestWriter が書く", "(任意でクラウドへ送る)"], "manifest / metadata"),
    ]
    xs = [130 + 197 * i for i in range(7)]
    for i, (icon, title, key, rows, product) in enumerate(stages):
        x = xs[i]
        s.tile(x, 215, icon, title, key, ORANGE[0])
        s.badge(x - 32, 183, i + 1, "#d9591f")
        s.box(x, 355, 172, 74, rows, GRAY, size=12)
        s.arrow([(x, 400), (x, 432)], "dash")
        s.box(x, 465, 172, 52, [product], MINT, size=12)
        if i < 6:
            s.arrow([(x + 34, 215), (xs[i + 1] - 34, 215)])

    # GPU leases
    s.box(xs[2], 540, 172, 30, ["GPU リース: ナレーション"], BLUE, size=11)
    s.box((xs[3] + xs[4]) / 2, 540, 369, 30, ["GPU リース: シーン組立 〜 レンダリング(排他・逐次)"], BLUE, size=11)
    s.text(1180, 540, "各段の所要時間 → metadata.json の pipeline[]", size=12, fill=MUTED)
    s.text(720, 572, "エンコードは描画のループの中で行うため、独立した所要時間は記録しない(pipeline[] に encode は出ない)",
           size=12, fill=MUTED)
    s.save(os.path.join(OUT, "pipeline-flow.svg"))


def narration_backend():
    s = Svg(1200, 850, "ナレーション音声の選び方",
            "RAGREEL_TTS_BACKEND の値に応じ、クラウド音声・VOICEVOX・SAPI のどれを使うかの分岐")
    s.pill(600, 40, 400, 34, "ナレーションを作る(NarrationEngine)", ORANGE)
    s.arrow([(600, 58), (600, 105)])
    s.diamond(600, 150, 300, 90, ["RAGREEL_TTS_BACKEND", "の値は?"], YELLOW)
    s.arrow([(600, 195), (600, 245)])
    s.label(600, 222, "auto(既定) / cloud", color=MUTED)

    s.diamond(600, 290, 260, 90, ["URL と キーが", "ある?"], YELLOW)
    s.arrow([(600, 335), (600, 388)])
    s.label(620, 366, "ある", color=MINT[0])
    s.box(600, 420, 280, 64, ["クラウド音声", "POST {URL}/tts(文ごと)"], BLUE)
    s.arrow([(600, 452), (600, 495)])
    s.diamond(600, 540, 200, 90, ["成功?"], YELLOW)
    s.arrow([(600, 585), (600, 637)])
    s.label(620, 615, "成功", color=MINT[0])
    s.pill(600, 665, 300, 34, "クラウドの WAV + 文ごとの時刻", MINT)

    # local branch
    s.arrow([(730, 290), (930, 290), (930, 508)])
    s.label(830, 282, "ない", color=PINK[0])
    s.arrow([(700, 540), (800, 540)])
    s.label(750, 532, "失敗", color=PINK[0])
    s.box(930, 540, 260, 64, ["ローカル音声", "失敗したときに初めて用意"], GRAY)
    s.arrow([(930, 572), (930, 620)])
    s.diamond(930, 665, 240, 90, ["VOICEVOX が", "使える?"], YELLOW)
    s.arrow([(1050, 665), (1080, 665), (1080, 742)])
    s.label(1075, 655, "使える", color=MINT[0])
    s.arrow([(810, 665), (780, 665), (780, 742)])
    s.label(785, 655, "使えない", color=PINK[0])
    s.pill(1080, 770, 180, 34, "VOICEVOX", MINT)
    s.pill(780, 770, 180, 34, "SAPI(Windows 標準)", MINT, 12)

    # forced values
    s.arrow([(750, 150), (1185, 150), (1185, 540), (1062, 540)], "dash")
    s.label(950, 142, "voicevox を指定(使えなければ警告して SAPI)", color=MUTED)
    s.arrow([(450, 150), (60, 150), (60, 770), (690, 770)], "dash")
    s.label(250, 142, "sapi を指定", color=MUTED)

    s.box(260, 540, 300, 90, ["切り替えの仕組み", "FallbackNarrationEngine が", "失敗を受けて次を呼ぶ"], YELLOW, size=12)
    s.text(600, 830, "字幕用の時刻は、文ごとに測った長さ(segmentsExact)から作る", size=12, fill=MUTED)
    s.save(os.path.join(OUT, "narration-backend.svg"))


def cloud_gallery():
    s = Svg(1440, 800, "共有ギャラリー(Cloudflare)の構成",
            "エンジンのアップロード、Worker、R2、静的アセット、閲覧者の関係")
    s.group(40, 110, 300, 540, "利用者のPC", GRAY, sub="Windows")
    s.tile(190, 260, "gear", "動画ファクトリー", "CloudGalleryUploader", ORANGE[0])
    s.box(190, 470, 250, 96, ["送り先の設定(環境変数)", "GALLERY_UPLOAD_URL", "GALLERY_UPLOAD_TOKEN"], YELLOW, size=12)
    s.text(190, 560, "無ければ送らない(ローカルだけ)", size=11, fill=MUTED)

    s.group(470, 110, 670, 540, "CLOUDFLARE", ORANGE, sub="ragreel-gallery")
    s.tile(560, 330, "key", "UPLOAD_TOKEN", "Worker の secret", YELLOW[0])
    s.tile(760, 330, "bolt", "Worker", "src/index.js", ORANGE[0])
    s.tile(980, 330, "layers", "静的アセット", "web/public/ (ASSETS)", BLUE[0])
    s.tile(760, 560, "bucket", "R2 バケット", "ragreel-gallery", ORANGE[0])
    s.box(980, 560, 270, 112, ["R2 のキー", "manifest.json", "videos/<id>/video.webm", "videos/<id>/thumb.png",
                               "videos/<id>/metadata.json"], GRAY, size=11, dashed=True)

    s.tile(1290, 330, "globe", "閲覧者", "ブラウザ / Houdiniパネル", INK)

    s.arrow([(222, 250), (745, 250), (745, 298)], "accent")
    s.badge(480, 250, 1, "#d9591f")
    s.label(380, 242, "POST /api/videos", color=ORANGE[0], mono=True, size=11)
    s.arrow([(592, 330), (728, 330)], "dash")
    s.arrow([(760, 412), (760, 528)])
    s.badge(760, 470, 2)
    s.arrow([(792, 330), (948, 330)], "dash")
    s.label(870, 322, "上記以外", color=MUTED)
    s.arrow([(1290, 298), (1290, 215), (775, 215), (775, 298)])
    s.badge(1030, 215, 3)
    s.arrow([(1258, 330), (1012, 330)])
    s.badge(1170, 330, 4)
    s.text(1290, 450, "管理ドロワーから", size=12, fill=MUTED)
    s.text(1290, 468, "削除もできる", size=12, fill=MUTED)

    legend(s, 60, 705, [
        (1, "アップロード: multipart(動画・サムネイル・entry・metadata)+ Authorization: Bearer トークン"),
        (2, "R2 に保存し、manifest.json の先頭へ追加(同じ id は置き換える)"),
        (3, "閲覧: GET /manifest.json・/videos/<id>/metadata.json・video.webm(Range で 206)"),
        (4, "画面の部品(gallery.html・app.js など)は静的アセットとして直接返す"),
        ("!", "削除は DELETE /api/videos/<id>(同じトークンが要る)。トークン無しは 401"),
    ], col_w=700, size=12)
    s.save(os.path.join(OUT, "cloud-gallery.svg"))


def dashboard_loading():
    s = Svg(1200, 790, "ダッシュボードのデータの読み方",
            "file:// と https で、manifest と metadata の読み方が変わる分岐")
    s.pill(600, 40, 440, 34, "ページを開く(gallery.html / video.html)", ORANGE)
    s.arrow([(600, 58), (600, 118)])
    s.diamond(600, 170, 340, 100, ["location.protocol が", "file: ?"], YELLOW)

    s.arrow([(430, 170), (260, 170), (260, 270)])
    s.label(340, 162, "はい(ローカルで開いた)", color=MINT[0])
    s.arrow([(770, 170), (940, 170), (940, 270)])
    s.label(860, 162, "いいえ(https)", color=BLUE[0])

    s.box(260, 315, 340, 90, ["<script src=\"manifest.js\"> で読む", "window.__VF_MANIFEST__ = [ … ]", "(fetch はブラウザに拒否される)"],
          MINT, size=12)
    s.box(940, 315, 340, 90, ["fetch(\"manifest.json\")", "共有ギャラリー: R2 の最新", "http 配信: 同じフォルダの .json"],
          BLUE, size=12)

    s.arrow([(260, 360), (260, 420), (560, 420), (560, 450)])
    s.arrow([(940, 360), (940, 420), (640, 420), (640, 450)])
    s.box(600, 480, 360, 56, ["manifest を配列として受け取る", "新しい順・1本1エントリ"], GRAY)
    s.arrow([(600, 508), (600, 548)])
    s.box(600, 580, 360, 56, ["カードを描く", "列数は件数と幅から(pickColumns)"], GRAY)
    s.arrow([(600, 608), (600, 650)])
    s.box(600, 690, 520, 70, ["カードを開く → video.html?id=…", "metadata.js(file:) / metadata.json(https)を同じ方式で読む",
                              "章タイムライン・出典・字幕(addTextTrack)を描く"], GRAY, size=12)

    s.box(1000, 530, 330, 110, ["なぜ .js が要るか", "file:// では fetch が失敗し、", "ES モジュールも読めない。", "だから classic script だけ使う"],
          YELLOW, size=12)
    s.box(200, 530, 330, 110, ["書き出し側(ManifestWriter)", "manifest.json と manifest.js を", "metadata.json と metadata.js を", "いつも同時に書く"],
          PURPLE, size=12)
    s.save(os.path.join(OUT, "dashboard-loading.svg"))


def compare_three():
    s = Svg(1300, 700, "同じ入力を3方式で動画にする比較",
            "Qt/QML、HyperFrames、Python の3方式の入力・出力・実測と、トランジションの方針")
    s.group(30, 110, 250, 330, "同じ入力", BLUE)
    s.tile(155, 200, "file", "手順書(.md)", "tutorial", BLUE[0])
    s.tile(155, 340, "mic", "音声", "narration.wav", BLUE[0])

    lanes = [
        (190, "Qt / QML", "標準 ・ engine/", "0.18 秒/秒", ORANGE),
        (330, "HyperFrames", "HTML + GSAP", "1.3〜1.5 秒/秒", PURPLE),
        (470, "Python", "NumPy + Pillow + ffmpeg", "2.2〜2.7 秒/秒", MINT),
    ]
    for y, name, sub, speed, color in lanes:
        s.group(360, y - 62, 520, 128, name, color, sub=sub)
        s.tile(450, y, "gear", "描画", "", color[0])
        s.tile(650, y, "film", "動画", "", color[0])
        s.arrow([(482, y), (618, y)])
        s.box(800, y, 130, 40, ["出力1秒あたり", speed], GRAY, size=11)
        s.arrow([(280, 330), (320, 330), (320, y), (360, y)], "dash")

    s.tile(1100, 330, "chart", "比較ページ", "web/public/compare.html", BLUE[0])
    for y, *_ in lanes:
        s.arrow([(880, y), (990, y), (990, 330), (1030, 330)], "dash")

    s.box(1080, 590, 380, 110,
          ["トランジションの方針", "章ごとに固定・1本で使うのは3種まで", "既定 fade / slide / hex", "候補 mask / shape / liquid / blinds / pixelate"],
          YELLOW, size=12)
    s.text(560, 665, "計測は1回・1入力。出力の長さが揃っておらず、実装行数も同列には比べられない(compare/README.md)",
           size=12, fill=MUTED)
    s.save(os.path.join(OUT, "compare-three-ways.svg"))


def generation_sequence():
    """1 本の動画ができるまでの、誰が誰に何を頼むかの時間順(シーケンス図)。"""
    s = Svg(1200, 1420, "動画 1 本ができるまでのやり取り",
            "利用者・RAGReel.exe・エンジン・Cloud RAG・音声合成・output/・共有ギャラリーの間で、起動から公開までに交わす要求と応答の時間順")
    X = dict(user=200, launcher=350, engine=540, rag=740, tts=890, out=1000, gallery=1105)
    heads = [
        ("user", "user", "利用者", ""), ("launcher", "terminal", "RAGReel.exe", "GUI"),
        ("engine", "gear", "エンジン", "video_factory_cloudrag_poc"), ("rag", "search", "Cloud RAG", "/query"),
        ("tts", "mic", "音声合成", "/tts"), ("out", "folder", "output/", "PC 内"),
        ("gallery", "bucket", "共有ギャラリー", "任意"),
    ]
    colors = dict(user=INK, launcher=BLUE[0], engine=ORANGE[0], rag=PURPLE[0], tts=PURPLE[0], out=MINT[0], gallery=ORANGE[0])
    bands = [  # (label, y0, y1, color)
        ("起動", 165, 275, GRAY), ("取り込み", 295, 490, BLUE), ("構成", 510, 585, GRAY),
        ("ナレーション", 605, 705, PURPLE), ("描画と書き出し", 725, 850, ORANGE), ("公開", 870, 990, MINT),
    ]
    for label, y0, y1, color in bands:
        s.group(30, y0, 1140, y1 - y0, label, color)
    for key, icon, title, sub in heads:
        s.tile(X[key], 62, icon, title, sub or None, colors[key])
    for key in ("launcher", "engine", "rag", "tts", "out", "gallery"):
        end = {"rag": 490, "tts": 705}.get(key, 990)
        s.lifeline(X[key], 165, end)
    s.lifeline(X["user"], 165, 275)

    tint = {b[0]: b[3][1] for b in bands}
    s.message(X["user"], X["launcher"], 220, "生成", 1, bg=tint["起動"])
    s.message(X["launcher"], X["engine"], 258, "起動", 2, bg=tint["起動"])
    s.message(X["engine"], X["rag"], 340, "POST /query", 3, bg=tint["取り込み"])
    s.message(X["rag"], X["engine"], 380, "回答 + 出典", kind="dash", bg=tint["取り込み"])
    s.message(X["engine"], X["rag"], 430, "追補の質問", 4, bg=tint["取り込み"])
    s.message(X["rag"], X["engine"], 470, "図解 + 説明", kind="dash", bg=tint["取り込み"])
    s.box(X["engine"], 548, 250, 50, ["スライド分割(3段階)", "要点・図解 PNG を付ける"], GRAY, size=13)
    s.badge(X["engine"] - 125 - 18, 548, 5)
    s.message(X["engine"], X["tts"], 656, "POST /tts(文ごと)", 6, bg=tint["ナレーション"])
    s.message(X["tts"], X["engine"], 692, "WAV", kind="dash", bg=tint["ナレーション"])
    s.box(X["engine"], 776, 300, 50, ["GPU リースの中で", "組立 → 描画 → エンコード"], ORANGE, size=13)
    s.badge(X["engine"] - 150 - 18, 776, 7)
    s.message(X["engine"], X["launcher"], 830, "進捗 k / n", 8, kind="dash", bg=tint["描画と書き出し"])
    s.message(X["engine"], X["out"], 922, "動画・サムネイル・metadata・manifest", 9, bg=tint["公開"])
    s.message(X["engine"], X["gallery"], 968, "POST /api/videos(任意)", 10, bg=tint["公開"])
    s.message(X["engine"], X["launcher"], 1030, "終了コード(0 = 完了)", kind="dash")

    legend(s, 40, 1090, [
        (1, "「Cloud RAGクエリ」タブに質問内容と dbKey を入れて生成(Houdini の .md を渡す入口もある → input-modes)"),
        (2, "サブプロセスで起動する。CLOUD_RAG_URL・KEY と GALLERY_UPLOAD_* は環境変数で渡す"),
        (3, "質問して、回答と出典を受け取る(Bearer 認証。--mock のときは送らない)"),
        (4, "追補: 図解 1 つとコード例の説明を頼む。任意で、失敗しても続行(Houdini の .md でも送る)"),
        (5, "スライド分割 → 要点を付け、図解は mmdc で PNG にする(スライドごとの追加質問もある)"),
        (6, "文ごとに /tts へ送って WAV を受け取る。失敗したら PC 内の音声(→ narration-backend)"),
        (7, "シーン組立 → フレームごとの描画 → VP9 + Opus のエンコードを、1 つの GPU リースの中で行う"),
        (8, "ランチャーが、エンジンの標準出力「Rendered frame k / n」を読んで進捗バーにする"),
        (9, "ManifestWriter が output/ へ書く(→ output-layout)"),
        (10, "GALLERY_UPLOAD_URL と TOKEN があるときだけ共有ギャラリーへ送る(→ cloud-gallery)"),
    ], cols=1, size=13, row_h=30)
    s.save(os.path.join(OUT, "generation-sequence.svg"))


def engine_structure():
    """engine/src の部品と、依存の向き(コードを読む人の地図)。"""
    s = Svg(1200, 1020, "エンジンの部品と依存の向き",
            "RAGReel.exe(launcher/)がエンジンを起動し、エンジンの main が Orchestrator と ServiceContainer を持ち、4 つのサービスはインターフェース越し、取り込みとシーン組立は直接呼ぶ")
    s.group(40, 50, 1120, 160, "RAGReel.exe", BLUE, sub="main_launcher.cpp ・ qml/Launcher.qml ・ engine/src/launcher/")
    cards = [
        (150, ["LauncherSettings", "接続設定を保存する"]),
        (365, ["ProcessRunner", "エンジンを起動し進捗を読む"]),
        (580, ["NamespaceLister", "使える dbKey を取ってくる"]),
        (795, ["TutorialQueue", "Houdini の .md を順に処理"]),
        (1010, ["VideoHistory", "output/ の履歴を開く"]),
    ]
    for x, rows in cards:
        s.box(x, 135, 200, 66, rows, BLUE, size=13)
    s.arrow([(440, 168), (440, 278)], "accent")
    s.label(590, 243, "サブプロセスで起動(引数 + 環境変数)", color=ORANGE[0], size=13)

    s.group(30, 270, 1140, 700, "video_factory_cloudrag_poc.exe", ORANGE, sub="engine/src/")
    s.group(55, 330, 225, 610, "main_cloudrag.cpp", GRAY, sub="main()")
    s.box(167, 395, 195, 66, ["段の順序を決める", "Ingest → … → Publish"], GRAY, size=13)
    s.box(167, 560, 195, 96, ["Orchestrator", "GPU リースの排他", "各段の所要時間を記録", "orchestrator/"], ORANGE, size=13)
    s.box(167, 740, 195, 96, ["ServiceContainer", "4 つのサービスを", "登録して渡す", "services/"], ORANGE, size=13)

    # direct calls
    s.box(900, 375, 470, 66, ["ingest/  ScriptComposer ・ houdini_script", "スライド分割・要点・Houdini 台本の読み込み"], MINT, size=13)
    s.box(900, 470, 470, 66, ["scene/  SceneAssembler  +  qml/CloudRagScene.qml", "ヘッドレス QML 描画(GPU リースの中)"], MINT, size=13)
    s.arrow([(265, 380), (665, 380)])
    s.label(470, 372, "直接呼ぶ", color=INK, size=13)
    s.arrow([(265, 410), (300, 410), (300, 470), (665, 470)])

    # services behind interfaces
    s.group(330, 540, 250, 395, "services/interfaces.h", PURPLE, sub="")
    rows = [
        (610, "INarrationEngine", ["narration/  音声を作る", "クラウド音声 → VOICEVOX → SAPI"]),
        (700, "IVectorStoreClient", ["ragclient/  CloudRagClient", "Cloud RAG の /query ・ /me/namespaces"]),
        (790, "IVideoEncoderFactory", ["encode/  VideoEncoder", "FFmpeg で VP9 + Opus の .webm"]),
        (880 - 0, None, None),
    ]
    for y, name, impl in rows[:3]:
        s.box(455, y, 210, 50, [name], PURPLE, size=13)
        s.box(900, y, 470, 66, impl, BLUE, size=13)
        s.arrow([(560, y), (665, y)], "dash")
    s.box(455, 880, 210, 50, ["IManifestWriter"], PURPLE, size=13)
    s.box(900, 880, 470, 66, ["manifest/  ManifestWriter ・ CloudGalleryUploader", "output/ へ書く + 共有ギャラリーへ送る(任意)"], BLUE, size=13)
    s.arrow([(560, 880), (665, 880)], "dash")
    s.arrow([(265, 740), (330, 740)])
    s.label(298, 732, "登録", color=INK, size=12)
    s.text(612, 598, "実装", size=12, fill=MUTED)
    s.text(600, 1000, "実線 = 呼び出し ・ 破線 = インターフェースを実装する実体 ・ テストではインターフェースに偽物を差し込める", size=13, fill=MUTED)
    s.save(os.path.join(OUT, "engine-structure.svg"))


def input_modes():
    """エンジンが「何を元に」台本を作るかの分かれ道(CLI の引数)。"""
    s = Svg(1200, 800, "エンジンの入力の選び方",
            "video_factory_cloudrag_poc.exe の引数で、Houdini のチュートリアル・モック・Cloud RAG の質問のどれを元にするかを決める分岐")
    s.pill(600, 36, 560, 34, "video_factory_cloudrag_poc.exe  [質問文] [dbKey]  [フラグ]", ORANGE, 14)
    s.box(215, 130, 400, 92, ["この入力を渡す側", "RAGReel.exe「Cloud RAGクエリ」: 質問文 + dbKey", "RAGReel.exe「Houdiniチュートリアル」: --houdini-md ほか",
                              "Houdini 側(別リポジトリ): --houdini-script も付ける"], GRAY, size=12, dashed=True)
    s.arrow([(600, 53), (600, 100)])
    s.diamond(600, 145, 300, 90, ["--houdini-md が", "ある?"], YELLOW, 14)

    # yes -> houdini
    s.arrow([(750, 145), (1150, 145), (1150, 205)])
    s.label(950, 137, "ある", color=MINT[0], size=13)
    s.group(925, 205, 250, 290, "Houdini の取り込み", PURPLE)
    s.box(1050, 275, 225, 100, ["Houdini チュートリアル", ".md を読む(題と本文)", "--houdini-json:", "ノード構成の要約(任意)"], PURPLE, size=13)
    s.box(1050, 382, 225, 50, ["--houdini-screenshots", "手順の画面(任意)"], GRAY, size=12, dashed=True)
    s.box(1050, 447, 225, 50, ["--houdini-script 初心者向け台本", "(任意。手順ごとの音声)"], GRAY, size=12, dashed=True)

    # no -> mock?
    s.arrow([(600, 190), (600, 235)])
    s.label(620, 217, "ない", color=PINK[0], size=13)
    s.diamond(600, 280, 300, 90, ["--mock か", "--mock-plain?"], YELLOW, 14)
    s.arrow([(450, 280), (330, 280)])
    s.label(390, 272, "ある", color=MINT[0], size=13)
    s.box(200, 280, 250, 66, ["モック回答(通信なし)", "--mock-plain は見出しなしの文章"], BLUE, size=13)

    s.arrow([(600, 325), (600, 370)])
    s.label(620, 352, "ない", color=PINK[0], size=13)
    s.diamond(600, 415, 340, 90, ["CLOUD_RAG_URL と", "API キーがある?"], YELLOW, 14)
    s.arrow([(770, 415), (830, 415), (830, 528)])
    s.label(800, 407, "ない", color=PINK[0], size=13)
    s.pill(830, 548, 170, 34, "エラー終了(コード 1)", PINK, 12)
    s.arrow([(600, 460), (600, 505)])
    s.label(620, 487, "ある", color=MINT[0], size=13)
    s.box(600, 535, 240, 56, ["Cloud RAG に質問", "POST /query(質問文, dbKey)"], PURPLE, size=13)

    # merge
    s.arrow([(200, 313), (200, 640), (340, 640), (340, 668)])
    s.arrow([(600, 563), (600, 668)])
    s.arrow([(1050, 495), (1050, 640), (860, 640), (860, 668)])
    s.box(600, 705, 640, 64, ["追補の質問(図解・コード説明)", "Cloud RAG が使えて、--mock でないときだけ。失敗しても続行"], BLUE, size=13)
    s.arrow([(600, 737), (600, 765)])
    s.pill(600, 782, 600, 32, "以降は共通: スライド分割 → ナレーション → 描画 → 公開(→ pipeline-flow)", MINT, 13)
    s.save(os.path.join(OUT, "input-modes.svg"))


def output_layout():
    """書き出した動画がどこに置かれ、誰が読むか(output/ の中身)。"""
    s = Svg(1200, 690, "output/ の中身と、書く側・読む側",
            "エンジンの隣の output/ にある画面の部品・一覧・動画 1 本ごとのファイルと、それらを書く ManifestWriter、読む画面、任意の共有ギャラリー")
    # writers
    s.group(30, 90, 230, 560, "書く側", GRAY)
    s.box(145, 190, 190, 86, ["CMake(ビルド時)", "web/public/ の画面の部品を", "output/ へコピー"], GRAY, size=13)
    s.box(145, 470, 190, 110, ["ManifestWriter", "動画 1 本を公開するたびに", "videos/<id>/ と一覧を", "書き換える"], ORANGE, size=13)

    # output/
    s.group(330, 90, 560, 560, "output/", MINT, sub="exe の隣・PC 内")
    s.group(350, 130, 520, 100, "画面の部品", BLUE)
    s.text(610, 170, "index.html ・ gallery.html ・ video.html ・ random.html", size=13, mono=True, fill=INK)
    s.text(610, 200, "styles.css ・ app.js ・ hero-disc.js", size=13, mono=True, fill=INK)
    s.group(350, 270, 520, 130, "一覧", YELLOW)
    s.box(470, 335, 200, 64, ["manifest.json", "新しい順・同じ id は置換"], YELLOW, size=13)
    s.box(710, 335, 200, 64, ["manifest.js", "同じ内容(file:// 用)"], YELLOW, size=13)
    s.group(350, 440, 520, 190, "videos/<id>/  1 本ごと", MINT)
    files = [
        (440, 490, "video.webm", "動画(VP9 + Opus)"), (610, 490, "thumb.png", "一覧の表紙"),
        (780, 490, "subtitles.vtt", "字幕(あるときだけ)"),
        (440, 575, "metadata.json", "章・出典・各段の時間"), (610, 575, "metadata.js", "同じ内容(file:// 用)"),
    ]
    for x, y, name, note in files:
        s.box(x, y, 156, 60, [name, note], MINT, size=12)

    # arrows: writers -> output
    s.arrow([(240, 190), (350, 190)])
    s.arrow([(240, 470), (300, 470), (300, 340), (350, 340)])
    s.arrow([(300, 470), (300, 520), (350, 520)])
    # readers
    s.group(950, 90, 220, 560, "読む側", BLUE)
    s.box(1060, 335, 180, 76, ["gallery.html", "一覧のカードを描く"], BLUE, size=13)
    s.box(1060, 520, 180, 76, ["video.html?id=…", "動画・章・出典・字幕"], BLUE, size=13)
    s.arrow([(870, 335), (970, 335)], "dash")
    s.arrow([(870, 520), (970, 520)], "dash")
    s.save(os.path.join(OUT, "output-layout.svg"))


# 図を足したら、ここへ関数を加える(tools/check_docs.py もこの一覧を使う)
FIGURES = (system_overview, pipeline_flow, narration_backend, cloud_gallery, dashboard_loading, compare_three,
           generation_sequence, engine_structure, input_modes, output_layout)


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    for fn in FIGURES:
        fn()
        print("wrote", fn.__name__)
