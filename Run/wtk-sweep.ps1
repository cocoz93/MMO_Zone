<#
.SYNOPSIS
    WT(IOCP 워커 수) x K(SendWorkers) 교차 스윕 — C4000 간섭 실험

.DESCRIPTION
    목적 ①: WorkerThreads 12→4 축소가 tick/RTT 지연을 개선하는지 (처리량 천장은 게임스레드라 지연만 본다)
    목적 ②: C4000에서 K4 tick 저하(평균 45.9ms > 예산 40)가 "코어 대비 스레드 과다" 때문인지 판별
            — WT4에서 K4 페널티가 사라지면 원인 규명 끝, 남으면 캐시/커널 경합(VTune 영역)

    프로토콜: 조합당 Reps회, 런당 LoadMin분 부하 후 마지막 WindowMin분 윈도우 수집 (기존 K스윕과 동일)
    전제    : USE_DB_WORKER=0 빌드 (C4000 베이스라인이 DB 도입 전 측정이라 조건 통일)
    결과    : Monitoring\metrics_out\window_metrics.csv (RunLabel = C4000_WT<wt>_K<k>_r<rep>)
    복원    : 종료/중단 시 INI를 운영 기본값(WorkerThreads=0, SendWorkers=2)으로 되돌림

.EXAMPLE
    .\wtk-sweep.ps1
    # 본실험: WT{12,4} x K{2,4} x 3회 = 12런 (~2.4시간)

    .\wtk-sweep.ps1 -WTList 4 -KList 2 -Reps 1 -LoadMin 1 -WindowMin 1 -LabelPrefix SMOKE
    # 동작 점검용 1런 (~3분)

    .\wtk-sweep.ps1 -WTList 0 -KList 1,2,4,8 -LabelPrefix C4000
    # 기존 SendWorker K스윕과 동일한 축 (WT0=auto)
#>
param(
    [int[]]  $WTList      = @(12, 4),   # 12 = 현행 auto(affinity 12코어)와 같은 값을 명시(라벨 정확성), 4 = 축소안
    [int[]]  $KList       = @(2, 4),
    [int]    $Reps        = 3,
    [int]    $LoadMin     = 10,
    [int]    $WindowMin   = 5,
    [string] $LabelPrefix = "C4000"
)

$ErrorActionPreference = "Stop"

# ── 안전장치: -WTList/-KList 오파싱 차단 ─────────────────────────────
# powershell -File 로 "-KList 1,2,4,8"을 넘기면 콤마가 천단위 구분자로 먹혀
# 단일값 1248 등으로 바인딩된다(예: -WTList 12,4 → 124) → 스레드 수천 개 요청·이상동작.
# 논리코어(12) 대비 16 초과는 오파싱으로 간주하고 중단. 콤마 배열은 -Command 모드로 실행할 것.
foreach ($v in @($WTList) + @($KList)) {
    if ($v -gt 16) {
        throw "WT/K 이상값 $v (>16). '-WTList 12,4'/'-KList 1,2,4,8'을 -File로 넘기면 124/1248로 오파싱됩니다. -Command 모드로 실행하세요. WT=[$($WTList -join ',')] K=[$($KList -join ',')]"
    }
}

$RunDir = $PSScriptRoot
$Bin    = Join-Path $RunDir "bin"
$Mon    = Join-Path (Split-Path $RunDir -Parent) "Monitoring"
$SrvIni = Join-Path $Bin "MMOServerConfig.ini"
$OutDir = Join-Path $Mon "metrics_out"

# INI 편집은 기존 배치(3. MMO_stress.bat)와 동일하게 ANSI(CP949)로 읽고 쓴다.
# BOM이 생기면 GetPrivateProfile이 첫 섹션을 못 읽으므로 Encoding Default를 유지할 것.
function Set-Ini([string]$file, [string]$key, [string]$value) {
    (Get-Content -Encoding Default $file) -replace "^$key=.*", "$key=$value" |
        Set-Content -Encoding Default $file
}
# INI 값 읽기 — finally 복원용 시작값 백업에 사용. 키가 없으면 $null.
function Get-Ini([string]$file, [string]$key) {
    $m = (Get-Content -Encoding Default $file) | Select-String "^$key=(.*)"
    if ($m) { $m.Matches[0].Groups[1].Value.Trim() } else { $null }
}

