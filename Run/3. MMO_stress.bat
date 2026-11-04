@echo off
setlocal

echo ============================================
echo   MMO Stress Test - GameServer Mode
echo ============================================
echo.

REM === 1. Kill running processes ===
REM 모니터링·서버가 실행 중이든 아니든 무조건 종료 후 재기동한다.
REM setup.ps1로 재주입한 prometheus.yml이 확실히 반영되게 하기 위해서다. (taskkill은 대상 없어도 무해)
echo [1/5] Killing running processes...
taskkill /F /IM MMOServer.exe >nul 2>nul
taskkill /F /IM MMOStressClient.exe >nul 2>nul
taskkill /F /IM GameClient.exe >nul 2>nul
taskkill /F /IM prometheus.exe >nul 2>nul
taskkill /F /IM windows_exporter.exe >nul 2>nul
taskkill /F /IM grafana-server.exe >nul 2>nul
echo   - Done
echo.

REM === 2. bin 산출물 확인 (없으면 .build.bat 먼저) ===
echo [2/5] Checking build output...
if not exist "%~dp0bin\MMOServer.exe" (
    echo [MISSING] bin\MMOServer.exe
    goto :NEED_BUILD
)
if not exist "%~dp0bin\MMOStressClient.exe" (
    echo [MISSING] bin\MMOStressClient.exe
    goto :NEED_BUILD
)
if not exist "%~dp0bin\GameClient.exe" (
    echo [MISSING] bin\GameClient.exe
    goto :NEED_BUILD
)
echo   - OK
echo.

REM === 4. Configure ===
echo [3/5] Configuring...
powershell -Command "(Get-Content -Encoding Default '%~dp0bin\MMOServerConfig.ini') -replace '^Mode=.*', 'Mode=GameServer' -replace '^MonitorEnabled=.*', 'MonitorEnabled=1' -replace '^UseServerLink=.*', 'UseServerLink=0' | Set-Content -Encoding Default '%~dp0bin\MMOServerConfig.ini'"
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] MMOServerConfig.ini update failed!
    goto :ERROR
)
echo   - MMOServerConfig.ini updated (Mode=GameServer, MonitorEnabled=1, UseServerLink=0)

REM 단일 PC 사용: 서버/클라가 같은 머신이므로 접속 IP를 127.0.0.1로 강제한다.
REM (3-2 클라 PC 배치는 IP를 건드리지 않지만, 3번은 무조건 로컬 보장이 목적)
powershell -Command "(Get-Content -Encoding Default '%~dp0bin\MMOStressConfig.ini') -replace '^ServerIp=.*', 'ServerIp=127.0.0.1' | Set-Content -Encoding Default '%~dp0bin\MMOStressConfig.ini'"
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] MMOStressConfig.ini update failed!
    goto :ERROR
)
echo   - MMOStressConfig.ini updated (ServerIp=127.0.0.1, single-PC local)

powershell -Command "(Get-Content -Encoding Default '%~dp0bin\ClientConfig.ini') -replace '^IP=.*', 'IP=127.0.0.1' | Set-Content -Encoding Default '%~dp0bin\ClientConfig.ini'"
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] ClientConfig.ini update failed!
    goto :ERROR
)
echo   - ClientConfig.ini updated (IP=127.0.0.1, single-PC local)

REM 단일 PC 사용: 부하 클라가 로컬에 있으므로 Prometheus stress_client 타깃을 localhost로 주입.
powershell -ExecutionPolicy Bypass -File "%~dp0..\Monitoring\config\setup.ps1" -StressClientIp localhost
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] setup.ps1 injection failed!
    goto :ERROR
)
echo   - Prometheus config injected (stress_client=localhost:9101)
echo.

REM === 5. Start Monitoring ===
echo [4/5] Starting monitoring...
start "" "%~dp0..\Monitoring\windows_exporter.exe"
echo   - windows_exporter started (:9182)

start "" /D "%~dp0..\Monitoring\prometheus-3.4.1.windows-amd64" prometheus.exe --web.listen-address=":9091"
echo   - Prometheus started (:9091)

start "" /D "%~dp0..\Monitoring\grafana\bin" grafana-server.exe
echo   - Grafana started (:3000)

timeout /t 3 /nobreak >nul
start http://localhost:9091
start http://localhost:3000
echo.

REM === 6. Run ===
echo [5/5] Starting...
start "" /D "%~dp0bin" MMOServer.exe
echo   - Server started

echo   - Waiting for server to listen on port 6000...
set WAIT_COUNT=0
:WAIT_SERVER
netstat -an | findstr "LISTENING" | findstr ":6000" >nul
if %ERRORLEVEL% EQU 0 goto SERVER_READY
set /a WAIT_COUNT+=1
if %WAIT_COUNT% GEQ 30 (
    echo [ERROR] Server did not start within 30 seconds!
    goto :ERROR
)
timeout /t 1 /nobreak >nul
goto WAIT_SERVER
:SERVER_READY
echo   - Server is ready

start "" /D "%~dp0bin" MMOStressClient.exe
echo   - MMOStressClient started

start "" /D "%~dp0bin" GameClient.exe
echo   - GameClient started (manual play)
echo.

echo ============================================
echo   Done! All services running.
echo   Prometheus UI : http://localhost:9091
echo   Grafana       : http://localhost:3000
echo   (Grafana login: admin / admin)
echo ============================================
pause
exit /b 0

:NEED_BUILD
echo.
echo ============================================
echo   [STOP] bin\ 에 실행 파일이 없습니다.
echo   먼저 .build.bat 을 실행하세요. (전체 빌드: .build.bat)
echo ============================================
pause
exit /b 1

:ERROR
echo.
echo ============================================
echo   [FAILED] Error occurred. Check log above.
echo ============================================
pause
exit /b 1
