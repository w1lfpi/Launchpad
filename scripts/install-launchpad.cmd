@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-launchpad.ps1" %*
if not errorlevel 1 exit /b 0
echo.
echo Installation failed. See the error above.
pause
exit /b 1
