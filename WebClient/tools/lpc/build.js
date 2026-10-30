'use strict';
// 캐릭터 그림 만들기 — src/(받은 원본) + palettes.json(원본 색 → 우리 색) → assets/lpc/
//   실행: node tools/lpc/build.js   (WebClient 에서 · 인터넷·크롬 필요 없음)
//   만드는 것
//     <부위>_<동작>.png   — layers.js 의 부위 × 동작(walk 9장 · slash 6장 · idle 2장 × 4방향 줄 · hurt 6장 × 정면 한 줄, 한 칸 64px). 원본 색을 표대로 바꿔 칠한다
//     weapon_<이름>_<동작>_<front|behind>.png — 무기 원본 그대로(색을 안 바꾼다 — 무기는 원본 색 한 벌).
//       원본에 없는 동작(layers.js WEAPON_DERIVED — idle)은 다른 동작의 한 장을 모든 칸에 복사해 만든다
//     anim.json · anim.js — 장면 표(아래 ANIM). 같은 값 두 벌 — json 은 C++ 클라가 읽고, js 는 웹이 <script> 로 읽는다
//       (웹은 run-webclient.bat 가 file:// 로 열어서 fetch 로 json 을 못 받는다 — 그림(<img>)은 file:// 에서도 된다)
//     ../../assets/lpc_credits.txt — src/CREDITS.csv 를 사람이 읽는 꼴로(파일 · 저자 · 라이선스 · 주소)
//   멈추는 경우 — 원본에 표에 없는 색이 있으면(원본을 다시 받았는데 바뀜 등) 그 부위·색을 찍고 멈춘다. 빈 칸으로 그리지 않는다
const fs = require('fs'), path = require('path');
const PNG = require('./png.js');
const { LAYERS, ANIMS, WEAPONS, WEAPON_DERIVED, layerImage } = require('./layers.js');
const SRC = path.join(__dirname, 'src');
const OUT = path.join(__dirname, '..', '..', 'assets', 'lpc');
const CREDITS_OUT = path.join(__dirname, '..', '..', 'assets', 'lpc_credits.txt');
const PALETTES = JSON.parse(fs.readFileSync(path.join(__dirname, 'palettes.json'), 'utf8'));
const fail = (...a) => { console.error('!!', ...a); process.exit(1); };
const hex = (d, o) => '#' + [0, 1, 2, 3].map(k => d[o + k].toString(16).padStart(2, '0')).join('');
const unhex = (s, d, o) => { for (let k = 0; k < 4; k++) d[o + k] = parseInt(s.slice(1 + k * 2, 3 + k * 2), 16); };

// 장면 표 — 동작마다 열 수(시트 가로 칸) · 고르는 장 · 장당 시간. 줄 순서는 모든 동작이 같다(LPC 규격)
//   walk: 서 있으면 0번 장, 걸으면 1~8번을 초당 8장(옛 live.html 의 걷기 식과 같은 값)
//   slash: 0~5번을 한 번. 장당 60ms = 0.36초 — 일반 공격 쿨타임 0.48초보다 짧아야 연타가 끊겨 보인다(플랜 2단계 처음 값)
//   idle: 서 있는 동안 loop 를 되풀이(숨쉬기). 차례 [0,0,1] · 장당 125ms 는 원본 생성기 미리보기 값 그대로
//     (sources/state/constants.ts ANIMATION_CONFIGS.idle · sources/canvas/preview-animation.ts 8 FPS). 그림이 없으면 받는 쪽은 walk.idle 한 장으로 선다
//   hurt: 죽을 때 0~5번을 한 번 돌고 마지막 장(엎어진 자세)에서 멈춘다. 원본이 정면 한 줄뿐이라 rows 를 따로 둔다(동작에 rows 가 있으면 그 줄 목록이 이긴다).
//     장당 125ms = 원본 생성기 값(ANIMATION_CONFIGS.hurt · 8 FPS). drop = 장마다 머리 끝이 서 있을 때보다 내려간 px(아래에서 원본으로 잼 — 받는 쪽이 이름표를 내린다)
const ANIM = {
  frame: 64,
  rows: ['up', 'left', 'down', 'right'],
  walk:  { cols: 9, idle: 0, loop: [1, 2, 3, 4, 5, 6, 7, 8], frameMs: 125 },
  slash: { cols: 6, frames: [0, 1, 2, 3, 4, 5], frameMs: 60 },
  idle:  { cols: 2, loop: [0, 0, 1], frameMs: 125 },
  hurt:  { cols: 6, rows: ['down'], frames: [0, 1, 2, 3, 4, 5], frameMs: 125 },
  // 무기 겉모습 이름(ItemTable.csv look 칸) — 그림은 weapon_<이름>_<동작>_<front|behind>.png. 받는 쪽이 이 목록으로 그림을 받는다
  weapons: Object.keys(WEAPONS),
};

