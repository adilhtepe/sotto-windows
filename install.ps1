# Installs the Sotto VBAN virtual sound card without the WDK's devcon.exe.
# Run as administrator from the folder holding SottoVBAN.inf, SottoVBAN.sys, sottovban.cat and
# SottoVBAN.cer (install.bat does this for you):
#   powershell -ExecutionPolicy Bypass -File install.ps1 [-Ip 192.168.1.10] [-Stream Desktop] [-Port 6980]
#   powershell -ExecutionPolicy Bypass -File install.ps1 -Uninstall
param(
    [string]$Ip,
    [string]$Stream,
    [int]$Port = 0,
    [switch]$Uninstall
)
$ErrorActionPreference = 'Stop'
$HwId = '*SottoVBAN'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Inf = Join-Path $Here 'SottoVBAN.inf'
$Cer = Join-Path $Here 'SottoVBAN.cer'
$Opts = 'HKLM:\SYSTEM\CurrentControlSet\Services\SottoVBAN\Options'

$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) { Write-Host 'Run this as administrator.' -ForegroundColor Red; exit 1 }

# What devcon install does: create a root-enumerated device node with our hardware ID, then
# install the driver from the INF on it. SetupAPI and newdev ship with every Windows.
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class SottoRootDevice {
    [StructLayout(LayoutKind.Sequential)]
    public struct SP_DEVINFO_DATA { public int cbSize; public Guid ClassGuid; public int DevInst; public IntPtr Reserved; }
    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool SetupDiGetINFClassW(string inf, out Guid cls, StringBuilder name, int size, IntPtr required);
    [DllImport("setupapi.dll", SetLastError = true)]
    static extern IntPtr SetupDiCreateDeviceInfoList(ref Guid cls, IntPtr hwnd);
    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool SetupDiCreateDeviceInfoW(IntPtr set, string name, ref Guid cls, string desc, IntPtr hwnd, int flags, ref SP_DEVINFO_DATA data);
    [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool SetupDiSetDeviceRegistryPropertyW(IntPtr set, ref SP_DEVINFO_DATA data, int prop, byte[] buf, int size);
    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiCallClassInstaller(int fn, IntPtr set, ref SP_DEVINFO_DATA data);
    [DllImport("setupapi.dll", SetLastError = true)]
    static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);
    [DllImport("newdev.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool UpdateDriverForPlugAndPlayDevicesW(IntPtr hwnd, string hwid, string inf, int flags, out bool reboot);

    const int DICD_GENERATE_ID = 1, SPDRP_HARDWAREID = 1, DIF_REGISTERDEVICE = 0x19, INSTALLFLAG_FORCE = 1;

    static Exception Fail(string what) {
        return new Exception(what + " failed: " + new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error()).Message);
    }

    public static bool Install(string inf, string hwid) {
        Guid cls;
        StringBuilder name = new StringBuilder(256);
        if (!SetupDiGetINFClassW(inf, out cls, name, name.Capacity, IntPtr.Zero)) throw Fail("Reading the INF class");
        IntPtr set = SetupDiCreateDeviceInfoList(ref cls, IntPtr.Zero);
        if (set == new IntPtr(-1)) throw Fail("SetupDiCreateDeviceInfoList");
        try {
            SP_DEVINFO_DATA d = new SP_DEVINFO_DATA();
            d.cbSize = Marshal.SizeOf(typeof(SP_DEVINFO_DATA));
            if (!SetupDiCreateDeviceInfoW(set, name.ToString(), ref cls, null, IntPtr.Zero, DICD_GENERATE_ID, ref d))
                throw Fail("Creating the device");
            byte[] ids = Encoding.Unicode.GetBytes(hwid + "\0\0");   // REG_MULTI_SZ
            if (!SetupDiSetDeviceRegistryPropertyW(set, ref d, SPDRP_HARDWAREID, ids, ids.Length))
                throw Fail("Setting the hardware ID");
            if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, ref d))
                throw Fail("Registering the device");
        } finally {
            SetupDiDestroyDeviceInfoList(set);
        }
        bool reboot;
        if (!UpdateDriverForPlugAndPlayDevicesW(IntPtr.Zero, hwid, inf, INSTALLFLAG_FORCE, out reboot))
            throw Fail("Installing the driver");
        return reboot;
    }
}
'@

function Get-SottoDevices {
    Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains $HwId }
}
function Remove-SottoDevices {
    # Returns $true when Windows needs a reboot to finish removing a device
    $reboot = $false
    foreach ($d in @(Get-SottoDevices)) {
        Write-Host "Removing $($d.InstanceId)"
        $out = (& pnputil /remove-device "$($d.InstanceId)") -join "`n"
        if ($out -match 'reboot|restart') { $reboot = $true }
    }
    return $reboot
}

