// Shared helpers for the static dashboard. This page ships two ways:
//
// 1. Locally, opened via file:// (VideoHistory::openDashboard() in the
//    desktop app). Chromium-based browsers refuse fetch()/XHR of local
//    files from a file:// document, so ManifestWriter (engine/src/manifest/
//    manifest_writer.cpp) writes manifest.js / metadata.js alongside the
//    .json files -- plain `window.__VF_MANIFEST__ = [...]` scripts, loaded
//    here via a classic <script> tag, which isn't subject to that
//    restriction.
// 2. On the optional shared gallery (cloudflare/ragreel-gallery), served
//    over https by a Cloudflare Worker backed by R2 -- there fetch() works
//    normally, and manifest.json / videos/<id>/metadata.json are live
//    routes the Worker serves from R2, not static files.
//
// IS_LOCAL_FILE picks between the two at load time; nothing else in this
// file (or gallery.html/video.html/random.html) needs to know which mode
// it's in.

const IS_LOCAL_FILE = location.protocol === "file:";

function loadScript(src) {
  return new Promise((resolve, reject) => {
    const el = document.createElement("script");
    el.src = src;
    el.onload = () => resolve();
    el.onerror = () => reject(new Error(`Failed to load ${src}`));
    document.head.appendChild(el);
  });
}

async function loadManifest() {
  if (!IS_LOCAL_FILE) {
    const res = await fetch("manifest.json", { cache: "no-store" });
    if (!res.ok) throw new Error(`Failed to load manifest.json: ${res.status}`);
    return res.json();
  }
  await loadScript("manifest.js");
  if (!window.__VF_MANIFEST__) {
    throw new Error("manifest.js did not set window.__VF_MANIFEST__");
  }
  return window.__VF_MANIFEST__;
}

async function loadVideoMetadata(id) {
  if (!IS_LOCAL_FILE) {
    const res = await fetch(`videos/${id}/metadata.json`, { cache: "no-store" });
    if (!res.ok) throw new Error(`Failed to load metadata.json: ${res.status}`);
    return res.json();
  }
  await loadScript(`videos/${id}/metadata.js`);
  if (!window.__VF_METADATA__) {
    throw new Error("metadata.js did not set window.__VF_METADATA__");
  }
  return window.__VF_METADATA__;
}

// Only meaningful against the Cloudflare-hosted gallery (IS_LOCAL_FILE ===
// false) -- video.html shows a delete button there. Prompts for the shared
// upload token on first use and remembers it in localStorage.
async function deleteVideoRemote(id) {
  let token = localStorage.getItem("vf_upload_token");
  if (!token) {
    token = prompt("削除には管理トークンが必要です。トークンを入力してください:");
    if (!token) return false;
    localStorage.setItem("vf_upload_token", token);
  }

  const res = await fetch(`api/videos/${encodeURIComponent(id)}`, {
    method: "DELETE",
    headers: { authorization: `Bearer ${token}` },
  });

  if (res.status === 401) {
    localStorage.removeItem("vf_upload_token");
    alert("トークンが正しくありません。");
    return false;
  }
  if (!res.ok) {
    alert("削除に失敗しました: " + res.status);
    return false;
  }
  return true;
}

function formatDuration(totalSeconds) {
  const m = Math.floor(totalSeconds / 60);
  const s = Math.round(totalSeconds % 60);
  return `${m}:${String(s).padStart(2, "0")}`;
}

function formatDate(iso) {
  const d = new Date(iso);
  return d.toLocaleDateString("ja-JP", { year: "numeric", month: "short", day: "numeric" });
}

function formatTokens(n) {
  return (n || 0).toLocaleString("ja-JP");
}

// quality.extraction_rate/extraction_detail come from gas_cloud_rag.js's
// citation-accuracy pipeline (how much of the answer it could ground in
// cited sources) -- 0/empty for --mock or Houdini-tutorial ingestion mode,
// neither of which calls the live query endpoint.
function extractionBadgeHTML(quality) {
  if (!quality || !quality.extraction_rate) {
    return "";
  }
  return `
    <div class="extraction-badge">
      <span class="extraction-badge-value">${Math.round(quality.extraction_rate)}%</span>
      <span class="extraction-badge-label">出典網羅率${quality.extraction_detail ? ` (${quality.extraction_detail})` : ""}</span>
    </div>
  `;
}

