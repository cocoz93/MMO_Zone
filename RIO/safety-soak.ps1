# ==========================================================================
# RIO 안전성 소크 — 접속폭풍·slot 재사용·종료경로를 강도 높여 N회 반복 타격
#
#  echo-smoke.ps1(회귀)과 달리 "안전성 게이트"에 특화한다:
#   - DisconnectTest=1 + ReconnectIntervalMs↓ + ClientCount↑ 로 접속/해제 폭풍을
#     조밀하게 만들어, 세션 index 재사용을 빈발시킨다 →
#     NewConn/Disconnect 역전·_queuedForSend 재사용 오염(방금 방어한 창)을 반복 노출.
#   - 판마다 서버 재기동 → 부하 → graceful 종료(활성 세션 대량을 비소유 스레드가
#     Disconnect 핸드오프 → closesocket → 에러완료 → IOCount 수렴 → 워커 join).
#
#  판정(HARD = 안전성): 로그 에러패턴 0 · Connect Fail 0 · Packet Error 0 ·
#                       Recv Total>0 · graceful exit 0 · 팔(arm) 일치.
#  성능지표(Echo Timeout / SendBuf Full)는 리포트만 — 안전성 아님(Phase 2 실측 소관).
#
#  INI는 CP949(무BOM) — GetEncoding(949)로만 읽고 쓰며, 원본은 바이트로 백업/복원.
#  팔 전환: RIO\build-B-rio.bat(RIO) / build-A-iocp.bat(IOCP) 후 -ExpectTransport 지정.
#
#  예)  powershell -NoProfile -ExecutionPolicy Bypass -File .\safety-soak.ps1 `
#          -ExpectTransport RIO -Clients 3000 -Reps 5 -DurationSec 60 -ReconnectMs 200
# ==========================================================================
param(
    [ValidateSet('RIO','IOCP')] [string]$ExpectTransport = 'RIO',
    [int]$Clients       = 3000,
    [int]$Reps          = 5,
    [int]$DurationSec   = 60,
    [int]$ReconnectMs   = 200,      # ↓일수록 접속폭풍 강화 (slot 재사용 빈발)
    [int]$MaxClientsIni = 0         # 0 = 자동(Clients + 500, 최소 2000)
)
$ErrorActionPreference = 'Stop'

$bin     = Join-Path (Split-Path $PSScriptRoot -Parent) 'Run\bin'
$srvExe  = Join-Path $bin 'MMOServer.exe'
$srvIni  = Join-Path $bin 'MMOServerConfig.ini'
$echoIni = Join-Path $bin 'EchoStressConfig.ini'
$cliExe  = Join-Path $bin 'EchoStressClient.exe'
$logFile = Join-Path $bin ('logs\' + (Get-Date -Format 'yyMMdd') + '_MMOServer.log')
$enc949  = [Text.Encoding]::GetEncoding(949)
if ($MaxClientsIni -le 0) { $MaxClientsIni = [Math]::Max(2000, $Clients + 500) }

$script:failures = @()
function Note-Fail([string]$m) { $script:failures += $m; Write-Host "  [FAIL] $m" -ForegroundColor Red }
function Note-Pass([string]$m) { Write-Host "  [PASS] $m" -ForegroundColor Green }
function Note-Info([string]$m) { Write-Host "  [ .. ] $m" -ForegroundColor DarkGray }

# ── CTRL_C 헬퍼: 별도 powershell이 서버 콘솔에 붙어 이벤트 발생 (echo-smoke.ps1 검증본) ──
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
    $tmp = Join-Path $env:TEMP 'soak_sendctrlc.ps1'
    [IO.File]::WriteAllText($tmp, $helper, [Text.Encoding]::UTF8)
    $hp = Start-Process powershell -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File', $tmp -PassThru -WindowStyle Hidden
    $hp.WaitForExit(5000) | Out-Null
    return $hp.ExitCode
}

# 서버가 로그를 쓰기 오픈 중이어도 읽기 (FileShare.ReadWrite)
function Read-LogText([string]$path) {
    if (-not (Test-Path $path)) { return '' }
    $fs = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    try { $sr = New-Object IO.StreamReader($fs, [Text.Encoding]::UTF8); return $sr.ReadToEnd() }
    finally { $fs.Close() }
}

function Set-IniKey([string]$path, [string]$key, [string]$val) {
    $lines = [IO.File]::ReadAllLines($path, $enc949)
    $found = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match ('^' + [regex]::Escape($key) + '=')) { $lines[$i] = "$key=$val"; $found = $true }
    }
    if (-not $found) { throw "INI key not found: $key in $path" }
    [IO.File]::WriteAllLines($path, $lines, $enc949)
}