if ($Uninstall) {
    if (Remove-SottoDevices) { Write-Host 'Windows asks for a reboot to finish removing the device.' -ForegroundColor Yellow }
    # Remove the driver package from the driver store too
    $pkgs = Get-WindowsDriver -Online -ErrorAction SilentlyContinue | Where-Object { $_.OriginalFileName -like '*SottoVBAN.inf' }
    foreach ($p in $pkgs) { & pnputil /delete-driver $p.Driver /uninstall /force | Out-Null; Write-Host "Removed $($p.Driver)" }
    Write-Host 'Sotto VBAN removed.' -ForegroundColor Green
    exit 0
}

foreach ($f in 'SottoVBAN.inf', 'SottoVBAN.sys', 'sottovban.cat') {
    if (-not (Test-Path (Join-Path $Here $f))) { Write-Host "$f is missing next to this script." -ForegroundColor Red; exit 1 }
}

# Test-signed drivers load only in test mode
$bcd = (& bcdedit /enum '{current}') -join "`n"
if ($bcd -notmatch 'testsigning\s+Yes') {
    Write-Host 'Test signing is off. Run:  bcdedit /set testsigning on   then reboot and run this again.' -ForegroundColor Yellow
    Write-Host '(With Secure Boot on, turn it off in the firmware settings first.)' -ForegroundColor Yellow
    exit 1
}

# Trust the test certificate the package was signed with (test machines only)
if (Test-Path $Cer) {
    & certutil -f -addstore Root $Cer | Out-Null
    & certutil -f -addstore TrustedPublisher $Cer | Out-Null
}

$pendingReboot = Remove-SottoDevices
# A driver image stays in memory until every device using it is gone; if it is still loaded,
# a new device would run the old code. Insist on a reboot first.
$svc = Get-CimInstance Win32_SystemDriver -Filter "Name='SottoVBAN'" -ErrorAction SilentlyContinue
if ($pendingReboot -or ($svc -and $svc.State -eq 'Running')) {
    Write-Host 'An older Sotto VBAN driver is still loaded in memory. Reboot, then run this again.' -ForegroundColor Yellow
    exit 2
}
# The driver reads its options once, when it loads: write them before the device is created
if ($Ip) {
    New-Item -Path $Opts -Force | Out-Null
    Set-ItemProperty -Path $Opts -Name UnicastIPv4 -Value $Ip
    if ($Stream) { Set-ItemProperty -Path $Opts -Name StreamName -Value $Stream }
    if ($Port -gt 0) { New-ItemProperty -Path $Opts -Name UnicastPort -Value $Port -PropertyType DWord -Force | Out-Null }
}
try {
    $reboot = [SottoRootDevice]::Install($Inf, $HwId)
} catch {
    Write-Host $_.Exception.Message -ForegroundColor Red
    exit 1
}
$dev = Get-SottoDevices | Where-Object { $_.Present } | Select-Object -First 1
if ($dev) { Write-Host "Device: $($dev.FriendlyName)  [$($dev.Status)]" }
# Check that the driver Windows loaded is the one in this folder
$svc = Get-CimInstance Win32_SystemDriver -Filter "Name='SottoVBAN'" -ErrorAction SilentlyContinue
if ($svc -and $svc.PathName) {
    $loaded = $svc.PathName -replace '^\\\?\\', ''
    $mine = (Get-FileHash (Join-Path $Here 'SottoVBAN.sys')).Hash
    if ((Test-Path $loaded) -and ((Get-FileHash $loaded).Hash -ne $mine)) {
        Write-Host "Windows is using a different SottoVBAN.sys ($loaded). Reboot and run this again." -ForegroundColor Yellow
    }
    Write-Host "Driver: $loaded  [$($svc.State)]"
}
if ($pendingReboot) {
    # The old driver instance is still loaded with its old settings until Windows restarts
    Write-Host 'An earlier Sotto device was still in use: reboot once so the new settings take effect.' -ForegroundColor Yellow
}
if ($reboot) { Write-Host 'Windows asks for a reboot to finish.' -ForegroundColor Yellow }
$dest = if ($Ip) { $Ip } else { '127.0.0.1' }
$p = if ($Port -gt 0) { $Port } else { 6980 }
Write-Host "Done. Pick 'Sotto out' as the playback device; it sends VBAN to ${dest}:$p." -ForegroundColor Green