// manifest.json's estimated_tokens is a rough character-count-based guess
// (main_cloudrag.cpp's estimateTokens()) -- the Cloud RAG backend never
// returns real token usage in its query response, so this is never a
// measured figure. Every place this is rendered must say so.
//
// `single: true` renders one video's own figure (video.html's detail
// panel) instead of the gallery's cumulative-across-all-videos ranking --
// a bar-ranked breakdown makes no sense against just one entry.
function tokenConsumptionHTML(manifest, { single = false } = {}) {
  const entries = manifest.filter((v) => v.estimated_tokens > 0);
  if (!entries.length) {
    return "";
  }
  const total = entries.reduce((sum, v) => sum + v.estimated_tokens, 0);

  if (single) {
    return `
      <div class="panel token-panel">
        <h3>トークン消費量（推定）</h3>
        <p class="token-note">
          RAGへのクエリ・応答の文字数から概算した推定値です（実測のAPIトークン数ではありません）。
        </p>
        <div class="token-total">
          <span class="token-total-value">${formatTokens(total)}</span>
          <span class="token-total-label">この動画の推定トークン消費量</span>
        </div>
      </div>
    `;
  }

  const max = Math.max(...entries.map((v) => v.estimated_tokens));
  const ranked = [...entries].sort((a, b) => b.estimated_tokens - a.estimated_tokens).slice(0, 8);

  const rows = ranked.map((v, i) => {
    const pct = Math.max(4, Math.round((v.estimated_tokens / max) * 100));
    const barClass = ["bar-orange", "bar-mint", "bar-blue", "bar-pink", "bar-yellow", "bar-purple"][i % 6];
    return `
      <div class="token-row">
        <span class="token-row-title">${v.title}</span>
        <div class="token-row-track">
          <div class="token-row-fill ${barClass}" style="width:${pct}%"></div>
        </div>
        <span class="token-row-value">${formatTokens(v.estimated_tokens)}</span>
      </div>
    `;
  }).join("");

  return `
    <div class="panel token-panel">
      <h3>トークン消費量（推定）</h3>
      <p class="token-note">
        RAGへのクエリ・応答の文字数から概算した推定値です（実測のAPIトークン数ではありません）。
      </p>
      <div class="token-total">
        <span class="token-total-value">${formatTokens(total)}</span>
        <span class="token-total-label">累計推定トークン消費量 / ${entries.length}本の動画</span>
      </div>
      <div class="token-rows">${rows}</div>
    </div>
  `;
}

// --- Phase 6 (IMPROVEMENT_PLAN.md): dashboard UI/animation components ---
// All three read only existing manifest.json / metadata.json fields
// (pipeline[].duration_sec/status, the manifest array itself) -- no new
// data model, no new endpoint.

// Split-flap: each character of `text` gets its own flap-in animation,
// staggered by index. video.html's data is a static post-hoc snapshot (a
// video only appears in the manifest once its whole pipeline already
// finished -- pipeline[].status is only ever "done"/"failed", there is no
// live "in_progress" state to watch), so this plays once on reveal
// instead of on a status transition that doesn't exist in this dashboard.
function splitFlapHTML(text) {
  return [...String(text)]
    .map((ch, i) => `<span class="digit"><span style="animation-delay:${i * 45}ms">${ch}</span></span>`)
    .join("");
}

// Radial progress: a circular gauge for how many of a video's pipeline
// stages completed. The plan's original spec gates this on "in-progress
// jobs" polling manifest.json for a live update -- this dashboard has no
// such state (publish() only ever writes a stage list where every entry
// is already terminal), so this renders the video's own completed/total
// ratio instead, as a second view onto the same pipeline-row it sits
// beside (which stays, unchanged, as the plan requires).
function radialProgressHTML(pipeline) {
  const total = 7; // JobStage has 7 phases (design doc §2); encode isn't
  // tracked as a separate pipeline entry (folded into render's timing,
  // a known gap -- IMPROVEMENT_PLAN.md Final Phase), so a fully-successful
  // video still reads 6/7 here, not 7/7.
  const done = (pipeline || []).filter((p) => p.status === "done").length;
  const r = 45;
  const circumference = 2 * Math.PI * r;
  const offset = circumference - (circumference * done) / total;
  return `
    <div class="radial-progress-wrap">
      <div class="radial-progress">
        <svg width="76" height="76" viewBox="0 0 100 100" aria-hidden="true">
          <circle class="radial-progress-track" cx="50" cy="50" r="${r}" />
          <circle class="radial-progress-fill" cx="50" cy="50" r="${r}"
            stroke-dasharray="${circumference.toFixed(2)}"
            stroke-dashoffset="${offset.toFixed(2)}" />
        </svg>
      </div>
      <div class="radial-progress-label">
        完了ステージ
        <strong>${done} / ${total}</strong>
      </div>
    </div>
  `;
}

