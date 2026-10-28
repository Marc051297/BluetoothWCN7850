#!/bin/bash
# build-bluetoolfixup.sh [out dir] -> <out>/BlueToolFixup.kext
# BlueToolFixup is acidanthera's Lilu plug-in that makes bluetoothd accept a non-Apple, non-Broadcom
# USB controller (it lives in the BrcmPatchRAM repo). Built here from source so the version matches the
# running Lilu; the release zip from https://github.com/acidanthera/BrcmPatchRAM/releases works just as well.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
OUT=${1:-$ROOT/build}
DEPS=$ROOT/deps
mkdir -p "$DEPS"
[ -d "$DEPS/BrcmPatchRAM/.git" ] || git clone -q --depth 1 https://github.com/acidanthera/BrcmPatchRAM "$DEPS/BrcmPatchRAM"
[ -d "$DEPS/Lilu/.git" ] || git clone -q --depth 1 https://github.com/acidanthera/Lilu "$DEPS/Lilu"
[ -d "$DEPS/MacKernelSDK/Headers" ] || git clone -q --depth 1 https://github.com/acidanthera/MacKernelSDK "$DEPS/MacKernelSDK"
cd "$DEPS/BrcmPatchRAM"
# The Xcode project expects a Lilu debug kext (Contents/Resources/{Headers,Library}) and MacKernelSDK next to it.
mkdir -p Lilu.kext/Contents/Resources
ln -sfn "$DEPS/Lilu/Lilu/Headers" Lilu.kext/Contents/Resources/Headers
ln -sfn "$DEPS/Lilu/Lilu/Library" Lilu.kext/Contents/Resources/Library
ln -sfn "$DEPS/MacKernelSDK" MacKernelSDK
# Tahoe fix for the -btlfxboardid patch: BlueToolFixup rewrites the first entry of bluetoothd's "boards with USB
# Bluetooth" table (MacPro6,1, Mac-F60DEB81FF30ACF6) to the running board-id, but macOS 26 dropped MacPro6,1 and the
# string is gone from bluetoothd and BlueTool, so the patch finds nothing and bluetoothd keeps the UART transport on a
# MacPro7,1 SMBIOS. The iMac14,2 entry (Mac-9F18E312C5C2BF0B, same length) is still present in both binaries on 26.7.1.
python3 - BrcmPatchRAM/BlueToolFixup.cpp <<'PY'
import sys; p=sys.argv[1]; s=open(p).read()
s=s.replace('kBoardIdSize = sizeof("Mac-F60DEB81FF30ACF6")','kBoardIdSize = sizeof("Mac-9F18E312C5C2BF0B")')
s=s.replace('    "Mac-F60DEB81FF30ACF6",\n','    "Mac-9F18E312C5C2BF0B",  /* was MacPro6,1; gone in macOS 26, see QcaBluetooth docs */\n',1)
open(p,'w').write(s)
PY
grep -q 'was MacPro6,1' BrcmPatchRAM/BlueToolFixup.cpp || { echo "STOP: board-id patch did not apply"; exit 1; }
# LE event mask fix, active with the -btlfxlemask boot-arg: bluetoothd gives a THIRD_PARTY_DONGLE the spec-default
# LE event mask (0x1F), which leaves out LE Data Length Change and LE PHY Update Complete, yet its link-ready state
# machine waits for them. BLE HID devices (Xbox controller, mice) then connect but never pair. The patch widens the
# mask to 0x87F (adds bits 5, 6 and 11, as Apple's own mask has) in bluetoothd's 26.x code path and table, plus the
# 27.x table. See docs/04-bringup.md, "BLE HID devices connect but never pair".
grep -q kDongleLEEventMaskCode26Original BrcmPatchRAM/BlueToolFixup.cpp || git apply "$ROOT/patches/bluetoolfixup-le-event-mask.patch"
grep -q kDongleLEEventMaskCode26Original BrcmPatchRAM/BlueToolFixup.cpp || { echo "STOP: LE event mask patch did not apply"; exit 1; }
xcodebuild -project BrcmPatchRAM.xcodeproj -target BlueToolFixup -configuration Release \
  CODE_SIGNING_ALLOWED=NO CODE_SIGN_IDENTITY="" MACOSX_DEPLOYMENT_TARGET=12.0 -quiet
rm -rf "$OUT/BlueToolFixup.kext"
mkdir -p "$OUT"
cp -R build/Products/Release/BlueToolFixup.kext "$OUT/"
echo "BlueToolFixup $(plutil -extract CFBundleVersion raw "$OUT/BlueToolFixup.kext/Contents/Info.plist") (BrcmPatchRAM $(git rev-parse --short HEAD), Lilu $(git -C "$DEPS/Lilu" rev-parse --short HEAD)) -> $OUT/BlueToolFixup.kext"
