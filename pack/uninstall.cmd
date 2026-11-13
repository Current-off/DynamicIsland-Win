@echo off
rem Double-click to uninstall. Your rules.json in %APPDATA% is kept.
setlocal
pushd "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -Uninstall %*
set "rc=%errorlevel%"
popd
if not "%rc%"=="0" (
  echo.
  echo Failed with code %rc%. Press any key to close.
  pause >nul
)
exit /b %rc%
