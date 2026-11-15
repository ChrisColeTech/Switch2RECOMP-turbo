@echo off
setlocal
if "%~1"=="" (
  echo Usage: scripts\build-from-nsp.bat GAME.nsp [switchrecomp options]
  echo Install prod.keys from the launcher's Keys menu, or set SWR_PROD_KEYS first.
  exit /b 2
)
if not exist "%~1" (echo [NSP BUILD] FAILED - file not found: "%~1"& exit /b 1)
call "%~dp0_env.bat" || exit /b 1
"%SWR_PYTHON%" "%SWR_ROOT%\switchrecomp.py" %* --target win64 --backend win32 --cc "%SWR_CC%"
if errorlevel 1 (echo [NSP BUILD] FAILED& exit /b 1)
echo [NSP BUILD] SUCCESS - RomFS remains in "%~f1"
exit /b 0
