// Shared helpers for the static dashboard. Both pages only ever read
// manifest.json / videos/<id>/metadata.json (design doc §5) -- there is no
// backend here, and nothing here should grow one.

async function fetchJSON(path) {
  const res = await fetch(path);
  if (!res.ok) {
    throw new Error(`Failed to load ${path}: ${res.status}`);
  }
  return res.json();
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
