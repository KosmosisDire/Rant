@echo off
REM Configure CMake and build all DART executables in Release.
REM Outputs land in the repo root (Debug builds go in Debug/).
setlocal
cd /d "%~dp0"

cmake --preset windows
if errorlevel 1 exit /b 1

cmake --build --preset windows
if errorlevel 1 exit /b 1

echo.
echo Release build complete. Executables are in the repo root.
endlocal
