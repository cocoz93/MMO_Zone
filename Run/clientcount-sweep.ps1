<#
.SYNOPSIS
    ClientCount 스윕 — P1+P2 채택 후 "이동한 천장"(broadcast 복사/송신) 실측.

.DESCRIPTION
    멤버십 A/B(#define 토글·리빌드)와 달리, 이건 런타임 INI(ClientCount)만 바꿔 스윕한다 → 리빌드 0.
    토글(P1·P2)은 이미 바이너리에 ON 고정. 동접을 4000→4500→5000→5500로 올려가며 각 지점 부하+수집:
      · 서버 천장: gameloop_cpu가 1.0 근접 / tick_p99 급등 = 게임루프 포화
      · 병목 정체: broadcast_copy vs membership vs sendworker 중 뭐가 먼저 차나
      · 클라 감시: dummy_loop_p99·dummy_send_buffer_full — 클라가 먼저 죽으면 그 지점부터 서버지표 신뢰불가

    클라 여유 자동보정: ClientsPerThread = ceil((ClientCount+1)/ClientCores4)로 맞춰 항상 ClientCores4개 스레드
    (기본 4 = ClientCores 6-9 4코어)로 고정 → 스레드>코어 오버섭스크립션 회피.
    한계: 단일 PC 루프백이라 5000+ 절대천장은 못 믿는다(추세만). 확정은 2nd 머신(사설 LAN / 클라우드 same-VPC).

    복원: 종료/중단 시 MMOStressConfig.ini의 ClientCount·ClientsPerThread를 시작 시점 값으로 되돌림.

.EXAMPLE
    .\clientcount-sweep.ps1
    # 동접 4000/4500/5000/5500 × 2rep, 리빌드 없음, 약 90분

    .\clientcount-sweep.ps1 -Counts 3999,4999 -Reps 1 -LoadMin 1 -WindowMin 1 -LabelPrefix SWSMK
    # 배관 스모크(약 10분) — 기동·수집·요약이 도는지 먼저 확인 권장
#>
param(
    [int[]]  $Counts        = @(3999, 4499, 4999, 5499),   # ClientCount 값 (동접 = +1, 관측용 GameClient 1 동반)
    [int]    $Reps          = 2,
    [int]    $LoadMin       = 10,
    [int]    $WindowMin     = 5,
    [int]    $WorkerThreads = 4,
    [int]    $SendWorkers   = 2,
    [int]    $ClientCores4  = 4,                            # 클라 스레드를 이 코어 수로 고정 (ClientCores=6-9 → 4)
    [string] $LabelPrefix   = "CSWEEP"
)

$ErrorActionPreference = "Stop"

# ── 안전장치: -Counts 오파싱(PC 먹통) 차단 ───────────────────────────
# powershell -File 로 "-Counts 3999,4999"를 넘기면 콤마가 천단위 구분자로 먹혀
# 단일값 39994999(약 4천만)로 바인딩된다 → 더미 4천만 선할당으로 RAM 고갈·PC 먹통.
# MaxClients(1만) 초과는 오파싱으로 간주하고 즉시 중단(정상 스윕 최대 5499는 통과).
# 콤마 배열은 -File 대신 -Command "& '...ps1' -Counts 3999,4999" 로 실행할 것.
foreach ($c in $Counts) {
    if ($c -gt 10000) {
        throw "ClientCount 이상값 $c (>10000). '-Counts 3999,4999'를 -File로 넘기면 39994999로 오파싱됩니다. -Command 모드로 실행하거나 값을 확인하세요. 입력=[$($Counts -join ',')]"
    }
}

$RunDir = $PSScriptRoot
$Bin    = Join-Path $RunDir "bin"
$Root   = Split-Path $RunDir -Parent
$Mon    = Join-Path $Root "Monitoring"
$SrvIni = Join-Path $Bin "MMOServerConfig.ini"
$StrIni = Join-Path $Bin "MMOStressConfig.ini"
$OutDir = Join-Path $Mon "metrics_out"

# INI 편집: CP949(ANSI). BOM 생기면 GetPrivateProfile이 첫 섹션을 못 읽으므로 Default 유지.
function Set-Ini([string]$file, [string]$key, [string]$value) {
    (Get-Content -Encoding Default $file) -replace "^$key=.*", "$key=$value" |
        Set-Content -Encoding Default $file
}
function Get-Ini([string]$file, [string]$key) {
    $m = (Get-Content -Encoding Default $file) | Select-String "^$key=(.*)"
    if ($m) { $m.Matches[0].Groups[1].Value.Trim() } else { $null }
}
# CSV 문자열 → double (InvariantCulture). 빈값/파싱불가면 $null.
function ToNum([string]$s) {
    $d = 0.0
    if ([double]::TryParse($s, [Globalization.NumberStyles]::Float,
                           [Globalization.CultureInfo]::InvariantCulture, [ref]$d)) { $d } else { $null }
}
# 클라 먼저, 서버 나중 종료 (서버 먼저 죽이면 클라 재접속 루프로 소음).
function Stop-Procs {
    foreach ($n in "MMOStressClient", "GameClient", "MMOServer") {
        try { Stop-Process -Name $n -Force -ErrorAction Stop } catch {}
    }
}

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }
Start-Transcript -Path (Join-Path $OutDir ("clientcount_sweep_{0}.log" -f (Get-Date -Format "yyyyMMdd_HHmmss"))) | Out-Null

