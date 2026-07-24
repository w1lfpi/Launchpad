@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall-launchpad.ps1" %*
if not errorlevel 1 exit /b 0
echo.
echo Uninstall failed. See the error above.
pause
exit /b 1
