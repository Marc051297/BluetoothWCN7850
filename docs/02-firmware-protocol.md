# WCN7850 Bluetooth firmware download over USB

What Linux does for this device, written down so the macOS loader can be checked against it. Source: `drivers/bluetooth/btusb.c` in the Linux kernel (`btusb_setup_qca()` and friends, the state as of 2026-10). `btqca.c` is **not** involved: that file is the UART (`hci_qca`) path. Over USB the whole download is plain USB control and bulk traffic, no HCI vendor commands at all.

## Device identity

| | |
|---|---|
| USB ID | `0489:e10a` (Foxconn / Hon Hai), listed in `btusb_table` under "QCA WCN785x chipset" with `BTUSB_QCA_WCN6855 \| BTUSB_WIDEBAND_SPEECH` |
| Device class | `e0/01/01` (Wireless Controller / RF / Bluetooth), the standard HCI-over-USB class |
| Interface 0 | interrupt IN `0x81` (HCI events, 16-byte packets), bulk IN `0x82` and bulk OUT `0x02` (ACL), 64-byte packets |
| Interface 1 | isochronous `0x83`/`0x03` for SCO, alternates 0–7 (0, 9, 17, 25, 33, 49, 63, 65 bytes) |
| HCI commands | control transfers on EP0, class request `0x00` to the interface (not used by the loader) |
| USB speed | full speed (12 Mbit/s) |

The chip reports ROM version `0x00190200` ("WCN785x 2.0" in `qca_devices_table`), which is the one entry Linux has for the whole WCN785x family.

## Vendor requests (all on EP0, `bmRequestType` vendor, recipient device, `wValue = wIndex = 0`)

| bRequest | Direction | Length | Meaning |
|---|---|---|---|
| `0x09` `QCA_GET_TARGET_VERSION` | IN | 20 | version block, below |
| `0x05` `QCA_CHECK_STATUS` | IN | 1 | status bits: `0x80` rampatch loaded, `0x40` NVM (system config) loaded |
| `0x01` `QCA_DFU_DOWNLOAD` | OUT | header length | starts a download; the header bytes of the firmware file are the payload |

Version block (little endian):

```
struct qca_version {
    u32 rom_version;    // 0x00190200 here
    u32 patch_version;  // build number of the running patch; 0x000043fb from ROM
    u32 ram_version;    // 0x40170200 here (WCN6855 uses it to pick the "_gf" NVM variant)
    u8  chip_id;
    u8  platform_id;
    u16 flag;           // high byte 0x80 = "multi NVM": board id = chip_id << 8 | platform_id
    u8  reserved[4];
};
```

This chip answers `flag = 0x8000, chip_id = 0, platform_id = 0`, so the board id is 0 and the plain NVM file applies.

## Firmware files (linux-firmware, `qca/`)

- `rampatch_usb_<rom_version>.bin` → `qca/rampatch_usb_00190200.bin` (250 276 bytes)
- `nvm_usb_<rom_version>[_gf][_<board_id>].bin` → `qca/nvm_usb_00190200.bin` (9 470 bytes). The `_gf` suffix is only for the two WCN6855 GF RAM versions (`0x400c1200`, `0x400c1211`); the `_<board_id>` suffix only when the multi-NVM flag gives a non-zero board id. Files for board ids `0104` … `0112` exist for other WCN7850 boards.
- Two QCA2066 boards (`0x00130201` with board id `030a`/`030b`) take their files from a `QCA2066/` subdirectory. Not this chip.

Licence: `LICENSE.QualcommAtheros_ath10k` (redistributable binaries). The files are fetched by `scripts/fetch-firmware.sh` from linux-firmware release `20260309` and pinned in `firmware/SHA256SUMS`.

Per-ROM header sizes from `qca_devices_table`:

| ROM | rampatch header | NVM header | version offset in rampatch |
|---|---|---|---|
| Rome 1.x `0x0000010x` | 20 | 4 | 8 |
| Rome 2.x/3.x `0x000002xx`, `0x000003xx` | 28 | 4 | 16 |
| WCN6855 `0x001301xx`, `0x001302xx` | 40 | 4 | 16 |
| WCN785x `0x00190200` | 40 | 4 | 16 |

Rampatch version check: at the version offset the file holds `u16 rom_version_high, rom_version_low, patch_version`. For ROMs with a non-zero high half (everything after Rome), the file's ROM must equal `rom_version_high << 16 | rom_version_low` and its `patch_version` must be ≥ the controller's. Our file: ROM `0x00190200`, build `0x8567`; controller build `0x43fb`.

## Sequence (`btusb_setup_qca`)

1. `GET_TARGET_VERSION`; look the ROM version up in the table. Unknown ROM with a non-zero high half → assume nothing to load.
2. `CHECK_STATUS`.
3. If `0x80` is clear: download the rampatch.
4. `GET_TARGET_VERSION` again (the patch build changes).
5. If `0x40` is clear: download the NVM. For ROM ≥ `0x00130201` (WCN6855 2.1 and newer, so WCN7850 too) wait 100 ms: **the controller resets itself to apply the NVM.**
6. Linux then marks `HCI_QUIRK_BROKEN_ENHANCED_SETUP_SYNC_CONN` (enhanced SCO setup does not work with mSBC on these chips) and sets the Microsoft extension opcode `0xFD70`. Neither is something a macOS loader can tell the stack.

Download of one file (`btusb_setup_qca_download_fw`):

1. Control OUT `QCA_DFU_DOWNLOAD` with the first *header* bytes of the file (40 for the rampatch, 4 for the NVM).
2. Sleep 20 ms ("ep2 needs time to switch from function ACL to function DFU").
3. Send the rest of the file on **bulk OUT endpoint 2** in 4096-byte writes, 3 s timeout each. A short write is an error.

No acknowledgement is read; success is confirmed by re-reading the version block and the status bits.

## Other QCA-specific behaviour in btusb worth knowing

- `btusb_set_bdaddr_wcn6855`: the public address is set with HCI vendor command `0xfc14` carrying the 6-byte address byte-swapped. Not needed, the NVM carries an address.
- `btusb_classify_qca_pkt_type`: ACL frames with connection handle `0x2EDC` are debug/diagnostic packets, and handle `0x2EDD` carries firmware memory dumps. Apple's stack will log "unknown connection handle" if the chip emits them.
- `btusb_qca_reset`: a GPIO `BT_EN` reset on some laptops; not applicable to a desktop board.
- USB autosuspend reset-resume quirks apply to Rome only.

## What this means for macOS

The loader needs exactly three primitives: control transfers on EP0 of the device, `SetConfiguration(1)`, and bulk writes on EP `0x02` of interface 0. Both IOUSBLib (userspace) and `IOUSBHostDevice`/`IOUSBHostPipe` (kext) provide them. The NVM-induced reset is the only timing subtlety: wait, then re-read.
