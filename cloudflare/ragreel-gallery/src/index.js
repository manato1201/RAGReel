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
//   GET    /manifest.json    - live manifest, read by app.js's loadManifest()
//   GET    /videos/:id/metadata.json  - one video's detail, for video.html
//   GET    /videos/:id/video.webm     - the video file
//   GET    /videos/:id/thumb.png      - the thumbnail
// Everything else falls through to ASSETS (the static dashboard shell).

const MANIFEST_KEY = "manifest.json";

function isValidId(id) {
  return typeof id === "string" && id.length > 0 && !id.includes("/") && !id.includes("..");
}

function isAuthorized(request, env) {
  if (!env.UPLOAD_TOKEN) return false;
  const auth = request.headers.get("authorization") || "";
  return auth === `Bearer ${env.UPLOAD_TOKEN}`;
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
  if (!isAuthorized(request, env)) return unauthorized();

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

async function handleDelete(id, request, env) {
  if (!isAuthorized(request, env)) return unauthorized();
  if (!isValidId(id)) return badRequest("invalid id");

  await env.GALLERY_BUCKET.delete([
    `videos/${id}/video.webm`,
    `videos/${id}/thumb.png`,
    `videos/${id}/metadata.json`,
  ]);

  const manifest = await getManifest(env);
  const updated = manifest.filter((v) => v.id !== id);
  await putManifest(env, updated);

  return Response.json({ ok: true });
}

async function serveManifest(env) {
  const obj = await env.GALLERY_BUCKET.get(MANIFEST_KEY);
  const body = obj ? obj.body : JSON.stringify([]);
  return new Response(body, {
    headers: { "content-type": "application/json", "cache-control": "no-store" },
  });
}

async function serveVideoObject(env, key, contentType) {
  const obj = await env.GALLERY_BUCKET.get(key);
  if (!obj) return new Response("Not found", { status: 404 });
  const headers = new Headers();
  obj.writeHttpMetadata(headers);
  if (!headers.get("content-type")) headers.set("content-type", contentType);
  headers.set("etag", obj.httpEtag);
  headers.set("cache-control", "public, max-age=31536000, immutable");
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
        return await serveVideoObject(env, `videos/${videoMatch[1]}/video.webm`, "video/webm");
      }

      const thumbMatch = pathname.match(/^\/videos\/([^/]+)\/thumb\.png$/);
      if (request.method === "GET" && thumbMatch) {
        return await serveVideoObject(env, `videos/${thumbMatch[1]}/thumb.png`, "image/png");
      }
    } catch (err) {
      return Response.json({ error: err && err.message ? err.message : String(err) }, { status: 500 });
    }

    return env.ASSETS.fetch(request);
  },
};
