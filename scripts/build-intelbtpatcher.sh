#!/bin/bash
# build-intelbtpatcher.sh [out dir] -> <out>/IntelBTPatcher.kext and <out>/IntelBTPatcher-debug.kext
# IntelBTPatcher (OpenIntelWireless) is a Lilu plug-in that hooks IOUSBHostDevice::deviceRequest and, on the first
# LE Set Scan Parameters after each HCI Reset, sends an LE Set Random Address first. It is not Intel-specific:
# bluetoothd scans with a random own-address it never set, which some controllers reject. The debug build with
# -ibtcompatdbg also logs every HCI command (sudo dmesg).
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$PWD
OUT=${1:-$ROOT/build}
DEPS=$ROOT/deps
mkdir -p "$DEPS" "$OUT"
[ -d "$DEPS/IntelBluetoothFirmware/.git" ] || git clone -q --depth 1 https://github.com/OpenIntelWireless/IntelBluetoothFirmware "$DEPS/IntelBluetoothFirmware"
[ -d "$DEPS/Lilu/.git" ] || git clone -q --depth 1 https://github.com/acidanthera/Lilu "$DEPS/Lilu"
[ -d "$DEPS/MacKernelSDK/Headers" ] || git clone -q --depth 1 https://github.com/acidanthera/MacKernelSDK "$DEPS/MacKernelSDK"
cd "$DEPS/IntelBluetoothFirmware"
mkdir -p Lilu.kext/Contents/Resources
ln -sfn "$DEPS/Lilu/Lilu/Headers" Lilu.kext/Contents/Resources/Headers
ln -sfn "$DEPS/Lilu/Lilu/Library" Lilu.kext/Contents/Resources/Library
ln -sfn "$DEPS/MacKernelSDK" MacKernelSDK
for cfg in Release Debug; do
  xcodebuild -project IntelBluetoothFirmware.xcodeproj -target IntelBTPatcher -configuration $cfg \
    CODE_SIGNING_ALLOWED=NO CODE_SIGN_IDENTITY="" MACOSX_DEPLOYMENT_TARGET=12.0 -quiet
done
rm -rf "$OUT/IntelBTPatcher.kext" "$OUT/IntelBTPatcher-debug.kext"
cp -R build/Release/IntelBTPatcher.kext "$OUT/IntelBTPatcher.kext"
cp -R build/Debug/IntelBTPatcher.kext "$OUT/IntelBTPatcher-debug.kext"
echo "IntelBTPatcher $(plutil -extract CFBundleVersion raw "$OUT/IntelBTPatcher.kext/Contents/Info.plist") ($(git rev-parse --short HEAD), Lilu $(git -C "$DEPS/Lilu" rev-parse --short HEAD)) -> $OUT/IntelBTPatcher.kext, -debug.kext"
