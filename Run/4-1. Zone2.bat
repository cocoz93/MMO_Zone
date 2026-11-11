@echo off
REM ============================================
REM   Second zone server + its relay. Run "4. Web_client.bat" first.
REM   zone  : tcp 127.0.0.1:6010 (metrics 9097), logs in bin\logs\zone2
REM   relay : ws://127.0.0.1:9010 -^> tcp 127.0.0.1:6010
REM ============================================
setlocal
netstat -an | findstr "LISTENING" | findstr ":6010" >nul
if %ERRORLEVEL% NEQ 0 start "MMO zone2" /D "%~dp0bin" MMOServer.exe --config MMOServerConfig.zone2.ini --log-dir logs/zone2
netstat -an | findstr "LISTENING" | findstr ":9010" >nul
if %ERRORLEVEL% NEQ 0 start "MMO relay zone2" /D "%~dp0..\WebClient" node relay.js --ws 9010 --server-host 127.0.0.1 --server-port 6010
echo   - zone2 : tcp 127.0.0.1:6010 (metrics 9097)
echo   - relay : ws://127.0.0.1:9010  -^>  tcp 127.0.0.1:6010
endlocal
