#!/usr/bin/env bash
# 사용법: bash render.sh 01_summary [02_xxx ...]   (인자 없으면 _src의 모든 *.html)
# _src/<name>.html → 상위 폴더 <name>.png (Chrome 헤드리스, 1200x700 논리 → 3x = 3600x2100)
set -e
CHROME="/c/Program Files/Google/Chrome/Application/chrome.exe"
SRCDIR="$(cd "$(dirname "$0")" && pwd)"
OUTDIR="$(dirname "$SRCDIR")"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP"

names=("$@")
if [ ${#names[@]} -eq 0 ]; then
  for f in "$SRCDIR"/*.html; do names+=("$(basename "$f" .html)"); done
fi

for n in "${names[@]}"; do
  n="${n%.html}"
  URL="file:///$(cygpath -m "$SRCDIR/$n.html" | sed 's/ /%20/g')"
  "$CHROME" --headless --disable-gpu --allow-file-access-from-files --force-device-scale-factor=3 --hide-scrollbars \
    --default-background-color=00000000 --window-size=1200,700 \
    --screenshot="$TMP/$n.png" "$URL" >/dev/null 2>&1
  cp -f "$TMP/$n.png" "$OUTDIR/$n.png"
  echo "OK  $n.png  ($(stat -c%s "$OUTDIR/$n.png") bytes)"
done
