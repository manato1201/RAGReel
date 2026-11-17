// ギャラリー Worker(src/index.js)の単体テスト。R2 と ASSETS は、メモリ上の偽物で置き換える。
//   cd cloudflare/ragreel-gallery && npm test      (node --test test/)
import assert from "node:assert/strict";
import test from "node:test";

import worker from "../src/index.js";

const TOKEN = "test-upload-token";

// R2 の偽物。get / put / delete(単独のキーも、配列も)だけ。
function fakeBucket(initial = {}) {
  const store = new Map(Object.entries(initial));
  return {
    store,
    async get(key) {
      if (!store.has(key)) return null;
      const value = store.get(key);
      return {
        body: value,
        async json() {
          return JSON.parse(value);
        },
      };
    },
    async put(key, value) {
      store.set(key, typeof value === "string" ? value : String(value));
    },
    async delete(keys) {
      for (const k of Array.isArray(keys) ? keys : [keys]) store.delete(k);
    },
  };
}

function makeEnv(videoIds, extra = {}) {
  const files = {};
  const manifest = videoIds.map((id) => ({ id, title: `動画 ${id}` }));
  for (const id of videoIds) {
    files[`videos/${id}/video.webm`] = "v";
    files[`videos/${id}/thumb.png`] = "t";
    files[`videos/${id}/metadata.json`] = "{}";
  }
  files["manifest.json"] = JSON.stringify(manifest);
  return {
    UPLOAD_TOKEN: TOKEN,
    GALLERY_BUCKET: fakeBucket(files),
    ASSETS: { fetch: async () => new Response("asset") },
    ...extra,
  };
}

function call(env, method, path, { token = TOKEN, body } = {}) {
  const headers = {};
  if (token !== null) headers.authorization = `Bearer ${token}`;
  if (body !== undefined) headers["content-type"] = "application/json";
  return worker.fetch(
    new Request(`https://gallery.example${path}`, {
      method,
      headers,
      body: body === undefined ? undefined : typeof body === "string" ? body : JSON.stringify(body),
    }),
    env,
    {}
  );
}

const manifestIds = (env) => JSON.parse(env.GALLERY_BUCKET.store.get("manifest.json")).map((v) => v.id);
const keysOf = (env, id) => [...env.GALLERY_BUCKET.store.keys()].filter((k) => k.startsWith(`videos/${id}/`));

test("まとめて削除: 指定した動画のファイルと一覧の項目だけが消える", async () => {
  const env = makeEnv(["a", "b", "c", "d"]);
  const res = await call(env, "DELETE", "/api/videos", { body: { ids: ["a", "c"] } });
  assert.equal(res.status, 200);
  const json = await res.json();
  assert.equal(json.ok, true);
  assert.deepEqual(json.deleted, ["a", "c"]);
  assert.equal(json.removedFromManifest, 2);
  assert.deepEqual(manifestIds(env), ["b", "d"]);
  assert.deepEqual(keysOf(env, "a"), []);
  assert.deepEqual(keysOf(env, "c"), []);
  assert.equal(keysOf(env, "b").length, 3);
  assert.equal(keysOf(env, "d").length, 3);
});

test("まとめて削除: 鍵が無い・違うときは 401 で、何も消えない", async () => {
  for (const token of [null, "wrong-token", ""]) {
    const env = makeEnv(["a", "b"]);
    const res = await call(env, "DELETE", "/api/videos", { token, body: { ids: ["a"] } });
    assert.equal(res.status, 401, `token=${JSON.stringify(token)}`);
    assert.deepEqual(manifestIds(env), ["a", "b"]);
    assert.equal(keysOf(env, "a").length, 3);
  }
});

test("まとめて削除: サーバーに UPLOAD_TOKEN が設定されていないときは、誰でも 401(鍵なしで消せない)", async () => {
  const env = makeEnv(["a"], { UPLOAD_TOKEN: undefined });
  const res = await call(env, "DELETE", "/api/videos", { token: "undefined", body: { ids: ["a"] } });
  assert.equal(res.status, 401);
  assert.deepEqual(manifestIds(env), ["a"]);
});

test("まとめて削除: 1 本でも id の形が不正なら、全部を拒否して何も消さない", async () => {
  const env = makeEnv(["a", "b"]);
  for (const ids of [["a", "../b"], ["a", "x/y"], ["a", ""], ["a", 5], ["a", null]]) {
    const res = await call(env, "DELETE", "/api/videos", { body: { ids } });
    assert.equal(res.status, 400, JSON.stringify(ids));
  }
  assert.deepEqual(manifestIds(env), ["a", "b"]);
  assert.equal(keysOf(env, "a").length, 3);
});

