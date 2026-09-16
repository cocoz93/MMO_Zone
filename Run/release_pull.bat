@echo off
REM ============================================
REM   MMO - Download latest EXEs (no auth, no build)
REM   For a PC that has the repo (git pull) but cannot build.
REM   Fetches the newest 'bin-latest' release zip from GitHub and
REM   drops ONLY the .exe files into .\bin .
REM   Your git-managed batch files and *.ini are left untouched.
REM   Requirements: Windows PowerShell + internet. Public repo.
REM ============================================
setlocal
set "DEST=%~dp0"

echo ============================================
echo   Downloading latest MMO EXEs (bin-latest)...
echo ============================================
echo.

powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; $h=@{'User-Agent'='mmo-fetch'}; $rel=Invoke-RestMethod -UseBasicParsing -Uri 'https://api.github.com/repos/cocoz93/MMO_Zone/releases/tags/bin-latest' -Headers $h; $a=$rel.assets | Where-Object { $_.name -like 'MMO_Run_*.zip' } | Select-Object -First 1; if(-not $a){ throw 'release asset not found' }; Write-Host ('  asset : ' + $a.name); $zip=Join-Path $env:TEMP $a.name; Invoke-WebRequest -UseBasicParsing -Uri $a.browser_download_url -OutFile $zip; $tmp=Join-Path $env:TEMP 'mmo_fetch_extract'; if(Test-Path $tmp){ Remove-Item $tmp -Recurse -Force }; Expand-Archive -Path $zip -DestinationPath $tmp -Force; $bin=Join-Path $env:DEST 'bin'; if(-not (Test-Path $bin)){ New-Item -ItemType Directory -Path $bin | Out-Null }; Copy-Item -Path (Join-Path $tmp 'Run\bin\*.exe') -Destination $bin -Force; Remove-Item $tmp -Recurse -Force; Write-Host ('  exe   -> ' + $bin)"
if %ERRORLEVEL% NEQ 0 (
    echo.
    echo [FAILED] Download error. Check internet / repo access above.
    pause
    exit /b 1
)

echo.
echo ============================================
echo   [OK] Latest EXEs are now in .\bin
echo        (batch / ini kept as-is)
echo ============================================
pause
exit /b 0
