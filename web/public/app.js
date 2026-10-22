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
// IS_LOCAL_FILE picks between the two at load time. The UI decisions
// (icons, colour roles, motion limits) are in docs/ui-rules.md.

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

// ---------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------

// Titles are the user's own questions and, on the shared gallery, may come
// from a teammate -- always escape before putting them into innerHTML.
function escapeHTML(value) {
  return String(value == null ? "" : value).replace(/[&<>"']/g, (c) => ({
    "&": "&amp;",
    "<": "&lt;",
    ">": "&gt;",
    '"': "&quot;",
    "'": "&#39;",
  })[c]);
}

function formatDuration(totalSeconds) {
  const m = Math.floor(totalSeconds / 60);
  const s = Math.round(totalSeconds % 60);
  return `${m}:${String(s).padStart(2, "0")}`;
}

// 2026/10/6 -- numeric, no zero padding, same everywhere (docs/ui-rules.md).
function formatDateShort(iso) {
  const d = new Date(iso);
  if (Number.isNaN(d.getTime())) return "";
  return `${d.getFullYear()}/${d.getMonth() + 1}/${d.getDate()}`;
}

function formatTokens(n) {
  return (n || 0).toLocaleString("ja-JP");
}

function formatBytes(bytes) {
  if (!(bytes > 0)) return "";
  const mb = bytes / (1024 * 1024);
  return mb >= 1 ? `約${mb.toFixed(1)} MB` : `約${Math.max(1, Math.round(bytes / 1024))} KB`;
}

function prefersReducedMotion() {
  return window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)").matches;
}

// ---------------------------------------------------------------------------
// Icons: one line-icon set (stroke 2, round caps). Add new ones here only.
// ---------------------------------------------------------------------------

const ICONS = {
  layers: '<path d="M12 3 3 8l9 5 9-5-9-5z"/><path d="m3 13 9 5 9-5"/>',
  shuffle: '<path d="M16 3h5v5"/><path d="M4 20 21 3"/><path d="M21 16v5h-5"/><path d="m15 15 6 6"/><path d="m4 4 5 5"/>',
  sun: '<circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"/>',
  moon: '<path d="M21 12.8A9 9 0 1 1 11.2 3a7 7 0 0 0 9.8 9.8z"/>',
  play: '<path d="M7 4.5v15l12-7.5z"/>',
  "skip-next": '<path d="M5 4.5v15l10-7.5z"/><path d="M19 5v14"/>',
  volume: '<path d="M11 5 6 9H3v6h3l5 4z"/><path d="M15.5 8.5a5 5 0 0 1 0 7M18.5 5.5a9 9 0 0 1 0 13"/>',
  "volume-off": '<path d="M11 5 6 9H3v6h3l5 4z"/><path d="m16 9 5 6M21 9l-5 6"/>',
  "arrow-right": '<path d="M5 12h14M13 6l6 6-6 6"/>',
  "arrow-left": '<path d="M19 12H5M11 6l-6 6 6 6"/>',
  "chevron-down": '<path d="m6 9 6 6 6-6"/>',
  trash: '<path d="M3 6h18M8 6V4h8v2M6 6l1 14h10l1-14M10 11v6M14 11v6"/>',
  copy: '<rect x="9" y="9" width="12" height="12" rx="2"/><path d="M5 15V5a2 2 0 0 1 2-2h8"/>',
  check: '<path d="m5 12.5 4.5 4.5L19 7.5"/>',
  x: '<path d="M6 6l12 12M18 6 6 18"/>',
  alert: '<path d="M12 4 2.5 20h19z"/><path d="M12 10v5M12 18.2v.1"/>',
  sliders: '<path d="M4 6h10M18 6h2M4 12h4M12 12h8M4 18h12M20 18h0"/><circle cx="16" cy="6" r="2"/><circle cx="10" cy="12" r="2"/><circle cx="18" cy="18" r="2"/>',
  keyboard: '<rect x="2" y="6" width="20" height="12" rx="2"/><path d="M6 10h.01M10 10h.01M14 10h.01M18 10h.01M7 14h10"/>',
  search: '<circle cx="11" cy="11" r="7"/><path d="m20 20-3.5-3.5"/>',
  cloud: '<path d="M7 18a4.5 4.5 0 0 1-.6-8.96A6 6 0 0 1 18 9.5 4 4 0 0 1 17 18z"/>',
};

