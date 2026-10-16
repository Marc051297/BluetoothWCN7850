# BluetoothWCN7850

Bluetooth for the **Qualcomm WCN7850** (FastConnect 7800) on macOS. The chip's Bluetooth half is a USB device (`0489:e10a`) that boots with only a ROM; this kext pushes Qualcomm's RAM patch and NVM into it at boot, the way Linux's `btusb` does, and macOS's own Bluetooth stack takes it from there. The Wi-Fi half is a separate project: [AirportWCN7850](https://github.com/razvanccr/AirportWCN7850).

**Tested on:** MSI MAG X870 Tomahawk WiFi (onboard WCN7850), macOS 26.7.1 Tahoe, OpenCore, SMBIOS MacPro7,1. Pairing, A2DP audio and LE devices work; the controller shows up in System Information with its own address.

## Download

[release/BluetoothWCN7850-1.0.0.zip](release/BluetoothWCN7850-1.0.0.zip): the kext plus the two companion kexts it needs, and an `INSTALL.txt`.

## Install

1. `Kernel → Add`: `Lilu.kext`, then `BlueToolFixup.kext` and `IntelBTPatcher.kext` (from the zip; both are Lilu plug-ins), then `BluetoothWCN7850.kext`.
2. `NVRAM → Add → 7C436110-AB2A-4BBB-A880-FE41995C9F82`: add `-btlfxboardid` to `boot-args`; `bluetoothExternalDongleFailed` = `00`; `bluetoothInternalControllerInfo` = 14 zero bytes. Put the same two keys under `NVRAM → Delete`.
3. Reboot.

Why the companions: Tahoe's `bluetoothd` boots the controller through a UART on a MacPro7,1 board-id, and only accepts Apple/Broadcom vendor ids. BlueToolFixup fixes both, but its board-id patch searched for a string macOS 26 removed, so the copy in the zip carries a one-line fix (`scripts/build-bluetoolfixup.sh`). IntelBTPatcher, despite the name, is a generic shim: without it `bluetoothd` restarts every 10 s on LE scan setup.

## What does not work, or is not tested

- Headset microphones are telephone quality (8 kHz CVSD). macOS enables wideband speech only through Apple and Broadcom vendor commands; a third-party controller stays narrowband. Playback is unaffected.
- Sleep/wake and a boot after a full power cut are not verified yet.
- Only `0489:e10a` with ROM `0x00190200` is tested. The other WCN785x USB ids from Linux are in `Info.plist` and share the firmware; WCN6855 would need its own files.

Diagnostics: the kext publishes `Log`, `Stage` and `LastResult` in the I/O Registry (`ioreg -r -c BluetoothWCN7850 -w0`); `-qcabtoff` disables it, `qcabt_stage=<n>` parks it before a stage. `tools/qcabt` does the same download from userspace. The bring-up log with every wrong turn is in [docs/04-bringup.md](docs/04-bringup.md).

## Building

Command Line Tools, git, curl, python3 (Xcode for the two companion kexts, whose upstream builds are Xcode projects):

```bash
scripts/fetch-firmware.sh && ./build.sh
```

```bash
scripts/build-bluetoolfixup.sh && scripts/build-intelbtpatcher.sh
```

Results in `build/`. The firmware is fetched from linux-firmware (pinned release, checksums in `firmware/SHA256SUMS`) and embedded; it is Qualcomm's, redistributable in binary form, not committed.

## Credits

Linux `drivers/bluetooth/btusb.c` and linux-firmware; [IntelBluetoothFirmware](https://github.com/OpenIntelWireless/IntelBluetoothFirmware) and [BrcmPatchRAM](https://github.com/acidanthera/BrcmPatchRAM) for the pattern and the companion kexts; [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK). MIT, see `LICENSE`; the firmware has its own licence.
