<#
.SYNOPSIS
    게임스레드 코어 격리(GameCore) A/B 자동 수집 — 무인. off(baseline) vs on(게임루프 전용 코어) 비교.

.DESCRIPTION
    코어 격리는 "런타임 INI"(GameCore) 토글이라 리빌드가 필요 없다(멤버십 A/B와 달리 서버 재시작만).
    프로토콜: A(off, GameCore 빈값) → B(on, GameCore=<코어>) 각각 Reps회 부하런.
             런당 LoadMin분 부하 후 WindowMin분 윈도우 수집. 전환은 INI만 바꿔 서버 재기동.
    가설    : 게임루프를 전용 코어로 격리하면 송신/IOCP 워커의 WSASend 메모리트래픽이 게임스레드 L2를
             밀어내는 캐시 간섭이 줄어 gamelogic/tick이 내려간다(균등화로 잃은 ~10% 회복 시도).
    결과    : Monitoring\metrics_out\window_metrics.csv (RunLabel = <Prefix>_A_off_r<rep> / <Prefix>_B_on_r<rep>)
             + 스윕 끝에 rep별 A→B Δ 표와 핵심지표 요약을 콘솔/트랜스크립트에 자동 출력 → 실행 한 번으로 결론까지.
    전제    : USE_RIO_TRANSPORT=0(기존 IOCP 경로) 빌드. ServerCores가 설정돼야 격리가 도출됨(스크립트가 세팅).
             부하클라는 서버(ServerCores) 밖 물리코어에 이미 핀돼 있어야 측정이 깨끗함(MMOStressConfig.ini).
    복원    : 종료/중단 시 INI의 GameCore를 빈값(off=baseline)으로 되돌린다.

.EXAMPLE
    .\affinity-ab.ps1
    # 본실험: off/on × 3회 = 6런 (리빌드 없음, 약 65분)

    .\affinity-ab.ps1 -Reps 1 -LoadMin 1 -WindowMin 1 -LabelPrefix SMOKE
    # 배관 점검용 드라이런(약 8분) — 기동·수집이 도는지 먼저 이걸로 확인 권장

    .\affinity-ab.ps1 -ClientCount 5499 -GameCore 0
    # 천장(5500) 부근 부하에서 격리 효과 측정
#>
param(
    [int]    $Reps          = 3,
    [int]    $LoadMin       = 10,
    [int]    $WindowMin     = 5,
    [int]    $ClientCount   = 4999,     # +관측 GameClient 1 = 5000. 게임스레드가 벽인 부하대(캐시 간섭이 드러남)
    [int]    $WorkerThreads = 4,
    [int]    $SendWorkers   = 3,        # digest 후 운영 K3. 양 팔 공통 고정 — A/B 교란요인 못박기
    [string] $ServerCores   = "0-5",    # 서버 프로세스 물리코어 (GameCore 격리의 전제 — 없으면 격리 도출 불가)
    [int]    $GameCore      = 0,        # B(on)에서 게임루프를 고정할 물리코어 (ServerCores 안이어야 함)
    [string] $LabelPrefix   = "AFF"
)

$ErrorActionPreference = "Stop"
$RunDir = $PSScriptRoot
$Bin    = Join-Path $RunDir "bin"
$Root   = Split-Path $RunDir -Parent
$Mon    = Join-Path $Root "Monitoring"
$SrvIni = Join-Path $Bin "MMOServerConfig.ini"
$OutDir = Join-Path $Mon "metrics_out"

# INI 편집: CP949(ANSI). BOM이 생기면 GetPrivateProfile이 첫 섹션을 못 읽으므로 Default 유지(무BOM).
function Set-Ini([string]$file, [string]$key, [string]$value) {
    (Get-Content -Encoding Default $file) -replace "^$key=.*", "$key=$value" |
        Set-Content -Encoding Default $file
}

