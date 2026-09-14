@echo off
REM Configure CMake and build all Ramble targets. Output lands in bin\ for every
REM config, so a Release build overwrites a Debug one (and vice versa).
REM Usage: build [release^|debug]   (default: release)
setlocal
cd /d "%~dp0"

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=release"

if /i "%CONFIG%"=="release" (
  set "BUILDPRESET=windows"
) else if /i "%CONFIG%"=="debug" (
  set "BUILDPRESET=windows-debug"
) else (
  echo Unknown argument "%CONFIG%". Usage: build [release^|debug]
  exit /b 2
)

cmake --preset windows -D RAMBLE_BUILD_JS_CLIENT=ON
if errorlevel 1 exit /b 1

cmake --build --preset %BUILDPRESET%
if errorlevel 1 exit /b 1

echo.
echo %CONFIG% build complete. Executables are in bin\.
endlocal
