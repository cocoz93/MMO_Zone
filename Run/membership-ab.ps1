<#
.SYNOPSIS
    Phase 1 (USE_MEMBERSHIP_FANOUT_DEDUP) OFF/ON A/B 자동 수집 — 멤버십 아웃바운드 팬아웃 중복 빌드 제거 효과 측정

.DESCRIPTION
    Phase 1은 컴파일타임 #define이라, WT×K(런타임 INI) 스윕과 달리 OFF/ON 각각 "리빌드"가 필요하다.
    이 스크립트가 그 리빌드까지 포함해 무인으로 A(OFF) → B(ON) 지표를 수집하고 서버를 끈다.

    프로토콜: 설정(OFF/ON)당 서버 리빌드 1회 → Reps회 부하런, 런당 LoadMin분 부하 후 WindowMin분 윈도우 수집.
    핵심지표: membership_ms_per_tick / membership_share_pct (queries.json에 이미 정의) + tick_p99_ms · send_per_pkt 등.
    결과    : Monitoring\metrics_out\window_metrics.csv (RunLabel = <Prefix>_A_off_r<rep> / <Prefix>_B_on_r<rep>)
              + 스윕 끝에 rep별 A→B Δ 표와 핵심지표(membership_ms_per_tick·tick_p99_ms·membership_sends_rate) 요약을
                콘솔/트랜스크립트 로그에 자동 출력 → 실행 한 번으로 결론까지.
    복원    : 종료/중단 시 BuildConfig.h의 USE_MEMBERSHIP_FANOUT_DEDUP를 시작 시점 값으로 되돌림.
              (주의: 마지막 빌드가 OFF인 채 중단되면 bin 바이너리는 OFF다 — 정상운영 전 server 리빌드 권장)
    측정점  : ClientCount 기본 3999(+관측 GameClient 1 = 4000). 게임스레드 단일코어가 벽인 프론티어.
              4000이 불안정하면 -ClientCount 2999(=3000)로 낮춰 재현 안정성 우선.
    미변경  : USE_DB_WORKER는 안 건드림(양쪽 동일 → delta 무영향). MySQL 없이 돌리려면 실행 전 수동으로 0.

.EXAMPLE
    .\membership-ab.ps1
    # 본실험: OFF/ON × 3회 = 6런 (리빌드 2회 포함, 약 75분)

    .\membership-ab.ps1 -Reps 1 -LoadMin 1 -WindowMin 1 -LabelPrefix SMOKE
    # 배관 점검용 드라이런(약 8분) — 빌드·기동·수집이 도는지 먼저 이걸로 확인 권장

    .\membership-ab.ps1 -Define USE_MEMBERSHIP_INBOUND_BUNDLE -LabelPrefix MEMB2
    # Phase 2(인바운드 묶음) A/B — OFF=P1만 / ON=P1+P2 ("6. MembershipP2_AB.bat"가 이 조합)
#>
param(
    [int]    $Reps          = 3,
    [int]    $LoadMin       = 10,
    [int]    $WindowMin     = 5,
    [int]    $ClientCount   = 3999,
    [int]    $WorkerThreads = 4,
    [int]    $SendWorkers   = 2,
    [string] $LabelPrefix   = "MEMB",
    [string] $Define        = "USE_MEMBERSHIP_FANOUT_DEDUP"   # A/B로 토글할 #define (P2는 USE_MEMBERSHIP_INBOUND_BUNDLE)
)

$ErrorActionPreference = "Stop"
$RunDir    = $PSScriptRoot
$Bin       = Join-Path $RunDir "bin"
$Root      = Split-Path $RunDir -Parent
$Mon       = Join-Path $Root "Monitoring"
$SrvIni    = Join-Path $Bin "MMOServerConfig.ini"
$BuildCfg  = Join-Path $Root "MMOServer\MMOServer\BuildConfig.h"
$ServerBuild = Join-Path $Root "build-vs"   # CMake 빌드 트리 (vcxproj/sln 은 생성물)
$OutDir    = Join-Path $Mon "metrics_out"

# INI 편집: CP949(ANSI). BOM이 생기면 GetPrivateProfile이 첫 섹션을 못 읽으므로 Default 유지.
function Set-Ini([string]$file, [string]$key, [string]$value) {
    (Get-Content -Encoding Default $file) -replace "^$key=.*", "$key=$value" |
        Set-Content -Encoding Default $file
}

# #define 값 편집: Latin1(28591)로 바이트 무손실 왕복 → BOM·CRLF·한글주석 그대로 두고 숫자만 교체.
function Set-Define([string]$file, [string]$name, [int]$val) {
    $enc  = [System.Text.Encoding]::GetEncoding(28591)
    $text = $enc.GetString([System.IO.File]::ReadAllBytes($file))
    $new  = [regex]::Replace($text, "(#define\s+$name\s+)\d+", ('${1}' + $val))
    [System.IO.File]::WriteAllBytes($file, $enc.GetBytes($new))
}
function Get-Define([string]$file, [string]$name) {
    $enc  = [System.Text.Encoding]::GetEncoding(28591)
    $text = $enc.GetString([System.IO.File]::ReadAllBytes($file))
    if ($text -match "#define\s+$name\s+(\d+)") { return [int]$Matches[1] }
    throw "$name 정의를 $file 에서 못 찾음"
}

