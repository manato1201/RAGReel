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
            sub="Orchestrator が順序・GPUリース・各段の実測時間を管理")

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
    s.text(1180, 545, "各段の所要時間 → metadata.json の pipeline[]", size=12, fill=MUTED)
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
    s.arrow([(450, 150), (120, 150), (120, 770), (690, 770)], "dash")
    s.label(260, 142, "sapi を指定", color=MUTED)

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

    s.group(470, 110, 620, 540, "CLOUDFLARE", ORANGE, sub="ragreel-gallery")
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
    s = Svg(1300, 640, "同じ入力を3方式で動画にする比較",
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
        s.group(360, y - 60, 520, 120, name, color, sub=sub)
        s.tile(450, y, "gear", "描画", "", color[0])
        s.tile(650, y, "film", "動画", "", color[0])
        s.arrow([(482, y), (618, y)])
        s.box(800, y, 130, 40, ["出力1秒あたり", speed], GRAY, size=11)
        s.arrow([(280, 275), (360, y)], "dash")

    s.tile(1100, 330, "chart", "比較ページ", "web/public/compare.html", BLUE[0])
    for y, *_ in lanes:
        s.arrow([(880, y), (1030, 330)], "dash")

    s.box(1080, 520, 380, 110,
          ["トランジションの方針", "章ごとに固定・1本で使うのは3種まで", "既定 fade / slide / hex", "候補 mask / shape / liquid / blinds / pixelate"],
          YELLOW, size=12)
    s.text(650, 600, "計測は1回・1入力。出力の長さが揃っておらず、実装行数も同列には比べられない(compare/README.md)",
           size=12, fill=MUTED)
    s.save(os.path.join(OUT, "compare-three-ways.svg"))


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    for fn in (system_overview, pipeline_flow, narration_backend, cloud_gallery, dashboard_loading, compare_three):
        fn()
        print("wrote", fn.__name__)
