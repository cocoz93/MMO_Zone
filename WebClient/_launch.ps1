<#
  MMO 웹 클라이언트 데모 원클릭 실행
  서버(MMOServer) + 더미(MMOStressClient) + WS릴레이(node) 를 띄우고 브라우저 클라를 엽니다.
  사용: 우클릭 → "PowerShell로 실행" 또는  run-all.bat(전체) 또는 run-webclient.bat(웹클라) 더블클릭
  옵션:  -Dummies 60   (더미 수, 기본 60)   -SkipServer   -SkipDummies
#>
param([int]$Dummies = 60, [switch]$SkipServer, [switch]$SkipDummies)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$repo = Split-Path $here -Parent
$bin  = Join-Path $repo 'Run\bin'

function Listening($port){ (netstat -an | Select-String "LISTENING" | Select-String ":$port ") -ne $null }
# 해당 포트를 리슨 중인 프로세스 PID (재사용 시 .relay.pid 갱신용)
function ListenPid($port){
  $m = netstat -ano | Select-String "LISTENING" | Select-String ":$port " | Select-Object -First 1
  if($m){ ($m.Line.Trim() -split '\s+')[-1] } else { $null }
}

# 0) node / ws 준비
if(-not (Get-Command node -ErrorAction SilentlyContinue)){ Write-Host "node가 필요합니다 (https://nodejs.org)"; exit 1 }
if(-not (Test-Path (Join-Path $here 'node_modules\ws'))){
  Write-Host "[1/5] ws 설치 중 (최초 1회)..."; Push-Location $here; npm install --silent; Pop-Location
}

# 1) 서버
if(-not $SkipServer){
  if(Listening 6000){ Write-Host "[2/5] 서버 이미 실행 중 (포트 6000)" }
  else{
    Write-Host "[2/5] 서버 기동..."; Start-Process -FilePath (Join-Path $bin 'MMOServer.exe') -WorkingDirectory $bin -WindowStyle Normal
    for($i=0;$i -lt 15 -and -not (Listening 6000);$i++){ Start-Sleep -Milliseconds 500 }
    if(Listening 6000){ Write-Host "     서버 리슨 OK" } else { Write-Host "     서버가 6000을 안 엽니다. Run\bin\logs 확인 (MySQL 필요할 수 있음)"; }
  }
}

# 2) 더미 (스트레스 설정을 잠깐 낮춰 실행 후 원복 — 이미 실행 중인 프로세스엔 영향 없음)
if(-not $SkipDummies){
  Write-Host "[3/5] 더미 $Dummies 개 기동..."
  $cfg = Join-Path $bin 'MMOStressConfig.ini'
  $enc = [Text.Encoding]::GetEncoding(949)
  $orig = [IO.File]::ReadAllText($cfg,$enc)
  try{
    [IO.File]::WriteAllText($cfg, ($orig -replace 'ClientCount=\d+', "ClientCount=$Dummies"), $enc)
    Start-Process -FilePath (Join-Path $bin 'MMOStressClient.exe') -WorkingDirectory $bin -WindowStyle Normal
    Start-Sleep -Seconds 2
  } finally {
    [IO.File]::WriteAllText($cfg, $orig, $enc)   # 원래 스트레스 설정 그대로 복원
  }
}

# 3) 릴레이 (WS 9000 -> TCP 6000), PID 기록 — 이미 떠 있으면 건너뜀(중복 방지)
if(Listening 9000){
  Write-Host "[4/5] WS 릴레이 이미 실행 중 (9000) — 재사용"
  # 옛 PID 가 남으면 stop.bat 이 엉뚱한 프로세스를 죽인다 → 실제 리슨 PID 로 갱신
  $rpid = ListenPid 9000
  if($rpid){ $rpid | Out-File (Join-Path $here '.relay.pid') -Encoding ascii }
} else {
  Write-Host "[4/5] WS 릴레이 기동 (ws://127.0.0.1:9000 -> 127.0.0.1:6000)..."
  $relay = Start-Process -FilePath 'node' -ArgumentList @((Join-Path $here 'relay.js'),'--ws','9000','--server-port','6000') -WorkingDirectory $here -WindowStyle Normal -PassThru
  $relay.Id | Out-File (Join-Path $here '.relay.pid') -Encoding ascii
  Start-Sleep -Milliseconds 800
}

# 4) 브라우저 클라
Write-Host "[5/5] 브라우저 클라 열기..."
Start-Process (Join-Path $here 'live.html')

Write-Host ""
Write-Host "준비 완료. 브라우저에서 캐릭터들이 움직이고 채팅이 흐르면 성공입니다."
Write-Host "방향키로 내 캐릭터를 실제 서버에서 이동할 수 있습니다."
Write-Host "종료하려면:  stop.bat"