function icon(name, extraClass = "") {
  return `<svg class="icon ${extraClass}" aria-hidden="true" focusable="false"><use href="#i-${name}"></use></svg>`;
}

function injectIconSprite() {
  if (document.getElementById("iconSprite")) return;
  const symbols = Object.keys(ICONS)
    .map((n) => `<symbol id="i-${n}" viewBox="0 0 24 24">${ICONS[n]}</symbol>`)
    .join("");
  document.body.insertAdjacentHTML(
    "afterbegin",
    `<svg id="iconSprite" width="0" height="0" style="position:absolute" aria-hidden="true"><defs>${symbols}</defs></svg>`
  );
}

// ---------------------------------------------------------------------------
// Theme (light/dark). The inline <head> script in each page applies the
// stored choice before first paint; this wires the toggle.
// ---------------------------------------------------------------------------

function currentTheme() {
  const explicit = document.documentElement.dataset.theme;
  if (explicit === "light" || explicit === "dark") return explicit;
  return window.matchMedia && window.matchMedia("(prefers-color-scheme: dark)").matches ? "dark" : "light";
}

function syncThemeButton() {
  const btn = document.getElementById("themeToggle");
  if (!btn) return;
  const dark = currentTheme() === "dark";
  btn.innerHTML = icon(dark ? "sun" : "moon");
  btn.setAttribute("aria-label", dark ? "ライトテーマにする" : "ダークテーマにする");
}

function toggleTheme() {
  const next = currentTheme() === "dark" ? "light" : "dark";
  document.documentElement.dataset.theme = next;
  try {
    localStorage.setItem("vf_theme", next);
  } catch (e) {
    // Private mode / blocked storage: the choice just won't persist.
  }
  syncThemeButton();
}

// Fills <header id="topbar"> (brand link, theme toggle, page-specific
// extras) so every page shares one header instead of five copies.
function mountTopbar(extrasHTML = "") {
  injectIconSprite();
  const host = document.getElementById("topbar");
  if (!host) return;
  host.classList.add("topbar");
  host.innerHTML = `
    <a class="kicker" href="index.html">VIDEO FACTORY</a>
    <span class="topbar-spacer"></span>
    ${extrasHTML}
    ${SHORTCUTS[document.body.dataset.page] ? `<button id="shortcutBtn" class="icon-btn" type="button" aria-label="キー操作の一覧" title="キー操作 (?)">${icon("keyboard")}</button>` : ""}
    <button id="themeToggle" class="icon-btn" type="button"></button>`;
  document.getElementById("themeToggle").addEventListener("click", toggleTheme);
  const shortcutBtn = document.getElementById("shortcutBtn");
  if (shortcutBtn) shortcutBtn.addEventListener("click", () => showShortcutHelp(document.body.dataset.page));
  syncThemeButton();
  consumePendingToast();
}

// ---------------------------------------------------------------------------
// Toast + modal. Both appear instantly (no animation) -- see ui-rules.md.
// ---------------------------------------------------------------------------

function showToast(message, { kind = "info", ms = 3500 } = {}) {
  let host = document.getElementById("toastHost");
  if (!host) {
    host = document.createElement("div");
    host.id = "toastHost";
    host.className = "toast-host";
    host.setAttribute("aria-live", "polite");
    document.body.appendChild(host);
  }
  const el = document.createElement("div");
  el.className = `toast toast-${kind}`;
  el.setAttribute("role", "status");
  const glyph = kind === "success" ? icon("check") : kind === "error" ? icon("alert") : "";
  el.innerHTML = `${glyph}<span>${escapeHTML(message)}</span>`;
  host.appendChild(el);
  setTimeout(() => el.remove(), ms);
}

// A toast for the *next* page load (e.g. "deleted" right before navigating).
function queueToast(message, kind = "success") {
  try {
    sessionStorage.setItem("vf_toast", JSON.stringify({ message, kind }));
  } catch (e) {
    // Storage unavailable: skip the toast, the navigation still happens.
  }
}

function consumePendingToast() {
  try {
    const raw = sessionStorage.getItem("vf_toast");
    if (!raw) return;
    sessionStorage.removeItem("vf_toast");
    const { message, kind } = JSON.parse(raw);
    showToast(message, { kind });
  } catch (e) {
    // Malformed/blocked: ignore.
  }
}

