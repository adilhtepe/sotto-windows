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

## Build
You need Visual Studio 2022 with the **Windows Driver Kit** (WDK 10) installed.
1. Open `SottoVBAN\Scream.vcxproj`.
2. Pick the *Release* configuration and the *x64* (or ARM64) platform, then build. The output
   folder contains `SottoVBAN.sys`, `SottoVBAN.inf` and `SottoVBAN.cat`, test-signed by Visual
   Studio.

## Install (test machine)
Windows loads only signed drivers. For a self-built driver, turn on test signing once and reboot:
```bat
bcdedit /set testsigning on
```
Then, as administrator, in the build output folder (`devcon.exe` is in the WDK's `Tools` folder):
```bat
install.bat 192.168.2.1 Desktop
```
This installs the device and points it at `192.168.2.1:6980` with stream name `Desktop`. Pick
**Sotto out** as the playback device in Windows or in an app.

To distribute to machines without test signing, the driver needs Microsoft attestation signing,
which requires an EV code-signing certificate.

## Receiving on Windows
This driver only has an output. To hear a VBAN stream on Windows, run `sotto play` (in `apps/`). It
plays to any device, e.g. `sotto play -n Mix -d Speakers`. A virtual *microphone* fed by VBAN is
not part of this driver.

## Status
Not built or run yet: Windows drivers cannot be built or loaded on the Linux machine this was
written on. The packet layout matches the SDK's VBAN tests. First build and install need checking
on a real Windows machine.

## Thanks
This is part of **Sotto** (the `sotto` repository links all of its repositories). VBAN is the
protocol of VB-Audio Software; Sotto is an independent implementation, not affiliated with
VB-Audio. The full list of sources and thanks (reference implementations, Monocypher, miniaudio,
Scream, Noise, the cacophony test vectors and the tools) is in the `sotto` repository's
ACKNOWLEDGEMENTS.md.
