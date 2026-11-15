@echo off
setlocal
call "%~dp0_env.bat" || exit /b 1
"%SWR_PYTHON%" "%SWR_ROOT%\switchrecomp.py" --build-runtime --target win64 --backend win32 --cc "%SWR_CC%" %*
if errorlevel 1 (echo [RUNTIME] FAILED& exit /b 1)
echo [RUNTIME] SUCCESS - bin\runtime_win64_win32.exe
exit /b 0
