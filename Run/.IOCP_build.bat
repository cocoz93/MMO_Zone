@echo off
REM ============================================
REM   MMO Build - Release x64 (vswhere 자동 탐색)
REM   사용법: .build.bat [target]
REM     (없음)     : 전체 (server + gameclient + echo + mmo)
REM     server     : MMOServer
REM     gameclient : GameClient
REM     echo       : EchoStressClient
REM     mmo        : MMOStressClient
REM   산출물: Run\bin\
REM ============================================
setlocal

set "TARGET=%~1"
if "%TARGET%"=="" set "TARGET=all"

echo ============================================
echo   MMO Build (Release x64) - target: %TARGET%
echo ============================================
echo.

REM === MSBuild 탐색 (vswhere: VS 에디션/버전/설치경로 무관) ===
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [ERROR] vswhere.exe not found! ^(Visual Studio 2017+ required^)
    goto :ERROR
)
set "MSBUILD="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do if not defined MSBUILD set "MSBUILD=%%i"
if not defined MSBUILD (
    echo [ERROR] MSBuild not found via vswhere!
    goto :ERROR
)
echo [MSBuild] %MSBUILD%
echo.

REM === cmake 탐색 (서버는 CMake 정본, 클라 3종은 아직 vcxproj) ===
set "CMK=cmake"
where cmake >nul 2>nul || set "CMK=%ProgramFiles%\CMake\bin\cmake.exe"
if not exist "%CMK%" if "%CMK%" neq "cmake" (
    echo [ERROR] cmake not found! ^(winget install Kitware.CMake^)
    goto :ERROR
)
echo [cmake] %CMK%
echo.

REM === 솔루션 경로 (서버는 CMake 빌드 트리) ===
set "BUILD_SERVER=%~dp0..\build-vs"
set "SLN_GAMECLIENT=%~dp0..\GameClient\GameClient.sln"
set "SLN_ECHO=%~dp0..\StressTest\2. Custom_echo_stress\EchoStressClient.sln"
set "SLN_MMO=%~dp0..\StressTest\3. MMO_stress\MMOStressClient.sln"

if /I "%TARGET%"=="all" (
    call :BUILD_SERVER                                 || goto :ERROR
    call :BUILD "GameClient"       "%SLN_GAMECLIENT%" || goto :ERROR
    call :BUILD "EchoStressClient" "%SLN_ECHO%"       || goto :ERROR
    call :BUILD "MMOStressClient"  "%SLN_MMO%"        || goto :ERROR
) else if /I "%TARGET%"=="server" (
    call :BUILD_SERVER || goto :ERROR
) else if /I "%TARGET%"=="gameclient" (
    call :BUILD "GameClient" "%SLN_GAMECLIENT%" || goto :ERROR
) else if /I "%TARGET%"=="echo" (
    call :BUILD "EchoStressClient" "%SLN_ECHO%" || goto :ERROR
) else if /I "%TARGET%"=="mmo" (
    call :BUILD "MMOStressClient" "%SLN_MMO%" || goto :ERROR
) else (
    echo [ERROR] Unknown target: %TARGET%
    echo         Usage: .build.bat [all ^| server ^| gameclient ^| echo ^| mmo]
    goto :ERROR
)

echo.
echo ============================================
echo   [OK] Build finished.  Output: %~dp0bin
echo ============================================
pause
exit /b 0

REM === 빌드 서브루틴: %1=표시이름, %2=.sln 경로 ===
:BUILD
echo   - Building %~1 ...
"%MSBUILD%" "%~2" /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] %~1 build failed!
    exit /b 1
)
echo   - %~1 OK
exit /b 0

REM === 서버 빌드 서브루틴: CMake 정본 (vcxproj/sln 은 생성물) ===
REM  주의: 괄호 블록 안에서 %ERRORLEVEL% 은 블록 진입 시점 값으로 고정된다.
REM        반드시 `if errorlevel N` 키워드 형식을 쓸 것 (그쪽은 실시간 평가)
:BUILD_SERVER
echo   - Building MMOServer ...
if exist "%BUILD_SERVER%\MMO.sln" goto :BUILD_SERVER_COMPILE
echo     ^(configure^) %BUILD_SERVER%
"%CMK%" -S "%~dp0.." -B "%BUILD_SERVER%" -G "Visual Studio 17 2022" -A x64
if errorlevel 1 (
    echo [ERROR] MMOServer configure failed!
    exit /b 1
)
:BUILD_SERVER_COMPILE
"%CMK%" --build "%BUILD_SERVER%" --config Release
if errorlevel 1 (
    echo [ERROR] MMOServer build failed!
    exit /b 1
)
echo   - MMOServer OK
exit /b 0

:ERROR
echo.
echo ============================================
echo   [FAILED] Build error. Check log above.
echo ============================================
pause
exit /b 1
