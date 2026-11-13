@echo off
rem Removes the Sotto VBAN virtual sound card and its driver package. Run as administrator.
net session >nul 2>&1
if not %errorLevel% == 0 (
  echo Run this as administrator.
  pause
  exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -Uninstall
pause
