@echo off
setlocal
rem RIO A/B arm switcher: arm A = IOCP baseline (USE_RIO_TRANSPORT=0) + rebuild Release x64
set "BC=%~dp0..\MMOServer\MMOServer\BuildConfig.h"
powershell -NoProfile -Command "$t=[IO.File]::ReadAllText('%BC%'); $t=$t -replace '#define USE_RIO_TRANSPORT \d', '#define USE_RIO_TRANSPORT 0'; [IO.File]::WriteAllText('%BC%', $t, (New-Object Text.UTF8Encoding($true)))"
rem cmake lookup: PATH first, then default install dir
set "CMK=cmake"
where cmake >nul 2>nul || set "CMK=%ProgramFiles%\CMake\bin\cmake.exe"
if "%CMK%" neq "cmake" if not exist "%CMK%" (
    echo [build] cmake not found
    exit /b 1
)
rem configure first if the build tree is missing (vcxproj/sln are CMake outputs)
if not exist "%~dp0..\build-vs\MMO.sln" (
    "%CMK%" -S "%~dp0.." -B "%~dp0..\build-vs" -G "Visual Studio 17 2022" -A x64
    if errorlevel 1 exit /b 1
)
"%CMK%" --build "%~dp0..\build-vs" --config Release
if errorlevel 1 exit /b 1
echo [OK] arm=A IOCP (USE_RIO_TRANSPORT=0) -^> Run\bin\MMOServer.exe
exit /b 0
