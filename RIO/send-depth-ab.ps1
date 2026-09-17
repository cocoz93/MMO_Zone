<#
  send-depth-ab.ps1  -  무인 송신 깊이(SendDepth) A/B: 전송 팔 {IOCP,RIO} x 깊이 D{1,2,4} x CC{4000,4800}

  질문: 세션당 동시 송신 제출 상한(SendDepth)을 1(현행 1-pending)에서 2/4로 올리면
  "완료를 받고 나서야 다음 제출"이 만드는 왕복 공백을 깊이가 흡수하는가? 값은 어디에 나타나는가?
    send_followup_rate   (kpi, 낮을수록 좋음)  완료 후 이어붙인 후속 제출 -- 깊이가 흡수하면 줄어든다 (핵심)
    bytes_per_send       (kpi, 높을수록 좋음)  제출 한 번의 굵기
    send_wrap_split_rate (control)             RIO 팔에서 링 랩 경계로 제출이 쪼개진 빈도
    send_contention_rate / worker_cpu / net_cpu / tick_p99 = 종합 비용

  아암이 2차원이다:
    전송 팔   = IOCP / RIO  -> 재빌드 (build-A-iocp.bat / build-B-rio.bat, USE_RIO_TRANSPORT 토글)
    송신 깊이 = 1 / 2 / 4   -> INI [Server] SendDepth 한 줄 (같은 바이너리 = 빌드 차이가 변인으로 안 섞임)
  팔이 바깥 루프라 재빌드는 팔당 1회. 빌드 실패 또는 스테일 바이너리(과거 LNK1104 exe 잠김으로
  exit!=0인데 옛 바이너리로 측정이 돈 사고 재발 방지 -- exit 0이어도 Run\bin\MMOServer.exe의
  LastWriteTime이 빌드 시작 이후로 갱신 안 됐으면 실패 취급)면 그 팔 전체를 건너뛰고 기록만 남긴다.

  아암 실적용 assert (같은 바이너리로 깊이를 바꾸는 구조의 유일한 방어선):
    (a) 서버 로그 Run\bin\logs\<yyMMdd>_MMOServer.log 의 "Send depth = N" 라인 + 팔 문구
        ("RIO workers=" 있으면 RIO / "worker threads=" 있으면 IOCP -- echo-smoke.ps1과 동일 판정).
        로거가 프로세스 종료 시 버퍼를 플러시하므로 CTRL_C 정상 종료 후에 읽는다 (실측 근거: echo-smoke).
    (b) 게이지 교차확인: mmo_send_depth / mmo_transport_rio (metrics CSV의 send_depth / transport_rio).
    불일치 런은 FAIL로 기록하고 계속 진행하며, 요약 평균에서 그 런을 제외한다
    (gqcs-ab는 즉시 중단이었으나 여기선 런 단위 격리 -- 남은 구성의 데이터는 살린다).

  게이트 = buffer_full==0 & tick_p99<40 & session>=CC*0.98 (gqcs-ab와 동일 기준).
  부하 비대칭 |pps%|>3 이면 net_cpu_per_kpps 정규화 지표를 인용하라고 경고한다.

  출력: Monitoring\metrics_out\window_metrics.csv (RunLabel = SD_<팔>_D<깊이>_CC<동접>_r<rep>)
        + 콘솔/트랜스크립트 요약 (팔xCC별로 D1 기준 깊이 효과)

  사용:
    .\send-depth-ab.ps1                    # 풀: 2팔 x D{1,2,4} x CC{4000,4800} x 2회 = 24런, 약 5시간
    .\send-depth-ab.ps1 -Smoke             # 파이프라인 점검: 2팔 x D{1,4} x CC2000 x1, 1분 부하, 약 20분
    .\send-depth-ab.ps1 -Transports IOCP   # 한 팔만 (재빌드 1회)
    .\send-depth-ab.ps1 -Depths 1,8        # 깊이 커스텀 (2의 거듭제곱 1..8만)