# 포트가 LISTENING에서 빠질 때까지 대기 (이전 판 서버/클라 잔존 방지)
function Wait-PortClear([int]$port, [int]$timeoutSec = 15) {
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        if (-not (netstat -an | Select-String (":$port\s+.*LISTENING"))) { return $true }
        Start-Sleep -Milliseconds 300
    }
    return $false
}

# 프로세스가 완전히 사라질 때까지 대기
function Wait-ProcGone([string]$name, [int]$timeoutSec = 10) {
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        if (-not (Get-Process -Name $name -ErrorAction SilentlyContinue)) { return $true }
        Start-Sleep -Milliseconds 300
    }
    return $false
}

function Start-Server {
    $p = Start-Process -FilePath $srvExe -WorkingDirectory $bin -PassThru -WindowStyle Minimized
    $deadline = (Get-Date).AddSeconds(30)
    while ((Get-Date) -lt $deadline) {
        if ($p.HasExited) { throw "server exited early (code $($p.ExitCode))" }
        if (netstat -an | Select-String ':6000\s+.*LISTENING') { Start-Sleep -Milliseconds 300; return $p }
        Start-Sleep -Milliseconds 500
    }
    throw 'server did not listen on :6000 within 30s'
}

function Stop-ServerGraceful([System.Diagnostics.Process]$p, [string]$tag) {
    $rc = Send-CtrlC $p.Id
    if ($rc -ne 0) {
        Start-Sleep -Milliseconds 700     # AttachConsole 일시 실패(부하/타이밍) 대비 1회 재시도
        $rc = Send-CtrlC $p.Id
    }
    if ($rc -ne 0) { Note-Fail "${tag}: CTRL_C helper failed (rc=$rc)"; cmd /c "taskkill /F /PID $($p.Id) >nul 2>nul"; return $false }
    if (-not $p.WaitForExit(30000)) {
        Note-Fail "${tag}: graceful shutdown timeout 30s (forced kill)"
        cmd /c "taskkill /F /PID $($p.Id) >nul 2>nul"; return $false
    }
    if ($p.ExitCode -eq 0) { return $true }
    Note-Fail "${tag}: exit code $($p.ExitCode)"; return $false
}

# ── 사전 정리 + INI 바이트 백업 ──
cmd /c "taskkill /F /IM MMOServer.exe >nul 2>nul"
cmd /c "taskkill /F /IM EchoStressClient.exe >nul 2>nul"
Start-Sleep -Milliseconds 500
$srvIniBak  = [IO.File]::ReadAllBytes($srvIni)
$echoIniBak = [IO.File]::ReadAllBytes($echoIni)

