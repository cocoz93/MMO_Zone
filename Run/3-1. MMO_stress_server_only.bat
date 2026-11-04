@echo off
REM ============================================
REM   MMO Stress - Server Only
REM   서버 + 모니터링만 실행 (클라이언트 없음)
REM ============================================
setlocal

REM ============================================
REM   [부하 클라 IP] 서버/클라가 다른 PC이므로 Prometheus 가 긁을 원격 부하 클라 IP.
REM   공인 IP 를 git 에 박지 않으려고 로컬 파일에서 읽는다(추적 제외).
REM     - Run\stress_client_ip.txt 에 부하 클라 IP 를 적는다. (3-2 가 도는 PC IP)
REM     - '#' 로 시작하는 줄과 빈 줄은 주석으로 무시, 첫 유효 줄의 첫 토큰만 IP 로 사용.
REM     - 파일이 없으면 localhost 로 폴백. 템플릿: stress_client_ip.txt.example
REM ============================================
set "STRESS_CLIENT_IP=localhost"
set "STRESS_CLIENT_IP_SET="
if exist "%~dp0stress_client_ip.txt" (
    for /f "usebackq eol=# tokens=1" %%i in ("%~dp0stress_client_ip.txt") do if not defined STRESS_CLIENT_IP_SET (
        set "STRESS_CLIENT_IP=%%i"
        set "STRESS_CLIENT_IP_SET=1"
    )
)
set "STRESS_CLIENT_IP_SET="

echo ============================================
echo   MMO Stress - Server Only
echo ============================================
echo.

REM === 1. Kill running processes ===
echo [1/4] Killing running processes...
taskkill /F /IM MMOServer.exe >nul 2>nul
taskkill /F /IM prometheus.exe >nul 2>nul
taskkill /F /IM windows_exporter.exe >nul 2>nul
taskkill /F /IM grafana-server.exe >nul 2>nul
echo   - Done
echo.

REM === 2. bin 산출물 확인 (없으면 .build.bat 먼저) ===
echo [2/4] Checking build output...
if not exist "%~dp0bin\MMOServer.exe" (
    echo [MISSING] bin\MMOServer.exe
    goto :NEED_BUILD
)
echo   - OK
echo.

REM === 4. Configure ===
echo [3/4] Configuring...
powershell -Command "(Get-Content -Encoding Default '%~dp0bin\MMOServerConfig.ini') -replace '^Mode=.*', 'Mode=GameServer' -replace '^MonitorEnabled=.*', 'MonitorEnabled=1' -replace '^UseServerLink=.*', 'UseServerLink=0' | Set-Content -Encoding Default '%~dp0bin\MMOServerConfig.ini'"
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] MMOServerConfig.ini update failed!
    goto :ERROR
)
echo   - MMOServerConfig.ini updated (Mode=GameServer, MonitorEnabled=1, UseServerLink=0)

REM 서버/클라 분리: Prometheus stress_client 타깃을 원격 부하 클라 IP로 주입.
powershell -ExecutionPolicy Bypass -File "%~dp0..\Monitoring\config\setup.ps1" -StressClientIp %STRESS_CLIENT_IP%
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] setup.ps1 injection failed!
    goto :ERROR
)
echo   - Prometheus config injected (stress_client=%STRESS_CLIENT_IP%:9101)
echo.

REM === 5. Start Monitoring ===
echo [4/4] Starting...
start "" "%~dp0..\Monitoring\windows_exporter.exe"
echo   - windows_exporter started (:9182)

start "" /D "%~dp0..\Monitoring\prometheus-3.4.1.windows-amd64" prometheus.exe --web.listen-address=":9091"
echo   - Prometheus started (:9091)

start "" /D "%~dp0..\Monitoring\grafana\bin" grafana-server.exe
echo   - Grafana started (:3000)

timeout /t 3 /nobreak >nul
start http://localhost:9091
start http://localhost:3000

REM === 6. Start Server ===
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
echo.

echo ============================================
echo   Server running on :6000 (GameServer mode)
echo   Prometheus : http://localhost:9091
echo   Grafana    : http://localhost:3000
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