// Morph Menu: expands a pipeline card in place to show its full detail
// (duration_sec/status/error) using the FLIP technique -- measure the
// rect before toggling `.expanded`, measure again after, animate only the
// transform delta between them (cheap: no layout-triggering properties are
// animated). Call once after the pipeline cards are in the DOM.
function wireMorphMenu(container) {
  container.querySelectorAll(".pipeline-card").forEach((card) => {
    card.addEventListener("click", () => {
      const wasExpanded = card.classList.contains("expanded");
      container.querySelectorAll(".pipeline-card.expanded").forEach((c) => {
        if (c !== card) c.classList.remove("expanded");
      });

      const first = card.getBoundingClientRect();
      card.classList.toggle("expanded", !wasExpanded);
      const last = card.getBoundingClientRect();

      const dx = first.left - last.left;
      const dy = first.top - last.top;
      const sx = first.width / last.width;
      const sy = first.height / last.height;
      card.animate(
        [
          { transform: `translate(${dx}px, ${dy}px) scale(${sx}, ${sy})` },
          { transform: "none" },
        ],
        { duration: 250, easing: "ease-out" }
      );
    });
  });
}

// Stack Browser: a swipeable card-stack alternative to the gallery grid,
// built from the same `manifest` array loadManifest() already returns (no
// separate endpoint -- IMPROVEMENT_PLAN.md's "GET /api/videos" doesn't
// exist in this codebase; loadManifest() is the one real data source both
// local and cloud modes share).
function initStackBrowser(root, manifest) {
  let order = manifest.map((_, i) => i);
  const SWIPE_THRESHOLD = 60;
  const VISIBLE = 4;

  function render() {
    root.innerHTML = order
      .slice(0, VISIBLE)
      .map((idx, i) => {
        const v = manifest[idx];
        return `
          <a class="stack-card" style="--i:${i}" href="video.html?id=${encodeURIComponent(v.id)}" data-i="${i}">
            <img src="${v.thumbnail_path}" alt="" loading="lazy" />
            <div class="stack-card-body">
              <p class="card-title">${v.title}</p>
              <div class="tags">${v.tags.map((t) => `<span class="tag">${t}</span>`).join("")}</div>
            </div>
          </a>
        `;
      })
      .join("");
    wireTopCard();
  }

  function advance() {
    order.push(order.shift());
    render();
  }

  function wireTopCard() {
    const top = root.querySelector('.stack-card[data-i="0"]');
    if (!top) return;
    let startX = 0;
    let dx = 0;
    let dragging = false;
    let swiped = false;

    top.addEventListener("pointerdown", (e) => {
      dragging = true;
      dx = 0;
      startX = e.clientX;
      top.classList.add("dragging");
      top.setPointerCapture(e.pointerId);
    });
    top.addEventListener("pointermove", (e) => {
      if (!dragging) return;
      dx = e.clientX - startX;
      top.style.transform = `translateX(${dx}px) rotate(${dx / 20}deg)`;
    });
    top.addEventListener("pointerup", () => {
      if (!dragging) return;
      dragging = false;
      top.classList.remove("dragging");
      top.style.transform = "";
      if (Math.abs(dx) > SWIPE_THRESHOLD) {
        swiped = true;
        // Defer the DOM rebuild past this event cycle (a plain macrotask,
        // not next frame) so the browser's synthetic `click` -- which
        // always follows `pointerup` -- still finds this same, still-
        // attached card to run the preventDefault below on. Rebuilding
        // synchronously here would detach the card first and leave click
        // navigation unguarded.
        setTimeout(advance, 0);
      }
    });
    // pointercancel (e.g. the browser took over for a vertical scroll)
    // must never advance the stack, unlike pointerup -- just reset the
    // visual drag state.
    top.addEventListener("pointercancel", () => {
      dragging = false;
      top.classList.remove("dragging");
      top.style.transform = "";
    });
    // A swipe that crossed the threshold suppresses the link's own
    // navigation (advance() already handles moving to the next card);
    // anything under the threshold is a tap and follows the href normally.
    top.addEventListener("click", (e) => {
      if (swiped) e.preventDefault();
    });
  }

  render();
}
