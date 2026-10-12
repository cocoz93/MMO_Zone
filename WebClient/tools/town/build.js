'use strict';
// 마을 그림 만들기 — tools/town/town0.js(배치) + tools/town/src/(받은 원본 그림) → assets/town.js
//   실행: node tools/town/build.js   (WebClient 에서 · 크롬 필요 — 그림 오리기·합치기를 헤드리스 크롬 캔버스로 한다. 경로는 CHROME 환경 변수로 바꿀 수 있다)
//   만드는 것
//     땅   — town0.js 의 모양을 [LPC] Terrains 의 모서리 지형 표(terrain-v7.tsx, 칸마다 terrain="왼위,오위,왼아래,오아래")로 칸 번호로 바꾼다.
//            없는 모서리 조합이 하나라도 나오면 멈춘다(그 칸이 빈 채로 그려지므로). 쓰는 칸만 모아 아틀라스 한 장으로
//     집   — [LPC] Thatched-roof Cottage 미리보기(preview_280.png)에서 다섯 채를 오려 낸다. 미리보기의 땅은 32px 격자(PREVIEW.ox·oy)에 맞춰
//            깔려 있어서 「그 자리 땅 타일 색과 같은 픽셀」을 지우면 집만 남는다 · 세 색이 같은 비율로 어두워진 픽셀은 그림자(반투명 검정)
//            · 얇은 선(땅 경계선)을 2px 깎아 한 채씩 나누고 · 테두리가 6px 안으로 닿는 조각은 다시 잇고 · 바깥에서 이어진 초록만 지운다
//            + 작은 초가 한 채는 조각(cottage.png · thatched-roof.png · windows-doors.png)으로 조립
//     소품 — Medieval Village Decorations 시트 칸 · 견본 장면(deco-med.tmx)의 조립된 좌판·수레 · LPC 통
//     포털 — Teleporter Circle 시트 그대로(256×128 다섯 장: 꺼짐 + 켜짐 넷)
//   그림 출처·크레딧 = src/CREDITS-*.txt (assets/town_credits.txt 로 모아 둔다)
const fs = require('fs'), path = require('path'), zlib = require('zlib'), { execFileSync } = require('child_process');
const SRC = path.join(__dirname, 'src');
const ASSETS = path.join(__dirname, '..', '..', 'assets');
const CHROME = process.env.CHROME || 'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe';
const LAYOUT = require('./town0.js');
const U = 1.5;                                    // 32px 한 칸 = 월드 1.5칸(LPC 캐릭터 64px = 3칸과 같은 비율)
const png = f => fs.readFileSync(path.join(SRC, f));
const dataUri = f => 'data:image/png;base64,' + png(f).toString('base64');
const pngSize = f => { const b = png(f); return [b.readUInt32BE(16), b.readUInt32BE(20)]; };
const fail = (...a) => { console.error('!!', ...a); process.exit(1); };

