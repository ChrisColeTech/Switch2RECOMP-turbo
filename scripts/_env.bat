@echo off
rem Copyright 2026 ChrisColeTech
rem Shared Windows build environment. CALL this file; do not run it directly.
set "SWR_ROOT=%~dp0.."
for %%I in ("%SWR_ROOT%") do set "SWR_ROOT=%%~fI"

call "%~dp0_python.bat"
if errorlevel 1 exit /b 1

if exist "%SWR_ROOT%\gcc\Library\bin\x86_64-w64-mingw32-gcc.exe" (
  set "SWR_CC=%SWR_ROOT%\gcc\Library\bin\x86_64-w64-mingw32-gcc.exe"
) else if exist "%SWR_ROOT%\gcc\bin\gcc.exe" (
  set "SWR_CC=%SWR_ROOT%\gcc\bin\gcc.exe"
) else (
  where gcc >nul 2>nul || (echo [ENV] FAILED - GCC not found. Run scripts\setup.bat.& exit /b 1)
  set "SWR_CC=gcc"
)
exit /b 0
