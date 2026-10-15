# Design: how the firmware gets in, and who talks to the controller afterwards

## The two halves of the problem

1. **Firmware.** Every boot (and possibly every wake), 250 KB of RAM patch and 9 KB of NVM must go into the chip through three vendor control requests and one bulk endpoint ([02-firmware-protocol.md](02-firmware-protocol.md)). Proven from userspace with `tools/qcabt` ([01-survey.md](01-survey.md)).
2. **The stack.** On Monterey and later there is no kernel USB transport for Bluetooth. `bluetoothd` opens the `IOUSBHostDevice` itself with IOUSBLib, and it only does so for board-ids it considers "USB Bluetooth" boards and for vendor ids on its whitelist. Nothing in this project can change that from the outside; acidanthera's **BlueToolFixup** patches both checks and has been the standard answer for Intel and Broadcom USB controllers since macOS 12 (it declares support up to Tahoe in its `PluginConfiguration`).

So the real choice is only about half 1: where does the loader run, and how does it stay out of `bluetoothd`'s way.

## Option A: userspace loader (LaunchDaemon + IOUSBLib)

What `tools/qcabt` already is, wrapped in a launchd job.

- Pro: no kernel code, trivial to debug, `qcabt` is kept anyway as the diagnostic tool.
- Con, and it is fatal: **ordering.** `bluetoothd` starts early and polls for USB controllers ("Waiting for bluetooth devices..."), then `USBDeviceOpenSeize`s the first one it likes. Once it holds the device, a userspace loader cannot open it (exclusive access; the survey shows `qcabt` would need `-s` to seize it back, which yanks it from `bluetoothd`). The loader would have to win a race at boot, or seize the device, upload, and then force a re-enumeration so that `bluetoothd` starts over. Doable, ugly, and it leaves a window where `bluetoothd` initialises an unpatched ROM.
- Con: userspace cannot rename the registry node ("Bluetooth USB Host Controller"), which IntelBluetoothFirmware does for `bluetoothd`'s benefit.
- Con: nothing hooks wake from sleep reliably.

## Option B: kext matching the USB device (the IntelBluetoothFirmware / BrcmPatchRAM3 pattern)

An `IOService` with `IOProviderClass = IOUSBHostDevice`, `idVendor/idProduct` of the controller, `IOProbeScore 4000`, its own `IOMatchCategory`. In `start()` it opens the device, runs the download, closes the device, and sets `FirmwareLoaded` plus the node name. The device is "matched" only when `start()` returns, so `bluetoothd`'s first-match notification cannot fire before the firmware is in. The kext never holds the device afterwards, so `bluetoothd` can seize it.

This is exactly what the two proven Hackintosh loaders do:

| | IntelBluetoothFirmware | BrcmPatchRAM3 | QcaBluetooth |
|---|---|---|---|
| Matches | `IOUSBHostDevice`, vendor 8087, score 4000 | `IOUSBHostDevice`, vendor 0a5c, score 4000 | `IOUSBHostDevice`, 0489:e10a and the other WCN785x ids from btusb, score 4000 |
| Upload path | HCI vendor commands over EP0 + bulk | HCI vendor commands (`.hcd` files) over EP0, events on the interrupt pipe | 3 vendor control requests + bulk OUT, no HCI at all |
| After upload | close device, set `FirmwareLoaded`, `setName("Bluetooth USB Host Controller")` on 12+ | close device | same as Intel |
| Stack glue on 12+ | BlueToolFixup (+ IntelBTPatcher for a LE scan quirk) | BlueToolFixup | BlueToolFixup |
| Firmware | embedded (zlib) | embedded (`BrcmFirmwareData`) | embedded, generated at build time |

The QCA download is the simplest of the three: no HCI events to wait for, no vendor command framing, so the kext has no interrupt pipe, no completion callbacks and no locks.

## Decision: Option B, with the userspace tool kept for bring-up

`kext/QcaBluetooth.cpp` is the loader. `tools/qcabt` stays as the way to look at the chip (`info`) and to load firmware by hand when the kext is not installed. Both implement the same sequence, so a disagreement between them is a bug to look at.

Details worth writing down:

- **Status bits first.** The kext reads `CHECK_STATUS` and only sends what is missing, so a warm reboot where the chip kept its RAM is a no-op, and so is a second match of the same device.
- **Power management.** The kext joins the PM tree under the device (`PMinit`/`joinPMtree`) so it sees the device's off/on transitions. After an off→on it schedules a thread that tries to open the device and re-run the status check. If `bluetoothd` still holds the device (likely), the open fails and the kext logs and leaves it alone: a wake-from-sleep reload is **not** solved yet, only attempted. If the chip loses power in S3 and the device re-enumerates, the normal match path handles it.
- **Configuration.** `setConfiguration(1, true)` creates the interface nubs. `bluetoothd` sets configuration 1 again later; that is fine, it is what it does with any dongle.
- **Name.** `setName("Bluetooth USB Host Controller")` on Darwin ≥ 21 is cargo-culted from IntelBluetoothFirmware. I could not find a hard requirement for it in `bluetoothd`'s strings; it is harmless.
- **No HCI reset, no bdaddr.** The NVM carries the address; Linux only sets one when the platform asks.
- **Boot argument `-qcabtoff`** disables matching, for bisection.

## What BlueToolFixup needs from the user's side

From the BrcmPatchRAM README and the source:

- `Lilu.kext` then `BlueToolFixup.kext` in OpenCore's `Kernel → Add` (Lilu is already loaded here).
- Boot argument **`-btlfxboardid`**: on Sonoma and newer the board-id rewrite is opt-in, and this machine's `Mac-27AD2F918AE68F61` (MacPro7,1) is not a USB-Bluetooth board, so without it `FindTransportType` stays on UART and `bluetoothd` never looks at USB.
- NVRAM (`7C436110-AB2A-4BBB-A880-FE41995C9F82`): `bluetoothExternalDongleFailed = 00`, `bluetoothInternalControllerInfo = 00 00 00 00 00 00 00 00 00 00 00 00 00 00` (14 bytes). Put them in OpenCore `NVRAM → Add` and `NVRAM → Delete`.
- Optional `-btlfxallowanyaddr` if `bluetoothd` complains that the dongle has the same address as an internal module. Not expected here.

## Still open

- Whether `bluetoothd` on 26.7 is happy with the controller once it opens it (SCO alternates, the `0x2EDC` debug ACL frames, the broken enhanced SCO setup that Linux quirks around). Only a boot with both kexts installed answers that: see [04-bringup.md](04-bringup.md).
- Wake from sleep, as above.
