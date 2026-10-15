# QcaBluetooth

A macOS firmware loader for the Bluetooth half of the **Qualcomm WCN7850** (FastConnect 7800), the combo chip on my ASUS X870 board. The Wi-Fi half is a PCIe device and lives in [QwzMac](../QwzMac); the Bluetooth half is a separate USB device, `0489:e10a`, that boots with nothing but a ROM and needs a RAM patch and an NVM blob pushed into it before it is a usable HCI controller. Linux does that in `btusb.c`; macOS has no idea what the device is. This does the same download from a kext, and leaves the rest to Apple's own Bluetooth stack plus [BlueToolFixup](https://github.com/acidanthera/BrcmPatchRAM), the way Intel and Broadcom USB controllers work on Hackintoshes since macOS 12.

> **Status (2026-10-15): firmware download proven from userspace, kext built but not yet booted.** `tools/qcabt load` takes the chip from its ROM build to patch build `0x8567` with both status bits set. The kext does the same thing at device match time; it has not been installed and rebooted with yet, and `bluetoothd` has not been seen talking to the controller. The step-by-step is in [docs/04-bringup.md](docs/04-bringup.md).

## What is in here

| | |
|---|---|
| `kext/` | `QcaBluetooth.kext` source: matches the USB device, loads the firmware in `start()`, closes the device, done. ~300 lines. |
| `tools/qcabt.c` | The same download from userspace with IOUSBLib. `qcabt info` shows the chip's ROM/patch version and status bits; `qcabt load` loads what is missing. This is the bring-up and diagnosis tool. |
| `tools/usbdesc.c` | Dumps a USB device's descriptors without opening it. |
| `scripts/fetch-firmware.sh` | Fetches the firmware from linux-firmware (pinned release and checksums) and embeds it into the kext. Not committed: it is Qualcomm's, redistributable in binary form. |
| `scripts/build-bluetoolfixup.sh` | Builds BlueToolFixup from the BrcmPatchRAM and Lilu sources so the build matches the running Lilu. |
| `docs/` | [Survey](docs/01-survey.md), [firmware protocol](docs/02-firmware-protocol.md), [design](docs/03-design.md), [bring-up log and test plan](docs/04-bringup.md). |

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

`build/QcaBluetooth.kext` and `build/BlueToolFixup.kext` are the results. MacKernelSDK, BrcmPatchRAM and Lilu are cloned into `deps/` on first use.

## Installing

Both kexts go into OpenCore's `Kernel → Add` (BlueToolFixup after Lilu), plus the boot argument `-btlfxboardid` and two NVRAM variables for `bluetoothd`. The exact entries and what to check after the reboot are in [docs/04-bringup.md](docs/04-bringup.md).

## How it works, in one paragraph

The chip answers three vendor control requests on EP0: *version* (ROM `0x00190200`, patch build, board id), *status* (patch loaded? NVM loaded?) and *download* (file header). The body of each file goes down the ACL bulk OUT endpoint in 4 KiB writes. The kext matches the `IOUSBHostDevice`, does that in `start()` for whatever the status bits say is missing, closes the device and marks it `FirmwareLoaded`. Because userspace only hears about the device once kernel matching is done, `bluetoothd` cannot grab it before the firmware is in. BlueToolFixup patches `bluetoothd` so it treats this board as one with USB Bluetooth and accepts a vendor id it does not know; after that the stock stack drives the controller like any other dongle.

## Credits

- Linux `drivers/bluetooth/btusb.c` (the Qualcomm USB download), and the linux-firmware project for the blobs.
- [IntelBluetoothFirmware](https://github.com/OpenIntelWireless/IntelBluetoothFirmware) and [BrcmPatchRAM](https://github.com/acidanthera/BrcmPatchRAM) for the kext pattern and for BlueToolFixup.
- [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK) for building a kext with plain clang.

MIT, see `LICENSE`. The firmware has its own licence.
