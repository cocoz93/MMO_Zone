# Rebuild figure PNGs from the HTML sources (PowerShell). ASCII only - no Korean literals in this file.
# Usage:  powershell -ExecutionPolicy Bypass -File build.ps1
# <name>.html is rendered at logical size x DSF3 -> ..\<name>.png
$dir = $PSScriptRoot

$chrome = "C:\Program Files\Google\Chrome\Application\chrome.exe"
if (-not (Test-Path $chrome)) { $chrome = "C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe" }
if (-not (Test-Path $chrome)) { throw "Chrome/Edge not found" }

$targets = @(
  @{ n = "01_sector_vs_circle"; w = 840; h = 598 },
  @{ n = "02_tick_three_steps"; w = 840; h = 520 },
  @{ n = "03_same_cell_two_circles"; w = 840; h = 560 }
)

# Render inside an ASCII temp dir (the source path has spaces + Korean, which splits Start-Process args).
# A fresh per-run profile keeps headless Chrome from attaching to a running Chrome (screenshot no-op).
$render     = Join-Path $env:TEMP "ig-render-aoi"
$profileDir = Join-Path $env:TEMP ("ig-prof-" + [Guid]::NewGuid().ToString('N').Substring(0,8))
if (Test-Path $render) { Remove-Item $render -Recurse -Force }
New-Item -ItemType Directory -Path $render -Force | Out-Null
Copy-Item (Join-Path $dir "*.html") $render -Force -ErrorAction SilentlyContinue
Copy-Item (Join-Path $dir "fonts")  $render -Recurse -Force

Write-Host "[build] using: $chrome"
foreach ($t in $targets) {
  $html = Join-Path $render ($t.n + ".html")
  if (-not (Test-Path $html)) { Write-Host "  skip   $($t.n) (no html)"; continue }
  $png = Join-Path $render ($t.n + ".png")
  if (Test-Path $png) { Remove-Item $png -Force }
  $url = "file:///" + ($render -replace '\\','/') + "/" + $t.n + ".html"
  $chromeArgs = @(
    '--headless=new','--disable-gpu','--hide-scrollbars','--allow-file-access-from-files',
    "--user-data-dir=$profileDir",'--force-device-scale-factor=3',
    "--screenshot=$png","--window-size=$($t.w),$($t.h)",$url
  )
  Start-Process -FilePath $chrome -ArgumentList $chromeArgs -NoNewWindow -Wait
  if (-not (Test-Path $png)) { throw "render failed: $($t.n)" }
  Copy-Item $png (Join-Path $dir ("..\" + $t.n + ".png")) -Force
  Write-Host "  built  ..\$($t.n).png  (logical $($t.w)x$($t.h), 3x)"
}
Remove-Item $render     -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item $profileDir -Recurse -Force -ErrorAction SilentlyContinue
Write-Host "[build] done."