# CSV 문자열 → double (InvariantCulture). 빈값/파싱불가면 $null.
function ToNum([string]$s) {
    $d = 0.0
    if ([double]::TryParse($s, [Globalization.NumberStyles]::Float,
                           [Globalization.CultureInfo]::InvariantCulture, [ref]$d)) { $d } else { $null }
}

# 클라 먼저, 서버 나중 종료 (서버 먼저 죽이면 클라가 재접속 루프로 소음).
function Stop-Procs {
    foreach ($n in "MMOStressClient", "GameClient", "MMOServer") {
        try { Stop-Process -Name $n -Force -ErrorAction Stop } catch {}
    }
}

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }
Start-Transcript -Path (Join-Path $OutDir ("affinity_ab_{0}.log" -f (Get-Date -Format "yyyyMMdd_HHmmss"))) | Out-Null

# A/B 두 팔: off(빈값=baseline) → on(GameCore 지정 코어)
$states = @(
    @{ v = "";          tag = "A_off" },
    @{ v = "$GameCore"; tag = "B_on"  }
)
$total = $states.Count * $Reps
Write-Host "=== GameCore 격리 A/B: off/on x ${Reps}회 = ${total}런 (리빌드 없음) | CC=$ClientCount(+1) WT=$WorkerThreads K=$SendWorkers Cores=$ServerCores GameCore=$GameCore ===" -ForegroundColor Cyan