# 원값 백업 (finally서 복원)
$origCC  = Get-Ini $StrIni "ClientCount"
$origCPT = Get-Ini $StrIni "ClientsPerThread"
$total = $Counts.Count * $Reps
Write-Host "=== ClientCount 스윕: 동접 $(($Counts | ForEach-Object { $_ + 1 }) -join '/') × ${Reps}rep = ${total}런 | WT=$WorkerThreads K=$SendWorkers | 리빌드 없음(P1+P2 ON 고정) ===" -ForegroundColor Cyan

try {
    # ── 0) 클린 스타트 ──
    Stop-Procs
    foreach ($n in "prometheus", "windows_exporter", "grafana-server") {
        try { Stop-Process -Name $n -Force -ErrorAction Stop } catch {}
    }

    # ── 1) 고정 설정 (동접만 스윕, 나머지는 못박음) ──
    Set-Ini $SrvIni "Mode" "GameServer"
    Set-Ini $SrvIni "UseServerLink" "0"   # 측정 조건 고정 - 서버 간 연결 없이 잰다
    Set-Ini $SrvIni "MonitorEnabled" "1"
    Set-Ini $SrvIni "WorkerThreads" $WorkerThreads
    Set-Ini $SrvIni "SendWorkers"   $SendWorkers
    Set-Ini $StrIni "ServerIp" "127.0.0.1"
    Set-Ini (Join-Path $Bin "ClientConfig.ini") "IP" "127.0.0.1"

    # ── 2) 모니터링 스택 (스윕 전체 1회) ──
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Mon "config\setup.ps1") -StressClientIp localhost
    if ($LASTEXITCODE -ne 0) { throw "setup.ps1 (prometheus 설정 주입) 실패" }
    Start-Process -FilePath (Join-Path $Mon "windows_exporter.exe")
    Start-Process -FilePath (Join-Path $Mon "prometheus-3.4.1.windows-amd64\prometheus.exe") `
                  -WorkingDirectory (Join-Path $Mon "prometheus-3.4.1.windows-amd64") `
                  -ArgumentList "--web.listen-address=:9091"
    Start-Process -FilePath (Join-Path $Mon "grafana\bin\grafana-server.exe") `
                  -WorkingDirectory (Join-Path $Mon "grafana\bin")
    Start-Sleep 5

    # ── 3) 스윕: 동접값마다 INI 세팅 → Reps회 부하런 ──
    $done = 0
    foreach ($cc in $Counts) {
        $cpt = [int][math]::Ceiling(($cc + 1) / $ClientCores4)   # 4스레드 유지 → 오버섭스크립션 회피
        Set-Ini $StrIni "ClientCount"      $cc
        Set-Ini $StrIni "ClientsPerThread" $cpt
        Write-Host ""
        Write-Host ">>> ClientCount=$cc (동접 $($cc + 1)) · ClientsPerThread=$cpt → 스레드 $ClientCores4개" -ForegroundColor Magenta

        for ($rep = 1; $rep -le $Reps; $rep++) {
            $done++
            $label = "{0}_{1}_r{2}" -f $LabelPrefix, $cc, $rep
            Write-Host ""
            Write-Host "[$done/$total] $label  ($(Get-Date -Format 'HH:mm:ss'))" -ForegroundColor Yellow

            Stop-Procs
            Start-Sleep 3
            Start-Process -FilePath (Join-Path $Bin "MMOServer.exe") -WorkingDirectory $Bin

            $ready = $false
            for ($i = 0; $i -lt 30; $i++) {
                if (Get-NetTCPConnection -State Listen -LocalPort 6000 -ErrorAction SilentlyContinue) { $ready = $true; break }
                Start-Sleep 1
            }
            if (-not $ready) { throw "서버가 30초 내에 :6000 리슨을 열지 않음 ($label)" }

            Start-Process -FilePath (Join-Path $Bin "MMOStressClient.exe") -WorkingDirectory $Bin
            Start-Process -FilePath (Join-Path $Bin "GameClient.exe")      -WorkingDirectory $Bin   # 관측용 실클라 1

            for ($m = 1; $m -le $LoadMin; $m++) {
                Start-Sleep 60
                Write-Host ("    부하 {0}/{1}분" -f $m, $LoadMin)
            }

            if (Get-Process MMOServer -ErrorAction SilentlyContinue) {
                & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Mon "metrics-collect.ps1") `
                    -RunLabel $label -WindowMin $WindowMin `
                    -QueriesFile (Join-Path $Mon "queries.json") -OutDir $OutDir
                if ($LASTEXITCODE -ne 0) { Write-Warning "수집 실패: $label (다음 런 계속)" }
            }
            else {
                Write-Warning "서버가 부하 도중 죽음 → 수집 스킵: $label  (★ 여기가 서버 천장일 수 있음)"
            }

            Stop-Procs
            Start-Sleep 60   # 소켓 정리(TIME_WAIT) 휴지
        }
    }

    # ── 4) 요약: 동접↑ 따라 뭐가 먼저 포화하나 (지표 = 행, 동접 = 열) ──
    Write-Host ""
    Write-Host "=== 스윕 요약 (평균) — 서버 천장 + 클라 건강 ===" -ForegroundColor Green
    $csv = Join-Path $OutDir "window_metrics.csv"
    if (Test-Path $csv) {
        $rows = Import-Csv $csv
        $metrics = "gameloop_cpu", "tick_p99_ms", "tick_avg_ms", "broadcast_copy_ms_per_tick",
                   "membership_ms_per_tick", "sendworker_cpu_total", "wsa_send_rate",
                   "dummy_loop_p99_ms", "dummy_send_buffer_full_rate", "dummy_rtt_p99_ms", "session_count"
        $hdr = "{0,-28}" -f "지표 \ 동접"
        foreach ($cc in $Counts) { $hdr += (" | {0,8}" -f ($cc + 1)) }
        Write-Host $hdr -ForegroundColor Cyan
        Write-Host ("-" * $hdr.Length)
        foreach ($mt in $metrics) {
            $line = "{0,-28}" -f $mt
            foreach ($cc in $Counts) {
                $vals = for ($rep = 1; $rep -le $Reps; $rep++) {
                    $lbl = "{0}_{1}_r{2}" -f $LabelPrefix, $cc, $rep
                    ToNum ($rows | Where-Object { $_.RunLabel -eq $lbl -and $_.Metric -eq $mt } | Select-Object -Last 1).Value
                }
                $vals = @($vals | Where-Object { $_ -ne $null })
                $cell = if ($vals.Count) { '{0:N2}' -f (($vals | Measure-Object -Average).Average) } else { '-' }
                $line += (" | {0,8}" -f $cell)
            }
            Write-Host $line
        }
        Write-Host ""
        Write-Host "판독: gameloop_cpu→1.0 근접 or tick_p99 급등 = 게임루프 천장. broadcast_copy가 그 주범인지 확인." -ForegroundColor DarkGray
        Write-Host "      dummy_loop_p99↑(예산 40 근접) or buffer_full>0 or session_count<목표 = 클라/루프백 한계 → 그 동접부터 서버지표 신뢰불가(2nd 머신 필요)." -ForegroundColor DarkGray
        Write-Host "원본 지표 전체: $csv (라벨 ${LabelPrefix}_<count>_r<rep>)" -ForegroundColor Green
    }
    else {
        Write-Warning "요약 스킵: $csv 없음"
    }
}
finally {
    Set-Ini $SrvIni "UseServerLink" "1"   # 위에서 끈 서버 간 연결을 커밋 기준으로 되돌린다
    if ($origCC)  { Set-Ini $StrIni "ClientCount"      $origCC }
    if ($origCPT) { Set-Ini $StrIni "ClientsPerThread" $origCPT }
    Write-Host "MMOStressConfig.ini 원복: ClientCount=$origCC · ClientsPerThread=$origCPT" -ForegroundColor DarkYellow
    Stop-Procs
    Stop-Transcript | Out-Null
}
