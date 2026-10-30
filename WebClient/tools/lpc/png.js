'use strict';
// PNG 읽기·쓰기 — 이 폴더의 도구만 쓴다(의존 패키지 없이 zlib 만).
//   읽기: 비월 PNG · 색 형식 팔레트(3) · RGB(2) · RGBA(6) · 회색(0) · 회색+알파(4) · 비트 깊이 1·2·4·8 → RGBA 8비트 한 장.
//     LPC 원본은 팔레트(몸·머리·바지)와 RGBA(머리카락·웃옷·무기)가 섞여 있다(10-30 22벌 확인) — 둘 다 받아야 한다.
//     16비트·인터레이스는 받지 않는다(만나면 멈춘다 — 조용히 틀린 그림을 내지 않게).
//   쓰기: RGBA 8비트 · 줄마다 거르기 0(없음) — 크기보다 「같은 픽셀이면 같은 바이트」를 고른 것(전후 대조가 바이트로 된다)
const zlib = require('zlib');

const SIG = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);

function decode(buf) {
  if (!buf.subarray(0, 8).equals(SIG)) throw new Error('PNG 아님');
  let p = 8, w = 0, h = 0, depth = 0, type = 0, interlace = 0, plte = null, trns = null;
  const idat = [];
  while (p < buf.length) {
    const len = buf.readUInt32BE(p), kind = buf.toString('latin1', p + 4, p + 8), data = buf.subarray(p + 8, p + 8 + len);
    if (kind === 'IHDR') { w = data.readUInt32BE(0); h = data.readUInt32BE(4); depth = data[8]; type = data[9]; interlace = data[12]; }
    else if (kind === 'PLTE') plte = data;
    else if (kind === 'tRNS') trns = data;
    else if (kind === 'IDAT') idat.push(data);
    else if (kind === 'IEND') break;
    p += 12 + len;
  }
  if (interlace) throw new Error('인터레이스 PNG 는 안 받는다');
  if (depth === 16) throw new Error('16비트 PNG 는 안 받는다');
  const ch = { 0: 1, 2: 3, 3: 1, 4: 2, 6: 4 }[type];
  if (!ch) throw new Error('모르는 색 형식 ' + type);
  const bpp = Math.max(1, (ch * depth) >> 3), stride = (w * ch * depth + 7) >> 3;
  const raw = zlib.inflateSync(Buffer.concat(idat));
  const rows = Buffer.alloc(stride * h);
  let prev = Buffer.alloc(stride);
  for (let y = 0; y < h; y++) {
    const f = raw[y * (stride + 1)], src = raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1)), cur = rows.subarray(y * stride, (y + 1) * stride);
    for (let i = 0; i < stride; i++) {
      const a = i >= bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0;
      let v;
      switch (f) {
        case 0: v = 0; break;
        case 1: v = a; break;
        case 2: v = b; break;
        case 3: v = (a + b) >> 1; break;
        case 4: { const pa = Math.abs(b - c), pb = Math.abs(a - c), pc = Math.abs(a + b - 2 * c); v = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c); break; }
        default: throw new Error('모르는 거르기 ' + f);
      }
      cur[i] = (src[i] + v) & 255;
    }
    prev = cur;
  }
  const out = Buffer.alloc(w * h * 4);
  const sample = (row, x, k) => {   // x 번째 픽셀의 k 번째 채널 값(비트 깊이 8 미만은 꺼내 늘린다)
    if (depth === 8) return row[x * ch + k];
    const bit = (x * ch + k) * depth, v = (row[bit >> 3] >> (8 - depth - (bit & 7))) & ((1 << depth) - 1);
    return type === 3 ? v : Math.round(v * 255 / ((1 << depth) - 1));
  };
  const rawSample = (row, x, k) => {   // 늘리기 전 값 — tRNS 견주기용
    if (depth === 8) return row[x * ch + k];
    const bit = (x * ch + k) * depth; return (row[bit >> 3] >> (8 - depth - (bit & 7))) & ((1 << depth) - 1);
  };
  for (let y = 0; y < h; y++) {
    const row = rows.subarray(y * stride, (y + 1) * stride);
    for (let x = 0; x < w; x++) {
      const o = (y * w + x) * 4;
      switch (type) {
        case 3: { const i = sample(row, x, 0); out[o] = plte[i * 3]; out[o + 1] = plte[i * 3 + 1]; out[o + 2] = plte[i * 3 + 2]; out[o + 3] = trns && i < trns.length ? trns[i] : 255; break; }
        case 6: for (let k = 0; k < 4; k++) out[o + k] = sample(row, x, k); break;
        // 회색·RGB 의 tRNS 는 「이 값이면 투명」 한 색(2바이트씩, 비트 깊이 그대로의 값) — 늘리기 전 값으로 견준다
        case 2: { let key = !!trns; for (let k = 0; k < 3; k++) { out[o + k] = sample(row, x, k); if (key && rawSample(row, x, k) !== trns.readUInt16BE(k * 2)) key = false; } out[o + 3] = key ? 0 : 255; break; }
        case 0: out[o] = out[o + 1] = out[o + 2] = sample(row, x, 0); out[o + 3] = trns && rawSample(row, x, 0) === trns.readUInt16BE(0) ? 0 : 255; break;
        case 4: out[o] = out[o + 1] = out[o + 2] = sample(row, x, 0); out[o + 3] = sample(row, x, 1); break;
      }
    }
  }
  return { w, h, data: out };
}

const CRC_TABLE = (() => { const t = new Uint32Array(256); for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; } return t; })();
function crc(buf) { let c = 0xffffffff; for (const b of buf) c = CRC_TABLE[(c ^ b) & 255] ^ (c >>> 8); return (c ^ 0xffffffff) >>> 0; }
function chunk(kind, data) {
  const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
  const kd = Buffer.concat([Buffer.from(kind, 'latin1'), data]);
  const c = Buffer.alloc(4); c.writeUInt32BE(crc(kd));
  return Buffer.concat([len, kd, c]);
}
function encode({ w, h, data }) {
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 6;
  const raw = Buffer.alloc((w * 4 + 1) * h);
  for (let y = 0; y < h; y++) data.copy(raw, y * (w * 4 + 1) + 1, y * w * 4, (y + 1) * w * 4);
  return Buffer.concat([SIG, chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw, { level: 9 })), chunk('IEND', Buffer.alloc(0))]);
}

module.exports = { decode, encode };
