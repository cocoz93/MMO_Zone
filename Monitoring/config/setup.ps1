<#
.SYNOPSIS
  MMO 모니터링 설정을 live Grafana/Prometheus 바이너리 트리에 주입한다.
.DESCRIPTION
  Monitoring/config/ 의 템플릿을 읽어 머신 종속 값(대시보드 절대경로, 부하 클라 IP)을
  치환한 뒤, 실제 Grafana provisioning 디렉토리와 Prometheus 디렉토리로 복사한다.
  source of truth 는 항상 Monitoring/config/ 이며, 복사 대상(바이너리 트리)은 .gitignore 됨.
.PARAMETER StressClientIp
  stress_client scrape 대상 IP. 부하 PC가 별도면 그 IP, 단일 PC면 localhost(기본값).
.EXAMPLE
  # 203.0.113.10 은 RFC 5737 문서용 예시 IP. 실제 부하 클라의 공인 IP 로 바꿔서 실행할 것.
  .\setup.ps1 -StressClientIp 203.0.113.10
#>
param(
    [string]$StressClientIp = "localhost"
)

$ErrorActionPreference = "Stop"

# 스크립트 위치 = Monitoring/config, 그 부모 = Monitoring/
$ConfigDir     = $PSScriptRoot
$MonitoringDir = Split-Path -Parent $ConfigDir

# live 바이너리 트리 경로
$GrafanaProvDir = Join-Path $MonitoringDir "grafana\conf\provisioning"

# Prometheus 는 버전 폴더(prometheus-x.y.z-windows-amd64)이므로 동적 탐색
$PromDirItem = Get-ChildItem -Path $MonitoringDir -Directory -Filter "prometheus-*" -ErrorAction SilentlyContinue | Select-Object -First 1

if (-not (Test-Path $GrafanaProvDir)) {
    throw "Grafana provisioning 디렉토리가 없습니다: $GrafanaProvDir`n→ Grafana 바이너리를 Monitoring\grafana\ 에 먼저 배치하세요. (SETUP.md 참고)"
}
if (-not $PromDirItem) {
    throw "Prometheus 디렉토리를 찾을 수 없습니다: $MonitoringDir\prometheus-*`n→ Prometheus 바이너리를 먼저 배치하세요. (SETUP.md 참고)"
}
$PromDir = $PromDirItem.FullName

# 대시보드 절대경로 = live grafana provisioning/dashboards (이 PC 기준 자동 산출)
$DashboardsPath   = Join-Path $GrafanaProvDir "dashboards"
$DatasourcesPath  = Join-Path $GrafanaProvDir "datasources"
New-Item -ItemType Directory -Force $DashboardsPath  | Out-Null
New-Item -ItemType Directory -Force $DatasourcesPath | Out-Null

# UTF-8 (BOM 없음)으로 기록 — Grafana/Prometheus YAML 파서의 BOM 회피
function Write-NoBom([string]$path, [string]$text) {
    [System.IO.File]::WriteAllText($path, $text, (New-Object System.Text.UTF8Encoding($false)))
}

# 1) dashboards.yml : __MMO_DASHBOARDS_PATH__ 치환 후 주입
$dashTpl = Get-Content -Raw (Join-Path $ConfigDir "grafana\provisioning\dashboards\dashboards.yml")
Write-NoBom (Join-Path $DashboardsPath "dashboards.yml") ($dashTpl -replace '__MMO_DASHBOARDS_PATH__', $DashboardsPath)

# 2) 대시보드 JSON 은 전부 머신 비종속 → 그대로 복사
Copy-Item (Join-Path $ConfigDir "grafana\provisioning\dashboards\*.json") $DashboardsPath -Force

# 3) datasource prometheus.yml 도 비종속 → 그대로 복사
Copy-Item (Join-Path $ConfigDir "grafana\provisioning\datasources\prometheus.yml") $DatasourcesPath -Force

# 4) prometheus.yml : __STRESS_CLIENT_IP__ 치환 후 주입
$promTpl = Get-Content -Raw (Join-Path $ConfigDir "prometheus\prometheus.yml")
Write-NoBom (Join-Path $PromDir "prometheus.yml") ($promTpl -replace '__STRESS_CLIENT_IP__', $StressClientIp)

Write-Host ""
Write-Host "[OK] 모니터링 설정 주입 완료" -ForegroundColor Green
Write-Host "  - 대시보드 경로 : $DashboardsPath"
Write-Host "  - stress_client : ${StressClientIp}:9101"
Write-Host "  - Prometheus cfg: $(Join-Path $PromDir 'prometheus.yml')"
Write-Host ""
Write-Host "다음: Prometheus → Grafana 순으로 기동하세요. (SETUP.md 4단계)"
