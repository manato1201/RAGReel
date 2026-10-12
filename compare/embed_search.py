"""V6: スクリーンショット / クリップをテキストで検索する(素材探し)。

Houdini ビューポート画像は、そのステップの説明文(manifest の result)と対にして索引化する。
埋め込みバックエンドは差し替え可能:

  hash   : 文字2-gram のハッシュ埋め込み(依存ゼロ。動作確認・フォールバック用。意味検索ではない)
  st     : sentence-transformers のモデルを model_id で指定。テキストはそのモデルで埋め込み、
           画像を直接埋め込めるモデル(例 clip-ViT-B-32)なら画像も同じ空間に入れて検索できる。
           テキストのみのモデル(EmbeddingGemma 等)では、画像は説明文で代用する(画像そのものは使えない)。

DevelopmentRAGEnvironment の R1(EmbeddingGemma 2 との精度比較)と共有できるよう、
Embedder インターフェースを揃えてある。モデルIDや次元数は引数で指定する。
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from typing import Optional

import numpy as np


class HashEmbedder:
    name = "hash-2gram"

    def __init__(self, dim: int = 512):
        self.dim = dim

    def embed_texts(self, texts):
        out = np.zeros((len(texts), self.dim), np.float32)
        for i, t in enumerate(texts):
            t = "".join(t.lower().split())
            grams = [t[j:j + 2] for j in range(max(1, len(t) - 1))] + list(t)
            for g in grams:
                h = int(hashlib.md5(g.encode("utf-8")).hexdigest()[:8], 16)
                out[i, h % self.dim] += 1.0 if len(g) == 2 else 0.5
        n = np.linalg.norm(out, axis=1, keepdims=True)
        return out / np.maximum(n, 1e-9)

    def embed_images(self, paths):
        return None  # 画像そのものは埋め込めない


class STEmbedder:
    def __init__(self, model_id: str, dim: Optional[int] = None, device: Optional[str] = None,
                 text_model_id: Optional[str] = None):
        """text_model_id: 画像モデルと同じ空間に入る多言語テキストモデル(例: clip-ViT-B-32 の画像側 +
        clip-ViT-B-32-multilingual-v1 のテキスト側)。未指定なら model_id が両方を担当する。"""
        from sentence_transformers import SentenceTransformer  # 遅延import
        self.model = SentenceTransformer(model_id, device=device, truncate_dim=dim)
        self.text_model = SentenceTransformer(text_model_id, device=device, truncate_dim=dim) if text_model_id else self.model
        self.name = f"st:{model_id}" + (f"+{text_model_id}" if text_model_id else "") + (f"@{dim}" if dim else "")

    def embed_texts(self, texts):
        return np.asarray(self.text_model.encode(list(texts), normalize_embeddings=True), np.float32)

    def embed_images(self, paths):
        from PIL import Image
        try:
            imgs = [Image.open(p).convert("RGB") for p in paths]
            return np.asarray(self.model.encode(imgs, normalize_embeddings=True), np.float32)
        except Exception:
            return None  # テキストのみのモデル


def build_index(shots_json: str, embedder, use_images: bool = True) -> dict:
    shots = json.load(open(shots_json, encoding="utf-8"))
    items = []
    for s in shots:
        for key in ("network", "viewport"):
            p = s.get(key)
            if p and os.path.exists(p):
                items.append({"step": s["step"], "kind": key, "path": p,
                              "text": f"{s.get('tool', '')} {s.get('result', '')}".strip()})
    text_vec = embedder.embed_texts([it["text"] for it in items])
    img_vec = embedder.embed_images([it["path"] for it in items]) if use_images else None
    return {"items": items, "text_vec": text_vec, "img_vec": img_vec, "embedder": embedder}


def search(index: dict, query: str, top_k: int = 3, mode: str = "auto") -> list:
    """mode: text(説明文と照合) / image(画像埋め込みと照合) / auto(画像があれば両方の最大)。"""
    q = index["embedder"].embed_texts([query])[0]
    score = index["text_vec"] @ q
    if mode in ("image", "auto") and index["img_vec"] is not None:
        img = index["img_vec"] @ q
        score = img if mode == "image" else np.maximum(score, img)
    order = np.argsort(-score)[:top_k]
    return [{"score": round(float(score[i]), 3), **{k: index["items"][i][k] for k in ("step", "kind", "path", "text")}}
            for i in order]


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--shots", required=True)
    ap.add_argument("--query", required=True)
    ap.add_argument("--backend", default="hash", help="hash | st")
    ap.add_argument("--model-id", default="sentence-transformers/clip-ViT-B-32")
    ap.add_argument("--dim", type=int, default=None)
    ap.add_argument("--text-model-id", default=None)
    ap.add_argument("--mode", default="auto", help="text | image | auto")
    a = ap.parse_args()
    emb = HashEmbedder() if a.backend == "hash" else STEmbedder(a.model_id, a.dim, text_model_id=a.text_model_id)
    idx = build_index(a.shots, emb)
    print(json.dumps({"embedder": emb.name, "hits": search(idx, a.query, mode=a.mode)}, ensure_ascii=False, indent=1))
