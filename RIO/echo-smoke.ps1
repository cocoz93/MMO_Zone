# ==========================================================================
# RIO 에코 런타임 스모크 — USE_RIO_TRANSPORT=1 빌드 검증 드라이버
#
#  T1: GameCodiEchoTest 모드 — 손제작 TcpClient 왕복 (4B/200B/1400B 바이트 정합)
#  T1b: 1400B x 50 왕복 (한 커넥션 70KB → 64KB 링버퍼 랩 구간 통과 검증)
#  T1c: graceful 종료 — CTRL_C 이벤트 → "Server shutdown complete" 로그 + exit 0
#  T2: NetWorkLib_EchoTest 모드 — EchoStressClient 1000클라 25초 자동 회귀
#      (DisconnectTest=1 → 접속폭풍 포함), Results 파일 판정
#  T2c: graceful 종료 재검 (1000클라 드레인 직후 셧다운)
#
#  INI는 CP949(무BOM) 규칙 준수 — GetEncoding(949)로만 읽고 쓴다. 원본은 백업/복원.
#
#  -ExpectTransport RIO(기본)|IOCP : 현재 Run\bin 바이너리가 어느 팔(arm)인지 검증에 사용.
#     (RIO\build-A-iocp.bat / build-B-rio.bat 로 팔을 바꾼 뒤 각각 실행)
#
#  -CompletionBatch N : 완료 수거 방식을 INI에 강제하고 서버 로그로 실제 적용을 확인한다.
#     0=GQCS(완료 1건당 수거) / N>0=GQCSEx(한 번에 최대 N건) / 생략=INI 원본값 그대로(기존 동작).
#     IOCP 팔 전용 — RIO 빌드는 이 키를 쓰지 않는다.
# ==========================================================================
param([ValidateSet('RIO', 'IOCP')] [string]$ExpectTransport = 'RIO',
      [int]$CompletionBatch = -1)
$ErrorActionPreference = 'Stop'
$bin     = Join-Path (Split-Path $PSScriptRoot -Parent) 'Run\bin'
$srvExe  = Join-Path $bin 'MMOServer.exe'
$srvIni  = Join-Path $bin 'MMOServerConfig.ini'
$echoIni = Join-Path $bin 'EchoStressConfig.ini'
$logFile = Join-Path $bin ('logs\' + (Get-Date -Format 'yyMMdd') + '_MMOServer.log')
$enc949  = [Text.Encoding]::GetEncoding(949)

$script:failures = @()
function Note-Fail([string]$msg) { $script:failures += $msg; Write-Host "  [FAIL] $msg" -ForegroundColor Red }
function Note-Pass([string]$msg) { Write-Host "  [PASS] $msg" -ForegroundColor Green }

# ── CTRL_C 송신 헬퍼: 별도 powershell 프로세스가 서버 콘솔에 붙어 이벤트 발생 ──
# (내 콘솔에서 직접 GenerateConsoleCtrlEvent 하면 이 스크립트도 맞는다)
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
    # ($targetPid는 위 here-string에서 즉시 보간됨)
    $tmp = Join-Path $env:TEMP 'rio_sendctrlc.ps1'
    [IO.File]::WriteAllText($tmp, $helper, [Text.Encoding]::UTF8)
    $hp = Start-Process powershell -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File', $tmp -PassThru -WindowStyle Hidden
    $hp.WaitForExit(5000) | Out-Null
    return $hp.ExitCode
}

