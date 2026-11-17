// web/public/app.js の、まとめて削除と出典表示の部分の単体テスト(ブラウザ無しで、vm に読み込む)。
//   cd cloudflare/ragreel-gallery && npm test
import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import test from "node:test";
import vm from "node:vm";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
const appSource = fs.readFileSync(path.join(here, "..", "..", "..", "web", "public", "app.js"), "utf8");

// app.js は読み込み時に location / document を触るだけ。画面の部品は、テストごとに差し替える。
function loadApp({ storage = {}, fetchImpl, modals = [] } = {}) {
  const store = new Map(Object.entries(storage));
  const toasts = [];
  const ctx = {
    location: { protocol: "https:", hostname: "gallery.example", search: "" },
    document: {},
    window: {},
    console,
    localStorage: {
      getItem: (k) => (store.has(k) ? store.get(k) : null),
      setItem: (k, v) => store.set(k, String(v)),
      removeItem: (k) => store.delete(k),
    },
    fetch: fetchImpl,
  };
  vm.createContext(ctx);
  vm.runInContext(appSource, ctx);
  // モーダル・トーストは、あらかじめ決めた答えを返す偽物にする
  ctx.openModal = async (opts) => {
    ctx.__modalCalls.push(opts);
    return modals.shift() ?? { confirmed: false, value: "" };
  };
  ctx.__modalCalls = [];
  ctx.showToast = (message, o) => toasts.push({ message, kind: o && o.kind });
  return { ctx, store, toasts };
}

const manifestOf = (n) => Array.from({ length: n }, (_, i) => ({ id: `v${i}`, title: `動画 ${i}` }));

test("planBulkDelete: 重複・一覧に無い id を除いて数え、100 本ごとに分ける", () => {
  const { ctx } = loadApp();
  const all = manifestOf(250).map((v) => v.id);
  const plan = ctx.planBulkDelete(["v1", "v1", "ghost", "v2"], all);
  assert.equal(plan.count, 2);
  assert.equal(plan.total, 250);
  assert.equal(plan.isAll, false);
  assert.deepEqual(Array.from(plan.ids), ["v1", "v2"]);

  const everything = ctx.planBulkDelete(all, all);
  assert.equal(everything.isAll, true);
  assert.deepEqual(Array.from(everything.chunks, (c) => c.length), [100, 100, 50]);

  assert.equal(ctx.planBulkDelete([], all).count, 0);
  assert.equal(ctx.planBulkDelete(["v1"], []).isAll, false);
});

test("まとめて削除: 確認でやめたら、通信しない", async () => {
  let called = 0;
  const { ctx } = loadApp({ fetchImpl: async () => { called++; return { ok: true, status: 200 }; }, modals: [{ confirmed: false, value: "" }] });
  assert.equal(await ctx.bulkDeleteVideosRemote(["v1"], manifestOf(5)), null);
  assert.equal(called, 0);
});

test("まとめて削除: 確認の画面に、選んだ本数と全体の本数が出る", async () => {
  const { ctx } = loadApp({ modals: [{ confirmed: false, value: "" }] });
  await ctx.bulkDeleteVideosRemote(["v1", "v2", "v3"], manifestOf(10));
  const dialog = ctx.__modalCalls[0];
  assert.match(dialog.bodyHTML, /全 10 本のうち 3 本/);
  assert.match(dialog.title, /3 本/);
  assert.equal(dialog.danger, true);
});

test("まとめて削除: 全部を消すときは、本数の入力が合わないと消さない", async () => {
  let called = 0;
  const { ctx, toasts } = loadApp({
    storage: { vf_upload_token: "tok" },
    fetchImpl: async () => { called++; return { ok: true, status: 200 }; },
    modals: [{ confirmed: true, value: "3" }, { confirmed: true, value: "5" }],
  });
  const all = manifestOf(5);
  // 全 5 本のうち 5 本を選び、確認の入力が「3」(違う)
  assert.equal(await ctx.bulkDeleteVideosRemote(all.map((v) => v.id), all), null);
  assert.equal(called, 0);
  assert.equal(toasts.length, 1);
  assert.match(ctx.__modalCalls[0].bodyHTML, /すべて（5 本）/);
  assert.ok(ctx.__modalCalls[0].inputLabel); // 本数の入力欄がある
});

