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

The machine does **not** boot OpenCore from the macOS disk. `nvram 4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:boot-path` names a USB device with partition GUID `1623DE29-…`, which is `disk3s1` (the USB stick's EFI); `disk1s2` is the NVMe's EFI and is not the one in use. Always check `boot-path` before writing anything.

`tools/install-efi.sh <disk>` does steps 1-3 below (run with `sudo`; it mounts the partition, backs up `config.plist` next to itself and into `build/`, copies the kexts and patches the plist idempotently).

1. Copy `build/QcaBluetooth.kext` and `build/BlueToolFixup.kext` to `EFI/OC/Kexts/`.
2. `config.plist → Kernel → Add`: `BlueToolFixup.kext` **after** `Lilu.kext`, then `QcaBluetooth.kext` (order between the two does not matter; the loader does not depend on Lilu). Executable path `Contents/MacOS/<name>`, plist path `Contents/Info.plist`.
3. `config.plist → NVRAM → Add → 7C436110-AB2A-4BBB-A880-FE41995C9F82`:
   - `boot-args`: append ` -btlfxboardid`
   - `bluetoothExternalDongleFailed` (Data): `00`
   - `bluetoothInternalControllerInfo` (Data): `00000000000000000000000000000000` (14 zero bytes: `0000000000000000000000000000`)
   and the same two keys under `NVRAM → Delete` for the same GUID so they are rewritten on every boot.
4. Reboot.

For a first try, `-qcabtoff` in boot-args disables the loader without touching anything else, and `qcabt_stage=<n>` parks it before stage n (1 open + version read, 2 set configuration, 3 rampatch, 4 NVM) so a bad boot can be bisected one stage per boot, without rebuilding. Runtime `nvram` writes do not persist on this board: both switches go into the USB config's boot-args.

If the machine freezes or resets right after the Bluetooth device enumerates, assume this kext (a panic looks like a freeze with the NVIDIA driver, and no panic report gets saved here). Hard power off, F11, pick the NVMe EFI; **do not reset NVRAM**. Picking the NVMe macOS from the USB stick's own OpenCore picker still injects the stick's kexts.

## What to look at after the reboot

```bash
kextstat | grep -E 'razvanccr|BlueToolFixup|Lilu'
```

```bash
ioreg -l -w0 -c QcaBluetoothFirmware | grep -E 'Log|Stage|LastResult|RomVersion|PatchVersion|"Status"|FirmwareLoaded'
```

```bash
sudo dmesg | grep QcaBluetooth
```

(An OpenCore-injected kext's `IOLog` lines do not reach the unified log, so `log show` finds nothing: the `Log` property on the driver and `dmesg` are the two places to read. Lesson from the Wi-Fi bring-up, see [05-lessons-checklist.md](05-lessons-checklist.md).) Expected lines in `Log`: `probe 0489:e10a ...`, `rom 0x00190200 (WCN785x 2.0) patch 0x000043fb ... status 0x20`, `rampatch_usb_00190200.bin: rom 0x00190200 build 0x8567`, `after rampatch: patch 0x00008567 status 0xa0`, `after nvm: patch 0x00008567 status 0xe0`, `match: firmware loaded`.

```bash
ioreg -p IOUSB -l -w0 -r -n 'Bluetooth USB Host Controller'
```

Should show `FirmwareLoaded = Yes` on the device and a `QcaBluetoothFirmware` child with `RomVersion`, `PatchVersion`, `Status = 0xe0`, `Stage = 100`, `LastAttempt = match`, `LastResult = 0`.

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

## Boot test log

### 2026-10-15 18:21, install

`sudo bash tools/install-efi.sh disk3s1` on the USB stick's EFI: kexts copied, `config.plist` patched (backup `config.plist.bak-20261015-182132`), verified on the mounted volume.

### 2026-10-15 18:25, first reboot: wrong EFI, no test

The machine came back from the **NVMe EFI** (`boot-path` GUID `9232EEDC-…` = `disk1s2`), not the USB stick. Consequences seen: `-btlfxboardid` absent from `boot-args`, neither kext loaded, no `QcaBluetooth` log lines, and `0489:e10a` not enumerated at all (the NVMe config has no USBToolBox port map, so the Bluetooth port is behind the 15-port limit). Oddly `bluetoothExternalDongleFailed` and `bluetoothInternalControllerInfo` were present in NVRAM anyway. Nothing about the kext was learned. Needs a boot from the USB stick (pick it in the OpenCore picker).

## 2026-10-16 00:20: second deploy to the USB EFI, source review

- **Last night's reboot came from the NVMe EFI** (`boot-path` → `HD(2,GPT,9232EEDC-…)` = `disk0s2` after the renumbering; the USB stick is `disk3s1`, GUID `1623DE29-…`). The machine picks the NVMe EFI unless the USB stick is chosen in the firmware boot menu / OpenCore picker. Disk numbers move between boots: identify partitions by GUID, not by `diskNsM`.
- **The Wi-Fi bisection rebuilt the USB EFI after the first Bluetooth install** (`config.plist.before-bisect-20261015-182851`), which removed both Bluetooth kexts and the boot argument again. The two NVRAM variables survived in the config.
- Redeployed: USB EFI = NVMe EFI (same `AirportWCN7850.kext` 0.0.1, hash `c3dc5976`, `MinKernel 25.0.0`, `Misc→Debug→Target 67`, NVMe boot-args without `qwz_debug`) **plus** `BlueToolFixup.kext` 2.7.3 (Kernel→Add index 1), `QcaBluetooth.kext` 0.1.0 build `d31dd7ab` (index 12), `-btlfxboardid`, and the two Bluetooth NVRAM keys in Add and Delete. Verified with a plist diff; backup `config.plist.before-qcabt-20261016-002143` next to it.
- **Source review before the first kernel load**, with the Wi-Fi driver's panic history in mind (NULL dereferences from init ordering, use after the owner went away, stack overflows). Findings, all fixed in this commit:
  1. `openBulk()` kept a borrowed pointer to the `IOUSBHostInterface`; if the device terminated between the download and `closeBulk()` that was a use-after-free. The interface is now retained while held.
  2. The wake path (`thread_call`) could run after `stop()` freed its owner: `thread_call_cancel()` does not wait for a running callback. `stop()` now uses `thread_call_cancel_wait()` (exported by the kernel), `setPowerState` retains the driver for the queued call and the callback releases it, and the callback checks `isInactive()` on itself and the device before touching USB.
  3. `probe()` dereferenced the device descriptor without a null check.
  Not changed, by design: the whole download runs synchronously in `start()` (as IntelBluetoothFirmware does), with a 4 KiB kernel-allocated transfer buffer and nothing large on the stack; the firmware arrays are `const` data in the kext image.
- The kext is still untested in the kernel. Next: boot from the USB stick and run the checks above. To bisect a boot problem: `-qcabtoff` disables only the loader.

## 2026-10-16 00:40: lessons pass, build `46e1de4e` on the USB stick

Went through every freeze/panic lesson of the Wi-Fi bring-up ([05-lessons-checklist.md](05-lessons-checklist.md)). Changes: thread call allocated before power management can call back; EP0 transfers moved off the stack into a `createIOBuffer` descriptor; `setConfiguration` unconditional (reference behaviour); `Log`/`Stage` IORegistry properties; `qcabt_stage=<n>`; OpenCore debug Target back to 0 on the USB config. Checks: no `__cxa_atexit`, every undefined symbol resolves in the kernel or IOUSBHostFamily, no kernel symbol redefined. USB EFI now: Lilu, BlueToolFixup 2.7.3, …, AirportWCN7850 0.0.1 (`c3dc5976`, same as the NVMe), QcaBluetooth 0.1.0 (`46e1de4e`); boot-args = NVMe's + `-btlfxboardid`. Still not booted.