# 클라 먼저, 서버 나중 순서로 종료 (서버 먼저 죽이면 클라가 재접속 루프를 돌며 소음 발생)
function Stop-Procs {
    foreach ($n in "MMOStressClient", "GameClient", "MMOServer") {
        try { Stop-Process -Name $n -Force -ErrorAction Stop } catch {}
    }
}

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }
Start-Transcript -Path (Join-Path $OutDir ("wtk_sweep_{0}.log" -f (Get-Date -Format "yyyyMMdd_HHmmss"))) | Out-Null

# 원값 백업 (finally서 실행 전 값으로 복원 — 하드코딩 대신, 운영 기본값 변경에 자동 대응)
$origWT = Get-Ini $SrvIni "WorkerThreads"
$origK  = Get-Ini $SrvIni "SendWorkers"
$total = $WTList.Count * $KList.Count * $Reps
Write-Host "=== WT{$($WTList -join ',')} x K{$($KList -join ',')} x ${Reps}회 = ${total}런, 런당 약 $($LoadMin + 2)분 (원값 WT=$origWT K=$origK) ===" -ForegroundColor Cyan

try {
    # ── 0) 초기화: 실행 중인 것 전부 종료 (3. MMO_stress.bat와 동일한 클린 스타트) ──
    Stop-Procs
    foreach ($n in "prometheus", "windows_exporter", "grafana-server") {
        try { Stop-Process -Name $n -Force -ErrorAction Stop } catch {}
    }

    # ── 1) 고정 설정: GameServer 모드 + 모니터링 on + 단일 PC 로컬 IP 강제 ──
    Set-Ini $SrvIni "Mode" "GameServer"
    Set-Ini $SrvIni "UseServerLink" "0"   # 측정 조건 고정 - 서버 간 연결 없이 잰다
    Set-Ini $SrvIni "MonitorEnabled" "1"
    Set-Ini (Join-Path $Bin "MMOStressConfig.ini") "ServerIp" "127.0.0.1"
    Set-Ini (Join-Path $Bin "ClientConfig.ini")    "IP"       "127.0.0.1"

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

    # ── 3) 본 루프 ──
    $done = 0
    foreach ($wt in $WTList) {
        foreach ($k in $KList) {
            for ($rep = 1; $rep -le $Reps; $rep++) {
                $done++
                $label = "{0}_WT{1}_K{2}_r{3}" -f $LabelPrefix, $wt, $k, $rep
                Write-Host ""
                Write-Host "[$done/$total] $label  ($(Get-Date -Format 'HH:mm:ss'))" -ForegroundColor Yellow

                Set-Ini $SrvIni "WorkerThreads" $wt
                Set-Ini $SrvIni "SendWorkers"   $k

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
                Start-Process -FilePath (Join-Path $Bin "GameClient.exe")      -WorkingDirectory $Bin   # 관측용 실클라 1 (3999+1=4000)

                for ($m = 1; $m -le $LoadMin; $m++) {
                    Start-Sleep 60
                    Write-Host ("    부하 {0}/{1}분" -f $m, $LoadMin)
                }

                if (Get-Process MMOServer -ErrorAction SilentlyContinue) {
                    # 수집기는 자체 exit를 쓰므로 반드시 자식 프로세스로 실행 (같은 세션이면 스윕 전체가 종료됨)
                    # -File 자식 실행에선 수집기 param 기본값의 $PSScriptRoot가 비므로 경로를 명시로 넘긴다
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
    }

    Write-Host ""
    Write-Host "=== 스윕 완료. 결과: $OutDir\window_metrics.csv ===" -ForegroundColor Green
    Write-Host "다음: Monitoring\metrics-compare.ps1 로 비교 / 실험 끝나면 BuildConfig.h USE_DB_WORKER=1 복귀" -ForegroundColor Green
}
finally {
    Set-Ini $SrvIni "UseServerLink" "1"   # 위에서 끈 서버 간 연결을 커밋 기준으로 되돌린다
    # 중단되더라도 INI를 실행 전 값으로 복원 (시작값 백업/복원 — clientcount·membership 스윕과 통일)
    if ($origWT) { Set-Ini $SrvIni "WorkerThreads" $origWT }
    if ($origK)  { Set-Ini $SrvIni "SendWorkers"   $origK }
    Stop-Procs
    Stop-Transcript | Out-Null
}
