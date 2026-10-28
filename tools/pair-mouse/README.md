# pair-mouse

Pairs a Bluetooth LE HID device (mouse, keyboard, game controller) from Terminal. It scans for the HID service (`0x1812`) and connects to the first connectable device it sees, within about a second of it appearing, so a device that changes its address every minute or so in pairing mode cannot slip away. Once the link is up, macOS's own `BTLEServer` claims the HID service and pairs it ("Just Works"); the tool also reads the HID Report Map, which needs an encrypted link, as a second pairing trigger.

Build (Intel binary for the hackintosh; works the same for arm64):

```zsh
swiftc -O -target x86_64-apple-macos12 pair-mouse.swift -o pair-mouse -Xlinker -sectcreate -Xlinker __TEXT -Xlinker __info_plist -Xlinker Info.plist
codesign -s - -f pair-mouse
```

Run it in Terminal on the machine itself (not over SSH: Bluetooth permission belongs to Terminal), allow Bluetooth access the first time, start it, then put the device in pairing mode:

```zsh
./pair-mouse
./pair-mouse GD
```

The optional argument limits it to devices whose advertised name contains that text. It prints every HID device it sees, gives up after 3 minutes, and exits about 25 s after connecting.
