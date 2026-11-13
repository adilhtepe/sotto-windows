@echo off
rem Installs the Sotto VBAN virtual sound card. Run as administrator, from the folder that holds
rem SottoVBAN.inf, SottoVBAN.sys and SottoVBAN.cat (the build output), with devcon.exe from the WDK
rem (e.g. "C:\Program Files (x86)\Windows Kits\10\Tools\10.0.xxxxx.0\x64\devcon.exe") on PATH.
net session >nul 2>&1
if not %errorLevel% == 0 (
  echo Run this as administrator.
  pause
  exit /b 1
)
devcon remove *SottoVBAN >nul 2>&1
devcon install SottoVBAN.inf *SottoVBAN
if "%~1"=="" goto done
rem optional: install.bat <destination ip> [stream name] [port]
reg add HKLM\SYSTEM\CurrentControlSet\Services\SottoVBAN\Options /v UnicastIPv4 /t REG_SZ /d %1 /f >nul
if not "%~2"=="" reg add HKLM\SYSTEM\CurrentControlSet\Services\SottoVBAN\Options /v StreamName /t REG_SZ /d %2 /f >nul
if not "%~3"=="" reg add HKLM\SYSTEM\CurrentControlSet\Services\SottoVBAN\Options /v UnicastPort /t REG_DWORD /d %3 /f >nul
devcon restart *SottoVBAN
:done
pause
