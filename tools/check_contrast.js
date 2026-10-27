#!/usr/bin/env node
// Checks text/background contrast of the dashboard's colour tokens in both
// themes (docs/ui-rules.md: secondary grey text must keep >= 4.5:1).
//   node tools/check_contrast.js            # report, exit 1 on any failure
//
// Tokens are read from web/public/styles.css: the light values from :root,
// the dark values from :root[data-theme="dark"] layered over them.

const fs = require("fs");
const path = require("path");

const css = fs.readFileSync(path.join(__dirname, "..", "web", "public", "styles.css"), "utf8");

function tokensOf(block) {
  const out = {};
  for (const m of block.matchAll(/(--[\w-]+):\s*([^;]+);/g)) out[m[1]] = m[2].trim();
  return out;
}

function blockAfter(selector) {
  const i = css.indexOf(selector);
  if (i < 0) throw new Error(`selector not found: ${selector}`);
  const start = css.indexOf("{", i);
  const end = css.indexOf("}", start);
  return css.slice(start + 1, end);
}

const light = tokensOf(blockAfter(":root {"));
const darkOverrides = tokensOf(blockAfter(':root[data-theme="dark"]'));
const dark = { ...light, ...darkOverrides };

// The dark values exist twice: an explicit choice ([data-theme="dark"]) and
// the OS preference (@media prefers-color-scheme). They must not drift.
const darkMedia = tokensOf(blockAfter(':root:not([data-theme="light"])'));
let drift = 0;
for (const key of new Set([...Object.keys(darkOverrides), ...Object.keys(darkMedia)])) {
  if (darkOverrides[key] !== darkMedia[key]) {
    drift++;
    console.log(`DRIFT ${key}: data-theme="dark" = ${darkOverrides[key]}, prefers-color-scheme = ${darkMedia[key]}`);
  }
}
if (drift) process.exit(1);

function parseColor(value, tokens) {
  const v = value.trim();
  const ref = v.match(/^var\((--[\w-]+)\)$/);
  if (ref) return parseColor(tokens[ref[1]], tokens);
  const hex = v.match(/^#([0-9a-f]{6})$/i);
  if (hex) {
    const n = parseInt(hex[1], 16);
    return { r: (n >> 16) & 255, g: (n >> 8) & 255, b: n & 255, a: 1 };
  }
  const rgba = v.match(/^rgba?\(([^)]+)\)$/);
  if (rgba) {
    const p = rgba[1].split(",").map((s) => parseFloat(s));
    return { r: p[0], g: p[1], b: p[2], a: p.length > 3 ? p[3] : 1 };
  }
  throw new Error(`cannot parse colour: ${value}`);
}

// Composite a (possibly translucent) colour over an opaque one.
function over(fg, bg) {
  return {
    r: fg.r * fg.a + bg.r * (1 - fg.a),
    g: fg.g * fg.a + bg.g * (1 - fg.a),
    b: fg.b * fg.a + bg.b * (1 - fg.a),
    a: 1,
  };
}

function luminance({ r, g, b }) {
  const f = (c) => {
    const s = c / 255;
    return s <= 0.03928 ? s / 12.92 : Math.pow((s + 0.055) / 1.055, 2.4);
  };
  return 0.2126 * f(r) + 0.7152 * f(g) + 0.0722 * f(b);
}

function ratio(a, b) {
  const [hi, lo] = [luminance(a), luminance(b)].sort((x, y) => y - x);
  return (hi + 0.05) / (lo + 0.05);
}

// [label, foreground token, background token or tokens to stack, minimum]
const PAIRS = [
  ["body text on page", "--text-1", ["--bg"], 4.5],
  ["heading on page", "--text-0", ["--bg"], 4.5],
  ["secondary text on page", "--text-2", ["--bg"], 4.5],
  ["subtitle grey on page", "--text-3", ["--bg"], 4.5],
  ["subtitle grey on card", "--text-3", ["--panel"], 4.5],
  ["secondary text on card", "--text-2", ["--panel"], 4.5],
  ["blue tag", "--blue-ink", ["--panel", "--blue-soft"], 4.5],
  ["mint state", "--mint-ink", ["--panel", "--mint-soft"], 4.5],
  ["pink state", "--pink-ink", ["--panel", "--pink-soft"], 4.5],
  ["purple RAG chip", "--purple-ink", ["--panel", "--purple-soft"], 4.5],
  ["orange kicker on page", "--orange-ink", ["--bg"], 3.0], // 13px bold letter-spaced label
  // Sushi belt (empty gallery): decorative ASCII, 12px bold monospace.
  ["belt yellow on page", "--yellow-ink", ["--bg"], 3.0],
  ["belt mint on page", "--mint-ink", ["--bg"], 3.0],
  ["belt pink on page", "--pink-ink", ["--bg"], 3.0],
];

let failures = 0;
for (const [themeName, tokens] of [["light", light], ["dark", dark]]) {
  console.log(`\n[${themeName}]`);
  for (const [label, fgToken, bgTokens, min] of PAIRS) {
    let bg = parseColor(tokens[bgTokens[0]], tokens);
    for (const t of bgTokens.slice(1)) bg = over(parseColor(tokens[t], tokens), bg);
    const fg = over(parseColor(tokens[fgToken], tokens), bg);
    const r = ratio(fg, bg);
    const ok = r >= min;
    if (!ok) failures++;
    console.log(`  ${ok ? "ok  " : "FAIL"} ${r.toFixed(2).padStart(5)}:1  (min ${min})  ${label}`);
  }
}

if (failures) {
  console.log(`\n${failures} pair(s) below the minimum.`);
  process.exit(1);
}
console.log("\nAll pairs pass.");
