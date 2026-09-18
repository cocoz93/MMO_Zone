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
  '.svg':'image/svg+xml', '.ico':'image/x-icon',
};

http.createServer((req, res) => {
  let rel = decodeURIComponent(req.url.split('?')[0]);
  if (rel === '/') rel = '/live.html';
  const file = path.join(ROOT, rel);

  // 루트 밖으로 새는 경로 차단
  if (!file.startsWith(ROOT)) { res.writeHead(403); res.end('forbidden'); return; }

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
