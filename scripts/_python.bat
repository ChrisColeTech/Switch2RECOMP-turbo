@echo off
rem SwitchRecomp - Copyright 2026 MZ Labs
rem Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
rem CALL this file: select Python without installing anything or changing PATH.
if defined SWR_PYTHON (
  "%SWR_PYTHON%" -c "import sys; sys.exit(sys.version_info < (3, 8))" >nul 2>nul
  if not errorlevel 1 exit /b 0
  echo [ENV] FAILED - SWR_PYTHON is not a runnable Python 3 interpreter. >&2
  exit /b 1
)
for %%P in ("%~dp0..\.venv\Scripts\python.exe" "%~dp0..\venv\Scripts\python.exe" "%~dp0..\python\python.exe") do (
  if exist "%%~fP" (
    "%%~fP" -c "import sys; sys.exit(sys.version_info < (3, 8))" >nul 2>nul
    if not errorlevel 1 (
      set "SWR_PYTHON=%%~fP"
      exit /b 0
    )
  )
)
for %%P in (python.exe python3.exe) do (
  for /f "delims=" %%I in ('where %%P 2^>nul') do (
    "%%I" -c "import sys; sys.exit(sys.version_info < (3, 8))" >nul 2>nul
    if not errorlevel 1 (
      set "SWR_PYTHON=%%I"
      exit /b 0
    )
  )
)
echo [ENV] FAILED - Python not found. Provide .venv, venv, bundled python, or SWR_PYTHON. >&2
exit /b 1
