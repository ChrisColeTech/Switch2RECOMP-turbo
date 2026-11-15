@echo off
setlocal
if "%~1"=="" (
  echo Usage: scripts\build-from-nup.bat GAME.nup [switchrecomp options]
  echo Example: scripts\build-from-nup.bat D:\Games\Game.nup --outdir D:\Games\Game-build
  exit /b 2
)
if not exist "%~1" (echo [NUP BUILD] FAILED - file not found: "%~1"& exit /b 1)
call "%~dp0_env.bat" || exit /b 1
"%SWR_PYTHON%" "%SWR_ROOT%\switchrecomp.py" %* --target win64 --backend win32 --cc "%SWR_CC%"
if errorlevel 1 (echo [NUP BUILD] FAILED& exit /b 1)
echo [NUP BUILD] SUCCESS - RomFS remains in "%~f1"
exit /b 0
