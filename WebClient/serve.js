// 웹 클라이언트용 최소 정적 서버.
//   file:// 로 열면 브라우저가 로컬 파일 접근을 막아 자산·fetch 가 실패하고, 그 여파로
//   boot() 가 connect() 까지 못 가서 서버에 아예 안 붙는다. http 로 띄우면 그 문제가 없다.
// 사용: node serve.js [--port 8200]
const http = require('http');
const fs = require('fs');
const path = require('path');

function arg(name, def){ const i = process.argv.indexOf('--' + name); return i >= 0 ? process.argv[i + 1] : def; }
const PORT = parseInt(arg('port', '8200'), 10);
const ROOT = __dirname;

const MIME = {
  '.html':'text/html; charset=utf-8', '.js':'text/javascript; charset=utf-8',
  '.json':'application/json; charset=utf-8', '.css':'text/css; charset=utf-8',
  '.png':'image/png', '.jpg':'image/jpeg', '.jpeg':'image/jpeg', '.gif':'image/gif',
  '.svg':'image/svg+xml', '.ico':'image/x-icon', '.csv':'text/csv; charset=utf-8',
};

// 기획 데이터 — 웹 클라가 아이템 이름·그림 번호를 서버와 "같은 파일"에서 읽는다(두 벌로 적으면 어긋난다).
//   아래 루트 밖 차단 규칙은 그대로 두고, 이 표에 이름으로 적은 파일만 연다(디렉토리를 통째로 열지 않는다).
//   Map 인 이유 — 평범한 객체면 요청 경로가 원형(prototype)의 속성 이름과 겹칠 때 엉뚱한 값이 걸린다.
const DATA = new Map([
  ['/data/ItemTable.csv', path.join(ROOT, '..', 'Run', 'bin', 'ItemTable.csv')],
  ['/data/SkillTable.csv', path.join(ROOT, '..', 'Run', 'bin', 'SkillTable.csv')],
]);

http.createServer((req, res) => {
  let rel = decodeURIComponent(req.url.split('?')[0]);
  if (rel === '/') rel = '/live.html';
  const data = DATA.get(rel);
  const file = data || path.join(ROOT, rel);

  // 루트 밖으로 새는 경로 차단 (DATA 에 이름으로 적은 파일만 예외)
  if (!data && !file.startsWith(ROOT)) { res.writeHead(403); res.end('forbidden'); return; }

  fs.readFile(file, (err, buf) => {
    if (err) { res.writeHead(404); res.end('not found: ' + rel); return; }
    res.writeHead(200, {
      'Content-Type': MIME[path.extname(file).toLowerCase()] || 'application/octet-stream',
      'Cache-Control': 'no-store',          // 고친 파일이 옛 판으로 열리는 것을 막는다
    });
    res.end(buf);
  });
}).listen(PORT, '127.0.0.1', () => {
  console.log(`[serve] http://127.0.0.1:${PORT}/live.html  (root: ${ROOT})`);
});
