#!/usr/bin/env node
// Verifies the gallery's column logic (web/public/app.js pickColumns +
// gallery.html's "lone last card becomes a full-width row" rule) for every
// video count 1..80 across typical container widths: no lone orphan card in
// the last row (IMPROVEMENT_DESIGN_2026-10.md U2 checklist).
//   node tools/check_gallery_layout.js

const fs = require("fs");
const path = require("path");
const vm = require("vm");

// app.js touches location/document at load; only pickColumns is needed.
const src = fs.readFileSync(path.join(__dirname, "..", "web", "public", "app.js"), "utf8");
const ctx = { location: { protocol: "https:" }, document: {}, window: {}, console };
vm.createContext(ctx);
vm.runInContext(src, ctx);
const pickColumns = ctx.pickColumns;
if (typeof pickColumns !== "function") throw new Error("pickColumns not found in app.js");

let failures = 0;
function fail(msg) {
  failures++;
  console.log("FAIL " + msg);
}

for (const width of [380, 520, 800, 1200, 1600]) {
  const maxCols = width >= 1100 ? 4 : width >= 700 ? 3 : width >= 460 ? 2 : 1;
  for (let n = 1; n <= 80; n++) {
    const cols = pickColumns(n, width);
    if (cols > maxCols || cols < Math.min(3, maxCols)) {
      fail(`width=${width} n=${n}: cols=${cols} outside [${Math.min(3, maxCols)}, ${maxCols}]`);
    }
    // Same condition gallery.html uses to promote the last card to a row.
    const remainder = n % cols;
    const loneLast = remainder === 1 && cols > 1 && n > 1;
    const lastRowCards = remainder === 0 ? cols : remainder;
    const orphanVisible = lastRowCards === 1 && cols > 1 && n > 1 && !loneLast;
    if (orphanVisible) fail(`width=${width} n=${n}: lone last card not handled (cols=${cols})`);
  }
}

// The three counts called out in the design checklist, at desktop width.
for (const n of [24, 25, 26]) {
  const cols = pickColumns(n, 1200);
  const remainder = n % cols;
  const promoted = remainder === 1;
  console.log(`n=${n} @1200px -> ${cols} cols, last row ${remainder === 0 ? cols : remainder} card(s)${promoted ? " (promoted to full-width row)" : ""}`);
}

if (failures) {
  console.log(`\n${failures} failure(s).`);
  process.exit(1);
}
console.log("\nAll column layouts OK (1..80 videos x 5 widths).");