// ---- 미리보기에서 집 오리기 — 미리보기 그림의 성질(바뀌지 않는 값) ----
//   ox·oy = 미리보기 땅 격자의 어긋남(지붕 칸이 가장 많이 맞는 값으로 찾음) · 빌드가 땅 픽셀 일치율로 다시 확인한다
//   regions = 집이 있는 칸 범위(격자 칸 번호 [x0,y0,x1,y1]) · names = 그 안의 집을 넓이 큰 순으로 붙일 이름
const PREVIEW = {
  ox: -13, oy: -23,
  groundCheck: [640, 400, 760, 470],              // 땅만 있는 자리 — 여기 픽셀의 90% 이상이 땅 색이어야 격자가 맞은 것
  regions: [
    { cells: [5, 2, 24, 19], names: ['house_lshape', 'house_twostory', 'house_annex'] },
    { cells: [27, 4, 43, 11], names: ['house_gate'] },
    { cells: [26, 11, 38, 20], names: ['house_dormer'] },
  ],
  groundTerrains: ['Grass', 'Dirt_Brown', 'Grass_Light', 'Grass_Dark', 'Dirt_Tan', 'Dirt_Dark'],
};
// 작은 초가 — [시트, sx, sy, sw, sh, dx, dy] · 문 시트는 문이 32px 칸에서 16px 밀려 있다(문 하나 = x 16~47 · 80~111 …)
const COTTAGE_SMALL = [
  ['cot', 288, 128, 96, 96, 8, 104],       // 벽(크림 · 기둥) 3칸 × 3줄
  ['wd', 448, 32, 32, 54, 8, 124],         // 꽃 상자 창 둘
  ['wd', 480, 32, 32, 54, 72, 124],
  ['roof', 88, 228, 112, 108, 0, 0],       // 초가 모임지붕(갈색)
  ['wd', 80, 512, 32, 48, 40, 138],        // 문
  ['cot', 96, 492, 32, 20, 40, 182],       // 돌계단
];
// 소품 — r = 장식 시트 칸 [열, 행, 폭, 높이] · px = 시트 px [x, y, 폭, 높이] · tmx = 견본 장면 칸 [x0, y0, x1, y1](조립된 좌판·수레)
const PROPS = {
  well: { img: 'deco', r: [14, 13, 2, 3] }, wheelbarrow: { img: 'deco', r: [6, 16, 3, 2] }, hay_square: { img: 'deco', r: [4, 21, 3, 2] },
  hay_round: { img: 'deco', r: [0, 20, 2, 3] }, firewood: { img: 'deco', r: [13, 20, 3, 2] }, scarecrow: { img: 'deco', r: [10, 4, 1, 2] },
  barrels2: { img: 'barrel', px: [0, 0, 64, 64] }, barrels3: { img: 'barrel', px: [64, 0, 64, 64] },
  stall_grey: { tmx: [23, 27, 25, 31] }, stall_orange: { tmx: [32, 24, 33, 27] }, stall_green: { tmx: [36, 27, 38, 31] },
  cart: { tmx: [23, 23, 26, 26] }, table: { tmx: [27, 32, 28, 33] }, signpost: { tmx: [35, 33, 35, 34] },
};

// ---- 땅 칸 번호 ----
const tsx = fs.readFileSync(path.join(SRC, 'terrain-v7.tsx'), 'utf8');
const names = [...tsx.matchAll(/<terrain name="([^"]+)"/g)].map(m => m[1]);
const tileTerrain = [...tsx.matchAll(/<tile id="(\d+)" terrain="([^"]*)"/g)].map(m => [Number(m[1]), m[2]]);
const byKey = {};
for (const [id, key] of tileTerrain) (byKey[key] = byKey[key] || []).push(id);
const TN = n => { const i = names.indexOf(n); if (i < 0) fail(`지형 이름이 terrain-v7.tsx 에 없다: ${n}`); return i; };
const inShape = (s, x, y) => {
  if (s.ell) { const [cx, cy, rx, ry] = s.ell; return ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2 <= 1; }
  if (s.rect) { const [x0, y0, x1, y1] = s.rect; return x >= x0 && x <= x1 && y >= y0 && y <= y1; }
  if (s.seg) { const [x0, y0, x1, y1, r] = s.seg, dx = x1 - x0, dy = y1 - y0, t = Math.max(0, Math.min(1, ((x - x0) * dx + (y - y0) * dy) / (dx * dx + dy * dy))); return Math.hypot(x - x0 - t * dx, y - y0 - t * dy) <= r; }
  return fail('모르는 모양', JSON.stringify(s));
};
const N = LAYOUT.cells;
const fillOf = t => byKey[`${t},${t},${t},${t}`] || fail(`채움 칸이 없는 지형: ${names[t]}`);
const baseFill = fillOf(TN(LAYOUT.ground.base));
const tileLayers = [Array.from({ length: N * N }, (_, i) => baseFill[(i * 7919) % baseFill.length])];
const missing = {};
for (const L of LAYOUT.ground.layers) {
  const t = TN(L.terrain), at = (x, y) => L.shapes.some(s => inShape(s, x, y)), row = [];
  for (let y = 0; y < N; y++) for (let x = 0; x < N; x++) {
    const c = [[x, y], [x + 1, y], [x, y + 1], [x + 1, y + 1]].map(([i, j]) => at(i * U, j * U));   // 칸의 네 모서리(왼위·오위·왼아래·오아래)
    if (!c.some(Boolean)) { row.push(-1); continue; }
    const key = c.map(b => (b ? t : '')).join(','), ids = byKey[key];
    if (!ids) { const k = `${L.terrain} [${key}]`; missing[k] = (missing[k] || 0) + 1; row.push(-1); continue; }
    row.push(ids[(x * 31 + y * 17) % ids.length]);
  }
  tileLayers.push(row);
}
if (Object.keys(missing).length) fail('없는 모서리 조합 — 그 칸이 비어 그려진다. 모양을 바꿀 것:', JSON.stringify(missing));
const used = [...new Set(tileLayers.flat().filter(v => v >= 0))].sort((a, b) => a - b);
const atlasIndex = new Map(used.map((t, i) => [t, i]));
const ATLAS_COLS = 16;