// Resolves { confirmed, value }. Focus starts on the safe action (cancel)
// unless there is an input, Esc / backdrop click cancel.
function openModal({
  title,
  bodyHTML = "",
  confirmLabel = "OK",
  cancelLabel = "キャンセル",
  danger = false,
  inputLabel = "",
  inputType = "text",
}) {
  return new Promise((resolve) => {
    const backdrop = document.createElement("div");
    backdrop.className = "modal-backdrop";
    backdrop.innerHTML = `
      <div class="modal" role="dialog" aria-modal="true" aria-labelledby="modalTitle">
        <h2 id="modalTitle">${escapeHTML(title)}</h2>
        <div class="modal-body">${bodyHTML}</div>
        ${
          inputLabel
            ? `<div class="modal-field"><label for="modalInput">${escapeHTML(inputLabel)}</label>
               <input id="modalInput" type="${inputType}" autocomplete="off" /></div>`
            : ""
        }
        <div class="modal-actions">
          ${cancelLabel ? `<button type="button" class="btn" data-act="cancel">${escapeHTML(cancelLabel)}</button>` : ""}
          <button type="button" class="btn ${danger ? "btn-danger" : "btn-primary"}" data-act="ok">${escapeHTML(confirmLabel)}</button>
        </div>
      </div>`;

    const previouslyFocused = document.activeElement;
    const input = backdrop.querySelector("input");

    function close(result) {
      document.removeEventListener("keydown", onKey);
      backdrop.remove();
      if (previouslyFocused && previouslyFocused.focus) previouslyFocused.focus();
      resolve(result);
    }
    const cancel = () => close({ confirmed: false, value: "" });
    const confirm = () => close({ confirmed: true, value: input ? input.value : "" });
    function onKey(e) {
      if (e.key === "Escape") cancel();
    }

    backdrop.addEventListener("click", (e) => {
      if (e.target === backdrop) cancel();
    });
    const cancelBtn = backdrop.querySelector('[data-act="cancel"]');
    if (cancelBtn) cancelBtn.addEventListener("click", cancel);
    backdrop.querySelector('[data-act="ok"]').addEventListener("click", confirm);
    if (input) {
      input.addEventListener("keydown", (e) => {
        if (e.key === "Enter") confirm();
      });
    }
    document.addEventListener("keydown", onKey);
    document.body.appendChild(backdrop);
    (input || cancelBtn || backdrop.querySelector('[data-act="ok"]')).focus();
  });
}

// Only meaningful against the Cloudflare-hosted gallery (IS_LOCAL_FILE is
// false). This deletes the *shared* copy (video, thumbnail, metadata in R2);
// files under the PC's own output/ folder are untouched -- the dialog says
// so, which is the "two kinds of delete are told apart" requirement.
async function deleteVideoRemote(id, title) {
  let stored = "";
  try {
    stored = localStorage.getItem("vf_upload_token") || "";
  } catch (e) {
    stored = "";
  }

  const result = await openModal({
    title: "共有ギャラリーから削除",
    bodyHTML: `
      <p>「${escapeHTML(title)}」を<strong>共有ギャラリー</strong>から削除します。元に戻せません。</p>
      <p>お使いのPCの <code>output</code> フォルダにある動画ファイルは削除されません。</p>`,
    confirmLabel: "共有ギャラリーから削除",
    danger: true,
    inputLabel: stored ? "" : "管理トークン",
    inputType: "password",
  });
  if (!result.confirmed) return false;

  const token = stored || result.value.trim();
  if (!token) {
    showToast("管理トークンを入力してください", { kind: "error" });
    return false;
  }

  let res;
  try {
    res = await fetch(`api/videos/${encodeURIComponent(id)}`, {
      method: "DELETE",
      headers: { authorization: `Bearer ${token}` },
    });
  } catch (e) {
    showToast("通信に失敗しました: " + e.message, { kind: "error" });
    return false;
  }

  if (res.status === 401) {
    try {
      localStorage.removeItem("vf_upload_token");
    } catch (e) {
      // ignore
    }
    showToast("トークンが正しくありません", { kind: "error" });
    return false;
  }
  if (!res.ok) {
    showToast("削除に失敗しました (" + res.status + ")", { kind: "error" });
    return false;
  }

  // Remember the token only once it has actually worked.
  try {
    localStorage.setItem("vf_upload_token", token);
  } catch (e) {
    // ignore
  }
  queueToast("共有ギャラリーから削除しました", "success");
  return true;
}

