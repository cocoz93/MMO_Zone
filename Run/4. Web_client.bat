@echo off
REM ============================================
REM   Web Client - relay + static server + browser
REM   게임 서버(:6000)에 붙는 웹 클라이언트를 한 번에 띄운다.
REM   relay  : 브라우저 WebSocket(:9000) <-> 게임 서버 TCP(:6000) 중계
REM   serve  : live.html 을 http 로 내려주는 최소 정적 서버(:8200)
REM            file:// 로 열면 브라우저가 로컬 파일을 막아 자산 로딩이 실패한다.
REM ============================================
setlocal

echo ============================================
echo   Web Client
echo ============================================
echo.

REM === 1. node 확인 ===
echo [1/5] Checking node...
where node >nul 2>nul
if %ERRORLEVEL% NEQ 0 goto :NEED_NODE
echo   - OK
echo.

REM === 2. 이전 실행분 정리 ===
REM   node.exe 를 통째로 죽이지 않는다 - 다른 작업의 node 까지 같이 꺼진다.
REM   9000/8200 을 듣고 있는 PID 만 골라 끝낸다.
echo [2/5] Cleaning previous run...
call :KILL_PORT 9000
call :KILL_PORT 8200
echo   - Done
echo.

REM === 3. 게임 서버 확인 ===
echo [3/5] Checking game server (:6000)...
netstat -an | findstr "LISTENING" | findstr ":6000" >nul
if %ERRORLEVEL% NEQ 0 (
    echo   - [WARN] 게임 서버가 :6000 에 없다.
    echo            "3-1. MMO_stress_server_only.bat" 이나 bin\MMOServer.exe 를 먼저 띄울 것.
    echo            릴레이는 그대로 뜨므로, 서버를 나중에 켜고 브라우저를 새로고침하면 붙는다.
) else (
    echo   - OK
)
echo.

REM === 4. 릴레이 + 정적 서버 ===
echo [4/5] Starting relay and static server...
start "MMO relay" /D "%~dp0..\WebClient" node relay.js --ws 9000 --server-host 127.0.0.1 --server-port 6000
start "MMO serve" /D "%~dp0..\WebClient" node serve.js --port 8200
echo   - relay : ws://127.0.0.1:9000  -^>  tcp 127.0.0.1:6000
echo   - serve : http://127.0.0.1:8200
echo.

REM === 5. 브라우저 ===
echo [5/5] Opening browser...
timeout /t 2 /nobreak >nul
start http://127.0.0.1:8200/live.html
echo   - Done
echo.

echo ============================================
echo   Web client : http://127.0.0.1:8200/live.html
echo   끄려면 이 창이 아니라 "MMO relay" / "MMO serve" 창을 닫을 것.
echo   (또는 이 배치를 다시 실행하면 이전 것을 정리하고 새로 띄운다)
echo ============================================
pause
exit /b 0

REM --- 지정 포트를 LISTENING 중인 PID 만 종료 ---
:KILL_PORT
for /f "tokens=5" %%p in ('netstat -ano ^| findstr "LISTENING" ^| findstr ":%1 "') do taskkill /F /PID %%p >nul 2>nul
goto :EOF

:NEED_NODE
echo.
echo ============================================
echo   [STOP] node.exe 를 PATH 에서 못 찾았다.
echo   Node.js 를 설치하거나 PATH 에 추가한 뒤 다시 실행할 것.
echo ============================================
pause
exit /b 1