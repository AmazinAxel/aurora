// Strips the glyphs a font never draws from a fontconvert header, in place.
//
// Usage: bun tools/subset_font.mjs <header.h> <chars>
//
// fontconvert only takes a contiguous range, so a face that needs "-0123456789"
// or the weekday letters drags in everything between. Unused glyphs at either
// end of the range are cut from the table; unused ones in the middle keep a
// zero-size entry (the table is indexed by code point) but lose their bitmap.

import { readFileSync, writeFileSync } from "node:fs";

const [path, keep] = process.argv.slice(2);
if (!path || !keep) {
  console.error("usage: subset_font.mjs <header.h> <chars>");
  process.exit(1);
}

const src = readFileSync(path, "utf8");
const name = src.match(/const GFXfont (\w+) PROGMEM/)[1];
const yAdvance = src.match(/0x[0-9A-F]+, 0x[0-9A-F]+, (\d+) \};/)[1];

const bitmap = src
  .match(/Bitmaps\[\] PROGMEM = \{([^}]*)\}/)[1]
  .match(/0x[0-9A-F]{2}/gi)
  .map((b) => parseInt(b, 16));

const glyphs = [
  ...src.matchAll(
    /\{\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(-?\d+),\s*(-?\d+)\s*\}.*?\/\/ 0x([0-9A-F]{2})/g,
  ),
].map((m, i, all) => ({
  offset: +m[1],
  metrics: m.slice(2, 7).map(Number),
  code: parseInt(m[7], 16),
  end: i + 1 < all.length ? +all[i + 1][1] : bitmap.length,
}));

const used = (g) => keep.includes(String.fromCharCode(g.code));
const first = glyphs.findIndex(used);
const last = glyphs.findLastIndex(used);
if (first < 0) throw new Error("no requested glyph in font");

const outBitmap = [];
const outGlyphs = glyphs.slice(first, last + 1).map((g) => {
  const offset = outBitmap.length;
  if (!used(g)) return { ...g, offset, metrics: [0, 0, 0, 0, 0] };
  outBitmap.push(...bitmap.slice(g.offset, g.end));
  return { ...g, offset };
});

const hex = (b) => "0x" + b.toString(16).toUpperCase().padStart(2, "0");
const rows = [];
for (let i = 0; i < outBitmap.length; i += 12) {
  rows.push("  " + outBitmap.slice(i, i + 12).map(hex).join(", "));
}

const pad = (v, n) => String(v).padStart(n);
const glyphRows = outGlyphs.map((g, i) => {
  const [w, h, adv, xo, yo] = g.metrics;
  const tail = i === outGlyphs.length - 1 ? " } };" : " },  ";
  const ch = String.fromCharCode(g.code);
  return `  { ${pad(g.offset, 5)}, ${pad(w, 3)}, ${pad(h, 3)}, ${pad(adv, 3)}, ${pad(xo, 4)}, ${pad(yo, 4)}${tail} // ${hex(g.code)} '${ch}'`;
});

const lo = hex(outGlyphs[0].code);
const hi = hex(outGlyphs[outGlyphs.length - 1].code);

writeFileSync(
  path,
  `const uint8_t ${name}Bitmaps[] PROGMEM = {
${rows.join(",\n")} };

const GFXglyph ${name}Glyphs[] PROGMEM = {
${glyphRows.join("\n")}

const GFXfont ${name} PROGMEM = {
  (uint8_t  *)${name}Bitmaps,
  (GFXglyph *)${name}Glyphs,
  ${lo}, ${hi}, ${yAdvance} };

// ${outBitmap.length + outGlyphs.length * 7 + 7} bytes, subset to "${keep}"
`,
);
