@echo off
REM ============================================
REM   Web Client - account + character server + relays + static server + browser
REM   계정 서버 · 캐릭터 서버 · 중계 · 정적 서버를 띄우고 로그인 창을 연다. 게임 서버(:6000)는 따로 띄운다.
REM   relay  : 브라우저 WebSocket(:9000) <-> 게임 서버 TCP(:6000) 중계
REM            :9001 <-> 캐릭터 서버(:6100) · :9002 <-> 계정 서버(:6200)
REM   serve  : live.html 을 http 로 내려주는 최소 정적 서버(:8200)
REM            file:// 로 열면 브라우저가 로컬 파일을 막아 자산 로딩이 실패한다.
REM ============================================
setlocal

echo ============================================
echo   Web Client
echo ============================================
echo.

REM === 1. node 확인 ===
echo [1/6] Checking node...
where node >nul 2>nul
if %ERRORLEVEL% NEQ 0 goto :NEED_NODE
echo   - OK
echo.

REM === 2. 이전 실행분 정리 ===
REM   node.exe 를 통째로 죽이지 않는다 - 다른 작업의 node 까지 같이 꺼진다.
REM   9000/9001/9002/8200 을 듣고 있는 PID 만 골라 끝낸다. 서버는 안 끈다(떠 있으면 그대로 쓴다).
echo [2/6] Cleaning previous run...
call :KILL_PORT 9000
call :KILL_PORT 9001
call :KILL_PORT 9002
call :KILL_PORT 8200
echo   - Done
echo.

REM === 3. 게임 서버 확인 ===
echo [3/6] Checking game server (:6000)...
netstat -an | findstr "LISTENING" | findstr ":6000" >nul
if %ERRORLEVEL% NEQ 0 (
    echo   - [WARN] 게임 서버가 :6000 에 없다.
    echo            bin\MMOServer.exe 를 띄울 것. "3-1" 은 측정용이라 캐릭터 서버에 안 붙는다.
    echo            나중에 켜도 캐릭터 서버에 저절로 붙는다.
) else (
    echo   - OK
)
REM   측정 bat(3-1 등)이 UseServerLink=0 을 남기면 게임 서버가 캐릭터 서버에 안 붙어 입장이 ZONE_DOWN 이 된다.
findstr /B /C:"UseServerLink=0" "%~dp0bin\MMOServerConfig.ini" >nul
if %ERRORLEVEL% EQU 0 (
    powershell -Command "(Get-Content -Encoding Default '%~dp0bin\MMOServerConfig.ini') -replace '^UseServerLink=.*', 'UseServerLink=1' | Set-Content -Encoding Default '%~dp0bin\MMOServerConfig.ini'"
    echo   - [WARN] UseServerLink=0 이 남아 있어 1 로 되돌렸다. 떠 있는 게임 서버가 있으면 껐다 켤 것.
)
echo.

REM === 4. 계정 서버 + 캐릭터 서버 ===
echo [4/6] Starting account and character server...
netstat -an | findstr "LISTENING" | findstr ":6200" >nul
if %ERRORLEVEL% NEQ 0 start "MMO account" /D "%~dp0bin" AccountServer.exe
netstat -an | findstr "LISTENING" | findstr ":6100" >nul
if %ERRORLEVEL% NEQ 0 start "MMO character" /D "%~dp0bin" CharacterServer.exe
netstat -an | findstr "LISTENING" | findstr ":6301" >nul
if %ERRORLEVEL% NEQ 0 start "MMO world" /D "%~dp0bin" WorldServer.exe
echo   - account   : tcp 127.0.0.1:6200 (metrics 9095)
echo   - character : tcp 127.0.0.1:6100 (metrics 9093)
echo   - world     : tcp 127.0.0.1:6301 (metrics 9096)
echo.

REM === 5. 릴레이 + 정적 서버 ===
echo [5/6] Starting relay and static server...
start "MMO relay" /D "%~dp0..\WebClient" node relay.js --ws 9000 --server-host 127.0.0.1 --server-port 6000
start "MMO relay character" /D "%~dp0..\WebClient" node relay.js --ws 9001 --server-host 127.0.0.1 --server-port 6100
start "MMO relay account" /D "%~dp0..\WebClient" node relay.js --ws 9002 --server-host 127.0.0.1 --server-port 6200
start "MMO serve" /D "%~dp0..\WebClient" node serve.js --port 8200
echo   - relay : ws://127.0.0.1:9000  -^>  tcp 127.0.0.1:6000
echo   - relay : ws://127.0.0.1:9001  -^>  tcp 127.0.0.1:6100
echo   - relay : ws://127.0.0.1:9002  -^>  tcp 127.0.0.1:6200
echo   - serve : http://127.0.0.1:8200
echo.

REM === 6. 브라우저 ===
echo [6/6] Opening browser...
timeout /t 2 /nobreak >nul
start http://127.0.0.1:8200/login.html
echo   - Done
echo.

echo ============================================
echo   Web client : http://127.0.0.1:8200/login.html
echo   끄려면 이 창이 아니라 "MMO relay" / "MMO serve" / "MMO account" / "MMO character" 창을 닫을 것.
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