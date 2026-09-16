# ==========================================================================
# RIO Application Verifier 런너 — 관리자 권한으로 실행(RunAs 상승) 전용.
#   Handles+Locks 검사로 MMOServer.exe를 후킹 → 중간규모 부하(safety-soak) →
#   위반 로그 수집 → 해제. 결과는 appverif-result.txt / appverif-log.xml 에 남긴다.
#
#   · Heaps(PageHeap)는 메모리 폭증이라 이번엔 제외 — RIO 핵심(닫힌 핸들 재사용·이중
#     close·락 오용)은 Handles/Locks가 커버. Heaps는 필요 시 더 작은 규모로 별도.
#   · -disable(해제)은 finally로 보장 — IFEO 잔존(그 exe 영구 후킹) 방지.
#   · 위반이 나면: 후킹된 서버가 verifier stop으로 비정상 종료 → safety-soak이
#     graceful 실패/조기종료로 잡고, appverif-log.xml에도 stop 엔트리가 남는다.
# ==========================================================================
param([int]$Clients = 800, [int]$Reps = 2, [int]$DurationSec = 45)
$ErrorActionPreference = 'Continue'

$exe    = 'MMOServer.exe'
$rioDir = $PSScriptRoot
$out    = Join-Path $rioDir 'appverif-result.txt'
$logXml = Join-Path $rioDir 'appverif-log.xml'

"[appverif-run] start $(Get-Date -Format s)  Clients=$Clients Reps=$Reps Dur=$DurationSec" | Out-File $out

# 관리자 확인 (RunAs 상승 안 됐으면 조기 종료)
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltinRole]::Administrator)
if (-not $admin) { "ERROR: not elevated — RunAs required" | Out-File -Append $out; exit 3 }

try {
    "== enable Handles Locks for $exe ==" | Out-File -Append $out
    & appverif -enable Handles Locks -for $exe *>&1 | Out-File -Append $out

    "== soak under AppVerifier (server hooked via IFEO) ==" | Out-File -Append $out
    & (Join-Path $rioDir 'safety-soak.ps1') -ExpectTransport RIO -Clients $Clients -Reps $Reps -DurationSec $DurationSec *>&1 | Out-File -Append $out
    "soak exit = $LASTEXITCODE" | Out-File -Append $out

    "== export verifier log ==" | Out-File -Append $out
    if (Test-Path $logXml) { Remove-Item $logXml -Force -ErrorAction SilentlyContinue }
    & appverif -export log -for $exe -with To=$logXml *>&1 | Out-File -Append $out
    if (Test-Path $logXml) {
        $xml = Get-Content $logXml -Raw
        $stops = ([regex]::Matches($xml, '<avrf:logEntry')).Count
        "verifier log entries (logEntry count) = $stops" | Out-File -Append $out
    } else {
        "no verifier log exported (likely no stops)" | Out-File -Append $out
    }
}
finally {
    "== disable (cleanup — IFEO 해제) ==" | Out-File -Append $out
    & appverif -disable * -for $exe *>&1 | Out-File -Append $out
}

"[appverif-run] done $(Get-Date -Format s)" | Out-File -Append $out
