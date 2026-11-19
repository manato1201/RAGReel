// 分けて送るアップロード(POST/PUT/DELETE /api/videos/uploads...)の単体テスト。R2 のマルチパートは、メモリ上の偽物で置き換える。
//   cd cloudflare/ragreel-gallery && npm test
import assert from "node:assert/strict";
import test from "node:test";

import worker from "../src/index.js";

const TOKEN = "test-upload-token";

function fakeBucket() {
  const store = new Map();
  const uploads = new Map(); // uploadId → { key, parts: Map(n → Uint8Array), aborted, completed }
  let counter = 0;
  const bucket = {
    store,
    uploads,
    async get(key) {
      if (!store.has(key)) return null;
      const value = store.get(key);
      return { body: value, async json() { return JSON.parse(value); } };
    },
    async put(key, value) {
      store.set(key, typeof value === "string" ? value : value);
    },
    async delete(keys) {
      for (const k of Array.isArray(keys) ? keys : [keys]) store.delete(k);
    },
    async createMultipartUpload(key) {
      const uploadId = `up${++counter}`;
      uploads.set(uploadId, { key, parts: new Map(), aborted: false, completed: false });
      return { uploadId, key };
    },
    resumeMultipartUpload(key, uploadId) {
      const state = uploads.get(uploadId);
      if (!state || state.key !== key) throw new Error("no such upload");
      return {
        async uploadPart(partNumber, bytes) {
          state.parts.set(partNumber, new Uint8Array(bytes));
          return { partNumber, etag: `etag-${partNumber}` };
        },
        async complete(parts) {
          const merged = [];
          for (const p of parts) {
            if (!state.parts.has(p.partNumber) || p.etag !== `etag-${p.partNumber}`) throw new Error("bad part");
            merged.push(...state.parts.get(p.partNumber));
          }
          state.completed = true;
          store.set(key, new Uint8Array(merged));
        },
        async abort() {
          state.aborted = true;
        },
      };
    },
  };
  return bucket;
}

function makeEnv() {
  return { UPLOAD_TOKEN: TOKEN, GALLERY_BUCKET: fakeBucket(), ASSETS: { fetch: async () => new Response("asset") } };
}

function call(env, method, path, { token = TOKEN, json, bytes } = {}) {
  const headers = {};
  if (token !== null) headers.authorization = `Bearer ${token}`;
  let body;
  if (json !== undefined) {
    headers["content-type"] = "application/json";
    body = JSON.stringify(json);
  } else if (bytes !== undefined) {
    body = bytes;
  }
  return worker.fetch(new Request(`https://gallery.example${path}`, { method, headers, body }), env, {});
}

async function start(env, id = "cloudrag_1") {
  const res = await call(env, "POST", "/api/videos/uploads", { json: { id } });
  assert.equal(res.status, 200);
  return res.json();
}

const ENTRY = { id: "cloudrag_1", title: "動画 1" };
const META = { id: "cloudrag_1", rag_sources: [] };

test("分けて送る: 開始 → パート → 完了で、動画・サムネイル・metadata・一覧がそろう", async () => {
  const env = makeEnv();
  const { uploadId, partSize } = await start(env);
  assert.equal(partSize, 32 * 1024 * 1024);
  const r1 = await call(env, "PUT", `/api/videos/uploads/${uploadId}/parts/1?id=cloudrag_1`, { bytes: new Uint8Array([1, 2, 3]) });
  const r2 = await call(env, "PUT", `/api/videos/uploads/${uploadId}/parts/2?id=cloudrag_1`, { bytes: new Uint8Array([4, 5]) });
  assert.equal(r1.status, 200);
  const p1 = await r1.json();
  const p2 = await r2.json();
  assert.deepEqual([p1.partNumber, p2.partNumber], [1, 2]);

  const done = await call(env, "POST", `/api/videos/uploads/${uploadId}/complete`, {
    json: {
      id: "cloudrag_1",
      parts: [{ partNumber: 2, etag: p2.etag }, { partNumber: 1, etag: p1.etag }], // 順不同でも、番号順に確定する
      entry: ENTRY,
      metadata: META,
      thumbnail_base64: Buffer.from("PNGDATA").toString("base64"),
    },
  });
  assert.equal(done.status, 200);
  assert.deepEqual([...env.GALLERY_BUCKET.store.get("videos/cloudrag_1/video.webm")], [1, 2, 3, 4, 5]);
  assert.equal(Buffer.from(env.GALLERY_BUCKET.store.get("videos/cloudrag_1/thumb.png")).toString(), "PNGDATA");
  assert.equal(JSON.parse(env.GALLERY_BUCKET.store.get("videos/cloudrag_1/metadata.json")).id, "cloudrag_1");
  assert.deepEqual(JSON.parse(env.GALLERY_BUCKET.store.get("manifest.json")).map((v) => v.id), ["cloudrag_1"]);
});

