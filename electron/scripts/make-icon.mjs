// Generate a placeholder app icon (build/icon.png, 1024x1024) with zero deps.
//
// electron-builder auto-derives the macOS .icns (and every other size) from a
// >=512px build/icon.png, so we only need to emit one PNG. The look is a soft
// "neumorphic" dark rounded square with a raised knob and a highlight sweep — a
// stand-in until a designed icon replaces it. Pure Node: hand-rolled PNG encoder
// (zlib deflate + CRC32), no image libraries.

import { deflateSync } from 'node:zlib';
import { writeFileSync, mkdirSync } from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const SIZE = 1024;

// --- tiny PNG encoder --------------------------------------------------------
const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();
function crc32(buf) {
  let c = 0xffffffff;
  for (let i = 0; i < buf.length; i++) c = CRC_TABLE[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}
function chunk(type, data) {
  const len = Buffer.alloc(4);
  len.writeUInt32BE(data.length, 0);
  const typeBuf = Buffer.from(type, 'ascii');
  const body = Buffer.concat([typeBuf, data]);
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(body), 0);
  return Buffer.concat([len, body, crc]);
}
function encodePng(width, height, rgba) {
  const sig = Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]);
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8; // bit depth
  ihdr[9] = 6; // color type RGBA
  // raw scanlines with filter byte 0
  const stride = width * 4;
  const raw = Buffer.alloc((stride + 1) * height);
  for (let y = 0; y < height; y++) {
    raw[y * (stride + 1)] = 0;
    rgba.copy(raw, y * (stride + 1) + 1, y * stride, y * stride + stride);
  }
  const idat = deflateSync(raw, { level: 9 });
  return Buffer.concat([
    sig,
    chunk('IHDR', ihdr),
    chunk('IDAT', idat),
    chunk('IEND', Buffer.alloc(0)),
  ]);
}

// --- draw --------------------------------------------------------------------
function clamp(v) { return v < 0 ? 0 : v > 255 ? 255 : v | 0; }
function lerp(a, b, t) { return a + (b - a) * t; }

const rgba = Buffer.alloc(SIZE * SIZE * 4);
const cx = SIZE / 2;
const cy = SIZE / 2;
const knobR = SIZE * 0.30;
const radius = SIZE * 0.22; // corner radius of the rounded square

for (let y = 0; y < SIZE; y++) {
  for (let x = 0; x < SIZE; x++) {
    const i = (y * SIZE + x) * 4;

    // rounded-square mask (transparent outside)
    const rx = Math.max(Math.abs(x - cx) - (cx - radius), 0);
    const ry = Math.max(Math.abs(y - cy) - (cy - radius), 0);
    const cornerDist = Math.sqrt(rx * rx + ry * ry);
    if (cornerDist > radius) {
      rgba[i + 3] = 0;
      continue;
    }

    // base plate: subtle top->bottom gradient
    const g = y / SIZE;
    let r = lerp(40, 24, g);
    let gg = lerp(46, 28, g);
    let b = lerp(56, 34, g);

    // raised knob with directional lighting (top-left light source)
    const dx = x - cx;
    const dy = y - cy;
    const dist = Math.sqrt(dx * dx + dy * dy);
    if (dist < knobR) {
      const nz = Math.sqrt(Math.max(0, 1 - (dist / knobR) ** 2));
      const light = (-dx - dy) / (knobR * 1.6) + nz * 0.8; // -~1..~1.6
      const shade = 0.5 + light * 0.5;
      r = clamp(lerp(70, 150, shade));
      gg = clamp(lerp(96, 190, shade));
      b = clamp(lerp(220, 255, shade));
      // pointer notch (a bright indicator toward top)
      const ang = Math.atan2(dy, dx);
      if (dist > knobR * 0.55 && dist < knobR * 0.9 && Math.abs(ang + Math.PI / 2) < 0.12) {
        r = 245; gg = 250; b = 255;
      }
    } else {
      // outer ring shadow just around the knob for the "pressed" feel
      const ringT = Math.min(1, (dist - knobR) / (SIZE * 0.06));
      const darken = 1 - 0.35 * (1 - ringT);
      r *= darken; gg *= darken; b *= darken;
    }

    rgba[i] = clamp(r);
    rgba[i + 1] = clamp(gg);
    rgba[i + 2] = clamp(b);
    rgba[i + 3] = 255;
  }
}

const outDir = path.join(__dirname, '..', 'build');
mkdirSync(outDir, { recursive: true });
const outFile = path.join(outDir, 'icon.png');
writeFileSync(outFile, encodePng(SIZE, SIZE, rgba));
console.log(`wrote ${outFile} (${SIZE}x${SIZE})`);
