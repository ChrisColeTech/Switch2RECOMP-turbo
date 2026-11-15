@echo off
setlocal
set "ROOT=%~dp0.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
set "SWR_NO_PAUSE=1"
call "%ROOT%\Install.bat"
if errorlevel 1 (echo [SETUP] FAILED& exit /b 1)
echo [SETUP] SUCCESS - portable Python and GCC are ready
exit /b 0
