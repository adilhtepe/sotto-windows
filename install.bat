@echo off
rem Installs the Sotto VBAN virtual sound card. Run as administrator from this folder:
rem   install.bat [destination ip] [stream name] [port] [frames per packet, 16..256, default 64]
rem Needs test signing (bcdedit /set testsigning on, then reboot). No WDK or devcon needed.
setlocal
net session >nul 2>&1
if not %errorLevel% == 0 (
  echo Run this as administrator.
  pause
  exit /b 1
)
set "ARGS="
if not "%~1"=="" set "ARGS=-Ip %~1"
if not "%~2"=="" set "ARGS=%ARGS% -Stream %~2"
if not "%~3"=="" set "ARGS=%ARGS% -Port %~3"
if not "%~4"=="" set "ARGS=%ARGS% -Frames %~4"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %ARGS%
pause
