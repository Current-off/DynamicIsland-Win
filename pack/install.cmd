@echo off
rem Double-click to install. Options are forwarded to install.ps1:
rem   install.cmd -NoAutostart   install without autostart
rem   install.cmd -Uninstall     remove (config is kept)
setlocal
pushd "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
set "rc=%errorlevel%"
popd
if not "%rc%"=="0" (
  echo.
  echo Failed with code %rc%. Press any key to close.
  pause >nul
)
exit /b %rc%
