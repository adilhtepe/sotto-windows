@echo off
rem Installs the Sotto VBAN virtual sound card. Run as administrator, from the folder that holds
rem SottoVBAN.inf, SottoVBAN.sys, sottovban.cat and SottoVBAN.cer (the build output).
rem Needs test signing on (bcdedit /set testsigning on, then reboot) and devcon.exe from the WDK
rem (found automatically in "C:\Program Files (x86)\Windows Kits\10\Tools", or put it next to this file).
rem   install.bat [destination ip] [stream name] [port]
setlocal
cd /d "%~dp0"
net session >nul 2>&1
if not %errorLevel% == 0 (
  echo Run this as administrator.
  pause
  exit /b 1
)

set "DEVCON="
if exist "%~dp0devcon.exe" set "DEVCON=%~dp0devcon.exe"
if not defined DEVCON for /f "delims=" %%d in ('where devcon 2^>nul') do if not defined DEVCON set "DEVCON=%%d"
set "ARCH=x64"
if /i "%PROCESSOR_ARCHITECTURE%"=="ARM64" set "ARCH=arm64"
if not defined DEVCON for /f "delims=" %%d in ('dir /b /s "%ProgramFiles(x86)%\Windows Kits\10\Tools\devcon.exe" 2^>nul ^| findstr /i "\\%ARCH%\\"') do set "DEVCON=%%d"
if not defined DEVCON (
  echo devcon.exe not found. Install the Windows Driver Kit, or copy devcon.exe next to this file.
  pause
  exit /b 1
)

rem Trust the test certificate the driver was signed with (test machines only)
if exist SottoVBAN.cer (
  certutil -f -addstore root SottoVBAN.cer >nul
  certutil -f -addstore TrustedPublisher SottoVBAN.cer >nul
)

"%DEVCON%" remove *SottoVBAN >nul 2>&1
"%DEVCON%" install SottoVBAN.inf *SottoVBAN
if errorlevel 1 (
  echo Install failed. Is test signing on? Run: bcdedit /set testsigning on   and reboot.
  pause
  exit /b 1
)
if "%~1"=="" goto done
reg add HKLM\SYSTEM\CurrentControlSet\Services\SottoVBAN\Options /v UnicastIPv4 /t REG_SZ /d %1 /f >nul
if not "%~2"=="" reg add HKLM\SYSTEM\CurrentControlSet\Services\SottoVBAN\Options /v StreamName /t REG_SZ /d %2 /f >nul
if not "%~3"=="" reg add HKLM\SYSTEM\CurrentControlSet\Services\SottoVBAN\Options /v UnicastPort /t REG_DWORD /d %3 /f >nul
"%DEVCON%" restart *SottoVBAN
:done
echo Done. Pick "Sotto out" as the playback device.
pause
