// Generates src/assets/cover.h from a 200x200 PNG.
//
// Usage: bun tools/gen_assets.mjs <png> [out.h]
//
// The panel is 1-bit: every pixel is either ink or paper. Pixels are packed
// MSB-first into rows of ceil(width/8) bytes, the layout GxEPD2's drawBitmap
// expects, with a set bit meaning ink.

import { readFileSync, writeFileSync } from "node:fs";
import { basename } from "node:path";
import { inflateSync } from "node:zlib";

const PNG_SIGNATURE = Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]);

// Anything at or below this 0-255 luminance becomes ink. Halfway is right for
// artwork that is already black-on-white; it matters only for the antialiased
// edge pixels.
const INK_THRESHOLD = 127;

function decodePng(buffer) {
  if (!buffer.subarray(0, 8).equals(PNG_SIGNATURE)) {
    throw new Error("not a PNG");
  }

  let width = 0;
  let height = 0;
  let bitDepth = 0;
  let colorType = 0;
  let palette = null;
  let transparency = null;
  const idat = [];

  for (let offset = 8; offset < buffer.length; ) {
    const length = buffer.readUInt32BE(offset);
    const type = buffer.toString("ascii", offset + 4, offset + 8);
    const data = buffer.subarray(offset + 8, offset + 8 + length);
    offset += 12 + length;

    if (type === "IHDR") {
      width = data.readUInt32BE(0);
      height = data.readUInt32BE(4);
      bitDepth = data[8];
      colorType = data[9];
      if (data[12] !== 0) {
        throw new Error("interlaced PNG is not supported");
      }
    } else if (type === "PLTE") {
      palette = data;
    } else if (type === "tRNS") {
      transparency = data;
    } else if (type === "IDAT") {
      idat.push(data);
    } else if (type === "IEND") {
      break;
    }
  }

  if (bitDepth !== 8 && !(colorType === 3 && bitDepth <= 8) && bitDepth !== 1) {
    throw new Error(`unsupported bit depth ${bitDepth}`);
  }

  const raw = inflateSync(Buffer.concat(idat));
  const samples = { 0: 1, 2: 3, 3: 1, 4: 2, 6: 4 }[colorType];
  if (samples === undefined) {
    throw new Error(`unsupported colour type ${colorType}`);
  }

  // Filters operate on whole bytes, and on the byte a full pixel back, so
  // sub-byte depths use a 1-byte step.
  const bitsPerPixel = samples * bitDepth;
  const bytesPerPixel = Math.max(1, bitsPerPixel >> 3);
  const bytesPerRow = Math.ceil((width * bitsPerPixel) / 8);

  const pixels = Buffer.alloc(height * bytesPerRow);
  let previous = Buffer.alloc(bytesPerRow);

  for (let y = 0; y < height; y++) {
    const filter = raw[y * (bytesPerRow + 1)];
    const source = raw.subarray(
      y * (bytesPerRow + 1) + 1,
      (y + 1) * (bytesPerRow + 1),
    );
    const row = pixels.subarray(y * bytesPerRow, (y + 1) * bytesPerRow);

    for (let x = 0; x < bytesPerRow; x++) {
      const a = x >= bytesPerPixel ? row[x - bytesPerPixel] : 0;
      const b = previous[x];
      const c = x >= bytesPerPixel ? previous[x - bytesPerPixel] : 0;
      let value = source[x];

      switch (filter) {
        case 0:
          break;
        case 1:
          value += a;
          break;
        case 2:
          value += b;
          break;
        case 3:
          value += (a + b) >> 1;
          break;
        case 4: {
          const p = a + b - c;
          const pa = Math.abs(p - a);
          const pb = Math.abs(p - b);
          const pc = Math.abs(p - c);
          value += pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
          break;
        }
        default:
          throw new Error(`unknown filter ${filter} on row ${y}`);
      }

      row[x] = value & 0xff;
    }

    previous = row;
  }

  return {
    width,
    height,
    bitDepth,
    colorType,
    palette,
    transparency,
    bytesPerRow,
    pixels,
  };
}

// Returns luminance 0-255, compositing any alpha over white, since the panel
// has no transparency and paper is what shows through.
function luminanceAt(image, x, y) {
  const { bitDepth, colorType, palette, transparency, bytesPerRow, pixels } =
    image;
  const row = y * bytesPerRow;

  if (colorType === 3) {
    const perByte = 8 / bitDepth;
    const byte = pixels[row + Math.floor(x / perByte)];
    const shift = (perByte - 1 - (x % perByte)) * bitDepth;
    const index = (byte >> shift) & ((1 << bitDepth) - 1);
    const r = palette[index * 3];
    const g = palette[index * 3 + 1];
    const b = palette[index * 3 + 2];
    const alpha = transparency && index < transparency.length
      ? transparency[index]
      : 255;
    const grey = 0.299 * r + 0.587 * g + 0.114 * b;
    return (grey * alpha + 255 * (255 - alpha)) / 255;
  }

  if (bitDepth === 1) {
    const byte = pixels[row + (x >> 3)];
    return ((byte >> (7 - (x & 7))) & 1) * 255;
  }

  if (colorType === 0 || colorType === 4) {
    const stride = colorType === 4 ? 2 : 1;
    const grey = pixels[row + x * stride];
    const alpha = colorType === 4 ? pixels[row + x * stride + 1] : 255;
    return (grey * alpha + 255 * (255 - alpha)) / 255;
  }

  const stride = colorType === 6 ? 4 : 3;
  const r = pixels[row + x * stride];
  const g = pixels[row + x * stride + 1];
  const b = pixels[row + x * stride + 2];
  const alpha = colorType === 6 ? pixels[row + x * stride + 3] : 255;
  const grey = 0.299 * r + 0.587 * g + 0.114 * b;
  return (grey * alpha + 255 * (255 - alpha)) / 255;
}

const [source, destination = "src/assets/cover.h"] = process.argv.slice(2);
if (!source) {
  console.error("usage: bun tools/gen_assets.mjs <png> [out.h]");
  process.exit(1);
}

const image = decodePng(readFileSync(source));
if (image.width !== 200 || image.height !== 200) {
  throw new Error(`expected a 200x200 image, got ${image.width}x${image.height}`);
}

const stride = Math.ceil(image.width / 8);
const bitmap = Buffer.alloc(stride * image.height);
for (let y = 0; y < image.height; y++) {
  for (let x = 0; x < image.width; x++) {
    if (luminanceAt(image, x, y) <= INK_THRESHOLD) {
      bitmap[y * stride + (x >> 3)] |= 0x80 >> (x & 7);
    }
  }
}

const lines = [];
for (let i = 0; i < bitmap.length; i += 16) {
  const row = [...bitmap.subarray(i, i + 16)].map(
    (byte) => `0x${byte.toString(16).padStart(2, "0")}`,
  );
  lines.push(`    ${row.join(", ")},`);
}

writeFileSync(
  destination,
  `// Generated from ${basename(source)} by tools/gen_assets.mjs -- do not edit.

#pragma once

#include <pgmspace.h>
#include <stdint.h>

#define COVER_WIDTH ${image.width}
#define COVER_HEIGHT ${image.height}

const uint8_t COVER_BITMAP[] PROGMEM = {
${lines.join("\n")}
};
`,
);

const ink = bitmap.reduce(
  (total, byte) => total + byte.toString(2).replace(/0/g, "").length,
  0,
);
console.log(
  `${destination}: ${bitmap.length} bytes, ${(
    (ink / (image.width * image.height)) *
    100
  ).toFixed(1)}% ink`,
);