fs.mkdirSync(OUT, { recursive: true });
let made = 0;
const rowsOf = anim => (ANIM[anim].rows || ANIM.rows).length;
for (const [name, dir] of Object.entries(LAYERS)) {
  const pal = PALETTES[name];
  if (!pal) fail('색 대응표에 없는 부위:', name, '— make-palettes.js 를 다시 돌릴 것');
  // 표가 전부 「원본 색 → 같은 색」인 부위(색을 안 바꾸는 부위)는 표에 없는 색도 원본 그대로 둔다 — 멈출 까닭(다시 칠할 색을 모름)이 없다.
  //   표는 걷기에서 뽑아서 다른 동작에만 있는 색은 표에 없을 수 있다(10-30 아프로 쓰러짐 — 앞머리가 이마에 드리우는 반투명 그림자 #0f14149c 12px)
  const sameColors = Object.entries(pal.map).every(([k, v]) => k === v);
  for (const anim of ANIMS) {
    const img = layerImage(PNG, SRC, dir, anim);
    if (img.w !== ANIM[anim].cols * ANIM.frame || img.h !== rowsOf(anim) * ANIM.frame) fail('크기가 장면 표와 다르다:', name, anim, img.w, img.h);
    const unknown = new Set();
    for (let i = 0; i < img.data.length; i += 4) {
      if (img.data[i + 3] === 0) { img.data.fill(0, i, i + 4); continue; }   // 투명 칸의 색 찌꺼기는 지운다(같은 그림이면 같은 바이트)
      const to = pal.map[hex(img.data, i)];
      if (!to) { if (!sameColors) unknown.add(hex(img.data, i)); continue; }
      unhex(to, img.data, i);
    }
    if (unknown.size) fail(`${name} ${anim}: 표에 없는 원본 색 ${unknown.size}개 —`, [...unknown].slice(0, 8).join(' '));
    fs.writeFileSync(path.join(OUT, `${name}_${anim}.png`), PNG.encode(img)); made++;
  }
}
// 받는 쪽은 동작마다 앞·뒤 두 장을 늘 읽는다 — 원본에 없는 쪽(쓰러짐의 뒤)은 같은 크기의 빈 그림으로 채운다
for (const [wname, anims] of Object.entries(WEAPONS)) for (const [anim, sides] of Object.entries(anims)) for (const side of ['front', 'behind']) {
  const file = sides[side], W = ANIM[anim].cols * ANIM.frame, H = rowsOf(anim) * ANIM.frame;
  const img = file ? PNG.decode(fs.readFileSync(path.join(SRC, file))) : { w: W, h: H, data: Buffer.alloc(W * H * 4) };
  if (img.w !== W || img.h !== H) fail('무기 크기가 장면 표와 다르다:', file, img.w, img.h);
  for (let i = 0; i < img.data.length; i += 4) if (img.data[i + 3] === 0) img.data.fill(0, i, i + 4);
  fs.writeFileSync(path.join(OUT, `weapon_${wname}_${anim}_${side}.png`), PNG.encode(img)); made++;
}
for (const [wname, anims] of Object.entries(WEAPONS)) for (const [anim, { from, frame }] of Object.entries(WEAPON_DERIVED)) {
  if (anims[anim]) continue;   // 원본이 생기면 원본을 쓴다
  if (!anims[from]) fail('무기에 복사할 동작이 없다:', wname, from);
  for (const [side, file] of Object.entries(anims[from])) {
    const s = PNG.decode(fs.readFileSync(path.join(SRC, file))), F = ANIM.frame, cols = ANIM[anim].cols;
    const img = { w: cols * F, h: s.h, data: Buffer.alloc(cols * F * s.h * 4) };
    for (let y = 0; y < s.h; y++) for (let c = 0; c < cols; c++) s.data.copy(img.data, (y * img.w + c * F) * 4, (y * s.w + frame * F) * 4, (y * s.w + frame * F + F) * 4);
    for (let i = 0; i < img.data.length; i += 4) if (img.data[i + 3] === 0) img.data.fill(0, i, i + 4);
    fs.writeFileSync(path.join(OUT, `weapon_${wname}_${anim}_${side}.png`), PNG.encode(img)); made++;
  }
}
// 쓰러짐 drop — 남자 몸 + 머리 원본으로 잰다(머리카락은 뺀다 — 모양마다 1~4px 다르다, 10-30 두 조합 대조). 서 있음 = 걷기 0번 장 정면 줄
{
  const F = ANIM.frame;
  const topOf = (imgs, c, r) => { for (let y = 0; y < F; y++) for (let x = 0; x < F; x++) if (imgs.some(im => im.data[((r * F + y) * im.w + c * F + x) * 4 + 3])) return y; return 0; };
  const load = anim => ['body/bodies/male', 'head/heads/human/male'].map(d => layerImage(PNG, SRC, d, anim));
  const stand = topOf(load('walk'), ANIM.walk.idle, ANIM.rows.indexOf('down')), hurt = load('hurt');
  ANIM.hurt.drop = ANIM.hurt.frames.map(c => topOf(hurt, c, 0) - stand);
}
const animJson = JSON.stringify(ANIM, null, 1);
fs.writeFileSync(path.join(OUT, 'anim.json'), animJson + '\n');
fs.writeFileSync(path.join(OUT, 'anim.js'), '// tools/lpc/build.js 가 만든다(손으로 고치지 말 것) — anim.json 과 같은 값. 웹은 file:// 에서도 읽히게 <script> 로 받는다\nwindow.LPC_ANIM = ' + animJson + ';\n');