test("まとめて削除: ids が空・配列でない・JSON でないときは 400", async () => {
  const env = makeEnv(["a"]);
  assert.equal((await call(env, "DELETE", "/api/videos", { body: { ids: [] } })).status, 400);
  assert.equal((await call(env, "DELETE", "/api/videos", { body: { ids: "a" } })).status, 400);
  assert.equal((await call(env, "DELETE", "/api/videos", { body: {} })).status, 400);
  assert.equal((await call(env, "DELETE", "/api/videos", { body: "not json" })).status, 400);
  assert.deepEqual(manifestIds(env), ["a"]);
});

test("まとめて削除: 1 回の上限(100 本)を超えると 400。ちょうど 100 本は通る", async () => {
  const ids = Array.from({ length: 101 }, (_, i) => `v${i}`);
  const env = makeEnv(ids);
  assert.equal((await call(env, "DELETE", "/api/videos", { body: { ids } })).status, 400);
  assert.equal(manifestIds(env).length, 101);

  const ok = await call(env, "DELETE", "/api/videos", { body: { ids: ids.slice(0, 100) } });
  assert.equal(ok.status, 200);
  assert.deepEqual(manifestIds(env), ["v100"]);
});

test("まとめて削除: 重複した id は 1 本として数え、一覧に無い id があっても失敗しない", async () => {
  const env = makeEnv(["a", "b"]);
  const res = await call(env, "DELETE", "/api/videos", { body: { ids: ["a", "a", "ghost"] } });
  assert.equal(res.status, 200);
  const json = await res.json();
  assert.deepEqual(json.deleted, ["a", "ghost"]);
  assert.equal(json.removedFromManifest, 1);
  assert.deepEqual(manifestIds(env), ["b"]);
});

test("1 本の削除(DELETE /api/videos/:id)は、これまでどおり動く。鍵が無ければ 401", async () => {
  const env = makeEnv(["a", "b"]);
  assert.equal((await call(env, "DELETE", "/api/videos/a", { token: null })).status, 401);
  assert.deepEqual(manifestIds(env), ["a", "b"]);
  assert.equal((await call(env, "DELETE", "/api/videos/a")).status, 200);
  assert.deepEqual(manifestIds(env), ["b"]);
  assert.equal((await call(env, "DELETE", "/api/videos/..%2Fb")).status, 400);
});

test("一覧(GET /manifest.json)は、鍵なしで読める。読み取りの経路では何も消えない", async () => {
  const env = makeEnv(["a"]);
  const res = await call(env, "GET", "/manifest.json", { token: null });
  assert.equal(res.status, 200);
  assert.equal((await res.json()).length, 1);
  // GET /api/videos は静的ファイル側へ落ちる(削除の経路ではない)
  const other = await call(env, "GET", "/api/videos", { token: null });
  assert.equal(await other.text(), "asset");
  assert.deepEqual(manifestIds(env), ["a"]);
});

// R2 の delete だけが失敗する偽物(ネットワーク障害など)。
function failingDeleteEnv(ids) {
  const env = makeEnv(ids);
  env.GALLERY_BUCKET.delete = async () => {
    throw new Error("R2 delete failed");
  };
  return env;
}

test("まとめて削除: ファイルの削除が失敗しても、一覧に残ったまま壊れた項目にならない(一覧を先に更新する)", async () => {
  const env = failingDeleteEnv(["a", "b"]);
  const res = await call(env, "DELETE", "/api/videos", { body: { ids: ["a"] } });
  assert.equal(res.status, 500);
  // 一覧からは外れている。残るのは、一覧に出ない余分なファイルだけ(やり直せば消える)
  assert.deepEqual(manifestIds(env), ["b"]);
});

test("1 本の削除: ファイルの削除が失敗しても、一覧に壊れた項目を残さない", async () => {
  const env = failingDeleteEnv(["a", "b"]);
  const res = await call(env, "DELETE", "/api/videos/a");
  assert.equal(res.status, 500);
  assert.deepEqual(manifestIds(env), ["b"]);
});

test("まとめて削除: 一覧の更新が失敗したら、ファイルは 1 つも消えない", async () => {
  const env = makeEnv(["a", "b"]);
  const realPut = env.GALLERY_BUCKET.put;
  env.GALLERY_BUCKET.put = async (key, ...rest) => {
    if (key === "manifest.json") throw new Error("R2 put failed");
    return realPut(key, ...rest);
  };
  const res = await call(env, "DELETE", "/api/videos", { body: { ids: ["a"] } });
  assert.equal(res.status, 500);
  assert.equal(keysOf(env, "a").length, 3);
});

test("まとめて削除: 長すぎる id・バックスラッシュ入りの id・膨大な本数は 400", async () => {
  const env = makeEnv(["a"]);
  const huge = Array.from({ length: 1001 }, () => "a");
  for (const ids of [["a".repeat(201)], ["a\\b"], huge]) {
    const res = await call(env, "DELETE", "/api/videos", { body: { ids } });
    assert.equal(res.status, 400);
  }
  assert.deepEqual(manifestIds(env), ["a"]);
});
