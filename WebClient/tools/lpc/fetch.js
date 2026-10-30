'use strict';
// 원본 받기 — LPC 캐릭터 생성기 저장소에서 layers.js 가 쓰는 원본 그림 + 크레딧 줄을 src/ 로 받는다.
//   실행: node tools/lpc/fetch.js [커밋]   (WebClient 에서 · 인터넷 필요 · 한 번만 — 받은 src/ 는 리포에 두고 build.js 는 src/ 만 읽는다)
//   받는 커밋을 하나로 못 박는다 — 저장소 기본 가지의 지금 커밋 번호를 먼저 묻고 그 번호로 받는다(src/SOURCE.txt 에 적음).
//   커밋을 주면 그 커밋으로 받는다 — 동작만 더할 때(10-30 숨쉬기) 이미 받은 원본이 새 커밋으로 바뀌지 않게 SOURCE.txt 의 번호를 넣는다.
//   다시 받으면 원본이 바뀌었을 수 있다 — 그러면 build.js 의 색 대응 검사가 알려 준다(아래 build.js 머리 주석).
const fs = require('fs'), path = require('path'), https = require('https');
const { LAYERS, ANIMS, WEAPONS, dirsOf } = require('./layers.js');
const REPO = 'liberatedpixelcup/Universal-LPC-Spritesheet-Character-Generator';
const SRC = path.join(__dirname, 'src');

function get(url) {
  return new Promise((res, rej) => https.get(url, { headers: { 'User-Agent': 'mmo-zone-lpc-fetch' } }, r => {
    if (r.statusCode !== 200) { r.resume(); return rej(new Error(r.statusCode + ' ' + url)); }
    const parts = []; r.on('data', d => parts.push(d)); r.on('end', () => res(Buffer.concat(parts)));
  }).on('error', rej));
}

// 받을 파일 = 원본 폴더 × 동작 + 무기 그림 (spritesheets/ 아래 경로)
function wanted() {
  const files = new Set();
  for (const dir of new Set(Object.values(LAYERS).flatMap(dirsOf))) for (const a of ANIMS) files.add(`${dir}/${a}.png`);
  for (const w of Object.values(WEAPONS)) for (const a of Object.values(w)) for (const f of Object.values(a)) files.add(f);
  return [...files].sort();
}

// 크레딧 표(CREDITS.csv)의 파일 이름 — 무기처럼 색별 파일(…/walk/dagger.png)은 표에 동작 이름(…/walk.png)으로 실려 있다
function creditKey(file, rows) {
  if (rows.has(file)) return file;
  const alt = path.posix.dirname(file) + '.png';
  return rows.has(alt) ? alt : null;
}

(async () => {
  const head = JSON.parse(await get(`https://api.github.com/repos/${REPO}/commits/${process.argv[2] || 'HEAD'}`));
  const sha = head.sha;
  const raw = f => `https://raw.githubusercontent.com/${REPO}/${sha}/${f}`;
  const files = wanted();
  for (const f of files) {
    const out = path.join(SRC, f);
    fs.mkdirSync(path.dirname(out), { recursive: true });
    fs.writeFileSync(out, await get(raw('spritesheets/' + f)));
  }
  // 크레딧 — 받은 파일의 줄만(머리 줄 포함). 줄 안의 쉼표는 따옴표 안에 있어 첫 칸(파일 이름)만 떼어 본다
  const csv = (await get(raw('CREDITS.csv'))).toString('utf8').split(/\r?\n/);
  const rows = new Map();
  for (const line of csv.slice(1)) { const m = /^"([^"]+)"/.exec(line); if (m) rows.set(m[1], line); }
  const used = [], missing = [];
  for (const f of files) { const k = creditKey(f, rows); if (k) { if (!used.includes(rows.get(k))) used.push(rows.get(k)); } else missing.push(f); }
  if (missing.length) { console.error('!! 크레딧 줄이 없는 파일:', missing.join(', ')); process.exit(1); }
  fs.writeFileSync(path.join(SRC, 'CREDITS.csv'), [csv[0], ...used].join('\n') + '\n');
  fs.writeFileSync(path.join(SRC, 'SOURCE.txt'),
    `저장소 https://github.com/${REPO}\n커밋 ${sha} (${head.commit.committer.date})\n받은 날 ${new Date().toISOString()}\n파일 ${files.length}개 — spritesheets/ 아래 경로 그대로\n`);
  console.log(`받음 ${files.length}개 · 크레딧 줄 ${used.length} · 커밋 ${sha.slice(0, 10)}`);
})().catch(e => { console.error('!!', e.message); process.exit(1); });