// ---------------------------------------------------------------------------
// Count-up (gallery hero). Plays once; instant under reduced motion.
// ---------------------------------------------------------------------------

function countUp(el, target, ms = 700) {
  if (prefersReducedMotion() || target <= 1) {
    el.textContent = String(target);
    return;
  }
  const start = performance.now();
  function frame(now) {
    const t = Math.min(1, (now - start) / ms);
    const eased = 1 - Math.pow(1 - t, 3);
    el.textContent = String(Math.round(target * eased));
    if (t < 1) requestAnimationFrame(frame);
  }
  requestAnimationFrame(frame);
}

// Column count for the gallery grid: 3-4 columns where the width allows,
// chosen so the last row is as full as possible (a remainder of 0 wins).
// Fewer columns only when the container can't hold three. The caller turns
// a remaining lone card into a full-width row (card-wide).
function pickColumns(count, width) {
  const max = width >= 1100 ? 4 : width >= 700 ? 3 : width >= 460 ? 2 : 1;
  const min = Math.min(3, max);
  let best = max;
  let bestFill = -1;
  for (let c = max; c >= min; c--) {
    const remainder = count % c;
    const fill = remainder === 0 ? 1 : remainder / c;
    if (fill > bestFill + 1e-9) {
      best = c;
      bestFill = fill;
    }
  }
  return best;
}

// ---------------------------------------------------------------------------
// Card pieces shared by the grid and the stack browser
// ---------------------------------------------------------------------------

function thumbHTML(v, { duration = true, play = true } = {}) {
  const img = v.thumbnail_path
    ? `<img src="${escapeHTML(v.thumbnail_path)}" alt="" loading="lazy" />`
    : `<div class="thumb-empty"></div>`;
  return `
    ${img}
    ${duration ? `<span class="thumb-duration">${formatDuration(v.duration_sec)}</span>` : ""}
    ${play ? `<span class="thumb-play">${icon("play")}</span>` : ""}`;
}

// Machine tags -> the labels used everywhere (chips, card tags, subtitle).
const TAG_LABELS = { "houdini-tutorial": "Houdiniチュートリアル", "cloud-rag": "クラウドRAG" };

function tagLabel(tag) {
  return TAG_LABELS[tag] || tag;
}

// "cloud-rag:houdini21" -> "クラウドRAG · houdini21"
function sourceLabel(source) {
  const s = String(source || "");
  const i = s.indexOf(":");
  if (i < 0) return tagLabel(s);
  return `${tagLabel(s.slice(0, i))} · ${s.slice(i + 1)}`;
}

function tagsHTML(tags) {
  return (tags || []).map((t) => `<span class="tag">${escapeHTML(tagLabel(t))}</span>`).join("");
}

// ---------------------------------------------------------------------------
// Detail page pieces
// ---------------------------------------------------------------------------