Write-Host "`n=== RIO SAFETY SOAK ===" -ForegroundColor Cyan
Write-Host ("arm={0}  clients={1}  reps={2}  duration={3}s  reconnect={4}ms  maxClients={5}" -f `
    $ExpectTransport, $Clients, $Reps, $DurationSec, $ReconnectMs, $MaxClientsIni)

$passReps = 0
try {
    # 판 공통 강도 세팅
    Set-IniKey $srvIni  'Mode'                'NetWorkLib_EchoTest'
    Set-IniKey $srvIni  'MaxClients'          "$MaxClientsIni"
    Set-IniKey $srvIni  'MonitorEnabled'      '0'
    Set-IniKey $echoIni 'ClientCount'         "$Clients"
    Set-IniKey $echoIni 'DisconnectTest'      '1'
    Set-IniKey $echoIni 'ReconnectIntervalMs' "$ReconnectMs"
    Set-IniKey $echoIni 'TestDurationSec'     "$DurationSec"

    $resultsDir = Join-Path $bin 'Results'

    for ($rep = 1; $rep -le $Reps; $rep++) {
        Write-Host "`n--- rep $rep / $Reps ---" -ForegroundColor Yellow

        # [정리 게이트] 이전 판 잔재 완전 제거 확인 — rep 간 자원(포트/프로세스) 정리 레이스로 인한
        #   거짓 FAIL(클라 미접속·CTRL_C attach 실패) 방지. 서버 안전성과 무관한 하네스 보강.
        cmd /c "taskkill /F /IM MMOServer.exe >nul 2>nul"
        cmd /c "taskkill /F /IM EchoStressClient.exe >nul 2>nul"
        [void](Wait-ProcGone 'MMOServer'); [void](Wait-ProcGone 'EchoStressClient')
        if (-not (Wait-PortClear 6000)) { Note-Info "rep${rep}: port 6000 still busy after wait" }
        if (-not (Wait-PortClear 9092)) { Note-Info "rep${rep}: port 9092 still busy after wait" }

        $failBase = $script:failures.Count
        $logStart = (Read-LogText $logFile).Length
        $before = @(Get-ChildItem $resultsDir -Filter 'EchoStress_*.txt' -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })

        $srv = Start-Server
        $cli = Start-Process -FilePath $cliExe -WorkingDirectory $bin -PassThru -WindowStyle Minimized
        if (-not $cli.WaitForExit(($DurationSec + 30) * 1000)) {
            Note-Fail "rep${rep}: stress client did not exit in $($DurationSec + 30)s"
            cmd /c "taskkill /F /PID $($cli.Id) >nul 2>nul"
        }

        # Results 판정 (안전성 HARD)
        $newResult = Get-ChildItem $resultsDir -Filter 'EchoStress_*.txt' -ErrorAction SilentlyContinue |
                     Where-Object { $before -notcontains $_.Name } | Sort-Object LastWriteTime | Select-Object -Last 1
        if ($null -eq $newResult) {
            Note-Fail "rep${rep}: no result file produced"
        } else {
            $report = Get-Content $newResult.FullName -Raw
            $vals = @{}
            foreach ($k in 'Connect Total','Connect Fail','Recv Total','Echo Timeout','Packet Error','SendBuf Full') {
                if ($report -match ([regex]::Escape($k) + '\s*:\s*(-?\d+)')) { $vals[$k] = [int64]$Matches[1] } else { $vals[$k] = -1 }
            }
            if ($vals['Connect Fail'] -eq 0) { Note-Pass "rep${rep}: connect fail 0 (total $($vals['Connect Total']))" }
            else { Note-Fail "rep${rep}: connect fail $($vals['Connect Fail'])" }
            if ($vals['Packet Error'] -eq 0) { Note-Pass "rep${rep}: packet error 0" }
            else { Note-Fail "rep${rep}: packet error $($vals['Packet Error'])" }
            if ($vals['Recv Total'] -gt 0) { Note-Pass "rep${rep}: recv total $($vals['Recv Total'])" }
            else { Note-Fail "rep${rep}: recv total 0" }
            Note-Info "rep${rep}: (perf) echo timeout=$($vals['Echo Timeout']) sendbuf full=$($vals['SendBuf Full'])"
        }

        # graceful 종료 = 대량 비소유 Disconnect 핸드오프 드레인 (RIO 종료경로)
        if (Stop-ServerGraceful $srv "rep${rep}(shutdown)") { Note-Pass "rep${rep}: graceful exit 0" }

        # 로그 에러패턴 스캔 (이 판 구간만)
        $logAll = Read-LogText $logFile
        $win = $logAll.Substring([Math]::Min($logStart, $logAll.Length))
        $hits = @()
        foreach ($pat in 'IOCount underflow','RIO_CORRUPT','buffer overflow','Partial send',
                         'RIOCreateRequestQueue failed','RIOReceive failed','RIOSend failed','init failed') {
            $c = ([regex]::Matches($win, [regex]::Escape($pat))).Count
            if ($c -gt 0) { $hits += "$pat x$c" }
        }
        if ($hits.Count -eq 0) { Note-Pass "rep${rep}: log clean (no error patterns)" }
        else { Note-Fail "rep${rep}: log errors: $($hits -join ', ')" }

        # 팔(arm) 검증
        if ($ExpectTransport -eq 'RIO') {
            if ($win -match 'RIO workers=(\d+)') { Note-Pass "rep${rep}: RIO transport (workers=$($Matches[1]))" }
            else { Note-Fail "rep${rep}: RIO start log missing (arm mismatch?)" }
        } else {
            if ($win -match 'IOCP concurrency=\d+') { Note-Pass "rep${rep}: IOCP transport" }
            elseif ($win -match 'RIO workers=') { Note-Fail "rep${rep}: expected IOCP but RIO log found" }
            else { Note-Fail "rep${rep}: IOCP start log missing" }
        }

        if ($script:failures.Count -eq $failBase) { $passReps++; Write-Host "  => rep $rep PASS" -ForegroundColor Green }
        else { Write-Host "  => rep $rep FAIL" -ForegroundColor Red }

        cmd /c "taskkill /F /IM EchoStressClient.exe >nul 2>nul"
        Start-Sleep -Milliseconds 500
    }
}
finally {
    cmd /c "taskkill /F /IM MMOServer.exe >nul 2>nul"
    cmd /c "taskkill /F /IM EchoStressClient.exe >nul 2>nul"
    [IO.File]::WriteAllBytes($srvIni, $srvIniBak)
    [IO.File]::WriteAllBytes($echoIni, $echoIniBak)
    Write-Host "`n(INI restored to original bytes)"
}

Write-Host "`n=== SOAK RESULT ===" -ForegroundColor Cyan
Write-Host ("passed reps: {0} / {1}" -f $passReps, $Reps)
if ($script:failures.Count -eq 0) { Write-Host 'ALL PASS' -ForegroundColor Green; exit 0 }
else { Write-Host ("FAILURES: " + $script:failures.Count) -ForegroundColor Red; $script:failures | ForEach-Object { Write-Host " - $_" }; exit 2 }