// ---- 견본 장면(deco-med.tmx) — 조립된 좌판·수레 칸 ----
const tmx = fs.readFileSync(path.join(SRC, 'deco-med.tmx'), 'utf8');
const tmxW = Number((tmx.match(/<map [^>]*?\swidth="(\d+)"/) || fail('tmx 폭을 못 읽었다'))[1]);   // \s — tilewidth 를 폭으로 잡지 않게
const gidOf = name => Number((tmx.match(new RegExp(`<tileset firstgid="(\\d+)" source="(?:[^"]*/)?${name}"`)) || fail(`tmx 에 ${name} 이 없다`))[1]);   // source 는 상대 경로가 붙어 있다(../fence/…)
const SHEETS_TMX = [['deco', gidOf('deco-med.tsx'), 'decorations-medieval.png'], ['fence', gidOf('fence_medieval.tsx'), 'fence_medieval.png']]
  .map(([k, first, f]) => { const [w, h] = pngSize(f); return { k, first, count: (w / 32) * (h / 32) }; });
const tmxLayers = [...tmx.matchAll(/<layer[^>]*>\s*<data encoding="base64" compression="zlib">\s*([\s\S]*?)\s*<\/data>/g)].map(m => zlib.inflateSync(Buffer.from(m[1].replace(/\s+/g, ''), 'base64')));
const tmxCells = rect => {
  const [x0, y0, x1, y1] = rect, cells = [];
  for (const buf of tmxLayers) for (let i = 0; i < buf.length / 4; i++) {
    const gid = buf.readUInt32LE(i * 4) & 0x1fffffff, x = i % tmxW, y = Math.floor(i / tmxW);
    if (!gid || x < x0 || x > x1 || y < y0 || y > y1) continue;
    const s = SHEETS_TMX.find(s => gid >= s.first && gid < s.first + s.count);
    if (s) cells.push([x - x0, y - y0, s.k, gid - s.first]);
  }
  return cells;
};
const propDefs = Object.fromEntries(Object.entries(PROPS).map(([k, v]) => [k, v.tmx ? { tmx: v.tmx, cells: tmxCells(v.tmx) } : v]));
for (const [k, v] of Object.entries(propDefs)) if (v.tmx && !v.cells.length) fail(`견본 장면에서 ${k} 칸을 못 찾았다`);

