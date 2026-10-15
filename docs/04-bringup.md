# Bring-up log and test plan

Everything here was done on 2026-10-15 unless dated otherwise. Newest at the bottom.

## Done

1. **Device survey** ([01-survey.md](01-survey.md)): enumerates, nothing claims it, standard HCI-over-USB layout.
2. **Protocol** ([02-firmware-protocol.md](02-firmware-protocol.md)) from Linux `btusb.c`; firmware fetched from linux-firmware `20260309`, pinned in `firmware/SHA256SUMS`.
3. **Userspace download works.** `tools/qcabt load` took the chip from `patch 0x43fb / status 0x20` to `patch 0x8567 / status 0xe0` in one go, no errors, no re-enumeration. Repeating it is a no-op.
4. **Kext written and built** (`build/QcaBluetooth.kext`, 392 KB with the firmware embedded). Static checks pass: `_kmod_info`/`_start`/`_stop`/`_realmain` present, no symbol clashes with the kernel, every undefined symbol resolves in the kernel except the two `IOUSBHostDevice`/`IOUSBHostInterface` `metaClass` objects that `IOUSBHostFamily` exports (all USB method calls go through vtables).
5. **BlueToolFixup built from source** (`build/BlueToolFixup.kext`, BrcmPatchRAM master of 2026-03-20 against Lilu master of the same day, `KernelVersion::Tahoe` present).

## Not done: nothing has been loaded into the kernel yet

Loading a kext and writing the EFI partition both need `sudo`, which this session does not have, and the real test needs a reboot anyway. So the kext is **untested on hardware**. The userspace path is what has been proven.

## Install (OpenCore)

The EFI partition of the macOS disk is `disk1s2` (not auto-mounted; `sudo diskutil mount disk1s2`).

1. Copy `build/QcaBluetooth.kext` and `build/BlueToolFixup.kext` to `EFI/OC/Kexts/`.
2. `config.plist → Kernel → Add`: `BlueToolFixup.kext` **after** `Lilu.kext`, then `QcaBluetooth.kext` (order between the two does not matter; the loader does not depend on Lilu). Executable path `Contents/MacOS/<name>`, plist path `Contents/Info.plist`.
3. `config.plist → NVRAM → Add → 7C436110-AB2A-4BBB-A880-FE41995C9F82`:
   - `boot-args`: append ` -btlfxboardid`
   - `bluetoothExternalDongleFailed` (Data): `00`
   - `bluetoothInternalControllerInfo` (Data): `00000000000000000000000000000000` (14 zero bytes: `0000000000000000000000000000`)
   and the same two keys under `NVRAM → Delete` for the same GUID so they are rewritten on every boot.
4. Reboot.

For a first try, `-qcabtoff` in boot-args disables the loader without touching anything else.

## What to look at after the reboot

```bash
kextstat | grep -E 'razvanccr|BlueToolFixup|Lilu'
```

```bash
log show --last 10m --predicate 'eventMessage CONTAINS "QcaBluetooth"' --style compact
```

Expected lines: `probe 0489:e10a ...`, `rom 0x00190200 (WCN785x 2.0) patch 0x000043fb ... status 0x20`, `rampatch_usb_00190200.bin: rom 0x00190200 build 0x8567`, `after rampatch: patch 0x00008567 status 0xa0`, `after nvm: patch 0x00008567 status 0xe0`, `match: firmware loaded`.

```bash
ioreg -p IOUSB -l -w0 -r -n 'Bluetooth USB Host Controller'
```

Should show `FirmwareLoaded = Yes` on the device and a `QcaBluetoothFirmware` child with `RomVersion`, `PatchVersion`, `Status`, `LastAttempt = match`, `LastResult = 0`.

```bash
log show --last 10m --predicate 'process == "bluetoothd" AND (eventMessage CONTAINS "bm3_usb" OR eventMessage CONTAINS "FindTransportType" OR eventMessage CONTAINS "Dongle")' --style compact
```

Wanted: `FindTransportType -- USB Transport`, `[bm3_usb][Found_USB_Dongle] ... returned true`, `[bm3_usb] Configuring bluetooth device`. Unwanted: `Not in supported list` (BlueToolFixup's vendor patch did not apply), `UART Transport` (the `-btlfxboardid` patch did not apply), `USBDeviceOpenSeize failed` (something else holds the device).

```bash
system_profiler SPBluetoothDataType
```

Should now report a controller with a real address, `Transport: USB`, vendor `0x0489`, and the Bluetooth menu should turn on.

`tools/qcabt info` still works while `bluetoothd` is **not** holding the device; once it is, `qcabt` fails to open (that is the expected state after a successful boot, do not use `-s` then, it seizes the device away from `bluetoothd`).

## If it fails

- Kext does not load: `kextstat` empty → OpenCore refused it. Check `Kernel → Add` paths and `MinKernel`; the kext asks for `IOUSBHostFamily 1.2` and `kpi.* 16.7`.
- Kext loads, no `probe` line: the personality did not match. Compare `idVendor`/`idProduct` in `ioreg` with `Info.plist`.
- `cannot open the device (held by another client)` at `match`: something opened the device before us; look for other drivers under the node in `ioreg`.
- Download errors (`bulk write failed`, `DFU_DOWNLOAD header failed`): compare with `tools/qcabt -n load` run before `bluetoothd` grabs the device (boot with `-qcabtoff`, run `tools/qcabt load`, then look at what differs).
- Firmware in, `bluetoothd` silent about USB: BlueToolFixup. `log show --predicate 'eventMessage CONTAINS "btlfx"'` for its own messages; make sure Lilu loaded before it and that `-btlfxboardid` is in `boot-args` (`nvram boot-args`).
- `bluetoothd` opens it but Bluetooth stays off or misbehaves: now it is HCI-level. Capture `sudo log stream --predicate 'process == "bluetoothd"'` and compare with the Linux quirks in the protocol doc (enhanced SCO setup is broken on these chips; debug ACL frames on handle `0x2EDC`).

## Known gaps

- Wake from sleep: the kext tries a reload if it can open the device after an off→on transition; it probably cannot while `bluetoothd` holds it. If Bluetooth dies after sleep, this is why. A fix would need either a re-enumeration after wake or cooperation from `bluetoothd`.
- Only `0489:e10a` with ROM `0x00190200` is tested. The other WCN785x ids in `Info.plist` are from btusb and share the firmware; WCN6855 would need its own files added to `scripts/fetch-firmware.sh`.
