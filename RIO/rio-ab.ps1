<#
  rio-ab.ps1  -  Unattended RIO vs IOCP transport comparison (Phase 2 measurement).

  Compares an already-tuned IOCP baseline (WT4 + SendWorker K3) against RIO transport
  (unified RioWorker N, swept N in {2,3,4}) at a FIXED sub-ceiling load. Everything above
  the transport layer is identical (ring buffer, coalescing, sector/membership bundle,
  game loop) -> the delta isolates the transport syscall cost.

  Controlled variable = LOAD (fixed ClientCount). Thread count is each arch's own knob:
  IOCP = WT4+K3 (7 threads), RIO = RioWorker N (swept). Compare best-vs-best.

  Headline = network-thread CPU (worker+sendworker) and its KERNEL fraction
  (mmo_thread_kernel_ratio, added step 1). Gate = buffer_full==0 & tick_p99<40 & equal load.
  Ceiling (~5000) is game-loop bound and RIO does NOT touch it -> do NOT compare max clients.

  IOCP<->RIO is a COMPILE-TIME toggle -> rebuild between arms (build-A-iocp / build-B-rio).
  RioWorkers is a RUNTIME INI key -> swept without rebuild inside the RIO arm.

  Output: Monitoring\metrics_out\window_metrics.csv (RunLabel = RIO_IOCP_r# / RIO_N2_r# / ...)
          + console/transcript summary (best IOCP vs best RIO).

  Usage:
    .\rio-ab.ps1                 # full: IOCP + RIO{2,3,4} x Reps, ~2h
    .\rio-ab.ps1 -Smoke          # pipeline check: IOCP + RIO{2} x1, 1min load, ~10min
    .\rio-ab.ps1 -IncludeN7      # add RIO N=7 (equal-thread-count diagnostic)
#>
param(
  [switch] $Smoke,
  [switch] $IncludeN7,
  [int]    $Reps          = 2,
  [int]    $LoadMin       = 10,
  [int]    $WindowMin     = 5,
  [int]    $ClientCount   = 4000,    # sub-ceiling (ceiling ~5000 = game-loop bound); gates pass, heavy net load
  [int]    $MaxClients    = 6000,    # RIO slab = MaxClients*128KB pinned = 750MB
  [int]    $WorkerThreads = 4,       # IOCP arm WT (RIO arm ignores; uses RioWorkers)
  [int]    $SendWorkers   = 3,       # IOCP arm K
  [string] $ServerCores   = "0-5",
  [string] $LabelPrefix   = "RIO",
  [int[]]  $RioNs         = @()      # override N sweep (e.g. 4,5,6,7); empty = default {2,3,4}
)
$ErrorActionPreference = "Stop"
if ($Smoke) { $Reps = 1; $LoadMin = 1; $WindowMin = 1; $ClientCount = 2000 }

$RioDir = $PSScriptRoot
$Root   = Split-Path $RioDir -Parent
$Bin    = Join-Path $Root "Run\bin"
$Mon    = Join-Path $Root "Monitoring"
$SrvIni = Join-Path $Bin "MMOServerConfig.ini"
$CliIni = Join-Path $Bin "MMOStressConfig.ini"
$BCfg   = Join-Path $Root "ServerCore\Base\CoreConfig.h"
$OutDir = Join-Path $Mon "metrics_out"
$Csv    = Join-Path $OutDir "window_metrics.csv"

# INI edit: CP949 (ANSI, no BOM). Set-Content -Encoding Default keeps codepage 949 on this box.
function Set-Ini([string]$file,[string]$key,[string]$value){
  (Get-Content -Encoding Default $file) -replace "^$key=.*","$key=$value" | Set-Content -Encoding Default $file
}
function ToNum([string]$s){ $d=0.0; if([double]::TryParse($s,[Globalization.NumberStyles]::Float,[Globalization.CultureInfo]::InvariantCulture,[ref]$d)){$d}else{$null} }
function Stop-Procs { foreach($n in "MMOStressClient","GameClient","MMOServer"){ try{ Stop-Process -Name $n -Force -ErrorAction Stop }catch{} } }
function Reset-Toggle {  # source USE_RIO_TRANSPORT -> 0 (fast, no rebuild) for a clean resting tree
  $t=[IO.File]::ReadAllText($BCfg); $t=$t -replace '#define USE_RIO_TRANSPORT \d','#define USE_RIO_TRANSPORT 0'
  [IO.File]::WriteAllText($BCfg,$t,(New-Object Text.UTF8Encoding($true)))
}
function Rebuild([string]$arm){
  Stop-Procs; Start-Sleep 2
  $bat = if($arm -eq "A"){ Join-Path $RioDir "build-A-iocp.bat" } else { Join-Path $RioDir "build-B-rio.bat" }
  Write-Host ">>> REBUILD arm=$arm ($(Split-Path $bat -Leaf))" -ForegroundColor Magenta
  & cmd /c "`"$bat`""
  if($LASTEXITCODE -ne 0){ throw "rebuild failed (arm=$arm exit=$LASTEXITCODE)" }
}

if(-not (Test-Path $OutDir)){ New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }
Start-Transcript -Path (Join-Path $OutDir ("rio_ab_{0}.log" -f (Get-Date -Format "yyyyMMdd_HHmmss"))) | Out-Null

$rioNs = if($Smoke){ @(2) } elseif($RioNs.Count){ $RioNs } elseif($IncludeN7){ @(2,3,4,7) } else { @(2,3,4) }
$configs = @( @{ tag="IOCP"; arm="A"; rio=$null } )
foreach($n in $rioNs){ $configs += @{ tag="N$n"; arm="B"; rio=$n } }
$total = $configs.Count * $Reps
$builtArm = ""

Write-Host "=== RIO vs IOCP : $($configs.Count) configs x $Reps reps = $total runs | CC=$ClientCount Load=${LoadMin}m Win=${WindowMin}m Cores=$ServerCores ===" -ForegroundColor Cyan
$configs | ForEach-Object { Write-Host ("   - {0} (arm {1}{2})" -f $_.tag,$_.arm,$(if($_.rio){", RioWorkers=$($_.rio)"}else{" WT$WorkerThreads+K$SendWorkers"})) }

$srvBak = [IO.File]::ReadAllBytes($SrvIni)
$cliBak = [IO.File]::ReadAllBytes($CliIni)

try {
  # ---- 0) clean start ----
  Stop-Procs
  foreach($n in "prometheus","windows_exporter","grafana-server"){ try{ Stop-Process -Name $n -Force -ErrorAction Stop }catch{} }

  # ---- 1) fixed common INI (both arms) - lock every delta confounder ----
  Set-Ini $SrvIni "Mode" "GameServer"
  Set-Ini $SrvIni "MonitorEnabled" "1"
  Set-Ini $SrvIni "ServerCores"   $ServerCores
  Set-Ini $SrvIni "WorkerThreads" $WorkerThreads
  Set-Ini $SrvIni "SendWorkers"   $SendWorkers
  Set-Ini $SrvIni "MaxClients"    $MaxClients
  Set-Ini $SrvIni "GameCore"      ""
  Set-Ini $CliIni "ServerIp"      "127.0.0.1"
  Set-Ini $CliIni "ClientCount"   $ClientCount
  Set-Ini $CliIni "ClientsPerThread" ([string][int][math]::Ceiling($ClientCount/4.0))  # keep 4 client threads (ClientCores 6-9) -> no client oversubscription

  # ---- 2) monitoring stack (once for the whole sweep) ----
  & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Mon "config\setup.ps1") -StressClientIp localhost
  if($LASTEXITCODE -ne 0){ throw "setup.ps1 (prometheus target inject) failed" }
  Start-Process -FilePath (Join-Path $Mon "windows_exporter.exe")
  Start-Process -FilePath (Join-Path $Mon "prometheus-3.4.1.windows-amd64\prometheus.exe") `
                -WorkingDirectory (Join-Path $Mon "prometheus-3.4.1.windows-amd64") -ArgumentList "--web.listen-address=:9091"
  Start-Process -FilePath (Join-Path $Mon "grafana\bin\grafana-server.exe") -WorkingDirectory (Join-Path $Mon "grafana\bin")
  Start-Sleep 5

  # ---- 3) main loop: rebuild on arm change, sweep RioWorkers within RIO arm ----
  $done = 0
  foreach($cfg in $configs){
    if($cfg.arm -ne $builtArm){ Rebuild $cfg.arm; $builtArm = $cfg.arm }
    if($cfg.rio){ Set-Ini $SrvIni "RioWorkers" $cfg.rio }

    for($rep=1; $rep -le $Reps; $rep++){
      $done++
      $label = "{0}_{1}_r{2}" -f $LabelPrefix,$cfg.tag,$rep
      Write-Host ""
      Write-Host "[$done/$total] $label  ($(Get-Date -Format 'HH:mm:ss'))" -ForegroundColor Yellow
      Stop-Procs; Start-Sleep 3
      Start-Process -FilePath (Join-Path $Bin "MMOServer.exe") -WorkingDirectory $Bin
      $ready=$false
      for($i=0;$i -lt 30;$i++){ if(Get-NetTCPConnection -State Listen -LocalPort 6000 -ErrorAction SilentlyContinue){$ready=$true;break}; Start-Sleep 1 }
      if(-not $ready){ throw "server did not listen on :6000 in 30s ($label)" }
      Start-Process -FilePath (Join-Path $Bin "MMOStressClient.exe") -WorkingDirectory $Bin

      for($m=1;$m -le $LoadMin;$m++){ Start-Sleep 60; Write-Host ("    load {0}/{1}m" -f $m,$LoadMin) }

      if(Get-Process MMOServer -ErrorAction SilentlyContinue){
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Mon "metrics-collect.ps1") `
            -RunLabel $label -WindowMin $WindowMin -QueriesFile (Join-Path $Mon "queries.json") -OutDir $OutDir
        if($LASTEXITCODE -ne 0){ Write-Warning "collect failed: $label (continue)" }
        # transport A/B assert: binary must match the arm we think we built (incremental-build mislabel guard)
        $expTr = if($cfg.rio){1}else{0}
        $gotTr = (Import-Csv $Csv | Where-Object { $_.RunLabel -eq $label -and $_.Metric -eq 'transport_rio' } | Select-Object -Last 1).Value
        if($null -ne $gotTr -and [int]$gotTr -ne $expTr){ throw "TRANSPORT MISLABEL: $label expected rio=$expTr but binary reports $gotTr -- arm/binary mismatch, aborting to avoid corrupt data" }
      } else { Write-Warning "server died during load -> skip collect: $label" }

      Stop-Procs; Start-Sleep 60   # TIME_WAIT / socket drain rest
    }
  }

  # ---- 4) summary: per-config avg + best IOCP vs best RIO ----
  Write-Host ""
  Write-Host "=== SUMMARY (avg across $Reps reps) ===" -ForegroundColor Green
  if(Test-Path $Csv){
    $rows = Import-Csv $Csv
    function Avg($tag,$metric){
      $vals = for($r=1;$r -le $Reps;$r++){ ToNum (($rows | Where-Object { $_.RunLabel -eq ("{0}_{1}_r{2}" -f $LabelPrefix,$tag,$r) -and $_.Metric -eq $metric -and [int]$_.WindowMin -eq $WindowMin } | Select-Object -Last 1).Value) }
      $vals = @($vals | Where-Object { $_ -ne $null })
      if($vals.Count){ ($vals | Measure-Object -Average).Average } else { $null }
    }
    $keys = "net_cpu_total","net_kernel_cpu_total","worker_cpu_total","sendworker_cpu_total","gameloop_cpu","tick_p99_ms","dummy_loop_p99_ms","dummy_send_buffer_full_rate","dummy_send_pps","wsa_send_rate","session_count"
    $tbl = foreach($cfg in $configs){
      $o = [ordered]@{ Config=$cfg.tag }
      foreach($k in $keys){ $v=Avg $cfg.tag $k; $o[$k]= if($v -ne $null){[math]::Round($v,4)}else{"NA"} }
      $bf=Avg $cfg.tag "dummy_send_buffer_full_rate"; $tp=Avg $cfg.tag "tick_p99_ms"; $sc=Avg $cfg.tag "session_count"
      $o["GATE"]= if(($bf -ne $null -and $bf -eq 0) -and ($tp -ne $null -and $tp -lt 40) -and ($sc -ne $null -and $sc -ge $ClientCount*0.98)){"PASS"}else{"FAIL"}
      [pscustomobject]$o
    }
    $tbl | Format-Table Config,net_cpu_total,net_kernel_cpu_total,worker_cpu_total,sendworker_cpu_total,tick_p99_ms,dummy_send_buffer_full_rate,dummy_send_pps,session_count,GATE -AutoSize

    $iocp = $tbl | Where-Object { $_.Config -eq "IOCP" } | Select-Object -First 1
    $rioPass = @($tbl | Where-Object { $_.Config -ne "IOCP" -and $_.GATE -eq "PASS" -and $_.net_cpu_total -ne "NA" } | Sort-Object { [double]$_.net_cpu_total })
    if($iocp -and $iocp.GATE -eq "PASS" -and $rioPass.Count){
      $best = $rioPass[0]
      $ic=[double]$iocp.net_cpu_total; $rc=[double]$best.net_cpu_total
      $ik=[double]$iocp.net_kernel_cpu_total; $rk=[double]$best.net_kernel_cpu_total
      $pc= if($ic -ne 0){ ($rc-$ic)/$ic*100 } else {0}
      $pk= if($ik -ne 0){ ($rk-$ik)/$ik*100 } else {0}
      Write-Host ""
      Write-Host ("VERDICT: best RIO = {0}" -f $best.Config) -ForegroundColor Cyan
      Write-Host ("  net CPU     IOCP {0,-8} -> RIO {1,-8}  ({2:+0.0;-0.0}%)" -f $ic,$rc,$pc)
      Write-Host ("  net KERNEL  IOCP {0,-8} -> RIO {1,-8}  ({2:+0.0;-0.0}%)  <- syscall burden recovered" -f $ik,$rk,$pk)
      $ip=[double]$iocp.dummy_send_pps; $rp=[double]$best.dummy_send_pps
      $pp= if($ip -ne 0){ ($rp-$ip)/$ip*100 } else {0}
      Write-Host ("  load(pps)   IOCP {0,-8} -> RIO {1,-8}  ({2:+0.0;-0.0}%)  <- load asymmetry; if |%|>3 quote net_cpu_per_kpps, not raw" -f $ip,$rp,$pp)
      Write-Host ("  (negative net % = RIO cheaper. load-normalized: net_cpu_per_kpps / net_cpu_per_mmemb.)")
    } else { Write-Warning "verdict skipped (IOCP gate fail or no gate-passing RIO config)" }
    Write-Host "raw metrics: $Csv" -ForegroundColor DarkGray
  } else { Write-Warning "no CSV produced (collection never succeeded)" }

  # ---- 5) restore binary to IOCP default (normal path) ----
  Rebuild "A"
}
finally {
  Stop-Procs
  [IO.File]::WriteAllBytes($SrvIni,$srvBak)
  [IO.File]::WriteAllBytes($CliIni,$cliBak)
  Reset-Toggle
  Write-Host "INIs restored + BuildConfig toggle reset to IOCP(0)" -ForegroundColor DarkYellow
  Stop-Transcript | Out-Null
}