# CSV 문자열 → double (InvariantCulture — metrics-compare.ps1과 동일 파싱). 빈값/파싱불가면 $null.
function ToNum([string]$s) {
    $d = 0.0
    if ([double]::TryParse($s, [Globalization.NumberStyles]::Float,
                           [Globalization.CultureInfo]::InvariantCulture, [ref]$d)) { $d } else { $null }
}

# 서버만 Release x64 빌드 — CMake 정본이라 cmake --build 로 부른다.
#   (.IOCP_build.bat은 pause가 있어 무인용으론 쓰지 않는다)
function Build-Server {
    $cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
    if (-not $cmake) { $cmake = Join-Path $env:ProgramFiles "CMake\bin\cmake.exe" }
    if (-not (Test-Path $cmake)) { throw "cmake 없음 (winget install Kitware.CMake)" }
    if (-not (Test-Path (Join-Path $ServerBuild "MMO.sln"))) {
        Write-Host "    구성: $ServerBuild" -ForegroundColor DarkGray
        & $cmake -S $Root -B $ServerBuild -G "Visual Studio 17 2022" -A x64
        if ($LASTEXITCODE -ne 0) { throw "서버 구성 실패 ($Define=$val)" }
    }
    Write-Host "    빌드: $ServerBuild (Release x64)" -ForegroundColor DarkGray
    & $cmake --build $ServerBuild --config Release
    if ($LASTEXITCODE -ne 0) { throw "서버 빌드 실패 ($Define=$val)" }
}

# 클라 먼저, 서버 나중 종료 (서버 먼저 죽이면 클라가 재접속 루프로 소음).
function Stop-Procs {
    foreach ($n in "MMOStressClient", "GameClient", "MMOServer") {
        try { Stop-Process -Name $n -Force -ErrorAction Stop } catch {}
    }
}

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }
Start-Transcript -Path (Join-Path $OutDir ("membership_ab_{0}.log" -f (Get-Date -Format "yyyyMMdd_HHmmss"))) | Out-Null

$origDefine = Get-Define $BuildCfg $Define
$states = @(
    @{ v = 0; tag = "A_off" },
    @{ v = 1; tag = "B_on"  }
)
$total = $states.Count * $Reps
Write-Host "=== $Define A/B: OFF/ON x ${Reps}회 = ${total}런 (리빌드 2회 포함) | ClientCount=$ClientCount(+1) WT=$WorkerThreads K=$SendWorkers | 원값=$origDefine ===" -ForegroundColor Cyan

try {
    # ── 0) 클린 스타트 ──
    Stop-Procs
    foreach ($n in "prometheus", "windows_exporter", "grafana-server") {
        try { Stop-Process -Name $n -Force -ErrorAction Stop } catch {}
    }

    # ── 1) 고정 설정 (양쪽 공통 — 이것들이 delta의 교란요인이 되지 않게 못박음) ──
    Set-Ini $SrvIni "Mode" "GameServer"
    Set-Ini $SrvIni "MonitorEnabled" "1"
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

    # ── 3) 본 루프: 설정당 리빌드 1회 → Reps회 부하런 ──
    $done = 0
    foreach ($st in $states) {
        Write-Host ""
        Write-Host ">>> $Define = $($st.v) ($($st.tag)) : BuildConfig 수정 + 서버 리빌드" -ForegroundColor Magenta
        Stop-Procs
        Start-Sleep 2
        Set-Define $BuildCfg $Define $st.v
        $script:val = $st.v   # 빌드 실패 메시지용
        Build-Server

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

    # ── 4) 결론: rep별 A(off)→B(on) Δ 표 + 핵심지표 요약 (실행→결론 무인 완결) ──
    $csv           = Join-Path $OutDir "window_metrics.csv"
    $compareScript = Join-Path $Mon "metrics-compare.ps1"
    Write-Host ""
    Write-Host "=== 결론: A(off) → B(on) Δ (rep별) ===" -ForegroundColor Green
    if (Test-Path $csv) {
        # 전체 지표 Δ 표: metrics-compare를 '자식 프로세스'로 실행 — 라벨 누락 시 자체 exit가 부모 루프까지 죽이지 않게.
        for ($rep = 1; $rep -le $Reps; $rep++) {
            $la = "{0}_A_off_r{1}" -f $LabelPrefix, $rep
            $lb = "{0}_B_on_r{1}"  -f $LabelPrefix, $rep
            & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $compareScript -Baseline $la -Variant $lb -CsvPath $csv
        }
        # 핵심지표 한눈 요약: membership(타깃)·tick_p99(천장 이동 여부)·membership_sends(통제-불변이어야 정상).
        $rows = Import-Csv $csv
        Write-Host ""
        Write-Host "--- 핵심지표 요약 (lower_is_better: Δ 음수면 개선 / sends는 불변이어야 정상) ---" -ForegroundColor Cyan
        foreach ($key in @("membership_ms_per_tick", "tick_p99_ms", "membership_sends_rate")) {
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
    # 중단되더라도 BuildConfig.h는 시작 시점 값으로 복원
    Set-Define $BuildCfg $Define $origDefine
    Write-Host "BuildConfig.h $Define 원복 = $origDefine (마지막 빌드가 OFF였다면 정상운영 전 'server' 리빌드 권장)" -ForegroundColor DarkYellow
    Stop-Procs
    Stop-Transcript | Out-Null
}
