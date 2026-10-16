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

## 2026-10-16 00:30–01:10: first boot with the kext (USB stick, build `46e1de4e`)

**The loader works.** `ioreg -l -w0 -c QcaBluetoothFirmware`: `Stage = 100`, `LastResult = 0`, `PatchVersion = 0x8567`, `Status = 0xe0`, `Log` = "rom 0x00190200 (WCN785x 2.0) … status 0xe0 / match: firmware already present" (the chip still had the firmware from the previous boot's userspace load, so the kext correctly sent nothing; a cold boot will exercise the download path). Device node renamed `Bluetooth USB Host Controller`, `bluetoothd` opened it (user clients on both interfaces).

**Then two stack problems, neither in the kext:**

1. **Transport.** `bluetoothd` logs `sysBoardID Mac-27AD2F918AE68F61` → `FindTransportType -- UART Transport` → `HCI Transport is set to H4BC`, and BlueTool's boot script waits on `/dev/cu.BLTH` (a UART) until it times out after 30 s: "BlueTool timed out running boot script!", daemon exits, launchd respawns it, forever. BlueToolFixup's `-btlfxboardid` rewrite is in the boot-args but has nothing to rewrite: `strings /usr/sbin/bluetoothd` has no `Mac-F60DEB81FF30ACF6` on 26.7.1 (MacPro6,1 is gone from Tahoe). The daemon honours **`BT_HCI_TRANSPORT`** from its environment before the board-id lookup ("BT_HCI_TRANSPORT not set - Attempting to read from plist"). `sudo launchctl setenv BT_HCI_TRANSPORT USB` + `kickstart -k system/com.apple.bluetoothd` gave: `HCI Transport is set to USB`, `BlueTool finished running boot script`, `Found external Bluetooth dongle -- setting chipset to THIRD_PARTY_DONGLE`, `[bm3_usb][Found_USB_Dongle] idVendor 0x0489 idProduct 0xE10A deviceClass 0xE0`. Persisted by `tools/install-transport-daemon.sh` (a LaunchDaemon that sets the variable at boot and restarts bluetoothd once). Upstream fix would be a BlueToolFixup change (rewrite a board-id string that still exists in Tahoe); to be reported.
2. **LE scan setup.** With USB transport the stack came up: `Local Device Address ac f2 3c 8d a6 9e` (the controller's own, read over HCI), SDP records, ATT; then `LE_InitMultipleAdvertising failed with status 152` (Apple-specific, expected on a third-party dongle), `Invalid chipset 0` (BlueToolFixup's chipset-range patch also has no match on 26.7.1; not fatal by itself), and finally `Reset scan in LE_Scan: STATUS 718` → `Bluetooth error - restarting { reason=718 }`, every 10 s. This is the Monterey-era Intel symptom: bluetoothd scans with a random own-address it never set. OpenIntelWireless's **IntelBTPatcher** fixes it by hooking `IOUSBHostDevice::deviceRequest` and sending LE Set Random Address before the first LE Set Scan Parameters after each HCI Reset. The hook is not Intel-specific, so it is built here (`scripts/build-intelbtpatcher.sh`) and installed by `tools/install-efi.sh`. Untested on this controller until the next boot.

Next boot = QcaBluetooth + BlueToolFixup + IntelBTPatcher (debug, `-ibtcompatdbg` dumps every HCI command to `sudo dmesg`) + the transport daemon. If the loop persists, the HCI dump shows which command the controller rejects.

## 2026-10-16 02:50–03:35: second kext boot, transport mechanism pinned down

Boot from the USB stick with QcaBluetooth + BlueToolFixup + IntelBTPatcher (debug) + the transport daemon. Loader: `Stage 100`, `LastResult 0` again (controller still held the firmware). `bluetoothd` took a *different* wrong path this time: `Searching for transport: "USB"` → `[bm3_usb][GetProductAndVendorID] -- Use internal Bluetooth USB Host Controller` → 15 s search for a controller that matches the (zeroed) internal-controller info → `Bluetooth fatal error - crashing { reason=114 }`, respawn, with launchd's backoff growing to 20 minutes (and `launchctl kickstart -k` blocking while a service is in that backoff: use `bootout` + `bootstrap` to reset it).

What was ruled out, one runtime test each: the `bluetoothExternalDongleFailed` flag (still 00), a `BT_HCI_TRANSPORT` key in `/Library/Preferences/com.apple.Bluetooth.plist` (a leftover of a `defaults write` test; removed), `bluetoothHostControllerSwitchBehavior=always` in NVRAM (no effect; removed), starting `bluetoothd` after the device exists, loading its job before or after the variable exists, and IntelBTPatcher (a boot with `-ibtcompatoff` behaved the same).

**The cause is where the variable lives.** The two runs differ in one log line inside the stack init ("Invoking customization callback"): the working run logs `BT_HCI_TRANSPORT not set - Attempting to read from plist` and then runs `[GetExternalDongleSwitchBehavior]` + `[bm3_usb][Found_USB_Dongle]` (external-dongle detection, `setting gUseInternalController to FALSE`); the failing runs log `Searching for transport: "USB"` straight away and never look for a dongle. The first instance reads the transport from launchd's domain environment early on; the stack init looks at the process's own environment. A system LaunchDaemon's `launchctl setenv` puts the variable in the system domain, every later `bluetoothd` inherits it into its process environment, and the dongle detection is skipped. The 00:41 run, and the confirming test at 03:33, used `launchctl setenv` **as the logged-in user** (gui/501 domain): the early check still resolves to USB, the process environment stays clean, and the dongle path is taken (`Use external Bluetooth USB Host Controller`, `Local Device Address ac f2 3c 8d a6 9e`). So the daemon became a per-user **LaunchAgent** (`tools/com.razvanccr.qcabt-transport.plist`, installed by `tools/install-transport-daemon.sh` into `~/Library/LaunchAgents`, old system daemon removed). `bluetoothd` keeps respawning on its own until the agent has run at login, so no restart step is needed.

Still open: the `reason=718` restart loop after `Local Device Address` (LE scan setup), seen again at 03:33 with IntelBTPatcher **disabled**. Next boot: patcher enabled (`-ibtcompatoff` removed, `-ibtcompatdbg` kept) to see whether its LE Set Random Address shim stops it.

Notes: the controller's USB location changed between boots (`0x02F00000` → `0x01F00000`, the XHCI enumeration order moved); `bluetoothd` does not care. Also the user's board is the MSI MAG X870 Tomahawk per the USB map comment in the config, not ASUS as first written in these notes.
