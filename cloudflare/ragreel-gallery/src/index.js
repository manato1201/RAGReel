// RAGReel shared gallery -- serves web/public (index.html/gallery.html/
// video.html/random.html/styles.css/app.js/hero-disc.js) as static assets,
// and layers a small API on top backed by R2 for the parts that are
// actually dynamic (manifest + per-video metadata + the video/thumbnail
// files themselves). See docs/architecture/video-factory-design.md and
// engine/src/manifest/manifest_writer.cpp for the desktop app's local
// equivalent of this same data shape -- this Worker is the optional cloud
// mirror of it, not a replacement.
//
// Routes (see wrangler.jsonc run_worker_first for why these reach this
// fetch handler instead of being served as static assets):
//   POST   /api/videos       - upload one video (auth required)
//   DELETE /api/videos/:id   - remove one video (auth required)
//   DELETE /api/videos       - remove several at once: body {"ids": ["a", "b"]} (auth required)
//   GET    /manifest.json    - live manifest, read by app.js's loadManifest()
//   GET    /videos/:id/metadata.json  - one video's detail, for video.html
//   GET    /videos/:id/video.webm     - the video file
//   GET    /videos/:id/thumb.png      - the thumbnail
// Everything else falls through to ASSETS (the static dashboard shell).

const MANIFEST_KEY = "manifest.json";
// 1 回で消せる本数の上限。R2 の delete は 1 回 1000 キーまでで、1 本につき 3 キー(video/thumb/metadata)を消すため、
// 余裕を見て 100 本にする。これより多いときは、画面側が分けて送る。
const MAX_BULK_DELETE = 100;

// R2 のキーは 1024 バイトまで。id は "cloudrag_<時刻>" のような短い名前なので、長すぎるものは弾く
// (長い id を通すと、R2 の delete が例外になり、削除が繰り返し失敗する)。
const MAX_ID_LENGTH = 200;

function isValidId(id) {
  return (
    typeof id === "string" &&
    id.length > 0 &&
    id.length <= MAX_ID_LENGTH &&
    !id.includes("/") &&
    !id.includes("\\") &&
    !id.includes("..")
  );
}

// 文字列の長さや一致位置が応答時間に出ないよう、両方を SHA-256 にしてから全バイトを比較する
// (WeatherGeoBridge/worker/src/auth.ts の safeEqual と同じ)。通常の === はタイミング差で推測されうる。
async function safeEqual(a, b) {
  const enc = new TextEncoder();
  const [da, db] = await Promise.all([
    crypto.subtle.digest("SHA-256", enc.encode(a)),
    crypto.subtle.digest("SHA-256", enc.encode(b)),
  ]);
  const va = new Uint8Array(da);
  const vb = new Uint8Array(db);
  let diff = 0;
  for (let i = 0; i < va.length; i++) diff |= va[i] ^ vb[i];
  return diff === 0;
}

async function isAuthorized(request, env) {
  if (!env.UPLOAD_TOKEN) return false;
  const auth = request.headers.get("authorization") || "";
  return safeEqual(auth, `Bearer ${env.UPLOAD_TOKEN}`);
}

function unauthorized() {
  return Response.json({ error: "unauthorized" }, { status: 401 });
}

function badRequest(message) {
  return Response.json({ error: message }, { status: 400 });
}

async function getManifest(env) {
  const obj = await env.GALLERY_BUCKET.get(MANIFEST_KEY);
  if (!obj) return [];
  return obj.json();
}

async function putManifest(env, manifest) {
  await env.GALLERY_BUCKET.put(MANIFEST_KEY, JSON.stringify(manifest), {
    httpMetadata: { contentType: "application/json" },
  });
}

async function handleUpload(request, env) {
  if (!(await isAuthorized(request, env))) return unauthorized();

  const form = await request.formData();
  const id = form.get("id");
  if (!isValidId(id)) return badRequest("invalid id");

  const entryRaw = form.get("entry");
  const metadataRaw = form.get("metadata");
  if (typeof entryRaw !== "string" || typeof metadataRaw !== "string") {
    return badRequest("missing entry/metadata");
  }

  let entry, metadata;
  try {
    entry = JSON.parse(entryRaw);
    metadata = JSON.parse(metadataRaw);
  } catch (err) {
    return badRequest("entry/metadata is not valid JSON: " + err.message);
  }
  if (entry.id !== id) return badRequest("entry.id does not match id");

  const video = form.get("video");
  if (video && typeof video !== "string") {
    await env.GALLERY_BUCKET.put(`videos/${id}/video.webm`, await video.arrayBuffer(), {
      httpMetadata: { contentType: "video/webm" },
    });
  }

  const thumbnail = form.get("thumbnail");
  if (thumbnail && typeof thumbnail !== "string") {
    await env.GALLERY_BUCKET.put(`videos/${id}/thumb.png`, await thumbnail.arrayBuffer(), {
      httpMetadata: { contentType: "image/png" },
    });
  }

  await env.GALLERY_BUCKET.put(`videos/${id}/metadata.json`, JSON.stringify(metadata), {
    httpMetadata: { contentType: "application/json" },
  });

  // Read-modify-write, no compare-and-swap: same "single-process,
  // sequential publish" assumption docs/technical-reference.md already
  // documents as an accepted, pre-existing limitation of the local
  // manifest.json (§ "manifest.json / metadata.jsonの信頼性"), not a new
  // risk introduced here.
  const manifest = await getManifest(env);
  const updated = [entry, ...manifest.filter((v) => v.id !== id)];
  await putManifest(env, updated);

  return Response.json({ ok: true, id });
}

function objectKeysFor(id) {
  return [`videos/${id}/video.webm`, `videos/${id}/thumb.png`, `videos/${id}/metadata.json`];
}

