# BluetoothWCN7850: BLE HID pairing fork

This branch (`ble-hid-pairing`) builds on razvanccr's BluetoothWCN7850 1.0.0. The kext itself is unchanged; the fork fixes Bluetooth LE HID pairing through the BlueToolFixup it builds, and adds a pairing tool.

**Test machine:** MSI MAG X870E Tomahawk WiFi (onboard WCN7850, Bluetooth `0489:e10a`, rampatch v34151), macOS Tahoe 26.7.1, OpenCore, SMBIOS MacPro7,1. Companion kexts: BlueToolFixup 2.7.3 (built by `scripts/build-bluetoolfixup.sh` with this patch) and IntelBTPatcher 2.5.0.

## Status

| Area | Status | Notes |
|---|---|---|
| Classic Bluetooth: AirPods, A2DP audio | **Works** | Unchanged from upstream |
| BLE HID: Xbox Series controller, BLE mouse, BLE keyboard | **Works with `-btlfxlemask`** | Without it they connect but spin forever (no `LE Link Ready`, no pairing). See `docs/04-bringup.md`, "BLE HID devices connect but never pair" |
| Mice that change address every minute in pairing mode | **Works via `tools/pair-mouse`** | System Settings dials the stale address; the tool connects within a second |
| Bluetooth after system sleep | **Works** | The kext reloads the firmware when the device comes back. With a second USB Bluetooth controller in the machine, `bluetoothd` may pick that one after wake: keep only one |
| Headset microphone quality | **Narrowband (8 kHz)** | Unchanged: macOS enables wideband speech only for Apple/Broadcom chipsets |
| Handoff / Universal Clipboard, Mac to iPhone | **Does not work** | macOS classes the controller as `THIRD_PARTY_DONGLE` and does not advertise; IntelBTPatcher's fixed random address would also stop an iPhone from resolving the Mac. Filling `bluetoothInternalControllerInfo` with the controller address makes `bluetoothd` look for an internal Apple controller and turns Bluetooth off: do not |
| AirDrop | **Does not work** | Needs AWDL on the Wi-Fi side, which the WCN7850 firmware does not provide |

## Changes

- `patches/bluetoolfixup-le-event-mask.patch`, applied by `scripts/build-bluetoolfixup.sh`: with `-btlfxlemask` in the boot-args, BlueToolFixup widens the LE event mask `bluetoothd` sets for a third-party controller from `0x1F` to `0x87F` (adds *Remote Connection Parameter Request*, *Data Length Change*, *PHY Update Complete*). Patterns for macOS 26 (x86_64 code and table) and 27 (table).
- Both build scripts use `MACOSX_DEPLOYMENT_TARGET=12.0`: Xcode 27 rejects 10.13. BlueToolFixup does nothing before Monterey anyway.
- `tools/pair-mouse`: pair a BLE HID device from Terminal.

## Boot-args used

`-btlfxboardid -btlfxlemask`, with `bluetoothExternalDongleFailed = 00` and `bluetoothInternalControllerInfo` = 14 zero bytes (upstream instructions).