#>
param(
  [switch] $Smoke,
  [int]    $GracefulTimeoutSec = 90,          # 정상 종료 대기. RIO 팔은 슬랩 해제·세션 드레인으로 느리다
                                              #   -- 짧으면 taskkill되어 로그가 안 남고 아암 assert가 무의미해진다
  [switch] $SkipBuild,                        # 재빌드 생략. 단일 -Transports 전용 -- 두 팔이면 한 팔은 반드시 아암 불일치 FAIL
  [int]    $Reps          = 2,
  [int]    $LoadMin       = 10,
  [int]    $WindowMin     = 5,
  [int[]]  $ClientCounts  = @(4000, 4800),
  [int[]]  $Depths        = @(1, 2, 4),       # INI [Server] SendDepth. 첫 값이 기준선(현행 1-pending)
  [ValidateSet("IOCP","RIO")]
  [string[]] $Transports  = @("IOCP","RIO"),  # 나열 순서 = 빌드 순서 (팔당 재빌드 1회)
  [int]    $MaxClients    = 6000,             # 고정 -- RIO 슬랩 = MaxClients x 128KB = 750MB
  [int]    $WorkerThreads = 4,                # IOCP 팔 WT (RIO 팔은 무시)
  [int]    $SendWorkers   = 3,                # IOCP 팔 K  (RIO 팔은 무시)
  [int]    $RioWorkers    = 4,                # RIO 팔 N   (IOCP 팔은 무시)
  [string] $ServerCores   = "0-5",
  [string] $LabelPrefix   = "SD"
)
$ErrorActionPreference = "Stop"
# 스모크는 라벨 접두사 분리(gqcs-ab 관례): 같은 RunLabel로 스모크/실측 행이 CSV에 섞이는 것 방지
if ($Smoke) { $Reps=1; $LoadMin=1; $WindowMin=1; $ClientCounts=@(2000); $Depths=@(1,4); $LabelPrefix="SDSMK" }

# 깊이 검증: 서버(IOCPServer.cpp)가 2의 거듭제곱 아니면 조용히 내림(3->2)하므로 여기서 막지 않으면
# 라벨(D3)과 실측(2)이 어긋난 채 돈다. 상한은 CSession::MAX_SEND_DEPTH=8 (IOCPServer.h).
foreach($d in $Depths){
  if($d -lt 1 -or $d -gt 8 -or (($d -band ($d-1)) -ne 0)){ throw "invalid -Depths $d : 2^n only (1/2/4/8)" }
}

$RioDir = $PSScriptRoot
$Root   = Split-Path $RioDir -Parent
$Bin    = Join-Path $Root "Run\bin"
$Mon    = Join-Path $Root "Monitoring"
$SrvIni = Join-Path $Bin "MMOServerConfig.ini"
$CliIni = Join-Path $Bin "MMOStressConfig.ini"
$BCfg   = Join-Path $Root "ServerCore\Base\CoreConfig.h"
$OutDir = Join-Path $Mon "metrics_out"
$Csv    = Join-Path $OutDir "window_metrics.csv"
$Exe    = Join-Path $Bin "MMOServer.exe"
$Enc949 = [Text.Encoding]::GetEncoding(949)   # 런타임 INI = CP949 무BOM. Set-Content/Out-File/UTF-8 저장 금지

# ---------- INI 편집: CP949 라인 배열 + 적용 검증 ----------
# ReadAllLines가 CRLF를 분리자로 소비해 각 라인에 \r이 없다 -> '^Key=' 매칭이 안전
# ('^Key=\d+$' 꼴의 $ 앵커는 통짜 텍스트에선 \r에 막혀 매칭 실패한다). WriteAllLines가 CRLF로 되돌린다.
function Set-Ini([string]$file,[string]$key,[string]$value){
  $lines = [IO.File]::ReadAllLines($file, $Enc949)
  $found = $false
  for($i=0; $i -lt $lines.Count; $i++){
    if($lines[$i] -match ('^' + [regex]::Escape($key) + '=')){ $lines[$i] = "$key=$value"; $found = $true }
  }
  if(-not $found){ throw "INI key not found: $key ($file)" }
  [IO.File]::WriteAllLines($file, $lines, $Enc949)
  # 치환 후 실제 값 재확인 -- 조용히 안 먹은 채 도는 런을 원천 차단
  $chk = @([IO.File]::ReadAllLines($file, $Enc949) | Where-Object { $_ -match ('^' + [regex]::Escape($key) + '=') })[0]
  if($chk -ne "$key=$value"){ throw "INI verify failed: wanted '$key=$value' got '$chk' ($file)" }
}

function ToNum([string]$s){ $d=0.0; if([double]::TryParse($s,[Globalization.NumberStyles]::Float,[Globalization.CultureInfo]::InvariantCulture,[ref]$d)){$d}else{$null} }
function Stop-Procs { foreach($n in "MMOStressClient","GameClient","MMOServer"){ try{ Stop-Process -Name $n -Force -ErrorAction Stop }catch{} } }

