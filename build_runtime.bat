@echo off
setlocal
call "%~dp0scripts\build-runtime.bat" %*
exit /b %errorlevel%