// ---- 헤드리스 크롬에서 그림 작업 ----
const famTiles = tileTerrain.filter(([, key]) => key.split(',').some(v => PREVIEW.groundTerrains.map(TN).map(String).includes(v))).map(([id]) => id);
const IMGS = {
  prev: dataUri('preview_280.png'), terr: dataUri('terrain-v7.png'), cot: dataUri('cottage.png'), roof: dataUri('thatched-roof.png'),
  wd: dataUri('windows-doors.png'), deco: dataUri('decorations-medieval.png'), fence: dataUri('fence_medieval.png'), barrel: dataUri('barrel.png'),
};
const PAGE = `<!doctype html><body><pre id=o>wait</pre><script>
const IMGS=${JSON.stringify(IMGS)}, PREVIEW=${JSON.stringify(PREVIEW)}, FAM=${JSON.stringify(famTiles)}, USED=${JSON.stringify(used)}, ATLAS_COLS=${ATLAS_COLS},
      COTTAGE=${JSON.stringify(COTTAGE_SMALL)}, PROPS=${JSON.stringify(propDefs)};
const IM={}; let n=0; const ks=Object.keys(IMGS);
for(const k of ks){ const im=new Image(); im.onload=()=>{ if(++n===ks.length) go(); }; im.onerror=()=>{ document.getElementById('o').textContent='ERR load '+k; }; im.src=IMGS[k]; IM[k]=im; }
const canvas=(w,h)=>{ const c=document.createElement('canvas'); c.width=w; c.height=h; const x=c.getContext('2d'); x.imageSmoothingEnabled=false; return [c,x]; };
const pixels=im=>{ const [c,x]=canvas(im.width,im.height); x.drawImage(im,0,0); return {w:im.width,h:im.height,d:x.getImageData(0,0,im.width,im.height).data}; };
// 알파 테두리로 자르고, 발밑 줄(불투명 픽셀이 있는 맨 아래 줄)을 잰다
function trim(c){ const x=c.getContext('2d'), W=c.width, H=c.height, d=x.getImageData(0,0,W,H).data; let x0=W,y0=H,x1=-1,y1=-1,foot=-1;
  for(let y=0;y<H;y++) for(let i=0;i<W;i++){ const a=d[(y*W+i)*4+3]; if(a>16){ if(i<x0)x0=i; if(i>x1)x1=i; if(y<y0)y0=y; if(y>y1)y1=y; } if(a===255 && y>foot) foot=y; }
  const w=x1-x0+1, h=y1-y0+1, [o,ox]=canvas(w,h); ox.drawImage(c,x0,y0,w,h,0,0,w,h); return {png:o.toDataURL('image/png'), w, h, foot:foot-y0}; }
function go(){
  const out={sprites:{}, report:{}};
  // 땅 아틀라스
  { const rows=Math.ceil(USED.length/ATLAS_COLS), [c,x]=canvas(ATLAS_COLS*32, rows*32);
    USED.forEach((t,i)=>x.drawImage(IM.terr,(t%32)*32,Math.floor(t/32)*32,32,32,(i%ATLAS_COLS)*32,Math.floor(i/ATLAS_COLS)*32,32,32));
    out.atlas=c.toDataURL('image/png'); }
  // 집 오리기
  const P=pixels(IM.prev), T=pixels(IM.terr), OX=PREVIEW.ox, OY=PREVIEW.oy;
  const cols=Array.from({length:1024},()=>[]);
  for(const t of FAM){ const tx=(t%32)*32, ty=Math.floor(t/32)*32;
    for(let y=0;y<32;y++) for(let x=0;x<32;x++){ const i=((ty+y)*T.w+tx+x)*4; if(T.d[i+3]===255){ const v=(T.d[i]<<16)|(T.d[i+1]<<8)|T.d[i+2], L=cols[y*32+x]; if(L.indexOf(v)<0) L.push(v); } } }
  const pos=(x,y)=>((((y-OY)%32)+32)%32)*32+((((x-OX)%32)+32)%32);
  const cls=(x,y)=>{ const i=(y*P.w+x)*4; if(P.d[i+3]<255) return [0,0]; const r=P.d[i],g=P.d[i+1],b=P.d[i+2], v=(r<<16)|(g<<8)|b, L=cols[pos(x,y)];
    if(L.indexOf(v)>=0) return [1,0];
    let best=-1; for(const c of L){ const R=c>>16,G=(c>>8)&255,B=c&255; if(R<24||G<24||B<24) continue; const k1=r/R,k2=g/G,k3=b/B, lo=Math.min(k1,k2,k3), hi=Math.max(k1,k2,k3); if(lo>=0.35&&hi<=0.95&&hi-lo<=0.08){ const k=(k1+k2+k3)/3; if(k>best) best=k; } }
    return best>0 ? [2,best] : [3,0]; };
  { const [x0,y0,x1,y1]=PREVIEW.groundCheck; let g=0,t=0; for(let y=y0;y<y1;y++) for(let x=x0;x<x1;x++){ t++; if(cls(x,y)[0]===1) g++; } out.report.groundMatch=g/t; }
  out.report.houses=[];
  for(const R of PREVIEW.regions){
    const [cx0,cy0,cx1,cy1]=R.cells, X0=Math.max(0,OX+cx0*32), Y0=Math.max(0,OY+cy0*32), X1=Math.min(P.w,OX+(cx1+1)*32), Y1=Math.min(P.h,OY+(cy1+1)*32), W=X1-X0, H=Y1-Y0;
    const K=new Uint8Array(W*H), S=new Float32Array(W*H);
    for(let y=0;y<H;y++) for(let x=0;x<W;x++){ const [k,s]=cls(X0+x,Y0+y); K[y*W+x]=k; S[y*W+x]=s; }
    const outside=new Uint8Array(W*H), st=[];                      // 테두리에서 땅·그림자로만 이어 들어간 곳 = 바깥
    for(let x=0;x<W;x++) st.push(x,(H-1)*W+x); for(let y=0;y<H;y++) st.push(y*W,y*W+W-1);
    while(st.length){ const i=st.pop(); if(outside[i]||K[i]===3) continue; outside[i]=1; const x=i%W, y=(i/W)|0;
      if(x>0) st.push(i-1); if(x<W-1) st.push(i+1); if(y>0) st.push(i-W); if(y<H-1) st.push(i+W); }
    const D=new Uint8ClampedArray(W*H*4);                            // 집 = 바깥이 아닌 것 · 바깥의 그림자 = 반투명 검정
    for(let i=0;i<W*H;i++){ const j=((Y0+((i/W)|0))*P.w+X0+i%W)*4, o=i*4;
      if(!outside[i]||K[i]===3){ D[o]=P.d[j]; D[o+1]=P.d[j+1]; D[o+2]=P.d[j+2]; D[o+3]=255; }
      else if(K[i]===2){ D[o+3]=Math.round((1-S[i])*255); } }
    // 한 채씩 — 2px 깎아 덩어리를 나누고(얇은 땅 경계선이 끊긴다) 원래 픽셀은 3px 안의 덩어리에 붙인다
    const M=i=>D[i*4+3]===255, E=new Uint8Array(W*H);
    for(let y=2;y<H-2;y++) for(let x=2;x<W-2;x++){ let ok=true; for(let dy=-2;dy<=2&&ok;dy++) for(let dx=-2;dx<=2;dx++) if(!M((y+dy)*W+x+dx)){ ok=false; break; } if(ok) E[y*W+x]=1; }
    const el=new Int32Array(W*H).fill(-1), ecount=[];
    for(let s=0;s<W*H;s++){ if(!E[s]||el[s]>=0) continue; const q=[s]; el[s]=ecount.length; let cnt=0;
      while(q.length){ const i=q.pop(); cnt++; const x=i%W, y=(i/W)|0; for(let dy=-1;dy<=1;dy++) for(let dx=-1;dx<=1;dx++){ const X=x+dx,Y=y+dy; if(X<0||Y<0||X>=W||Y>=H) continue; const j=Y*W+X; if(E[j]&&el[j]<0){ el[j]=ecount.length; q.push(j); } } }
      ecount.push(cnt); }
    const lab=new Int32Array(W*H).fill(-1), comps=ecount.map(()=>[]);
    for(let i=0;i<W*H;i++){ if(!M(i)) continue; const x=i%W, y=(i/W)|0; let L=-1;
      for(let r=0;r<=3&&L<0;r++) for(let dy=-r;dy<=r&&L<0;dy++) for(let dx=-r;dx<=r;dx++){ const X=x+dx,Y=y+dy; if(X<0||Y<0||X>=W||Y>=H) continue; const l=el[Y*W+X]; if(l>=0 && ecount[l]>=2500){ L=l; break; } }
      if(L>=0){ lab[i]=L; comps[L].push(i); } }
    // 테두리가 6px 안으로 닿는 덩어리는 한 채로(처마 그림자 줄에서 지붕·벽이 끊긴 것을 다시 잇는다)
    const bb=comps.map(p=>{ let a=W,b=H,c=-1,e=-1; for(const i of p){ const x=i%W,y=(i/W)|0; if(x<a)a=x; if(x>c)c=x; if(y<b)b=y; if(y>e)e=y; } return [a,b,c,e]; });
    const par=comps.map((_,i)=>i), fd=i=>par[i]===i?i:(par[i]=fd(par[i]));
    for(let i=0;i<comps.length;i++) for(let j=i+1;j<comps.length;j++){ if(!comps[i].length||!comps[j].length) continue; const A=bb[i],B=bb[j];
      if(A[0]-6<=B[2] && B[0]-6<=A[2] && A[1]-6<=B[3] && B[1]-6<=A[3]) par[fd(j)]=fd(i); }
    for(let i=0;i<comps.length;i++){ const r=fd(i); if(r!==i){ for(const p of comps[i]){ lab[p]=r; comps[r].push(p); } comps[i]=[]; } }
    const houses=comps.map((p,ci)=>({p,ci})).filter(h=>h.p.length>=4000).sort((a,b)=>b.p.length-a.p.length);
    if(houses.length!==R.names.length){ out.report.error='집 수가 다르다 '+JSON.stringify({cells:R.cells, want:R.names.length, got:houses.length}); break; }
    houses.forEach(({p,ci},hi)=>{
      let x0=W,y0=H,x1=-1,y1=-1; for(const i of p){ const x=i%W,y=(i/W)|0; if(x<x0)x0=x; if(x>x1)x1=x; if(y<y0)y0=y; if(y>y1)y1=y; }
      const bx0=Math.max(0,x0-8), by0=Math.max(0,y0-8), bx1=Math.min(W-1,x1+8), by1=Math.min(H-1,y1+8), w=bx1-bx0+1, h=by1-by0+1;
      const [c,g]=canvas(w,h), id=g.createImageData(w,h), d=id.data;
      for(let y=0;y<h;y++) for(let x=0;x<w;x++){ const i=(by0+y)*W+bx0+x, o=(y*w+x)*4, a=D[i*4+3]; if(!a) continue; if(a===255 && lab[i]!==ci) continue;
        d[o]=D[i*4]; d[o+1]=D[i*4+1]; d[o+2]=D[i*4+2]; d[o+3]=a; }
      // 풀 부스러기 — 투명·그림자 자리에서 시작해 초록 픽셀로만 이어 들어가며 지운다(벽 안 꽃 상자 잎은 바깥과 안 이어져 남는다)
      const green=i=>d[i*4+3]===255 && d[i*4+1]>d[i*4]+20 && d[i*4+1]>d[i*4+2]+10, q=[], seen=new Uint8Array(w*h); let nGreen=0;
      for(let i=0;i<w*h;i++) if(d[i*4+3]<255){ seen[i]=1; q.push(i); }
      while(q.length){ const i=q.pop(), x=i%w, y=(i/w)|0; for(let dy=-1;dy<=1;dy++) for(let dx=-1;dx<=1;dx++){ const X=x+dx,Y=y+dy; if(X<0||Y<0||X>=w||Y>=h) continue; const j=Y*w+X; if(seen[j]||!green(j)) continue; seen[j]=1; d[j*4+3]=0; nGreen++; q.push(j); } }
      g.putImageData(id,0,0);
      const name=R.names[hi]; out.sprites[name]={png:c.toDataURL('image/png'), w, h, foot:y1-by0};
      out.report.houses.push({name, w, h, px:p.length, green:nGreen});
    });
  }
  // 작은 초가
  { const [c,x]=canvas(120,224); for(const [k,sx,sy,sw,sh,dx,dy] of COTTAGE) x.drawImage(IM[k],sx,sy,sw,sh,dx,dy,sw,sh); out.sprites.cottage_small=trim(c); }
  // 소품
  for(const [name,s] of Object.entries(PROPS)){ let c,x;
    if(s.tmx){ const [x0,y0,x1,y1]=s.tmx; [c,x]=canvas((x1-x0+1)*32,(y1-y0+1)*32); for(const [cx,cy,k,t] of s.cells){ const im=IM[k], n=im.width/32; x.drawImage(im,(t%n)*32,Math.floor(t/n)*32,32,32,cx*32,cy*32,32,32); } }
    else { const p=s.px || [s.r[0]*32,s.r[1]*32,s.r[2]*32,s.r[3]*32]; [c,x]=canvas(p[2],p[3]); x.drawImage(IM[s.img],p[0],p[1],p[2],p[3],0,0,p[2],p[3]); }
    out.sprites[name]=trim(c); }
  document.getElementById('o').textContent=JSON.stringify(out);
}
</script></body>`;
const tmpHtml = path.join(require('os').tmpdir(), `town_build_${process.pid}.html`);
const prof = path.join(require('os').tmpdir(), `town_build_prof_${process.pid}`);
fs.writeFileSync(tmpHtml, PAGE);
let dom = '';
try {
  dom = execFileSync(CHROME, ['--headless=new', `--user-data-dir=${prof}`, '--no-first-run', '--virtual-time-budget=300000', '--dump-dom', 'file:///' + tmpHtml.split(path.sep).join('/')],
    { maxBuffer: 1 << 28, timeout: 300000, stdio: ['ignore', 'pipe', 'ignore'] }).toString();
} finally { try { fs.unlinkSync(tmpHtml); } catch (e) {} try { fs.rmSync(prof, { recursive: true, force: true }); } catch (e) {} }
const m = dom.match(/<pre id="o">([\s\S]*?)<\/pre>/);
if (!m || m[1] === 'wait' || m[1].startsWith('ERR')) fail('크롬 그림 작업 결과가 없다', m ? m[1].slice(0, 200) : dom.slice(0, 200));
const res = JSON.parse(m[1].replace(/&quot;/g, '"').replace(/&amp;/g, '&').replace(/&lt;/g, '<').replace(/&gt;/g, '>'));
if (res.report.error) fail(res.report.error);
if (!(res.report.groundMatch >= 0.9)) fail(`미리보기 땅 격자가 안 맞는다 — 일치율 ${res.report.groundMatch}`);

