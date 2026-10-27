@echo off
rem Re-launches itself as administrator, then lets Lost Planet 2 through Windows Firewall.
net session >nul 2>&1
if errorlevel 1 (
	powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
	exit /b
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -FixFirewall
pause