# 서버가 로그를 쓰기 오픈 중이어도 읽기 -- FileShare.ReadWrite (echo-smoke.ps1과 동일)
function Read-LogText([string]$path){
  if(-not (Test-Path $path)){ return "" }
  $fs = [IO.File]::Open($path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
  try { $sr = New-Object IO.StreamReader($fs,[Text.Encoding]::UTF8); return $sr.ReadToEnd() } finally { $fs.Close() }
}

# ---------- CTRL_C 정상 종료 (echo-smoke.ps1 이식) ----------
# 별도 powershell 프로세스가 서버 콘솔에 붙어 이벤트를 쏜다 (내 콘솔에서 직접 쏘면 이 스크립트도 맞는다)
function Send-CtrlC([int]$targetPid) {
    $helper = @"
Add-Type -Namespace W -Name K -MemberDefinition '
[DllImport("kernel32.dll", SetLastError=true)] public static extern bool AttachConsole(uint pid);
[DllImport("kernel32.dll")] public static extern bool FreeConsole();
[DllImport("kernel32.dll")] public static extern bool SetConsoleCtrlHandler(IntPtr h, bool add);
[DllImport("kernel32.dll", SetLastError=true)] public static extern bool GenerateConsoleCtrlEvent(uint e, uint g);'
[W.K]::FreeConsole() | Out-Null
if (-not [W.K]::AttachConsole($targetPid)) { exit 2 }
[W.K]::SetConsoleCtrlHandler([IntPtr]::Zero, `$true) | Out-Null
[W.K]::GenerateConsoleCtrlEvent(0, 0) | Out-Null
exit 0
"@
    $tmp = Join-Path $env:TEMP 'sd_sendctrlc.ps1'
    [IO.File]::WriteAllText($tmp, $helper, [Text.Encoding]::UTF8)
    $hp = Start-Process powershell -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File', $tmp -PassThru -WindowStyle Hidden
    $hp.WaitForExit(5000) | Out-Null
    return $hp.ExitCode
}

function Stop-ServerGraceful([System.Diagnostics.Process]$p){
  # $true = 정상 종료(로그 플러시 신뢰 가능) / $false = 강제 종료 폴백(startup 라인이 로그에 없을 수 있음)
  if($null -eq $p -or $p.HasExited){ return $true }
  $rc = Send-CtrlC $p.Id
  if($rc -ne 0){
    Write-Warning "CTRL_C helper failed (rc=$rc) -> taskkill"
    cmd /c "taskkill /F /PID $($p.Id) >nul 2>nul" | Out-Null
    return $false
  }
  # 실제 소요를 찍는다 -- "느린 것"과 "행"을 구별해야 임계를 근거 있게 잡을 수 있다.
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $ok = $p.WaitForExit($GracefulTimeoutSec * 1000)
  $sw.Stop()
  if(-not $ok){
    Write-Warning ("graceful shutdown timeout {0}s -> taskkill" -f $GracefulTimeoutSec)
    cmd /c "taskkill /F /PID $($p.Id) >nul 2>nul" | Out-Null
    return $false
  }
  Write-Host ("    graceful shutdown in {0:N1}s" -f $sw.Elapsed.TotalSeconds)
  return $true
}

# ---------- 팔 재빌드 (rio-ab.ps1 이식 + 스테일 바이너리 검증 추가) ----------
function Rebuild([string]$arm){
  # $true = 성공(새 바이너리 확인). 실패/스테일이면 $false -- 호출부가 그 팔 전체를 건너뛰고 기록한다.
  Stop-Procs; Start-Sleep 2   # exe를 잠글 잔류 프로세스 정리 (LNK1104 재발 방지 1선)
  $bat = if($arm -eq "IOCP"){ Join-Path $RioDir "build-A-iocp.bat" } else { Join-Path $RioDir "build-B-rio.bat" }
  Write-Host ">>> REBUILD arm=$arm ($(Split-Path $bat -Leaf))" -ForegroundColor Magenta
  $t0 = (Get-Date).AddSeconds(-2)   # 파일시간 해상도 여유
  & cmd /c "`"$bat`""
  if($LASTEXITCODE -ne 0){ Write-Warning "rebuild FAILED: arm=$arm exit=$LASTEXITCODE"; return $false }
  $bi = Get-Item $Exe -ErrorAction SilentlyContinue
  if($null -eq $bi -or $bi.LastWriteTime -lt $t0){
    # 재발 방지 2선: exit 0이어도 exe가 안 새로워졌으면(링크 스킵/잠김) 옛 바이너리 측정 사고로 이어진다
    Write-Warning "rebuild exit=0 but $Exe NOT refreshed (stale binary) -> treating as build failure"
    return $false
  }
  Write-Host ("    binary: {0}  {1:yyyy-MM-dd HH:mm:ss}  {2} bytes" -f (Split-Path $Exe -Leaf), $bi.LastWriteTime, $bi.Length) -ForegroundColor DarkGray
  return $true
}

function Reset-Toggle {  # 소스 USE_RIO_TRANSPORT -> 0 (재빌드 없이 토글만 -- 쉬는 트리를 IOCP 기본으로)
  $t=[IO.File]::ReadAllText($BCfg); $t=$t -replace '#define USE_RIO_TRANSPORT \d','#define USE_RIO_TRANSPORT 0'
  [IO.File]::WriteAllText($BCfg,$t,(New-Object Text.UTF8Encoding($true)))
}

if(-not (Test-Path $OutDir)){ New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }
Start-Transcript -Path (Join-Path $OutDir ("send_depth_ab_{0}.log" -f (Get-Date -Format "yyyyMMdd_HHmmss"))) | Out-Null

# 구성: 팔(바깥) x 깊이 x CC -- 같은 팔이 연속으로 오게 정렬돼 재빌드가 팔당 1회로 끝난다
$configs = @()
foreach($tr in $Transports){ foreach($d in $Depths){ foreach($cc in $ClientCounts){ $configs += @{ tr=$tr; d=$d; cc=$cc } } } }
$total    = $configs.Count * $Reps
$estBuild = 0; if(-not $SkipBuild){ $estBuild = 3 * $Transports.Count }
$estMin   = [int]($total * ($LoadMin + 2.5) + $estBuild)   # 런당 오버헤드 ~2.5분(기동+수집+정상종료+드레인60s) + 팔당 빌드 ~3분
Write-Host ("=== SendDepth A/B : {0} configs x {1} reps = {2} runs | Load={3}m Win={4}m | est ~{5}m (~{6:N1}h) ===" -f $configs.Count,$Reps,$total,$LoadMin,$WindowMin,$estMin,($estMin/60.0)) -ForegroundColor Cyan
$configs | ForEach-Object { Write-Host ("   - {0,-4} D{1} CC{2}" -f $_.tr,$_.d,$_.cc) }

$srvBak = [IO.File]::ReadAllBytes($SrvIni)
$cliBak = [IO.File]::ReadAllBytes($CliIni)
$runRecs    = New-Object System.Collections.Generic.List[object]  # 런 단위 assert/사망 기록
$skipped    = @()    # 빌드 실패로 통째로 건너뛴 팔
$failedArms = @()
$builtArm   = ""

try {
  # ---- 0) 잔류 프로세스 정리 ----
  Stop-Procs
  foreach($n in "prometheus","windows_exporter","grafana-server"){ try{ Stop-Process -Name $n -Force -ErrorAction Stop }catch{} }

  # ---- 1) 공통 INI 고정 (팔 무관 동일 -- 루프에서 움직이는 변인은 SendDepth와 ClientCount 뿐) ----
  Set-Ini $SrvIni "Mode" "GameServer"
  Set-Ini $SrvIni "MonitorEnabled" "1"
  Set-Ini $SrvIni "ServerCores"     $ServerCores
  Set-Ini $SrvIni "WorkerThreads"   $WorkerThreads
  Set-Ini $SrvIni "SendWorkers"     $SendWorkers
  Set-Ini $SrvIni "RioWorkers"      $RioWorkers
  Set-Ini $SrvIni "CompletionBatch" "0"          # GQCS 고정 -- GQCSEx는 기각(2026-07-30), 남아있으면 혼입 변인
  Set-Ini $SrvIni "MaxClients"      $MaxClients
  Set-Ini $SrvIni "GameCore"        ""
  Set-Ini $CliIni "ServerIp"        "127.0.0.1"

  # ---- 2) 모니터링 스택 (스윕 전체에 1회) ----
  & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Mon "config\setup.ps1") -StressClientIp localhost
  if($LASTEXITCODE -ne 0){ throw "setup.ps1 (prometheus target inject) failed" }
  Start-Process -FilePath (Join-Path $Mon "windows_exporter.exe")
  Start-Process -FilePath (Join-Path $Mon "prometheus-3.4.1.windows-amd64\prometheus.exe") `
                -WorkingDirectory (Join-Path $Mon "prometheus-3.4.1.windows-amd64") -ArgumentList "--web.listen-address=:9091"
  Start-Process -FilePath (Join-Path $Mon "grafana\bin\grafana-server.exe") -WorkingDirectory (Join-Path $Mon "grafana\bin")
  Start-Sleep 5

  # ---- 3) 메인 루프: 팔 전환 시에만 재빌드, 깊이/CC는 INI 한 줄 ----
  $done = 0
  foreach($cfg in $configs){
    if($failedArms -contains $cfg.tr){ $done += $Reps; continue }
    if($cfg.tr -ne $builtArm){
      $ok = $false
      if($SkipBuild){
        Write-Host ">>> SkipBuild: current Run\bin binary is assumed arm=$($cfg.tr) (log/gauge assert will verify)" -ForegroundColor Magenta
        $ok = Test-Path $Exe
      } else { $ok = Rebuild $cfg.tr }
      if(-not $ok){
        $n = @($configs | Where-Object { $_.tr -eq $cfg.tr }).Count * $Reps
        $skipped += ("arm={0}: build failed or stale binary -> {1} runs SKIPPED" -f $cfg.tr,$n)
        Write-Warning $skipped[-1]
        $failedArms += $cfg.tr
        $done += $Reps
        continue
      }
      $builtArm = $cfg.tr
    }

    Set-Ini $SrvIni "SendDepth"        $cfg.d     # <- 깊이 축: 같은 바이너리에서 INI 한 줄
    Set-Ini $CliIni "ClientCount"      $cfg.cc
    Set-Ini $CliIni "ClientsPerThread" ([string][int][math]::Ceiling($cfg.cc/4.0))  # 클라 4스레드 유지(ClientCores 6-9)

    for($rep=1; $rep -le $Reps; $rep++){
      $done++
      $label = "{0}_{1}_D{2}_CC{3}_r{4}" -f $LabelPrefix,$cfg.tr,$cfg.d,$cfg.cc,$rep
      Write-Host ""
      Write-Host "[$done/$total] $label  ($(Get-Date -Format 'HH:mm:ss'))" -ForegroundColor Yellow
      Stop-Procs; Start-Sleep 3

      # 이번 런의 로그 구간 시작점 -- "문자" 길이 기준 (바이트 길이로 Substring하면 멀티바이트에서 어긋난다)
      $logFile  = Join-Path $Bin ("logs\" + (Get-Date -Format "yyMMdd") + "_MMOServer.log")
      $logStart = (Read-LogText $logFile).Length

      $srv = Start-Process -FilePath $Exe -WorkingDirectory $Bin -PassThru
      $ready = $false
      for($i=0;$i -lt 30;$i++){ if(Get-NetTCPConnection -State Listen -LocalPort 6000 -ErrorAction SilentlyContinue){$ready=$true;break}; Start-Sleep 1 }
      if(-not $ready){ throw "server did not listen on :6000 in 30s ($label)" }
      Start-Process -FilePath (Join-Path $Bin "MMOStressClient.exe") -WorkingDirectory $Bin

      for($m=1;$m -le $LoadMin;$m++){ Start-Sleep 60; Write-Host ("    load {0}/{1}m" -f $m,$LoadMin) }

      # ---- 수집 (서버 생존 중 -- 게이지 send_depth/transport_rio가 스크레이프에 살아있어야 함) ----
      $alive = -not $srv.HasExited
      $collected = $false
      if($alive){
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Mon "metrics-collect.ps1") `
            -RunLabel $label -WindowMin $WindowMin -QueriesFile (Join-Path $Mon "queries.json") -OutDir $OutDir
        if($LASTEXITCODE -ne 0){ Write-Warning "collect failed: $label (continue)" } else { $collected = $true }
      } else { Write-Warning "server died during load -> skip collect: $label" }

      # ---- 정상 종료 -> 로그 assert (로거가 종료 시 플러시: 살아있는 동안 읽으면 startup 라인이 없다) ----
      foreach($n in "MMOStressClient","GameClient"){ try{ Stop-Process -Name $n -Force -ErrorAction Stop }catch{} }
      $graceful = Stop-ServerGraceful $srv

      $armNote = @()
      if(-not $alive){ $armNote += "server died during load" }
      $logNow = Read-LogText $logFile
      $logWin = ""
      if($logNow.Length -gt $logStart){ $logWin = $logNow.Substring([Math]::Min($logStart,$logNow.Length)) }
      $logEmpty = ($logWin -eq "")
      if(-not $logEmpty){
        if($logWin -match 'Send depth = (\d+)'){
          if([int]$Matches[1] -ne $cfg.d){ $armNote += ("log Send depth={0} but wanted {1} (INI not applied?)" -f $Matches[1],$cfg.d) }
        } else { $armNote += "log line 'Send depth =' missing" }
        $hasRio  = $logWin -match 'RIO workers='
        $hasIocp = $logWin -match 'worker threads='
        if($cfg.tr -eq "RIO"  -and -not $hasRio ){ $armNote += "expected RIO arm but no 'RIO workers=' in log (IOCP binary?)" }
        if($cfg.tr -eq "IOCP" -and ($hasRio -or -not $hasIocp)){ $armNote += ("expected IOCP arm but log says rio={0} iocp={1}" -f $hasRio,$hasIocp) }
      }
      # 게이지 교차확인 (지표 쪽 2선 -- 강제종료 폴백으로 로그가 빈 경우에도 이 선은 남는다)
      $gaugeOk = $false
      if($collected -and (Test-Path $Csv)){
        $rows = Import-Csv $Csv
        $gotD = ($rows | Where-Object { $_.RunLabel -eq $label -and $_.Metric -eq 'send_depth' } | Select-Object -Last 1).Value
        $gotDn = ToNum $gotD
        if($null -ne $gotDn -and $gotDn -ne $cfg.d){ $armNote += "gauge send_depth=$gotD != $($cfg.d)" }
        $expTr = 0; if($cfg.tr -eq "RIO"){ $expTr = 1 }
        $gotT = ($rows | Where-Object { $_.RunLabel -eq $label -and $_.Metric -eq 'transport_rio' } | Select-Object -Last 1).Value
        $gotTn = ToNum $gotT
        if($null -ne $gotTn -and $gotTn -ne $expTr){ $armNote += "gauge transport_rio=$gotT != $expTr" }
        # 둘 다 읽혔고 둘 다 일치하면 게이지만으로 아암이 확정된다
        $gaugeOk = ($null -ne $gotDn -and $gotDn -eq $cfg.d -and $null -ne $gotTn -and $gotTn -eq $expTr)
      }

      # 로그가 비어도 게이지가 아암을 확정했으면 유효한 런으로 본다.
      #   로그는 서버가 정상 종료해야 플러시되는데(로거가 async), 종료는 측정 창이 끝난 "뒤"의
      #   일이라 창 안의 데이터를 무효화하지 않는다. 종료 실패 자체는 Graceful 컬럼에 남긴다.
      if($logEmpty){
        if($gaugeOk){ Write-Warning ("{0}: log missing (graceful={1}) -- arm verified by gauge only" -f $label,$graceful) }
        else        { $armNote += ("log window empty (graceful={0}) and gauge did not confirm arm" -f $graceful) }
      }

      $okRun = ($armNote.Count -eq 0)
      if($okRun){ Write-Host "    arm assert OK (log + gauge)" -ForegroundColor DarkGreen }
      else      { Write-Warning ("RUN FAIL {0}: {1}" -f $label, ($armNote -join "; ")) }
      $runRecs.Add([pscustomobject]@{ Label=$label; Arm=$cfg.tr; D=$cfg.d; CC=$cfg.cc; Rep=$rep; Ok=$okRun; Graceful=$graceful; Note=($armNote -join "; ") })

      Stop-Procs; Start-Sleep 60   # TIME_WAIT / 소켓 드레인
    }
  }

  # ---- 4) 요약: 구성별 평균 + 팔xCC별 깊이 효과 ----
  Write-Host ""
  Write-Host "=== SUMMARY (avg across $Reps reps; arm-assert FAIL runs excluded) ===" -ForegroundColor Green
  if(Test-Path $Csv){
    $rows = Import-Csv $Csv
    $badLabels = @($runRecs | Where-Object { -not $_.Ok } | ForEach-Object { $_.Label })
    function Avg($tr,$d,$cc,$metric){
      $vals = for($r=1;$r -le $Reps;$r++){
        $lb = "{0}_{1}_D{2}_CC{3}_r{4}" -f $LabelPrefix,$tr,$d,$cc,$r
        if($badLabels -contains $lb){ continue }   # 아암 불일치 런의 값은 딴 아암의 값 -- 평균에서 제외
        ToNum (($rows | Where-Object { $_.RunLabel -eq $lb -and $_.Metric -eq $metric -and [int]$_.WindowMin -eq $WindowMin } | Select-Object -Last 1).Value)
      }
      $vals = @($vals | Where-Object { $_ -ne $null })
      if($vals.Count){ ($vals | Measure-Object -Average).Average } else { $null }
    }
    $keys = "send_depth","send_followup_rate","send_wrap_split_rate","bytes_per_send","send_contention_rate",
            "wsa_send_rate","net_cpu_total","net_kernel_cpu_total","worker_cpu_total","sendworker_cpu_total",
            "gameloop_cpu","tick_p99_ms","dummy_loop_p99_ms","dummy_send_buffer_full_rate","dummy_send_pps",
            "net_cpu_per_kpps","session_count"

    $tbl = foreach($cfg in $configs){
      $o = [ordered]@{ Arm=$cfg.tr; D=$cfg.d; CC=$cfg.cc }
      foreach($k in $keys){ $v = Avg $cfg.tr $cfg.d $cfg.cc $k; $o[$k] = if($v -ne $null){[math]::Round($v,4)}else{"NA"} }
      $bf=Avg $cfg.tr $cfg.d $cfg.cc "dummy_send_buffer_full_rate"; $tp=Avg $cfg.tr $cfg.d $cfg.cc "tick_p99_ms"; $sc=Avg $cfg.tr $cfg.d $cfg.cc "session_count"
      $o["GATE"] = if(($bf -ne $null -and $bf -eq 0) -and ($tp -ne $null -and $tp -lt 40) -and ($sc -ne $null -and $sc -ge $cfg.cc*0.98)){"PASS"}else{"FAIL"}
      $recs = @($runRecs | Where-Object { $_.Arm -eq $cfg.tr -and $_.D -eq $cfg.d -and $_.CC -eq $cfg.cc })
      $badN = @($recs | Where-Object { -not $_.Ok }).Count
      # 키 이름 주의: PowerShell 속성명은 대소문자를 구분하지 않는다. "ARM"으로 쓰면 위의
      #   Arm(팔 이름)을 덮어써서 표에 팔이 사라지고 VERDICT가 baseline을 못 찾는다.
      $o["ArmChk"] = if($recs.Count -eq 0){"SKIP"} elseif($badN -eq $recs.Count){"FAIL"} elseif($badN){"PART($badN/$($recs.Count))"} else {"OK"}
      [pscustomobject]$o
    }
    $tbl | Format-Table Arm,D,CC,send_depth,send_followup_rate,bytes_per_send,send_wrap_split_rate,send_contention_rate,worker_cpu_total,net_cpu_total,tick_p99_ms,dummy_send_pps,session_count,ArmChk,GATE -AutoSize

    Write-Host ""
    Write-Host ("=== VERDICT per arm x load (baseline = D{0}) ===" -f $Depths[0]) -ForegroundColor Cyan
    $baseD = $Depths[0]
    $pctOf = { param($a,$b) if($a -ne "NA" -and $b -ne "NA" -and [double]$a -ne 0){ ([double]$b-[double]$a)/[double]$a*100 } else { $null } }
    foreach($tr in $Transports){
      foreach($cc in $ClientCounts){
        $base = $tbl | Where-Object { $_.Arm -eq $tr -and $_.D -eq $baseD -and $_.CC -eq $cc } | Select-Object -First 1
        if(-not $base -or $base.ArmChk -eq "SKIP"){ Write-Warning ("{0} CC{1}: no baseline D{2} row" -f $tr,$cc,$baseD); continue }
        foreach($d in $Depths){
          if($d -eq $baseD){ continue }
          $arm = $tbl | Where-Object { $_.Arm -eq $tr -and $_.D -eq $d -and $_.CC -eq $cc } | Select-Object -First 1
          if(-not $arm -or $arm.ArmChk -eq "SKIP"){ continue }
          if($base.GATE -ne "PASS" -or $arm.GATE -ne "PASS"){ Write-Warning ("{0} CC{1} D{2}: gate FAIL (base={3} arm={4}) -- numbers below are not decision-grade" -f $tr,$cc,$d,$base.GATE,$arm.GATE) }
          if($base.ArmChk -ne "OK" -or $arm.ArmChk -ne "OK"){ Write-Warning ("{0} CC{1} D{2}: arm assert not clean (base={3} arm={4})" -f $tr,$cc,$d,$base.ArmChk,$arm.ArmChk) }

          $pF = & $pctOf $base.send_followup_rate $arm.send_followup_rate
          $pB = & $pctOf $base.bytes_per_send     $arm.bytes_per_send
          $pW = & $pctOf $base.worker_cpu_total   $arm.worker_cpu_total
          $pN = & $pctOf $base.net_cpu_total      $arm.net_cpu_total
          $pT = & $pctOf $base.tick_p99_ms        $arm.tick_p99_ms
          $pP = & $pctOf $base.dummy_send_pps     $arm.dummy_send_pps
          $pNn= & $pctOf $base.net_cpu_per_kpps   $arm.net_cpu_per_kpps

          Write-Host ("{0} CC{1}  D{2} -> D{3}" -f $tr,$cc,$baseD,$d) -ForegroundColor Yellow
          Write-Host ("   followup/s     {0,-12} -> {1,-12}  ({2:+0.0;-0.0}%)   <- 완료 왕복을 깊이가 흡수했는가 (핵심, 낮을수록 좋음)" -f $base.send_followup_rate,$arm.send_followup_rate,$pF)
          Write-Host ("   bytes/send     {0,-12} -> {1,-12}  ({2:+0.0;-0.0}%)   <- 제출 굵기 (높을수록 좋음)" -f $base.bytes_per_send,$arm.bytes_per_send,$pB)
          Write-Host ("   submit calls/s {0,-12} -> {1,-12}" -f $base.wsa_send_rate,$arm.wsa_send_rate)
          Write-Host ("   NET cpu        {0,-12} -> {1,-12}  ({2:+0.0;-0.0}%)   <- 종합 비용" -f $base.net_cpu_total,$arm.net_cpu_total,$pN)
          Write-Host ("   worker cpu     {0,-12} -> {1,-12}  ({2:+0.0;-0.0}%)" -f $base.worker_cpu_total,$arm.worker_cpu_total,$pW)
          Write-Host ("   tick p99       {0,-12} -> {1,-12}  ({2:+0.0;-0.0}%)" -f $base.tick_p99_ms,$arm.tick_p99_ms,$pT)
          if($tr -eq "RIO"){ Write-Host ("   wrap splits/s  {0,-12} -> {1,-12}  <- 링 랩 경계 분할 (control, RIO 전용)" -f $base.send_wrap_split_rate,$arm.send_wrap_split_rate) }
          if($pP -ne $null -and [math]::Abs($pP) -gt 3){
            Write-Host ("   !! load asymmetry >3% -- use normalized: net_cpu_per_kpps {0} -> {1} ({2:+0.0;-0.0}%)" -f $base.net_cpu_per_kpps,$arm.net_cpu_per_kpps,$pNn) -ForegroundColor Yellow
          }

          # 해석 라인 -- "후속 제출이 원래 없더라"는 결과는 실패가 아니라 부하에 대한 답이다
          # (gqcs-ab의 NO BATCHING 해석과 동형: 흡수할 왕복이 없으면 깊이는 살 게 없다)
          $bF = $null; if($base.send_followup_rate -ne "NA"){ $bF = [double]$base.send_followup_rate }
          $bS = $null; if($base.wsa_send_rate -ne "NA"){ $bS = [double]$base.wsa_send_rate }
          if($bF -ne $null -and $bS -ne $null -and $bS -gt 0 -and ($bF/$bS) -lt 0.01){
            Write-Host ("   => D{0} baseline already has followup on only {1:P2} of submits -- nothing for depth to absorb at this load (an answer, not a failure)" -f $baseD,($bF/$bS)) -ForegroundColor DarkCyan
          } elseif($pF -ne $null -and $pF -lt -30){
            Write-Host ("   => depth D{0} absorbed completion round-trips (followup {1:+0.0;-0.0}%) -- judge net gain by worker/net cpu + tick" -f $d,$pF) -ForegroundColor DarkCyan
          }
        }
      }
    }

    if($skipped.Count){ Write-Host ""; Write-Host "=== SKIPPED (build) ===" -ForegroundColor Red; $skipped | ForEach-Object { Write-Host "   $_" } }
    $failRuns = @($runRecs | Where-Object { -not $_.Ok })
    if($failRuns.Count){
      Write-Host ""; Write-Host "=== RUN-LEVEL FAIL (arm assert / server death) -- excluded from averages ===" -ForegroundColor Red
      $failRuns | ForEach-Object { Write-Host ("   {0}: {1}" -f $_.Label,$_.Note) }
    }
    Write-Host ""
    Write-Host "raw metrics: $Csv" -ForegroundColor DarkGray
  } else { Write-Warning "no CSV produced (collection never succeeded)" }

  # ---- 5) 바이너리를 IOCP 기본으로 복원 (마지막 빌드가 RIO였을 때만 재빌드) ----
  if(-not $SkipBuild -and $builtArm -eq "RIO"){ [void](Rebuild "IOCP") }
}
finally {
  Stop-Procs
  [IO.File]::WriteAllBytes($SrvIni,$srvBak)
  [IO.File]::WriteAllBytes($CliIni,$cliBak)
  Reset-Toggle
  Write-Host "INIs restored to original bytes + BuildConfig toggle reset to IOCP(0)" -ForegroundColor DarkYellow
  Stop-Transcript | Out-Null
}
