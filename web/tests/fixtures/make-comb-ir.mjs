// Generates comb-ir.wav — a DISTINCTIVE test cab IR used by the upload Playwright
// test: a two-spike "delay pair" (spike at n=0 and n=240) whose magnitude
// response is a 200 Hz-spaced COMB (peaks at multiples of 200 Hz, notches at
// odd multiples of 100 Hz, @48 kHz). Feeding broadband audio through a cab loaded
// with this IR leaves an easily-measured comb signature (a 400 Hz peak far above
// a 500 Hz notch), proving the output was actually convolved by the uploaded IR.
//
// Run: node web/tests/fixtures/make-comb-ir.mjs
import { writeFileSync } from 'node:fs';

const sampleRate = 48000;
const len = 512; // > one 128 partition
const data = new Float32Array(len);
data[0] = 1.0;
data[240] = 0.9;

// 16-bit PCM mono WAV.
const bytesPerSample = 2;
const dataBytes = len * bytesPerSample;
const buf = Buffer.alloc(44 + dataBytes);
buf.write('RIFF', 0);
buf.writeUInt32LE(36 + dataBytes, 4);
buf.write('WAVE', 8);
buf.write('fmt ', 12);
buf.writeUInt32LE(16, 16); // PCM chunk size
buf.writeUInt16LE(1, 20); // PCM
buf.writeUInt16LE(1, 22); // mono
buf.writeUInt32LE(sampleRate, 24);
buf.writeUInt32LE(sampleRate * bytesPerSample, 28);
buf.writeUInt16LE(bytesPerSample, 32);
buf.writeUInt16LE(16, 34);
buf.write('data', 36);
buf.writeUInt32LE(dataBytes, 40);
for (let i = 0; i < len; i++) {
  const s = Math.max(-1, Math.min(1, data[i]));
  buf.writeInt16LE(Math.round(s * 32767), 44 + i * bytesPerSample);
}
const out = new URL('./comb-ir.wav', import.meta.url);
writeFileSync(out, buf);
console.log('wrote', out.pathname, dataBytes + 44, 'bytes');