try {
    # ── 0) 클린 스타트 ──
    Stop-Procs
    foreach ($n in "prometheus", "windows_exporter", "grafana-server") {
        try { Stop-Process -Name $n -Force -ErrorAction Stop } catch {}
    }

    # ── 1) 고정 설정 (양 팔 공통 — delta 교란요인이 되지 않게 못박음) ──
    Set-Ini $SrvIni "Mode" "GameServer"
    Set-Ini $SrvIni "UseServerLink" "0"   # 측정 조건 고정 - 서버 간 연결 없이 잰다
    Set-Ini $SrvIni "MonitorEnabled" "1"
    Set-Ini $SrvIni "ServerCores"   $ServerCores      # 격리 도출의 전제
    Set-Ini $SrvIni "WorkerThreads" $WorkerThreads
    Set-Ini $SrvIni "SendWorkers"   $SendWorkers
    Set-Ini (Join-Path $Bin "MMOStressConfig.ini") "ServerIp"    "127.0.0.1"
    Set-Ini (Join-Path $Bin "MMOStressConfig.ini") "ClientCount" $ClientCount
    Set-Ini (Join-Path $Bin "ClientConfig.ini")    "IP"          "127.0.0.1"

    # ── 2) 모니터링 스택 기동 (스윕 전체에 1회) ──
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Mon "config\setup.ps1") -StressClientIp localhost
    if ($LASTEXITCODE -ne 0) { throw "setup.ps1 (prometheus 설정 주입) 실패" }
    Start-Process -FilePath (Join-Path $Mon "windows_exporter.exe")
    Start-Process -FilePath (Join-Path $Mon "prometheus-3.4.1.windows-amd64\prometheus.exe") `
                  -WorkingDirectory (Join-Path $Mon "prometheus-3.4.1.windows-amd64") `
                  -ArgumentList "--web.listen-address=:9091"
    Start-Process -FilePath (Join-Path $Mon "grafana\bin\grafana-server.exe") `
                  -WorkingDirectory (Join-Path $Mon "grafana\bin")
    Start-Sleep 5

    # ── 3) 본 루프: 팔당 INI 세팅(리빌드 없음) → Reps회 부하런 ──
    $done = 0
    foreach ($st in $states) {
        Write-Host ""
        Write-Host ">>> GameCore = '$($st.v)' ($($st.tag)) : INI 세팅 (리빌드 없음, 서버 재기동만)" -ForegroundColor Magenta
        Stop-Procs
        Start-Sleep 2
        Set-Ini $SrvIni "GameCore" $st.v

        for ($rep = 1; $rep -le $Reps; $rep++) {
            $done++
            $label = "{0}_{1}_r{2}" -f $LabelPrefix, $st.tag, $rep
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
                # 수집기는 자체 exit를 쓰므로 반드시 자식 프로세스로 실행 (같은 세션이면 스윕 전체가 종료됨)
                & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Mon "metrics-collect.ps1") `
                    -RunLabel $label -WindowMin $WindowMin `
                    -QueriesFile (Join-Path $Mon "queries.json") -OutDir $OutDir
                if ($LASTEXITCODE -ne 0) { Write-Warning "수집 실패: $label (다음 런 계속)" }
            }
            else {
                Write-Warning "서버가 부하 도중 죽음 → 수집 스킵: $label"
            }

            Stop-Procs
            Start-Sleep 60   # 소켓 정리(TIME_WAIT) 휴지
        }
    }

    Write-Host ""
    Write-Host "=== 스윕 완료. 원본 지표: $OutDir\window_metrics.csv ===" -ForegroundColor Green

    # ── 4) 결론: rep별 A(off)→B(on) Δ 표 + 핵심지표 요약 ──
    $csv           = Join-Path $OutDir "window_metrics.csv"
    $compareScript = Join-Path $Mon "metrics-compare.ps1"
    Write-Host ""
    Write-Host "=== 결론: A(off) → B(on) Δ (rep별) ===" -ForegroundColor Green
    if (Test-Path $csv) {
        for ($rep = 1; $rep -le $Reps; $rep++) {
            $la = "{0}_A_off_r{1}" -f $LabelPrefix, $rep
            $lb = "{0}_B_on_r{1}"  -f $LabelPrefix, $rep
            & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $compareScript -Baseline $la -Variant $lb -CsvPath $csv
        }
        # 핵심지표 한눈 요약. 타깃(격리 시 하락 기대): tick·gamelogic·gameloop_cpu / control(불변이어야 정상): 송신·세션·버퍼풀
        $rows = Import-Csv $csv
        Write-Host ""
        Write-Host "--- 핵심지표 요약 (Δ 음수=개선 / control은 불변이어야 정상) ---" -ForegroundColor Cyan
        $keys = @("tick_p99_ms", "tick_avg_ms", "gamelogic_ms_per_tick", "gameloop_cpu",
                  "sendworker_cpu_total", "wsa_send_rate", "session_count", "dummy_send_buffer_full_rate")
        foreach ($key in $keys) {
            $summary = for ($rep = 1; $rep -le $Reps; $rep++) {
                $la = "{0}_A_off_r{1}" -f $LabelPrefix, $rep
                $lb = "{0}_B_on_r{1}"  -f $LabelPrefix, $rep
                $va = ($rows | Where-Object { $_.RunLabel -eq $la -and $_.Metric -eq $key } | Select-Object -Last 1).Value
                $vb = ($rows | Where-Object { $_.RunLabel -eq $lb -and $_.Metric -eq $key } | Select-Object -Last 1).Value
                $na = ToNum $va; $nb = ToNum $vb
                $delta = if ($null -ne $na -and $null -ne $nb -and $na -ne 0) { "{0:+0.0;-0.0}%" -f ((($nb - $na) / $na) * 100) } else { "" }
                [pscustomobject]@{ Metric = $key; Rep = $rep; A_off = $va; B_on = $vb; Delta = $delta }
            }
            $summary | Format-Table -AutoSize
        }
        Write-Host "원본 지표 전체는 $csv (추가 분석 필요하면 이 CSV 공유)" -ForegroundColor DarkGray
    }
    else {
        Write-Warning "결론 스킵: $csv 없음 (수집이 한 번도 성공 못함)"
    }
}
finally {
    Set-Ini $SrvIni "UseServerLink" "1"   # 위에서 끈 서버 간 연결을 커밋 기준으로 되돌린다
    # 중단되더라도 INI GameCore는 baseline(off=빈값)으로 복원
    Set-Ini $SrvIni "GameCore" ""
    Write-Host "INI GameCore 복원 = 빈값(off, baseline)" -ForegroundColor DarkYellow
    Stop-Procs
    Stop-Transcript | Out-Null
}