// 크레딧 — CSV 한 줄 = 파일,메모,저자,라이선스,주소 (칸 안 쉼표는 따옴표 안)
const csvCells = line => { const out = []; let cur = '', q = false; for (const ch of line) { if (ch === '"') q = !q; else if (ch === ',' && !q) { out.push(cur.trim()); cur = ''; } else cur += ch; } out.push(cur.trim()); return out; };
const lines = fs.readFileSync(path.join(SRC, 'CREDITS.csv'), 'utf8').split(/\r?\n/).filter(Boolean).slice(1).map(csvCells);
const src = fs.readFileSync(path.join(SRC, 'SOURCE.txt'), 'utf8');
const repo = (/https:\/\/\S+/.exec(src) || ['?'])[0], sha = (/[0-9a-f]{40}/.exec(src) || ['?'])[0];
const credit = [
  'Character sprites (assets/lpc/*.png) — Universal LPC Spritesheet Character Generator, recolored to fixed palettes (tools/lpc).',
  'Source: ' + repo + ' @ ' + sha,
  'Per-file authors and licenses below (from the generator CREDITS.csv).', '',
  ...lines.flatMap(([file, notes, authors, licenses, urls]) => [file, '  authors: ' + authors, '  licenses: ' + licenses, ...(urls ? ['  urls: ' + urls] : []), ...(notes ? ['  notes: ' + notes] : []), '']),
].join('\n');
fs.writeFileSync(CREDITS_OUT, credit);
console.log(`만듦 ${made}장 + anim.json · anim.js · lpc_credits.txt (${lines.length}줄)`);