const CHAPTER_KIND_LABEL = { node: "ノードグラフ", viewport: "ビューポート", other: "その他" };

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
      <span class="extraction-badge-label">出典網羅率${quality.extraction_detail ? ` (${escapeHTML(quality.extraction_detail)})` : ""}</span>
    </div>
  `;
}

// One bar, segment width proportional to the chapter's length. Returns ""
// when the video predates chapter metadata (the section is then omitted).
function chapterTimelineHTML(chapters, totalSec) {
  if (!chapters || !chapters.length) return "";
  const segments = chapters
    .map((c, i) => {
      const kind = CHAPTER_KIND_LABEL[c.kind] || CHAPTER_KIND_LABEL.other;
      const tip = `${c.title} · ${kind} · ${formatDuration(c.start_sec)}`;
      return `<button type="button" class="chapter" data-start="${c.start_sec}" data-i="${i}"
        style="flex-grow:${Math.max(c.duration_sec, 0.2)}; --c:var(--${["blue", "mint", "orange", "purple", "pink", "yellow"][i % 6]})"
        data-tip="${escapeHTML(tip)}" aria-label="${escapeHTML(tip)}"></button>`;
    })
    .join("");
  return `
    <div class="chapter-timeline" id="chapterTimeline" data-total="${totalSec}">
      <div class="chapter-bar">${segments}<div class="chapter-playhead" id="chapterPlayhead"></div></div>
      <div class="chapter-legend"><span>0:00</span><span>${chapters.length} 章</span><span>${formatDuration(totalSec)}</span></div>
    </div>`;
}

// Click a chapter -> seek; playhead + current-chapter highlight follow
// playback. State changes only (no motion).
function wireChapterTimeline(video) {
  const root = document.getElementById("chapterTimeline");
  if (!root || !video) return;
  const total = Number(root.dataset.total) || 0;
  const playhead = document.getElementById("chapterPlayhead");
  const segments = [...root.querySelectorAll(".chapter")];

  segments.forEach((seg) => {
    seg.addEventListener("click", () => {
      video.currentTime = Number(seg.dataset.start) || 0;
    });
  });

  function update() {
    const t = video.currentTime || 0;
    if (total > 0) playhead.style.left = `${Math.min(100, (t / total) * 100)}%`;
    let current = 0;
    segments.forEach((seg, i) => {
      if (t >= (Number(seg.dataset.start) || 0)) current = i;
    });
    segments.forEach((seg, i) => seg.classList.toggle("current", i === current && t > 0));
  }
  video.addEventListener("timeupdate", update);
  video.addEventListener("seeked", update);
}

// "1280×720 · 4:22 · 15 fps · 約12.6 MB" from whatever is known.
function exportSummaryText(entry, meta) {
  const r = (meta && meta.render) || {};
  const parts = [];
  if (r.width && r.height) parts.push(`${r.width}×${r.height}`);
  if (entry.duration_sec) parts.push(formatDuration(entry.duration_sec));
  if (r.fps) parts.push(`${r.fps} fps`);
  const size = formatBytes(r.file_size_bytes);
  if (size) parts.push(size);
  return parts.join(" · ");
}

function sourcesHTML(sources) {
  if (!sources || !sources.length) {
    return `<p style="color:var(--text-3);margin:0">出典なし</p>`;
  }
  return sources
    .map((s, i) => `
      <div class="source-row">
        <span class="source-mark">${i + 1}</span>
        <span class="source-name">${escapeHTML(s.file)}</span>
        <span class="source-score">${Number(s.similarity).toFixed(2)}</span>
      </div>`)
    .join("");
}

// Generation process: one accordion row per pipeline stage (state shown by
// colour + glyph, details closed by default).
function stepsHTML(pipeline) {
  if (!pipeline || !pipeline.length) {
    return `<p style="color:var(--text-3);margin:0">記録がありません</p>`;
  }
  const done = pipeline.filter((p) => p.status === "done").length;
  const totalSec = pipeline.reduce((sum, p) => sum + (p.duration_sec || 0), 0);
  const rows = pipeline
    .map((p) => {
      const failed = p.status !== "done";
      const dur = (p.duration_sec || 0).toFixed(1);
      return `
        <li class="step ${failed ? "failed" : ""}">
          <button type="button" class="accordion-head" aria-expanded="false">
            <span class="step-dot">${icon(failed ? "x" : "check")}</span>
            <span class="step-label">${escapeHTML(p.label)}</span>
            <span class="step-dur">${dur} s</span>
            ${icon("chevron-down", "chev")}
          </button>
          <div class="accordion-body"><div>
            <div class="step-detail">
              状態: ${failed ? "失敗" : "完了"}<br />
              所要時間: ${(p.duration_sec || 0).toFixed(2)} 秒
              ${p.error ? `<br /><span class="error">${escapeHTML(p.error)}</span>` : ""}
            </div>
          </div></div>
        </li>`;
    })
    .join("");
  return `
    <p class="steps-summary">完了 ${done} / ${pipeline.length} ステージ · 合計 ${totalSec.toFixed(1)} 秒</p>
    <ol class="steps">${rows}</ol>`;
}

// Any .accordion-head inside `root` toggles aria-expanded (CSS opens the
// following .accordion-body).
function wireAccordions(root) {
  root.querySelectorAll(".accordion-head").forEach((head) => {
    head.addEventListener("click", () => {
      const open = head.getAttribute("aria-expanded") === "true";
      head.setAttribute("aria-expanded", String(!open));
    });
  });
}

// manifest.json's estimated_tokens is a rough character-count-based guess
// (main_cloudrag.cpp's estimateTokens()) -- the Cloud RAG backend never
// returns real token usage in its query response, so this is never a
// measured figure. Every place this is rendered must say so.
//
// `single: true` renders one video's own figure (video.html) instead of the
// gallery's cumulative-across-all-videos ranking.
function tokenConsumptionHTML(manifest, { single = false } = {}) {
  const entries = manifest.filter((v) => v.estimated_tokens > 0);
  if (!entries.length) {
    return "";
  }
  const total = entries.reduce((sum, v) => sum + v.estimated_tokens, 0);
  const note = `<p class="token-note">RAGへのクエリ・応答の文字数から概算した推定値です（実測のAPIトークン数ではありません）。</p>`;

  if (single) {
    return `
      ${note}
      <div class="token-total">
        <span class="token-total-value">${formatTokens(total)}</span>
        <span class="token-total-label">この動画の推定トークン消費量</span>
      </div>`;
  }

  const max = Math.max(...entries.map((v) => v.estimated_tokens));
  const ranked = [...entries].sort((a, b) => b.estimated_tokens - a.estimated_tokens).slice(0, 8);
  const rows = ranked
    .map((v) => {
      const pct = Math.max(4, Math.round((v.estimated_tokens / max) * 100));
      return `
        <div class="token-row">
          <span class="token-row-title">${escapeHTML(v.title)}</span>
          <div class="token-row-track"><div class="token-row-fill" style="width:${pct}%"></div></div>
          <span class="token-row-value">${formatTokens(v.estimated_tokens)}</span>
        </div>`;
    })
    .join("");

  return `
    ${note}
    <div class="token-total">
      <span class="token-total-value">${formatTokens(total)}</span>
      <span class="token-total-label">累計推定トークン消費量 / ${entries.length}本の動画</span>
    </div>
    <div class="token-rows">${rows}</div>`;
}

// ---------------------------------------------------------------------------
// Keyboard (IMPROVEMENT_DESIGN_2026-10.md T6). The list is also shown on
// screen: the keyboard button in the topbar, or "?".
// ---------------------------------------------------------------------------

const SHORTCUTS = {
  gallery: [
    ["/", "検索に移動"],
    ["← →", "前後のカードへ"],
    ["C", "方式の比較へ"],
    ["?", "このキー操作の一覧"],
  ],
  video: [
    ["Space", "再生 / 一時停止"],
    ["← →", "前後の章へ(章の情報が無い動画は5秒)"],
    ["/", "ギャラリーの検索へ"],
    ["C", "方式の比較へ"],
    ["?", "このキー操作の一覧"],
  ],
  compare: [
    ["Space", "A・Bをまとめて再生 / 一時停止"],
    ["← →", "5秒戻る / 進む"],
    ["/", "ギャラリーの検索へ"],
    ["?", "このキー操作の一覧"],
  ],
};

function isTypingTarget(el) {
  if (!el || !el.tagName) return false;
  const tag = el.tagName;
  return tag === "INPUT" || tag === "TEXTAREA" || tag === "SELECT" || el.isContentEditable === true;
}

// Elements that already act on Space / arrows themselves; letting both run
// would toggle twice (a focused <video> toggles play on Space natively).
function handlesOwnKeys(el, key) {
  if (!el || !el.tagName) return false;
  const isTab = el.getAttribute("role") === "tab";
  // Space activates buttons/summary and toggles a focused <video>; arrows
  // are only native on <video> (seek) and tabs. Links use neither.
  if (key === "Space") return ["BUTTON", "VIDEO", "SUMMARY"].includes(el.tagName) || isTab;
  return el.tagName === "VIDEO" || isTab;
}

// handlers: { "/": fn, "c": fn, "Space": fn, "ArrowLeft": fn, "ArrowRight": fn }.
// A handler returning false leaves the browser default alone. Never fires
// from a text field, with a modifier held, or while a dialog is open.
function initShortcuts(handlers) {
  document.addEventListener("keydown", (e) => {
    if (e.ctrlKey || e.metaKey || e.altKey) return;
    if (isTypingTarget(e.target)) return;
    if (document.querySelector(".modal-backdrop")) return;

    if (e.key === "?") {
      e.preventDefault();
      showShortcutHelp(document.body.dataset.page);
      return;
    }
    const key = e.key === " " ? "Space" : e.key.length === 1 ? e.key.toLowerCase() : e.key;
    const fn = handlers[key];
    if (!fn) return;
    if ((key === "Space" || key.startsWith("Arrow")) && handlesOwnKeys(e.target, key)) return;
    if (fn(e) !== false) e.preventDefault();
  });
}

function showShortcutHelp(page) {
  const rows = (SHORTCUTS[page] || [])
    .map(([key, what]) => `<tr><th><kbd>${escapeHTML(key)}</kbd></th><td>${escapeHTML(what)}</td></tr>`)
    .join("");
  return openModal({
    title: "キー操作",
    bodyHTML: `<table class="keys">${rows}</table><p class="keys-note">入力欄にいるときは効きません。</p>`,
    confirmLabel: "閉じる",
    cancelLabel: "",
  });
}

// ---------------------------------------------------------------------------
// Card previews (IMPROVEMENT_DESIGN_2026-10.md T3 + T4): the list shows
// thumbnails; a <video> is created only when a preview is wanted, so the
// initial page load never fetches video bytes. This is the *preview*
// playback mode/speed -- unrelated to the narration speaking speed
// (RAGREEL_VOICEVOX_SPEED).
//   hover : the card under the mouse / with keyboard focus
//   all   : cards in view, at most PREVIEW_MAX_ALL at a time
//   paused: thumbnails only
// ---------------------------------------------------------------------------

const PREVIEW_MODES = [["hover", "ホバーのみ"], ["all", "すべて再生"], ["paused", "停止"]];
const PREVIEW_SPEEDS = [0.5, 1, 1.5, 2];
const PREVIEW_MAX_ALL = 4;

function loadPreviewPrefs() {
  let mode = null;
  let speed = null;
  try {
    mode = localStorage.getItem("vf_preview_mode");
    speed = Number(localStorage.getItem("vf_preview_speed"));
  } catch (e) {
    // Storage blocked: fall back to the defaults below.
  }
  const saveData = navigator.connection && navigator.connection.saveData;
  const calm = prefersReducedMotion() || saveData;
  return {
    mode: PREVIEW_MODES.some(([m]) => m === mode) ? mode : calm ? "paused" : "hover",
    speed: PREVIEW_SPEEDS.includes(speed) ? speed : 1,
  };
}

function savePreviewPrefs(prefs) {
  try {
    localStorage.setItem("vf_preview_mode", prefs.mode);
    localStorage.setItem("vf_preview_speed", String(prefs.speed));
  } catch (e) {
    // The choice just won't persist.
  }
}

function createPreviewManager(grid, initial) {
  let mode = initial.mode;
  let speed = initial.speed;
  const active = new Map(); // card -> <video>
  let observer = null;

  function start(card) {
    if (active.has(card) || !card.dataset.video) return;
    const thumb = card.querySelector(".thumb");
    if (!thumb) return;
    const v = document.createElement("video");
    v.className = "thumb-preview";
    v.muted = true;
    v.loop = true;
    v.playsInline = true;
    v.preload = "none";
    v.setAttribute("aria-hidden", "true");
    v.defaultPlaybackRate = speed;
    v.addEventListener("playing", () => v.classList.add("ready"), { once: true });
    v.src = card.dataset.video;
    v.playbackRate = speed;
    thumb.appendChild(v);
    active.set(card, v);
    const p = v.play();
    if (p && p.catch) p.catch(() => {});
  }

  function stop(card) {
    const v = active.get(card);
    if (!v) return;
    v.pause();
    // Drop the source so an abandoned preview stops downloading.
    v.removeAttribute("src");
    v.load();
    v.remove();
    active.delete(card);
  }

  function stopAll() {
    [...active.keys()].forEach(stop);
  }

  function cardOf(node) {
    return node && node.closest ? node.closest(".card") : null;
  }

  const onOver = (e) => {
    if (mode !== "hover" || e.pointerType === "touch") return;
    const card = cardOf(e.target);
    if (card && card !== cardOf(e.relatedTarget)) start(card);
  };
  const onOut = (e) => {
    if (mode !== "hover") return;
    const card = cardOf(e.target);
    if (card && card !== cardOf(e.relatedTarget)) stop(card);
  };
  const onFocusIn = (e) => {
    if (mode === "hover") {
      const card = cardOf(e.target);
      if (card) start(card);
    }
  };
  const onFocusOut = (e) => {
    if (mode === "hover") {
      const card = cardOf(e.target);
      if (card && card !== cardOf(e.relatedTarget)) stop(card);
    }
  };
  grid.addEventListener("pointerover", onOver);
  grid.addEventListener("pointerout", onOut);
  grid.addEventListener("focusin", onFocusIn);
  grid.addEventListener("focusout", onFocusOut);

  function observeAll() {
    if (!("IntersectionObserver" in window)) return;
    observer = new IntersectionObserver(
      (entries) => {
        for (const entry of entries) {
          const card = entry.target;
          if (entry.isIntersecting) {
            if (active.size < PREVIEW_MAX_ALL) start(card);
          } else {
            stop(card);
          }
        }
      },
      { threshold: 0.6 }
    );
    grid.querySelectorAll(".card").forEach((c) => observer.observe(c));
  }

  // Call after the grid's cards were re-rendered, or after a mode change.
  function refresh() {
    stopAll();
    if (observer) {
      observer.disconnect();
      observer = null;
    }
    if (mode === "all") observeAll();
  }

  return {
    setMode(m) {
      mode = m;
      refresh();
    },
    setSpeed(s) {
      speed = s;
      active.forEach((v) => {
        v.defaultPlaybackRate = s;
        v.playbackRate = s;
      });
    },
    refresh,
    activeCount: () => active.size,
  };
}

// ---------------------------------------------------------------------------
// Per-chapter fixed fields (IMPROVEMENT_DESIGN_2026-10.md T2): 見るポイント
// (the slide's two key bullets) and 元の出典 (sources cited on a 参考
// slide). A "使うときの注意" field isn't shown: nothing in the pipeline
// produces that text, and an empty fixed field would only be noise.
// ---------------------------------------------------------------------------

function chapterListHTML(chapters) {
  if (!chapters || !chapters.length) return "";
  const rows = chapters
    .map((c, i) => {
      const kind = CHAPTER_KIND_LABEL[c.kind] || CHAPTER_KIND_LABEL.other;
      const points = (c.points || []).map((p) => `<li>${escapeHTML(p)}</li>`).join("");
      const sources = (c.sources || []).map((s) => `<li>${escapeHTML(s)}</li>`).join("");
      return `
        <li class="chapter-row">
          <button type="button" class="accordion-head" aria-expanded="false">
            <span class="chapter-time">${formatDuration(c.start_sec)}</span>
            <span class="chapter-name">${escapeHTML(c.title)}</span>
            <span class="chapter-kind">${escapeHTML(kind)}</span>
            ${icon("chevron-down", "chev")}
          </button>
          <div class="accordion-body"><div>
            <div class="chapter-detail">
              <button type="button" class="btn chapter-seek" data-start="${c.start_sec}">${icon("play")}<span>この章から再生</span></button>
              ${points ? `<h4>見るポイント</h4><ul>${points}</ul>` : ""}
              ${sources ? `<h4>元の出典</h4><ul>${sources}</ul>` : ""}
            </div>
          </div></div>
        </li>`;
    })
    .join("");
  return `<ol class="chapter-list">${rows}</ol>`;
}

// ---------------------------------------------------------------------------
// Stack browser: a swipeable card stack, the gallery grid's mobile
// alternative, built from the same manifest array.
// ---------------------------------------------------------------------------

function initStackBrowser(root, manifest) {
  const order = manifest.map((_, i) => i);
  const SWIPE_THRESHOLD = 60;
  const VISIBLE = 4;

  function render() {
    root.innerHTML = order
      .slice(0, VISIBLE)
      .map((idx, i) => {
        const v = manifest[idx];
        return `
          <a class="stack-card" style="--i:${i}" href="video.html?id=${encodeURIComponent(v.id)}" data-i="${i}" draggable="false">
            <div class="thumb">${thumbHTML(v, { duration: true, play: false })}</div>
            <div class="stack-card-body">
              <p class="card-title">${escapeHTML(v.title)}</p>
              <p class="card-sub">${formatDateShort(v.created_at)}</p>
              <div class="tags">${tagsHTML(v.tags)}</div>
            </div>
          </a>`;
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
      try {
        top.setPointerCapture(e.pointerId);
      } catch (err) {
        // Capture is only a nicety (keeps the drag alive outside the card);
        // an already-gone pointer must not break the gesture.
      }
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
        // Defer the DOM rebuild past this event cycle so the synthetic
        // `click` that follows `pointerup` still finds this card attached
        // and can be cancelled below; rebuilding synchronously would detach
        // it first and let the link navigate.
        setTimeout(advance, 0);
      }
    });
    // pointercancel (the browser took over for a vertical scroll) must never
    // advance the stack -- just reset the drag visuals.
    top.addEventListener("pointercancel", () => {
      dragging = false;
      top.classList.remove("dragging");
      top.style.transform = "";
    });
    top.addEventListener("click", (e) => {
      if (swiped) e.preventDefault();
    });
  }

  render();
}
