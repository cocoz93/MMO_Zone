'use strict';
// 마을 막힌 칸 — tools/town/town0.js(집 자리) + 아래 바닥 표 → Run/bin/BlockTable.csv 의 마을(맵 0) 줄
//   실행: node tools/town/blocks.js          (WebClient 에서 · 크롬 필요 없음. build.js 가 끝에 같이 부른다)
//         node tools/town/blocks.js --check  (지금 town0.js 로 뽑은 줄과 표의 마을 줄이 다르면 실패 — 집을 옮기고 표 갱신을 잊은 것)
//   표의 다른 맵 줄과 머리 주석은 그대로 둔다 — 던전 바위 같은 줄은 손으로 적는다(표 머리 주석)
//   막는 것 = 집 8채의 벽 아랫부분(바닥 자리)만. 지붕은 안 막는다 — 집 뒤로 지나가면 지붕에 가려진다(웹이 발밑 y 로 깊이 정렬)
const fs = require('fs'), path = require('path');
const LAYOUT = require('./town0.js');
const CSV = path.join(__dirname, '..', '..', '..', 'Run', 'bin', 'BlockTable.csv');

// 집 그림마다 벽 아래 줄의 가로 구간 — 그림 기준점(가로 가운데 · 발밑 줄)에서 월드 칸. 그림의 벽 픽셀을 재서 적은 값(10-25)
//   한 그림에 구간이 둘이면 사이가 뚫린 것 — house_gate 는 가운데 대문으로 북쪽 큰길(town0.js 돌바닥 rect [28,-1,32,30])이 지나간다
const FOOTPRINT = {
  house_gate:     [[-10.95, -2.32], [2.2, 10.85]],    // 오른쪽 구간은 대문 오른쪽 돌기둥(흰 자갈 기둥)부터
  house_lshape:   [[-6.59, 7.05]],
  house_dormer:   [[-6.56, 6.75]],
  house_twostory: [[-3.59, 3.73]],
  house_annex:    [[-2.74, 2.88]],
  cottage_small:  [[-2.16, 2.11]],
};
const DEPTH = 3;   // 발밑 줄에서 위로 막는 칸 수 — 캐릭터 키(3칸)만큼. 그 위로는 지붕 뒤라 걸어 다닌다

// 실수 구간 → 그 안에 칸 가운데가 드는 칸 번호(양끝 포함). 비면 null
const cellSpan = (a, b) => { const c0 = Math.ceil(a - 0.5), c1 = Math.floor(b - 0.5); return c0 <= c1 ? [c0, c1] : null; };

function townRows() {
  const rows = [];
  for (const [name, x, footY] of LAYOUT.objects) {
    const spans = FOOTPRINT[name]; if (!spans) continue;   // 집이 아닌 소품은 안 막는다
    const ys = cellSpan(footY - DEPTH, footY);
    for (const [a, b] of spans) { const xs = cellSpan(x + a, x + b); if (xs && ys) rows.push([LAYOUT.mapId, xs[0], ys[0], xs[1], ys[1], name]); }
  }
  return rows.map(r => r.join(','));
}

const HEADER = [
  '# Blocked cells - design data, read once at startup (CBlockTable). Every build reads it (no toggle) - like MapTable.csv.',
  '#   One row = one blocked rectangle of cells on mapId. Nobody walks into a blocked cell (server CZone, web live.html, console GameClient).',
  '#   A cell is (floor(x), floor(y)). x0,y0,x1,y1 are cell numbers, both ends included, inside the map, x0 <= x1, y0 <= y1.',
  '#   The map edge is not in this table - it stays the old clamp (0..width-1).',
  '#   A map with any row must have a start area (MapTable.csv). Start areas, portals (both ends), NPCs and spawn regions',
  '#   must not touch a blocked cell - the server refuses to start otherwise.',
  '#   Map 0 (town) rows are written by WebClient/tools/town/blocks.js from town0.js - do not edit them by hand',
  '#   (blocks.js --check fails when they drift). Rows of other maps are hand-written and kept by blocks.js.',
  '#   note : free text (house sprite name for the town). No commas.',
  '#   The first cell must be a number - GameDataCsv treats a line whose first cell is not a number as the header.',
  'mapId,x0,y0,x1,y1,note',
];

const isRowOf = (line, mapId) => line.split(',')[0].trim() === String(mapId);
const readLines = () => fs.existsSync(CSV) ? fs.readFileSync(CSV, 'utf8').replace(/\r\n/g, '\n').replace(/\n$/, '').split('\n') : null;

const want = townRows();
const lines = readLines();
if (process.argv.includes('--check')) {
  const have = (lines || []).filter(l => isRowOf(l, LAYOUT.mapId));
  if (have.join('\n') !== want.join('\n')) {
    console.error(`!! BlockTable.csv 의 맵 ${LAYOUT.mapId} 줄이 town0.js 와 다르다 — node tools/town/blocks.js 로 다시 써라`);
    console.error('   표:', have.length, '줄 · town0.js:', want.length, '줄');
    process.exit(1);
  }
  console.log(`BlockTable.csv 맵 ${LAYOUT.mapId} ${want.length}줄 — town0.js 와 같음`);
} else {
  // 머리 주석·열 이름은 있던 것을 둔다(손으로 고친 설명을 지우지 않게) · 없으면 위 HEADER
  const keep = lines ? lines.filter(l => !isRowOf(l, LAYOUT.mapId)) : HEADER;
  fs.writeFileSync(CSV, [...keep, ...want].join('\r\n') + '\r\n');
  console.log(`BlockTable.csv 맵 ${LAYOUT.mapId} ${want.length}줄 씀 (다른 줄 ${keep.length} 그대로)`);
}