// ---- 배치 검사 · 쓰기 ----
for (const [name] of LAYOUT.objects) if (!res.sprites[name]) fail(`town0.js 의 그림 이름이 없다: ${name}`);
const strip = s => s.replace(/^data:image\/png;base64,/, '');
const sprites = Object.fromEntries(Object.entries(res.sprites).map(([k, v]) => [k, { png: strip(v.png), w: v.w, h: v.h, foot: v.foot }]));
const [pw, ph] = pngSize('teleporter-circle_4.png');
const TOWN = {
  tile: U,
  maps: { [LAYOUT.mapId]: { cells: N, atlasCols: ATLAS_COLS, layers: tileLayers.map(row => row.map(v => (v < 0 ? -1 : atlasIndex.get(v)))), objects: LAYOUT.objects, trees: LAYOUT.trees } },
  atlas: strip(res.atlas),
  sprites,
  portal: { png: png('teleporter-circle_4.png').toString('base64'), fw: pw, fh: 128, frames: ph / 128 },
};
const header = '// 자동 생성 — tools/town/build.js 가 만든다. 손으로 고치지 말 것(배치 = tools/town/town0.js · 그림 출처 = assets/town_credits.txt)\n';
fs.writeFileSync(path.join(ASSETS, 'town.js'), header + 'window.TOWN=' + JSON.stringify(TOWN) + ';\n');
// 크레딧 모음 — src/CREDITS-*.txt 를 그대로 잇는다
const credits = fs.readdirSync(SRC).filter(f => /^CREDITS-.*\.txt$/.test(f)).sort()
  .map(f => `==== ${f} ====\n` + fs.readFileSync(path.join(SRC, f), 'utf8').replace(/\r\n/g, '\n').trim() + '\n').join('\n');
fs.writeFileSync(path.join(ASSETS, 'town_credits.txt'), '마을 그림(assets/town.js) 크레딧 — tools/town/src/CREDITS-*.txt 모음(build.js 가 만든다)\n\n' + credits);
const kb = f => Math.round(fs.statSync(path.join(ASSETS, f)).size / 1024);
console.log(`땅 ${N}×${N}칸 · 층 ${tileLayers.length} · 없는 모서리 조합 0 · 쓰는 땅 칸 ${used.length}`);
console.log(`미리보기 땅 일치율 ${(res.report.groundMatch * 100).toFixed(1)}%`);
for (const h of res.report.houses) console.log(`집 ${h.name} ${h.w}×${h.h} · 픽셀 ${h.px} · 지운 풀 ${h.green}`);
console.log(`그림 ${Object.keys(sprites).length}개(${Object.keys(sprites).join(' ')}) · 물건 ${LAYOUT.objects.length} · 나무 ${LAYOUT.trees.length}`);
console.log(`assets/town.js ${kb('town.js')}KB · assets/town_credits.txt ${kb('town_credits.txt')}KB`);
