<#
  rio-ceiling.ps1  -  Unattended CEILING sweep: does RIO buy CAPACITY (not just efficiency)?

  For each arm (IOCP WT4+K3, RIO N4) sweep ClientCount up until the gate breaks; compare each
  arm's breaking point (ceiling) + net_cpu trend into higher load.

  Ceiling gate = tick_p99<40 AND dummy_send_buffer_full==0 AND dummy_loop_p99<40 (client healthy)
                 AND session_count >= target*0.98.
  Reading: game-loop ceiling = gameloop_cpu->1.0 / tick_p99 spike (RIO can't move this, separate core).
           send-path ceiling = buffer_full>0 first (RIO could move this — more efficient send).

  LIMITS: loopback rig -> 5000+ absolute ceiling is TREND ONLY (client+server share the box);
          interpret as IOCP-vs-RIO relative. Client threads pinned to 4 (ClientsPerThread auto) to
          avoid client oversubscription; if dummy_loop_p99 rises, the CLIENT is the limit, not the server.

  IOCP<->RIO = compile-time toggle (rebuild once each). RioWorkers=N4 runtime INI.

  Usage:
    .\rio-ceiling.ps1                 # IOCP,RIO x CC{4500..5500} x1, ~2.2h
    .\rio-ceiling.ps1 -Smoke          # pipeline check, ~10min
#>
param(
  [switch] $Smoke,
  [int]    $Reps          = 1,
  [int]    $LoadMin       = 10,
  [int]    $WindowMin     = 5,
  [int[]]  $ClientCounts  = @(4500,5000,5200,5400,5500),
  [int]    $RioN          = 4,
  [int]    $WorkerThreads = 4,
  [int]    $SendWorkers   = 3,
  [int]    $ClientCores4  = 4,        # pin client to this many threads (ClientCores 6-9 = 4 cores)
  [int]    $MaxClients    = 6000,
  [string] $ServerCores   = "0-5",
  [string] $LabelPrefix   = "CEIL"
)
$ErrorActionPreference = "Stop"
if ($Smoke) { $Reps=1; $LoadMin=1; $WindowMin=1; $ClientCounts=@(4500,5000) }

# safety: comma-int misparse guard (-File "-ClientCounts 4500,5000" -> 45005000 -> RAM blowup)
foreach ($c in $ClientCounts) { if ($c -gt 10000) { throw "ClientCount $c >10000 (misparse? run via -Command). input=[$($ClientCounts -join ',')]" } }

$RioDir = $PSScriptRoot
$Root   = Split-Path $RioDir -Parent
$Bin    = Join-Path $Root "Run\bin"
$Mon    = Join-Path $Root "Monitoring"
$SrvIni = Join-Path $Bin "MMOServerConfig.ini"
$StrIni = Join-Path $Bin "MMOStressConfig.ini"
$BCfg   = Join-Path $Root "ServerCore\Base\CoreConfig.h"
$OutDir = Join-Path $Mon "metrics_out"
$Csv    = Join-Path $OutDir "window_metrics.csv"

function Set-Ini($f,$k,$v){ (Get-Content -Encoding Default $f) -replace "^$k=.*","$k=$v" | Set-Content -Encoding Default $f }
function ToNum($s){ $d=0.0; if([double]::TryParse($s,[Globalization.NumberStyles]::Float,[Globalization.CultureInfo]::InvariantCulture,[ref]$d)){$d}else{$null} }
function Stop-Procs { foreach($n in "MMOStressClient","GameClient","MMOServer"){ try{Stop-Process -Name $n -Force -ErrorAction Stop}catch{} } }
function Reset-Toggle { $t=[IO.File]::ReadAllText($BCfg); $t=$t -replace '#define USE_RIO_TRANSPORT \d','#define USE_RIO_TRANSPORT 0'; [IO.File]::WriteAllText($BCfg,$t,(New-Object Text.UTF8Encoding($true))) }
function Rebuild($arm){ Stop-Procs; Start-Sleep 2; $bat=if($arm -eq "A"){Join-Path $RioDir "build-A-iocp.bat"}else{Join-Path $RioDir "build-B-rio.bat"}; Write-Host ">>> REBUILD arm=$arm" -ForegroundColor Magenta; & cmd /c "`"$bat`""; if($LASTEXITCODE -ne 0){throw "rebuild failed arm=$arm exit=$LASTEXITCODE"} }

if(-not (Test-Path $OutDir)){ New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }
Start-Transcript -Path (Join-Path $OutDir ("rio_ceiling_{0}.log" -f (Get-Date -Format "yyyyMMdd_HHmmss"))) | Out-Null

$arms = @( @{tag="IOCP"; arm="A"; rio=$null}, @{tag="RIO"; arm="B"; rio=$RioN} )
$total = $arms.Count * $ClientCounts.Count * $Reps
$builtArm=""
$srvBak=[IO.File]::ReadAllBytes($SrvIni); $strBak=[IO.File]::ReadAllBytes($StrIni)

Write-Host "=== CEILING sweep: [IOCP(WT$WorkerThreads+K$SendWorkers), RIO(N$RioN)] x CC=$($ClientCounts -join '/') x $Reps = $total runs ===" -ForegroundColor Cyan
Write-Host "    loopback => TREND ONLY (relative IOCP vs RIO). client auto 4-thread. gate: tick_p99<40 & buffer_full=0 & dummy_loop_p99<40 & session~target" -ForegroundColor DarkGray

try {
  Stop-Procs
  foreach($n in "prometheus","windows_exporter","grafana-server"){ try{Stop-Process -Name $n -Force -ErrorAction Stop}catch{} }
  Set-Ini $SrvIni "Mode" "GameServer"; Set-Ini $SrvIni "MonitorEnabled" "1"
  Set-Ini $SrvIni "ServerCores" $ServerCores; Set-Ini $SrvIni "WorkerThreads" $WorkerThreads
  Set-Ini $SrvIni "SendWorkers" $SendWorkers; Set-Ini $SrvIni "MaxClients" $MaxClients; Set-Ini $SrvIni "GameCore" ""
  Set-Ini $StrIni "ServerIp" "127.0.0.1"
  Set-Ini (Join-Path $Bin "ClientConfig.ini") "IP" "127.0.0.1"

  & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Mon "config\setup.ps1") -StressClientIp localhost
  if($LASTEXITCODE -ne 0){ throw "setup.ps1 failed" }
  Start-Process -FilePath (Join-Path $Mon "windows_exporter.exe")
  Start-Process -FilePath (Join-Path $Mon "prometheus-3.4.1.windows-amd64\prometheus.exe") -WorkingDirectory (Join-Path $Mon "prometheus-3.4.1.windows-amd64") -ArgumentList "--web.listen-address=:9091"
  Start-Process -FilePath (Join-Path $Mon "grafana\bin\grafana-server.exe") -WorkingDirectory (Join-Path $Mon "grafana\bin")
  Start-Sleep 5

  $done=0
  foreach($a in $arms){
    if($a.arm -ne $builtArm){ Rebuild $a.arm; $builtArm=$a.arm }
    if($a.rio){ Set-Ini $SrvIni "RioWorkers" $a.rio }
    foreach($cc in $ClientCounts){
      $cpt=[int][math]::Ceiling($cc/$ClientCores4)   # keep 4 client threads -> no client oversubscription
      Set-Ini $StrIni "ClientCount" $cc; Set-Ini $StrIni "ClientsPerThread" $cpt
      for($rep=1;$rep -le $Reps;$rep++){
        $done++
        $label="{0}_{1}_{2}_r{3}" -f $LabelPrefix,$a.tag,$cc,$rep
        Write-Host ""
        Write-Host "[$done/$total] $label (CC=$cc cpt=$cpt) $(Get-Date -Format 'HH:mm:ss')" -ForegroundColor Yellow
        Stop-Procs; Start-Sleep 3
        Start-Process -FilePath (Join-Path $Bin "MMOServer.exe") -WorkingDirectory $Bin
        $ready=$false; for($i=0;$i -lt 30;$i++){ if(Get-NetTCPConnection -State Listen -LocalPort 6000 -ErrorAction SilentlyContinue){$ready=$true;break}; Start-Sleep 1 }
        if(-not $ready){ throw "no :6000 listen ($label)" }
        Start-Process -FilePath (Join-Path $Bin "MMOStressClient.exe") -WorkingDirectory $Bin
        for($m=1;$m -le $LoadMin;$m++){ Start-Sleep 60; Write-Host ("    load {0}/{1}m" -f $m,$LoadMin) }
        if(Get-Process MMOServer -ErrorAction SilentlyContinue){
          & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Mon "metrics-collect.ps1") -RunLabel $label -WindowMin $WindowMin -QueriesFile (Join-Path $Mon "queries.json") -OutDir $OutDir
          if($LASTEXITCODE -ne 0){ Write-Warning "collect failed: $label" }
        } else { Write-Warning "server died during load -> possible ceiling breach: $label" }
        Stop-Procs; Start-Sleep 60
      }
    }
  }

  # ---- summary: per-arm ceiling + trend ----
  Write-Host ""
  Write-Host "=== CEILING SUMMARY (gate=tick_p99<40 & buffer_full=0 & dummy_loop_p99<40 & session>=CC*0.98) ===" -ForegroundColor Green
  if(Test-Path $Csv){
    $rows=Import-Csv $Csv
    function Avg($tag,$cc,$m){ $vals=for($r=1;$r -le $Reps;$r++){ ToNum ($rows|Where-Object{$_.RunLabel -eq ("{0}_{1}_{2}_r{3}" -f $LabelPrefix,$tag,$cc,$r) -and $_.Metric -eq $m -and [int]$_.WindowMin -eq $WindowMin}|Select-Object -Last 1).Value }; $vals=@($vals|Where-Object{$_ -ne $null}); if($vals.Count){($vals|Measure-Object -Average).Average}else{$null} }
    foreach($a in $arms){
      Write-Host ""
      Write-Host ("--- {0} ---" -f $a.tag) -ForegroundColor Cyan
      $ceiling=0
      foreach($cc in $ClientCounts){
        $tp=Avg $a.tag $cc 'tick_p99_ms'; $bf=Avg $a.tag $cc 'dummy_send_buffer_full_rate'; $dl=Avg $a.tag $cc 'dummy_loop_p99_ms'; $s=Avg $a.tag $cc 'session_count'; $nc=Avg $a.tag $cc 'net_cpu_total'; $gl=Avg $a.tag $cc 'gameloop_cpu'
        $pass = ($tp -ne $null -and $tp -lt 40) -and ($bf -ne $null -and $bf -eq 0) -and ($dl -ne $null -and $dl -lt 40) -and ($s -ne $null -and $s -ge $cc*0.98)
        if($pass -and $cc -gt $ceiling){ $ceiling=$cc }
        Write-Host ("  CC {0,5}: tick_p99={1,6} buf_full={2,5} dummy_loop_p99={3,6} session={4,6} net_cpu={5,6} gameloop={6,5}  {7}" -f `
          $cc,("{0:N1}" -f $tp),("{0:N0}" -f $bf),("{0:N1}" -f $dl),("{0:N0}" -f $s),("{0:N2}" -f $nc),("{0:N2}" -f $gl),$(if($pass){"PASS"}else{"FAIL"}))
      }
      Write-Host ("  => {0} ceiling (max CC passing gate) = {1}" -f $a.tag,$(if($ceiling){$ceiling}else{"<lowest load already failed"})) -ForegroundColor Yellow
    }
    Write-Host ""
    Write-Host "NOTE: loopback -> 5000+ absolute ceiling is trend-only; read as IOCP-vs-RIO relative. If both pass to top CC, server ceiling > client capacity here (need 2nd machine)." -ForegroundColor DarkGray
    Write-Host "raw: $Csv (labels ${LabelPrefix}_<arm>_<cc>_r<rep>)" -ForegroundColor DarkGray
  } else { Write-Warning "no CSV produced" }

  Rebuild "A"
}
finally {
  Stop-Procs
  [IO.File]::WriteAllBytes($SrvIni,$srvBak); [IO.File]::WriteAllBytes($StrIni,$strBak)
  Reset-Toggle
  Write-Host "INIs restored + BuildConfig toggle reset to IOCP(0)" -ForegroundColor DarkYellow
  Stop-Transcript | Out-Null
}
