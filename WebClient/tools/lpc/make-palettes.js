'use strict';
// 색 대응표 만들기 — 옛 걷기 그림(assets/lpc_layers.js)과 원본 걷기(src/)를 픽셀마다 겹쳐 「원본 색 → 우리 색」을 뽑는다 → palettes.json
//   실행: node tools/lpc/make-palettes.js   (WebClient 에서 · 한 번만 — 10-30 에 돌려 palettes.json 을 리포에 두었다.
//         옛 그림은 그 뒤 지웠으므로 다시 돌리려면 git 이력에서 assets/lpc_layers.js 를 꺼내 같은 자리에 두면 된다)
//   원본 색 하나에 우리 색이 여럿 대응되면(원본이 그 뒤 고쳐진 13장 — layers.js 머리 주석) 가장 많이 겹친 색을 쓴다.
//   어긋남은 표에 그대로 적는다(exact · conflictPx) — 아침에 사람이 볼 수 있게.
const fs = require('fs'), path = require('path');
const PNG = require('./png.js');
const { LAYERS, layerImage } = require('./layers.js');
const SRC = path.join(__dirname, 'src');
const OLD = path.join(__dirname, '..', '..', 'assets', 'lpc_layers.js');
// 원본 색을 그대로 쓰는 부위 — 대응표로 칠하면 오히려 옛 그림과 멀어지는 것. 표는 「원본 색 → 같은 색」으로 둔다
//   b_skeleton: 원본이 몸·머리로 나뉘며 음영이 바뀌어, 다수결 대응이 해골 머리 밝은 쪽을 한 색으로 뭉갰다 — 원본 색 그대로가 옛 그림에 더 가깝다(10-30 사진 셋 대조)
const KEEP_ORIGINAL = new Set(['b_skeleton']);
const hex = (d, o) => '#' + [0, 1, 2, 3].map(k => d[o + k].toString(16).padStart(2, '0')).join('');

const text = fs.readFileSync(OLD, 'utf8');
const old = new Map([...text.matchAll(/"([a-z_0-9]+)":"data:image\/png;base64,([^"]+)"/g)].map(m => [m[1], Buffer.from(m[2], 'base64')]));
const out = {};
for (const [name, dir] of Object.entries(LAYERS)) {
  if (!old.has(name)) { console.error('!! 옛 그림에 없는 부위:', name); process.exit(1); }
  const ours = PNG.decode(old.get(name)), base = layerImage(PNG, SRC, dir, 'walk');
  if (ours.w !== base.w || ours.h !== base.h) { console.error('!! 크기가 다르다:', name, ours.w, ours.h, base.w, base.h); process.exit(1); }
  const votes = new Map();   // 원본 색 → (우리 색 → 픽셀 수)
  let both = 0, onlyOurs = 0, onlyBase = 0;
  for (let i = 0; i < ours.data.length; i += 4) {
    const a = ours.data[i + 3] > 0, b = base.data[i + 3] > 0;
    if (a && b) {
      both++;
      const k = hex(base.data, i), v = hex(ours.data, i);
      if (!votes.has(k)) votes.set(k, new Map());
      const m = votes.get(k); m.set(v, (m.get(v) || 0) + 1);
    } else if (a) onlyOurs++; else if (b) onlyBase++;
  }
  const map = {}; let conflictPx = 0;
  for (const [k, m] of [...votes].sort()) {
    const ranked = [...m].sort((x, y) => y[1] - x[1] || (x[0] < y[0] ? -1 : 1));   // 표가 같으면 글자 순 — 돌릴 때마다 같은 표
    map[k] = KEEP_ORIGINAL.has(name) ? k : ranked[0][0];
    for (const [, n] of ranked.slice(1)) conflictPx += n;
  }
  out[name] = { base: dir, keepOriginal: KEEP_ORIGINAL.has(name), exact: conflictPx === 0 && onlyOurs === 0 && onlyBase === 0, overlapPx: both, conflictPx, onlyOursPx: onlyOurs, onlyBasePx: onlyBase, map };
}
fs.writeFileSync(path.join(__dirname, 'palettes.json'), JSON.stringify(out, null, 1) + '\n');
const ex = Object.values(out).filter(v => v.exact).length;
console.log(`부위 ${Object.keys(out).length} · 1:1 그대로 ${ex} · 어긋남 ${Object.keys(out).length - ex}`);
for (const [n, v] of Object.entries(out)) if (!v.exact) console.log(`  ${n.padEnd(12)} 겹침 ${v.overlapPx} · 다른 색 ${v.conflictPx} · 우리만 ${v.onlyOursPx} · 원본만 ${v.onlyBasePx}`);