test("まとめて削除: 一部だけを選ぶ確認には、本数の入力欄は出ない", async () => {
  const { ctx } = loadApp({ modals: [{ confirmed: false, value: "" }] });
  await ctx.bulkDeleteVideosRemote(["v1"], manifestOf(5));
  assert.equal(ctx.__modalCalls[0].inputLabel, "");
});

test("まとめて削除: DELETE /api/videos に、Bearer と ids を送り、動いたトークンを覚える", async () => {
  const sent = [];
  const { ctx, store } = loadApp({
    fetchImpl: async (url, init) => { sent.push({ url, init }); return { ok: true, status: 200 }; },
    modals: [{ confirmed: true, value: "" }, { confirmed: true, value: "  secret  " }],
  });
  const result = await ctx.bulkDeleteVideosRemote(["v1", "v2"], manifestOf(5));
  assert.deepEqual(Array.from(result.deleted), ["v1", "v2"]);
  assert.equal(result.failed, null);
  assert.equal(sent.length, 1);
  assert.equal(sent[0].url, "api/videos");
  assert.equal(sent[0].init.method, "DELETE");
  assert.equal(sent[0].init.headers.authorization, "Bearer secret");
  assert.deepEqual(JSON.parse(sent[0].init.body), { ids: ["v1", "v2"] });
  assert.equal(store.get("vf_upload_token"), "secret");
});

test("まとめて削除: トークンが無く、入力もしなければ、通信しない", async () => {
  let called = 0;
  const { ctx, toasts } = loadApp({
    fetchImpl: async () => { called++; return { ok: true, status: 200 }; },
    modals: [{ confirmed: true, value: "" }, { confirmed: true, value: "   " }],
  });
  assert.equal(await ctx.bulkDeleteVideosRemote(["v1"], manifestOf(5)), null);
  assert.equal(called, 0);
  assert.equal(toasts.length, 1);
});

test("まとめて削除: 401 ならトークンを忘れ、そこで止める。消えた本数も正しく返す", async () => {
  const statuses = [200, 401, 200];
  const { ctx, store } = loadApp({
    storage: { vf_upload_token: "old" },
    fetchImpl: async () => { const status = statuses.shift(); return { ok: status === 200, status }; },
    modals: [{ confirmed: true, value: "" }],
  });
  const all = manifestOf(250);
  const result = await ctx.bulkDeleteVideosRemote(all.slice(0, 250).map((v) => v.id).slice(0, 220), all);
  assert.equal(result.deleted.length, 100); // 1 塊目だけ消えた
  assert.match(result.failed, /トークン/);
  assert.equal(store.has("vf_upload_token"), false);
});

test("sourcesHTML: 空なら「出典なし」。類似度が無い出典(Houdini)は 0.00 と出さず、活用/検索のみを出す", () => {
  const { ctx } = loadApp();
  assert.match(ctx.sourcesHTML([]), /出典なし/);
  assert.match(ctx.sourcesHTML(undefined), /出典なし/);
  const html = ctx.sourcesHTML([
    { file: "VEX ループ", namespace: "houdini22", cited: true },
    { file: "Shelf tools", namespace: "houdini22", cited: false },
    { file: "Cloud RAG の出典", namespace: "houdini21", similarity: 0.86 },
  ]);
  assert.doesNotMatch(html, /0\.00|NaN/);
  assert.match(html, /活用/);
  assert.match(html, /検索のみ/);
  assert.match(html, /0\.86/);
  assert.match(html, /VEX ループ/);
});

test("sourcesHTML: <script> を含む名前は無害化し、壊れたデータ(文字列・null・数値)でも落ちない", () => {
  const { ctx } = loadApp();
  const html = ctx.sourcesHTML([
    { file: "<script>alert(1)</script>", namespace: '"><img src=x onerror=alert(2)>', cited: true },
    null,
    "文字列",
    42,
  ]);
  assert.doesNotMatch(html, /<script>|<img/);
  assert.match(html, /&lt;script&gt;/);
  assert.match(ctx.sourcesHTML("not an array"), /出典なし/);
  assert.match(ctx.sourcesHTML([null, "x"]), /出典なし/);
});