test("分けて送る: 同じ id をもう一度送ると、一覧は置き換わる(重複しない)", async () => {
  const env = makeEnv();
  for (let i = 0; i < 2; i++) {
    const { uploadId } = await start(env);
    const part = await (await call(env, "PUT", `/api/videos/uploads/${uploadId}/parts/1?id=cloudrag_1`, { bytes: new Uint8Array([i]) })).json();
    const res = await call(env, "POST", `/api/videos/uploads/${uploadId}/complete`, {
      json: { id: "cloudrag_1", parts: [part], entry: { ...ENTRY, title: `v${i}` }, metadata: META },
    });
    assert.equal(res.status, 200);
  }
  const manifest = JSON.parse(env.GALLERY_BUCKET.store.get("manifest.json"));
  assert.equal(manifest.length, 1);
  assert.equal(manifest[0].title, "v1");
});

test("分けて送る: 鍵が無い・違う・サーバーに鍵が無いときは、どの経路も 401 で、何も作られない", async () => {
  const env = makeEnv();
  for (const [method, path, extra] of [
    ["POST", "/api/videos/uploads", { json: { id: "a" } }],
    ["PUT", "/api/videos/uploads/up1/parts/1?id=a", { bytes: new Uint8Array([1]) }],
    ["POST", "/api/videos/uploads/up1/complete", { json: { id: "a" } }],
    ["DELETE", "/api/videos/uploads/up1?id=a", {}],
  ]) {
    for (const token of [null, "wrong"]) {
      const res = await call(env, method, path, { token, ...extra });
      assert.equal(res.status, 401, `${method} ${path}`);
    }
  }
  const noToken = { ...makeEnv(), UPLOAD_TOKEN: undefined };
  assert.equal((await call(noToken, "POST", "/api/videos/uploads", { json: { id: "a" } })).status, 401);
  assert.equal(env.GALLERY_BUCKET.uploads.size, 0);
});

test("分けて送る: 不正な id・パート番号・本文は 400", async () => {
  const env = makeEnv();
  for (const id of ["", "a/b", "a\\b", "..", "x".repeat(201), undefined]) {
    assert.equal((await call(env, "POST", "/api/videos/uploads", { json: { id } })).status, 400, String(id));
  }
  assert.equal((await call(env, "POST", "/api/videos/uploads", { json: undefined })).status, 400);
  const { uploadId } = await start(env);
  for (const n of ["0", "-1", "1.5", "abc", "201"]) {
    const res = await call(env, "PUT", `/api/videos/uploads/${uploadId}/parts/${n}?id=cloudrag_1`, { bytes: new Uint8Array([1]) });
    assert.equal(res.status, 400, n);
  }
  assert.equal((await call(env, "PUT", `/api/videos/uploads/${uploadId}/parts/1?id=bad/id`, { bytes: new Uint8Array([1]) })).status, 400);
  assert.equal((await call(env, "PUT", `/api/videos/uploads/${uploadId}/parts/1?id=cloudrag_1`, { bytes: new Uint8Array([]) })).status, 400);
});

test("分けて送る: 完了の本文が不正なら、動画を確定せず、一覧にも載せない", async () => {
  const env = makeEnv();
  const { uploadId } = await start(env);
  const part = await (await call(env, "PUT", `/api/videos/uploads/${uploadId}/parts/1?id=cloudrag_1`, { bytes: new Uint8Array([1]) })).json();
  const bad = [
    { id: "cloudrag_1", parts: [], entry: ENTRY, metadata: META },
    { id: "cloudrag_1", parts: [{ partNumber: 0, etag: "e" }], entry: ENTRY, metadata: META },
    { id: "cloudrag_1", parts: [part], entry: { id: "other" }, metadata: META },
    { id: "cloudrag_1", parts: [part], entry: ENTRY },
    { id: "cloudrag_1", parts: [part], entry: ENTRY, metadata: META, thumbnail_base64: "***not base64***" },
    { id: "cloudrag_1", parts: [part], entry: ENTRY, metadata: META, thumbnail_base64: Buffer.alloc(2 * 1024 * 1024 + 1).toString("base64") },
  ];
  for (const json of bad) {
    const res = await call(env, "POST", `/api/videos/uploads/${uploadId}/complete`, { json });
    assert.ok([400, 413].includes(res.status), JSON.stringify(res.status));
  }
  assert.equal(env.GALLERY_BUCKET.uploads.get(uploadId).completed, false);
  assert.equal(env.GALLERY_BUCKET.store.has("manifest.json"), false);
});

