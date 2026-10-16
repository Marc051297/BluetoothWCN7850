# QcaBluetooth: BluetoothWCN7850.kext

A macOS firmware loader for the Bluetooth half of the **Qualcomm WCN7850** (FastConnect 7800), the combo chip on my MSI MAG X870 Tomahawk board. The Wi-Fi half is a PCIe device and lives in [QwzMac](../QwzMac); the Bluetooth half is a separate USB device, `0489:e10a`, that boots with nothing but a ROM and needs a RAM patch and an NVM blob pushed into it before it is a usable HCI controller. Linux does that in `btusb.c`; macOS has no idea what the device is. This does the same download from a kext, and leaves the rest to Apple's own Bluetooth stack plus [BlueToolFixup](https://github.com/acidanthera/BrcmPatchRAM), the way Intel and Broadcom USB controllers work on Hackintoshes since macOS 12.

> **Status (2026-10-16, v1.0.0): working.** The kext loads the firmware at boot, and with BlueToolFixup and IntelBTPatcher macOS's own stack drives the controller from the first boot, before login: `system_profiler` shows it with its real address, state On, firmware v34151 (the patch build), transport USB. AirPods pair and play at full A2DP quality. Known limitation: the headset microphone is telephone quality, because macOS enables wideband speech only through Apple and Broadcom vendor commands and treats this controller as a third-party dongle. Cold boot and sleep/wake are not verified yet. The whole bring-up, including the wrong turns, is in [docs/04-bringup.md](docs/04-bringup.md).

## What is in here

| | |
|---|---|
| `kext/` | `BluetoothWCN7850.kext` source: matches the USB device, loads the firmware in `start()`, closes the device, done. ~300 lines. |
| `tools/qcabt.c` | The same download from userspace with IOUSBLib. `qcabt info` shows the chip's ROM/patch version and status bits; `qcabt load` loads what is missing. This is the bring-up and diagnosis tool. |
| `tools/usbdesc.c` | Dumps a USB device's descriptors without opening it. |
| `scripts/fetch-firmware.sh` | Fetches the firmware from linux-firmware (pinned release and checksums) and embeds it into the kext. Not committed: it is Qualcomm's, redistributable in binary form. |
| `scripts/build-bluetoolfixup.sh` | Builds BlueToolFixup from the BrcmPatchRAM and Lilu sources, with a one-line fix for macOS 26 (see Installing). |
| `scripts/build-intelbtpatcher.sh` | Builds IntelBTPatcher from the OpenIntelWireless sources (release and debug). |
| `tools/install-efi.sh` | Puts the three kexts into an OpenCore EFI and patches `config.plist` (idempotent, keeps a backup). `tools/boot-arg.sh` adds or removes boot arguments there. |
| `docs/` | [Survey](docs/01-survey.md), [firmware protocol](docs/02-firmware-protocol.md), [design](docs/03-design.md), [bring-up log and test plan](docs/04-bringup.md), [lessons from the Wi-Fi kext checked against this one](docs/05-lessons-checklist.md). |

## Building

Needs the Command Line Tools, git, curl and python3 for the kext and the tools; Xcode for BlueToolFixup (its upstream build is an Xcode project).

```bash
scripts/fetch-firmware.sh
```

```bash
./build.sh
```

```bash
make -C tools
```

```bash
scripts/build-bluetoolfixup.sh
```

```bash
scripts/build-intelbtpatcher.sh
```

`build/BluetoothWCN7850.kext`, `build/BlueToolFixup.kext` and `build/IntelBTPatcher.kext` are the results. MacKernelSDK, BrcmPatchRAM, Lilu and IntelBluetoothFirmware are cloned into `deps/` on first use.

## Installing

```bash
sudo bash tools/install-efi.sh diskNsM
```

with the EFI partition OpenCore boots from (`nvram 4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:boot-path` says which; partition numbers move between boots). Three kexts go into `Kernel → Add` (BlueToolFixup and IntelBTPatcher after Lilu, BluetoothWCN7850 anywhere), plus the boot argument `-btlfxboardid` and two NVRAM variables for `bluetoothd`; the script does all of it and keeps a backup of `config.plist`. Then reboot. The BlueToolFixup built here carries a one-line fix without which `-btlfxboardid` does nothing on macOS 26 (the board-id string it searches for belonged to the MacPro6,1, which Tahoe dropped); on a MacPro7,1 SMBIOS that patch is what makes `bluetoothd` look for a USB dongle instead of a UART. No launch agent or daemon is needed; `tools/install-transport-daemon.sh` is kept only as a fallback. `-qcabtoff` disables the loader, `qcabt_stage=<n>` parks it before stage n; the driver publishes its log and progress as `Log` and `Stage` in the I/O Registry (`ioreg -r -c BluetoothWCN7850 -w0`), because an OpenCore-injected kext's `IOLog` never reaches the unified log. The exact entries and what to check after the reboot are in [docs/04-bringup.md](docs/04-bringup.md).

## How it works, in one paragraph

The chip answers three vendor control requests on EP0: *version* (ROM `0x00190200`, patch build, board id), *status* (patch loaded? NVM loaded?) and *download* (file header). The body of each file goes down the ACL bulk OUT endpoint in 4 KiB writes. The kext matches the `IOUSBHostDevice`, does that in `start()` for whatever the status bits say is missing, closes the device and marks it `FirmwareLoaded`. Because userspace only hears about the device once kernel matching is done, `bluetoothd` cannot grab it before the firmware is in. BlueToolFixup patches `bluetoothd` so it treats this board as one with USB Bluetooth and accepts a vendor id it does not know; after that the stock stack drives the controller like any other dongle.

## Credits

- Linux `drivers/bluetooth/btusb.c` (the Qualcomm USB download), and the linux-firmware project for the blobs.
- [IntelBluetoothFirmware](https://github.com/OpenIntelWireless/IntelBluetoothFirmware) and [BrcmPatchRAM](https://github.com/acidanthera/BrcmPatchRAM) for the kext pattern and for BlueToolFixup.
- [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK) for building a kext with plain clang.

MIT, see `LICENSE`. The firmware has its own licence.
