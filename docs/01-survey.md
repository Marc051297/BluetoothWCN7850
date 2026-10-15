# Survey: the WCN7850 Bluetooth half on my X870 board (2026-10-15)

Machine: ASUS X870, macOS 26.7.1 Tahoe (Darwin 25.6.0), OpenCore, SMBIOS MacPro7,1 (board-id `Mac-27AD2F918AE68F61`), AMFI off. Kexts loaded at the time: Lilu 1.7.3, USBToolBox, no Bluetooth-related third-party kext.

## The device enumerates, nobody claims it

`ioreg -p IOUSB` shows it directly under the root hub of the XHCI controller at location `0x02f00000`:

```
IOUSBHostDevice@02f00000  idVendor 0x0489  idProduct 0xe10a  bcdDevice 0x0001
  bDeviceClass 0xe0  bDeviceSubClass 1  bDeviceProtocol 1   (Wireless Controller / RF / Bluetooth)
  USBSpeed 1 (full speed, 12 Mbit/s)  bNumConfigurations 1  bMaxPacketSize0 64
  no iManufacturer / iProduct / iSerialNumber strings
  USBPortType 2 (internal)
```

The node is `registered, matched, active` but has **no children**: no `AppleUSBHostCompositeDevice`, no interface nubs, `kUSBCurrentConfiguration` absent. Nothing in macOS has even sent `SetConfiguration` to it. The port-map worry from the Wi-Fi project did not apply any more: USBToolBox's map already exposes this port, otherwise the device would not be in the registry at all.

`tools/usbdesc` (IOUSBLib, reads descriptors without opening the device) gives the layout:

```
Configuration 1: 2 interfaces, bus powered + remote wakeup (0xe0), 100 mA
  Interface 0 alt 0  e0/01/01   EP 0x81 interrupt IN  16 bytes, interval 1   <- HCI events
                                EP 0x82 bulk IN       64 bytes                <- ACL in
                                EP 0x02 bulk OUT      64 bytes                <- ACL out, and the firmware body
  Interface 1 alt 0..7 e0/01/01 EP 0x83 / 0x03 isochronous, 0/9/17/25/33/49/63/65 bytes  <- SCO
```

HCI commands go to EP0 as a class request to the interface, per the Bluetooth USB transport spec. This is the stock layout every HCI-over-USB dongle has, so Apple's userspace USB transport in `bluetoothd` (see below) can drive it unchanged once firmware is in.

## What the chip says about itself

`tools/qcabt info` (vendor request 0x09 / 0x05, see [02-firmware-protocol.md](02-firmware-protocol.md)), fresh from power-on:

```
rom_version   0x00190200   (WCN785x 2.0, the single table entry Linux has for the family)
patch_version 0x000043fb   (ROM build)
ram_version   0x40170200
chip_id 0x00  platform_id 0x00  flag 0x8000  -> board_id 0
status        0x20          rampatch NOT loaded, NVM NOT loaded
```

So the files are `qca/rampatch_usb_00190200.bin` and `qca/nvm_usb_00190200.bin` (no board-id suffix).

## First download, from userspace

`tools/qcabt load` did the Linux sequence through IOUSBLib (SetConfiguration 1, vendor requests on EP0, bulk writes on EP 0x02):

```
before:         patch_version 0x000043fb  status 0x20
after rampatch: patch_version 0x00008567  status 0xa0   (250 276 bytes, 40-byte header)
after nvm:      patch_version 0x00008567  status 0xe0   (9 470 bytes, 4-byte header)
```

Observations:

- The whole thing takes well under a second at full speed.
- The post-NVM reset that Linux waits 100 ms for does **not** re-enumerate the device: the registry entry id stayed the same, the handle could be reopened right away, and `kUSBCurrentConfiguration` stayed 1. So a loader does not have to handle a disconnect/reconnect cycle.
- A second `qcabt load` is a no-op: both status bits are set and it skips both files. The firmware is held in RAM; it survives a USB reset but not a power cycle, so it has to be reloaded every boot (and after sleep, if the chip loses power).

## What bluetoothd does with it: nothing, yet

On Tahoe there is no `IOBluetoothHostControllerUSBTransport` kext any more (`IOBluetoothFamily.kext/Contents/PlugIns` is empty). USB controllers are driven from userspace by `bluetoothd` itself: its `bm3_usb` module opens the `IOUSBHostDevice` with IOUSBLib (`USBDeviceOpenSeize`, `SetConfiguration`, then the interrupt/bulk/isoch pipes), exactly like `qcabt` does. The relevant strings in `/usr/sbin/bluetoothd`:

```
FindTransportType -- USB Transport / UART Transport / PCIe Transport   (chosen by board-id)
[bm3_usb][Found_USB_Dongle] -- FoundBluetoothInterfaces() for deviceClass (%d) ... returned true
[bm3_usb][Found_USB_Dongle] -- deviceClass (%d), idVendor = 0x%04x, idProduct = 0x%04x -- Not in supported list
[bm3_usb] USBDeviceOpenSeize failed with %d
[bm3_usb] Configuring bluetooth device: %s [0x%08lx]
bluetoothExternalDongleFailed / bluetoothInternalControllerInfo / bluetoothHostControllerSwitchBehavior   (NVRAM)
```

Two gates stop it here:

1. **Transport type.** `FindTransportType` decides from the board-id. MacPro7,1 is not a "USB Bluetooth" board, so `system_profiler SPBluetoothDataType` reports a phantom `BCM_4350C2` controller on `Transport: UART`, state Off, and `bluetoothd` never looks at USB. Nothing about our device appears in its log after the firmware load.
2. **Vendor whitelist.** `Found_USB_Dongle` only accepts Broadcom (0x0a5c) and Apple vendor ids; everything else is "Not in supported list".

Both are what acidanthera's **BlueToolFixup** (from the BrcmPatchRAM repo) patches in `bluetoothd` at page-validation time: the vendor compare becomes an unconditional jump, a "bad chipset" range check is NOPed (Sequoia variant, which also covers Tahoe), and with the `-btlfxboardid` boot argument the board-id list is rewritten to contain the running board-id, which flips `FindTransportType` to USB. The README also asks for two NVRAM variables (`bluetoothExternalDongleFailed = 00`, `bluetoothInternalControllerInfo = 00×14`). This is the same recipe Intel and Broadcom USB controllers use on Monterey through Tahoe, and `IntelBluetoothFirmware` additionally renames the device node to `Bluetooth USB Host Controller` on Monterey+, which is the name `bluetoothd` uses for the controller entry.

Apple's own controllers get one more thing from `IOBluetoothFamily`'s `AppleUSBHostMergeProperties` personalities: `UsbUserClientEntitlementRequired = com.apple.bluetooth.control`, so only `bluetoothd` can open them. Our device carries no such property, which is why an ordinary tool can open it, and also why `bluetoothd` can.

## Conclusions for the design

- The firmware step is solved and small (three control requests, one bulk pipe).
- The ordering is the only hard part: `bluetoothd` must not seize the device before the firmware is in. A kext that matches the `IOUSBHostDevice` and loads the firmware in `start()` closes that window, because userspace first-match notifications fire only after kernel matching finished (this is why IntelBluetoothFirmware works that way).
- Getting `bluetoothd` to accept the controller at all is BlueToolFixup's job, not ours.

See [03-design.md](03-design.md).