test("分けて送る: 大きすぎるパートは 413(Worker のメモリに収める)", async () => {
  const env = makeEnv();
  const { uploadId } = await start(env);
  const res = await call(env, "PUT", `/api/videos/uploads/${uploadId}/parts/1?id=cloudrag_1`, { bytes: new Uint8Array(50 * 1024 * 1024 + 1) });
  assert.equal(res.status, 413);
  assert.equal(env.GALLERY_BUCKET.uploads.get(uploadId).parts.size, 0);
});

test("分けて送る: 途中でやめる(DELETE)と、R2 の途中の分を捨てる。動画の id と取り違えない", async () => {
  const env = makeEnv();
  env.GALLERY_BUCKET.store.set("manifest.json", JSON.stringify([{ id: "uploads" }]));
  env.GALLERY_BUCKET.store.set("videos/uploads/video.webm", "keep");
  const { uploadId } = await start(env);
  const res = await call(env, "DELETE", `/api/videos/uploads/${uploadId}?id=cloudrag_1`);
  assert.equal(res.status, 200);
  assert.equal(env.GALLERY_BUCKET.uploads.get(uploadId).aborted, true);
  // 動画の id が "uploads" の動画は、消えていない(/api/videos/uploads/... は、1 本の削除の経路ではない)
  assert.equal(env.GALLERY_BUCKET.store.get("videos/uploads/video.webm"), "keep");
  assert.deepEqual(JSON.parse(env.GALLERY_BUCKET.store.get("manifest.json")).map((v) => v.id), ["uploads"]);
});

test("分けて送る: 知らないメソッドは、動画の削除にも回さず、静的ファイル側(ASSETS)に渡るだけ", async () => {
  const env = makeEnv();
  env.GALLERY_BUCKET.store.set("manifest.json", JSON.stringify([{ id: "uploads" }]));
  const res = await call(env, "GET", "/api/videos/uploads/up1/complete");
  assert.equal(await res.text(), "asset");
  assert.deepEqual(JSON.parse(env.GALLERY_BUCKET.store.get("manifest.json")).map((v) => v.id), ["uploads"]);
});

test("1 回の POST は、90MB までを受ける。それより大きい動画は 413 で、分けて送るように伝える", async () => {
  const env = makeEnv();
  const form = new FormData();
  form.set("id", "big");
  form.set("entry", JSON.stringify({ id: "big" }));
  form.set("metadata", JSON.stringify({}));
  form.set("video", new Blob([new Uint8Array(90 * 1024 * 1024 + 1)], { type: "video/webm" }), "video.webm");
  const res = await worker.fetch(
    new Request("https://gallery.example/api/videos", { method: "POST", headers: { authorization: `Bearer ${TOKEN}` }, body: form }), env, {});
  assert.equal(res.status, 413);
  assert.match((await res.json()).error, /uploads/);
  assert.equal(env.GALLERY_BUCKET.store.has("videos/big/video.webm"), false);
  assert.equal(env.GALLERY_BUCKET.store.has("manifest.json"), false);
});

test("小さい動画の、これまでの 1 回の POST は、そのまま動く", async () => {
  const env = makeEnv();
  const form = new FormData();
  form.set("id", "small");
  form.set("entry", JSON.stringify({ id: "small" }));
  form.set("metadata", JSON.stringify({}));
  form.set("video", new Blob([new Uint8Array([1, 2, 3])], { type: "video/webm" }), "video.webm");
  const res = await worker.fetch(
    new Request("https://gallery.example/api/videos", { method: "POST", headers: { authorization: `Bearer ${TOKEN}` }, body: form }), env, {});
  assert.equal(res.status, 200);
  assert.deepEqual(JSON.parse(env.GALLERY_BUCKET.store.get("manifest.json")).map((v) => v.id), ["small"]);
});