# 서버가 로그 파일을 쓰기 오픈 중이어도 읽기 — FileShare.ReadWrite (기본 ReadAllText는 충돌)
function Read-LogText([string]$path) {
    if (-not (Test-Path $path)) { return '' }
    $fs = [IO.File]::Open($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    try {
        $sr = New-Object IO.StreamReader($fs, [Text.Encoding]::UTF8)
        return $sr.ReadToEnd()
    }
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

function Start-Server {
    $p = Start-Process -FilePath $srvExe -WorkingDirectory $bin -PassThru -WindowStyle Minimized
    $deadline = (Get-Date).AddSeconds(30)
    while ((Get-Date) -lt $deadline) {
        if ($p.HasExited) { throw "server exited early (code $($p.ExitCode))" }
        $listening = netstat -an | Select-String ':6000\s+.*LISTENING'
        if ($listening) { Start-Sleep -Milliseconds 300; return $p }
        Start-Sleep -Milliseconds 500
    }
    throw 'server did not listen on :6000 within 30s'
}

function Stop-ServerGraceful([System.Diagnostics.Process]$p, [string]$tag) {
    $rc = Send-CtrlC $p.Id
    if ($rc -ne 0) { Note-Fail "$tag : CTRL_C helper failed (rc=$rc)"; cmd /c "taskkill /F /PID $($p.Id) >nul 2>nul"; return }
    if (-not $p.WaitForExit(20000)) {
        Note-Fail "$tag : graceful shutdown timeout 20s (forced kill)"
        cmd /c "taskkill /F /PID $($p.Id) >nul 2>nul"
        return
    }
    if ($p.ExitCode -eq 0) { Note-Pass "$tag : graceful exit code 0" }
    else { Note-Fail "$tag : exit code $($p.ExitCode)" }
}

function Test-GameCodiEcho([System.Net.Sockets.TcpClient]$c, [System.IO.Stream]$s, [int]$payloadLen, [Random]$rng) {
    $payload = New-Object byte[] $payloadLen
    $rng.NextBytes($payload)
    $pkt = [BitConverter]::GetBytes([uint16]$payloadLen) + $payload
    $s.Write($pkt, 0, $pkt.Length)
    $buf = New-Object byte[] $pkt.Length
    $got = 0
    while ($got -lt $pkt.Length) {
        $r = $s.Read($buf, $got, $pkt.Length - $got)
        if ($r -le 0) { throw "connection closed during echo (len=$payloadLen)" }
        $got += $r
    }
    if ([Convert]::ToBase64String($buf) -ne [Convert]::ToBase64String($pkt)) { throw "echo mismatch (len=$payloadLen)" }
}

# ── 사전 정리 + INI 백업 (taskkill stderr는 cmd 안에서 삼킴 — EAP=Stop 승격 방지) ──
cmd /c "taskkill /F /IM MMOServer.exe >nul 2>nul"
cmd /c "taskkill /F /IM EchoStressClient.exe >nul 2>nul"
Start-Sleep -Milliseconds 500
$srvIniBak  = [IO.File]::ReadAllBytes($srvIni)
$echoIniBak = [IO.File]::ReadAllBytes($echoIni)
# 이번 스모크 구간의 로그 시작점 — "문자" 길이 기준 (파일 바이트 길이를 Substring에 쓰면
# 멀티바이트 로그에서 오프셋이 어긋나 새 내용을 지나쳐 자른다)
$logStart = (Read-LogText $logFile).Length

try {
    # ════ T1: GameCodi 손스모크 ════
    Write-Host "`n=== T1: GameCodiEchoTest hand smoke ===" -ForegroundColor Cyan
    Set-IniKey $srvIni 'Mode' 'GameCodiEchoTest'
    Set-IniKey $srvIni 'MaxClients' '2000'          # RIO 슬랩 = 2000 x 128KB = 250MB (원본 10000은 1.25GB)
    Set-IniKey $srvIni 'MonitorEnabled' '0'
    # 완료 수거 방식 강제 (IOCP 팔 A/B) — 생략(-1)이면 INI 원본을 그대로 둔다.
    if ($CompletionBatch -ge 0) { Set-IniKey $srvIni 'CompletionBatch' "$CompletionBatch" }
    $srv = Start-Server

    # RIO 기동 확인은 T1c 종료 후 로그에서 수행 — 로거가 프로세스 종료 시점에 버퍼를
    # 플러시하므로 살아있는 동안 파일을 읽으면 startup 라인이 아직 없다 (실측).

    $c = New-Object System.Net.Sockets.TcpClient
    $c.Connect('127.0.0.1', 6000)
    $s = $c.GetStream()
    $s.ReadTimeout = 5000
    $rng = New-Object Random 42
    foreach ($len in 4, 200, 1400) { Test-GameCodiEcho $c $s $len $rng }
    Note-Pass 'T1 roundtrip 4/200/1400B byte-exact'

    for ($i = 0; $i -lt 50; $i++) { Test-GameCodiEcho $c $s 1400 $rng }
    Note-Pass 'T1b ring-wrap: 1400B x 50 (70KB > 64KB ring) byte-exact'
    $c.Close()

    # 멀티 커넥션 가벼운 확인 (10개 x 3왕복)
    $conns = @()
    for ($k = 0; $k -lt 10; $k++) {
        $cc = New-Object System.Net.Sockets.TcpClient
        $cc.Connect('127.0.0.1', 6000)
        $ss = $cc.GetStream(); $ss.ReadTimeout = 5000
        $conns += ,@($cc, $ss)
    }
    foreach ($pair in $conns) { foreach ($len in 16, 700, 1400) { Test-GameCodiEcho $pair[0] $pair[1] $len $rng } }
    foreach ($pair in $conns) { $pair[0].Close() }
    Note-Pass 'T1 multi-conn 10x3 roundtrips'

    Stop-ServerGraceful $srv 'T1c'
    $logNow = Read-LogText $logFile
    $logWin = $logNow.Substring([Math]::Min($logStart, $logNow.Length))
    if ($ExpectTransport -eq 'RIO') {
        if ($logWin -match 'RIO workers=(\d+)') { Note-Pass "server ran with RIO transport (workers=$($Matches[1]))" }
        else { Note-Fail 'RIO start log not found — IOCP binary? (arm mismatch)' }
    } else {
        if ($logWin -match 'IOCP concurrency=\d+') { Note-Pass 'server ran with IOCP transport (arm A)' }
        elseif ($logWin -match 'RIO workers=') { Note-Fail 'expected IOCP arm but RIO log found (arm mismatch)' }
        else { Note-Fail 'IOCP start log not found' }
    }
    if ($logWin -match 'Server shutdown complete') { Note-Pass 'T1c shutdown log present' }
    else { Note-Fail 'T1c "Server shutdown complete" log missing' }

    # 완료 수거 방식 assert — INI가 실제로 먹었는지 서버 로그로 확인한다.
    #   같은 바이너리로 팔을 바꾸는 구조라, "INI가 안 먹은 채 돈 런"을 잡을 방법이 이것뿐이다.
    if ($CompletionBatch -ge 0 -and $ExpectTransport -eq 'IOCP') {
        if ($CompletionBatch -eq 0) {
            if ($logWin -match 'Completion harvest = GQCS \(') { Note-Pass 'harvest mode = GQCS (one-at-a-time)' }
            else { Note-Fail 'expected GQCS harvest log, not found (INI not applied?)' }
        } else {
            if ($logWin -match 'Completion harvest = GQCSEx.*?(\d+)') { Note-Pass "harvest mode = GQCSEx (cap $($Matches[1]))" }
            else { Note-Fail "expected GQCSEx harvest log (cap $CompletionBatch), not found (INI not applied?)" }
        }
    }

    # ════ T2: EchoStressClient 회귀 (1000클라, 25초, DisconnectTest=1) ════
    Write-Host "`n=== T2: EchoStressClient regression ===" -ForegroundColor Cyan
    Set-IniKey $srvIni 'Mode' 'NetWorkLib_EchoTest'
    Set-IniKey $echoIni 'TestDurationSec' '25'
    Set-IniKey $echoIni 'ClientCount' '1000'
    $resultsDir = Join-Path $bin 'Results'
    $beforeResults = @(Get-ChildItem $resultsDir -Filter 'EchoStress_*.txt' -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })

    $srv2 = Start-Server
    $cli = Start-Process -FilePath (Join-Path $bin 'EchoStressClient.exe') -WorkingDirectory $bin -PassThru -WindowStyle Minimized
    if (-not $cli.WaitForExit(90000)) { Note-Fail 'T2 stress client did not exit in 90s'; cmd /c "taskkill /F /PID $($cli.Id) >nul 2>nul" }

    $newResult = Get-ChildItem $resultsDir -Filter 'EchoStress_*.txt' | Where-Object { $beforeResults -notcontains $_.Name } |
                 Sort-Object LastWriteTime | Select-Object -Last 1
    if ($null -eq $newResult) {
        Note-Fail 'T2 no result file produced'
    } else {
        $rep = Get-Content $newResult.FullName -Raw   # ccs=UTF-8(BOM) — Get-Content가 BOM 자동 감지
        Write-Host $rep
        $vals = @{}
        # 라벨은 EchoStressClient main.cpp 출력과 1:1 (ac06be5에서 Packet Error -> Pad/Order 분리됨).
        # 라벨을 못 찾으면 -1 -> FAIL: 클라 출력이 또 바뀌면 조용히 통과하지 않고 여기서 걸린다.
        foreach ($k in 'Connect Total','Connect Fail','Recv Total','Echo Timeout','Pad Error','Order Error','Late Arrival','SendBuf Full') {
            if ($rep -match ([regex]::Escape($k) + '\s*:\s*(-?\d+)')) { $vals[$k] = [int64]$Matches[1] } else { $vals[$k] = -1 }
        }
        if ($vals['Connect Total'] -ge 1000 -and $vals['Connect Fail'] -eq 0) { Note-Pass "T2 connects $($vals['Connect Total'])/fail 0" } else { Note-Fail "T2 connect total=$($vals['Connect Total']) fail=$($vals['Connect Fail'])" }
        if ($vals['Recv Total'] -gt 0) { Note-Pass "T2 recv total $($vals['Recv Total'])" } else { Note-Fail 'T2 recv total 0' }
        # 무결성 판정 = pad(페이로드 훼손) + order(에코 값 역전) 둘 다 0. late는 관측치라 표시만.
        if ($vals['Pad Error'] -eq 0 -and $vals['Order Error'] -eq 0) {
            Note-Pass "T2 integrity: pad 0 / order 0 (late $($vals['Late Arrival']))"
        } else {
            Note-Fail "T2 integrity: pad $($vals['Pad Error']) / order $($vals['Order Error'])"
        }
        if ($vals['Echo Timeout'] -eq 0) { Note-Pass 'T2 echo timeout 0' } else { Note-Fail "T2 echo timeout $($vals['Echo Timeout'])" }
    }

    Stop-ServerGraceful $srv2 'T2c'

    # ════ T3: 유휴 타임아웃 킥 + 트래픽 한창중 graceful 셧다운 ════
    # 둘 다 "비소유 스레드발 Disconnect 핸드오프" 체인(RIO의 CancelIoEx 대체 기계장치)을 실증:
    #  - 킥: 타이머 스레드 → 단건 핸드오프 (SESSION_TIMEOUT_SEC=60, recv 없으면 ~60초에 발화)
    #  - 셧다운: 콘솔 스레드 → 활성 세션 대량 핸드오프 → IOCount 수렴 → 워커 join
    Write-Host "`n=== T3: idle-timeout kick + mid-traffic graceful shutdown ===" -ForegroundColor Cyan
    Set-IniKey $echoIni 'TestDurationSec' '90'
    $srv3 = Start-Server
    $idle = New-Object System.Net.Sockets.TcpClient
    $idle.Connect('127.0.0.1', 6000)
    $idleStream = $idle.GetStream()
    $idleStream.ReadTimeout = 8000
    $cli3 = Start-Process -FilePath (Join-Path $bin 'EchoStressClient.exe') -WorkingDirectory $bin -PassThru -WindowStyle Minimized
    Write-Host '  (waiting 68s for idle-timeout kick, stress traffic running...)'
    Start-Sleep -Seconds 68

    $kicked = $false
    $kickDetail = ''
    try {
        $b = $idleStream.Read((New-Object byte[] 4), 0, 4)
        if ($b -le 0) { $kicked = $true; $kickDetail = 'FIN' }
        else { $kickDetail = "unexpected $b bytes" }
    } catch {
        # PS의 Read 예외는 MethodInvocationException→IOException→SocketException 다층 래핑 — 체인을 걸어 찾는다
        $ex = $_.Exception
        $sock = $null
        while ($null -ne $ex) {
            if ($ex -is [System.Net.Sockets.SocketException]) { $sock = $ex; break }
            $ex = $ex.InnerException
        }
        if ($null -ne $sock) {
            if ($sock.SocketErrorCode -eq 'ConnectionReset') { $kicked = $true; $kickDetail = 'RST' }
            else { $kickDetail = "socket err $($sock.SocketErrorCode)" }   # TimedOut = 킥 안 됨
        } else { $kickDetail = $_.Exception.Message }
    }
    if ($kicked) { Note-Pass "T3 idle session kicked by timeout (~60s, non-owner handoff, $kickDetail)" }
    else { Note-Fail "T3 idle session NOT kicked within 68s ($kickDetail)" }
    $idle.Close()

    # 트래픽 한창중(클라 90초 중 ~70초 지점, ~1000세션 활성) CTRL_C — 대량 드레인 셧다운
    Stop-ServerGraceful $srv3 'T3c(mid-traffic)'
    cmd /c "taskkill /F /PID $($cli3.Id) >nul 2>nul"   # 서버 사망 후 클라 리포트는 무의미 — 정리만

    # ── 서버 로그 오류 패턴 검사 (이번 스모크 구간만) ──
    $logAll = Read-LogText $logFile
    $smokeLog = $logAll.Substring([Math]::Min($logStart, $logAll.Length))
    $errHits = @()
    foreach ($pat in 'IOCount underflow', 'RIO_CORRUPT', 'buffer overflow', 'Partial send', 'RIOCreateRequestQueue failed', 'RIOReceive failed', 'RIOSend failed', 'init failed') {
        $cnt = ([regex]::Matches($smokeLog, [regex]::Escape($pat))).Count
        if ($cnt -gt 0) { $errHits += "$pat x$cnt" }
    }
    if ($errHits.Count -eq 0) { Note-Pass 'server log: no error patterns in smoke window' }
    else { Note-Fail ('server log errors: ' + ($errHits -join ', ')) }
}
finally {
    cmd /c "taskkill /F /IM MMOServer.exe >nul 2>nul"
    cmd /c "taskkill /F /IM EchoStressClient.exe >nul 2>nul"
    [IO.File]::WriteAllBytes($srvIni, $srvIniBak)
    [IO.File]::WriteAllBytes($echoIni, $echoIniBak)
    Write-Host "`n(INI restored to original bytes)"
}

Write-Host "`n=== SMOKE RESULT ===" -ForegroundColor Cyan
if ($script:failures.Count -eq 0) { Write-Host 'ALL PASS'; exit 0 }
else { Write-Host ("FAILURES: " + $script:failures.Count); $script:failures | ForEach-Object { Write-Host " - $_" }; exit 2 }
