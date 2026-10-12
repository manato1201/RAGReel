"""V6 の検索精度を、言い換えクエリ10問で測る(Top-1 / Top-3)。

正解は「そのステップ番号の画像」(network / viewport どちらでも可)。クエリは説明文の
コピーではなく言い換え。比較するのは:
  hash/text   : 依存ゼロの文字2-gram(説明文と照合)
  st/text     : 多言語テキストモデルで説明文と照合
  st/image    : 画像そのものを CLIP の画像側で埋め込み、テキストと照合(画像検索の可否の判定)
"""
from __future__ import annotations

import argparse
import json
import sys
import os

sys.path.insert(0, os.path.dirname(__file__))
import embed_search as es  # noqa: E402

# (クエリ, 正解ステップ)  ※ chashu-pork-block_20261004 の10ステップ
QUERIES = [
    ("Geometryオブジェクトを新しく作ってchashuと名付ける", 1),
    ("球体のノードを置いて base という名前にする", 2),
    ("凹凸を出すためにMountainを追加して bumps にする", 3),
    ("色を塗るためにAttribute Wrangleノードを作成する", 4),
    ("球のプリミティブタイプをpolymeshにして半径と頻度を指定する", 5),
    ("WrangleのVEXpression欄に色付けのコードを書き込む", 6),
    ("baseの出力を次のノードbumpsの入力につなげる", 7),
    ("bumpsからcolorへ線を繋ぐ", 8),
    ("山の高さとエレメントサイズを変更する", 9),
    ("cookして何もエラーが出ないことを確かめる", 10),
]


def evaluate(index, mode):
    top1 = top3 = 0
    detail = []
    for q, step in QUERIES:
        hits = es.search(index, q, top_k=10, mode=mode)
        # 同じステップの network/viewport 重複を畳んで順位を数える
        seen, order = set(), []
        for h in hits:
            if h["step"] not in seen:
                seen.add(h["step"]); order.append(h["step"])
        top1 += order[:1] == [step]
        top3 += step in order[:3]
        detail.append({"q": q, "answer": step, "got": order[:3]})
    n = len(QUERIES)
    return {"top1": round(top1 / n, 2), "top3": round(top3 / n, 2), "n": n, "detail": detail}


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--shots", required=True)
    ap.add_argument("--st", action="store_true", help="sentence-transformers(CLIP + 多言語)も評価する")
    ap.add_argument("--out")
    a = ap.parse_args()

    result = {}
    h = es.HashEmbedder()
    result["hash/text"] = evaluate(es.build_index(a.shots, h, use_images=False), "text")
    if a.st:
        e = es.STEmbedder("sentence-transformers/clip-ViT-B-32", text_model_id="sentence-transformers/clip-ViT-B-32-multilingual-v1")
        idx = es.build_index(a.shots, e, use_images=True)
        result["st/text"] = evaluate(idx, "text")
        result["st/image"] = evaluate(idx, "image")
        result["st/auto"] = evaluate(idx, "auto")
    for k, v in result.items():
        print(f"{k:10s} top1={v['top1']:.2f} top3={v['top3']:.2f} (n={v['n']})")
    if a.out:
        json.dump(result, open(a.out, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