async function handleDelete(id, request, env) {
  if (!(await isAuthorized(request, env))) return unauthorized();
  if (!isValidId(id)) return badRequest("invalid id");

  // 先に一覧(manifest.json)から外し、そのあとファイルを消す。途中で失敗しても、
  // 「一覧にあるのに、ファイルが無い(開くと壊れる)」状態は残らない(残るのは、一覧に出ない余分なファイルだけで、やり直せば消える)。
  const manifest = await getManifest(env);
  const updated = manifest.filter((v) => v.id !== id);
  await putManifest(env, updated);
  await env.GALLERY_BUCKET.delete(objectKeysFor(id));

  return Response.json({ ok: true });
}

// 複数の動画をまとめて消す。認証は 1 本ずつの削除と同じ(Authorization: Bearer <UPLOAD_TOKEN>)。
// 本文: {"ids": ["id1", "id2", ...]}。1 本でも id の形が不正なら、何も消さずに 400 を返す(途中まで消えた状態を作らない)。
// 重複した id は 1 本として数える。一覧(manifest.json)の書き換えは、まとめて 1 回だけ。
async function handleBulkDelete(request, env) {
  if (!(await isAuthorized(request, env))) return unauthorized();

  let body;
  try {
    body = await request.json();
  } catch (err) {
    return badRequest("body must be JSON: {\"ids\": [...]}");
  }
  const rawIds = body && body.ids;
  if (!Array.isArray(rawIds) || rawIds.length === 0) return badRequest("ids must be a non-empty array");
  // 巨大な配列を 1 本ずつ調べないよう、重複を含む生の本数にも上限を置く
  if (rawIds.length > MAX_BULK_DELETE * 10) return badRequest("too many ids");
  if (!rawIds.every(isValidId)) return badRequest("invalid id in ids");
  const ids = [...new Set(rawIds)];
  if (ids.length > MAX_BULK_DELETE) {
    return badRequest(`too many ids (max ${MAX_BULK_DELETE} per request)`);
  }

  // 先に一覧から外し、そのあとファイルを消す(1 本ずつの削除と同じ順。理由は handleDelete を参照)。
  const manifest = await getManifest(env);
  const idSet = new Set(ids);
  const updated = manifest.filter((v) => !idSet.has(v.id));
  await putManifest(env, updated);
  await env.GALLERY_BUCKET.delete(ids.flatMap(objectKeysFor));

  return Response.json({ ok: true, deleted: ids, removedFromManifest: manifest.length - updated.length });
}

async function serveManifest(env) {
  const obj = await env.GALLERY_BUCKET.get(MANIFEST_KEY);
  const body = obj ? obj.body : JSON.stringify([]);
  return new Response(body, {
    headers: { "content-type": "application/json", "cache-control": "no-store" },
  });
}

// Honours HTTP Range so <video> can seek (the chapter timeline jumps to a
// chapter's start) without downloading the whole file first. Without
// Accept-Ranges/206 a browser can only seek within what it has already
// buffered.
async function serveVideoObject(request, env, key, contentType) {
  const rangeHeader = request.headers.get("range");
  const obj = await env.GALLERY_BUCKET.get(key, rangeHeader ? { range: request.headers } : undefined);
  if (!obj) return new Response("Not found", { status: 404 });

  const headers = new Headers();
  obj.writeHttpMetadata(headers);
  if (!headers.get("content-type")) headers.set("content-type", contentType);
  headers.set("etag", obj.httpEtag);
  headers.set("cache-control", "public, max-age=31536000, immutable");
  headers.set("accept-ranges", "bytes");

  if (rangeHeader && obj.range) {
    const r = obj.range;
    const offset = r.offset !== undefined ? r.offset : Math.max(0, obj.size - r.suffix);
    const length = r.length !== undefined ? r.length : obj.size - offset;
    headers.set("content-range", `bytes ${offset}-${offset + length - 1}/${obj.size}`);
    headers.set("content-length", String(length));
    return new Response(obj.body, { status: 206, headers });
  }
  return new Response(obj.body, { headers });
}

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);
    const { pathname } = url;

    try {
      if (request.method === "POST" && pathname === "/api/videos") {
        return await handleUpload(request, env);
      }

      if (request.method === "DELETE" && pathname === "/api/videos") {
        return await handleBulkDelete(request, env);
      }

      if (request.method === "DELETE" && pathname.startsWith("/api/videos/")) {
        return await handleDelete(pathname.slice("/api/videos/".length), request, env);
      }

      if (request.method === "GET" && pathname === "/manifest.json") {
        return await serveManifest(env);
      }

      const metaMatch = pathname.match(/^\/videos\/([^/]+)\/metadata\.json$/);
      if (request.method === "GET" && metaMatch) {
        const obj = await env.GALLERY_BUCKET.get(`videos/${metaMatch[1]}/metadata.json`);
        if (!obj) return new Response("Not found", { status: 404 });
        return new Response(obj.body, { headers: { "content-type": "application/json" } });
      }

      const videoMatch = pathname.match(/^\/videos\/([^/]+)\/video\.webm$/);
      if (request.method === "GET" && videoMatch) {
        return await serveVideoObject(request, env, `videos/${videoMatch[1]}/video.webm`, "video/webm");
      }

      const thumbMatch = pathname.match(/^\/videos\/([^/]+)\/thumb\.png$/);
      if (request.method === "GET" && thumbMatch) {
        return await serveVideoObject(request, env, `videos/${thumbMatch[1]}/thumb.png`, "image/png");
      }
    } catch (err) {
      return Response.json({ error: err && err.message ? err.message : String(err) }, { status: 500 });
    }

    return env.ASSETS.fetch(request);
  },
};
