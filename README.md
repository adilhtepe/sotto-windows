# Sotto VBAN: virtual sound card for Windows

A Windows audio device called **Sotto out**. Anything played into it leaves the PC as a VBAN stream.
It goes to Voicemeeter, the Sotto hub, `sotto play` or any VBAN receiver. The kernel driver sends
the packets itself (WSK), so no app has to run.

It is a fork of the [Scream](https://github.com/duncanthrax/scream) driver (Ms-PL, see
`LICENSE-MS-PL.txt` and `UPSTREAM`), which is itself based on Microsoft's MSVAD sample. The
changes are:
- **Packets.** Each datagram is a VBAN AUDIO packet: a 28-byte header (sample-rate index, frames,
  channels, data type, stream name, frame counter), then PCM. Packets carry at most 1436 bytes and
  256 frames; 48 kHz stereo 16-bit gives 256 frames (5.3 ms). Scream's 5-byte header is gone.
- **Formats.** int16, int24 and int32 at the VBAN rates the driver offers (44.1 to 192 kHz).
  Other rates are refused instead of sent wrong.
- **Settings** in `HKLM\SYSTEM\CurrentControlSet\Services\SottoVBAN\Options`:

  | Value | Type | Default |
  |---|---|---|
  | `UnicastIPv4` | REG_SZ | `127.0.0.1` |
  | `UnicastPort` | REG_DWORD | `6980` |
  | `StreamName` | REG_SZ, ≤ 16 chars | `Windows` |

  Scream's other values (`UnicastSrcIPv4`, `UnicastSrcPort`, `TTL`, `SilenceThreshold`) still work.
- **Identity.** Its own hardware ID (`*SottoVBAN`), service, `.sys` name and device names, so it
  installs next to Scream instead of replacing it.

The IVSHMEM mode inherited from Scream is untouched and still uses Scream's format; Sotto does not
use it.

## Download
Every push builds the driver on GitHub Actions for x64 and ARM64 and test-signs it. Open the latest
run under **Actions**, download `SottoVBAN-x64` (or `SottoVBAN-ARM64`) from *Artifacts*. Windows 10
1709 (build 16299) or later, including Windows 11.

## Build it yourself
You need Visual Studio 2022 with the **Windows Driver Kit** (WDK 10) installed.
1. Open `SottoVBAN\Scream.vcxproj`.
2. Pick the *Win10* configuration and the *x64* (or ARM64) platform, then build. The package folder
   (`x64\Win10\Scream`) holds `SottoVBAN.sys`, `SottoVBAN.inf` and `sottovban.cat`; the test
   certificate is `x64\Win10\SottoVBAN.cer`.

## Install (test machine)
Windows loads only signed drivers. For a test-signed driver, turn on test signing once and reboot
(on PCs with Secure Boot, turn Secure Boot off in the firmware first):
```bat
bcdedit /set testsigning on
```
Then, in the downloaded `SottoVBAN-x64` folder, as administrator (in PowerShell type `.\install.bat`):
```bat
install.bat 192.168.1.10 Desktop
```
It trusts the test certificate, creates the device and installs the driver (no WDK or `devcon`
needed), then points it at `192.168.1.10:6980` with stream name `Desktop`. Pick **Sotto out** as
the playback device. `uninstall.bat` removes the device and the driver package.

To distribute to machines without test signing, the driver needs Microsoft attestation signing,
which requires an EV code-signing certificate.

## Receiving on Windows
This driver only has an output. To hear a VBAN stream on Windows, run `sotto play` (in `apps/`). It
plays to any device, e.g. `sotto play -n Mix -d Speakers`. A virtual *microphone* fed by VBAN is
not part of this driver.

## Status
Builds, passes the WDK's INF verification and is test-signed in CI for x64 and ARM64. CI installs the
x64 driver on a Windows Server runner, checks that the device starts and the service runs from the
driver store, then uninstalls it. The packet layout matches the SDK's VBAN tests. Runs on a Windows 11 24H2 desktop.

## Thanks
This is part of **Sotto** (the `sotto` repository links all of its repositories). VBAN is the
protocol of VB-Audio Software; Sotto is an independent implementation, not affiliated with
VB-Audio. The full list of sources and thanks (reference implementations, Monocypher, miniaudio,
Scream, Noise, the cacophony test vectors and the tools) is in the `sotto` repository's
ACKNOWLEDGEMENTS.md.
