@echo off
rem SwitchRecomp - Copyright 2026 MZ Labs
rem Shared Windows build environment. CALL this file; do not run it directly.
set "SWR_ROOT=%~dp0.."
for %%I in ("%SWR_ROOT%") do set "SWR_ROOT=%%~fI"

if exist "%SWR_ROOT%\python\python.exe" (
  set "SWR_PYTHON=%SWR_ROOT%\python\python.exe"
) else (
  where python >nul 2>nul || (echo [ENV] FAILED - Python not found. Run scripts\setup.bat.& exit /b 1)
  set "SWR_PYTHON=python"
)

if exist "%SWR_ROOT%\gcc\Library\bin\x86_64-w64-mingw32-gcc.exe" (
  set "SWR_CC=%SWR_ROOT%\gcc\Library\bin\x86_64-w64-mingw32-gcc.exe"
) else if exist "%SWR_ROOT%\gcc\bin\gcc.exe" (
  set "SWR_CC=%SWR_ROOT%\gcc\bin\gcc.exe"
) else (
  where gcc >nul 2>nul || (echo [ENV] FAILED - GCC not found. Run scripts\setup.bat.& exit /b 1)
  set "SWR_CC=gcc"
)
exit /b 0
