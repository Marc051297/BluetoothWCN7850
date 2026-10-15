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
xcodebuild -project BrcmPatchRAM.xcodeproj -target BlueToolFixup -configuration Release \
  CODE_SIGNING_ALLOWED=NO CODE_SIGN_IDENTITY="" MACOSX_DEPLOYMENT_TARGET=10.13 -quiet
rm -rf "$OUT/BlueToolFixup.kext"
mkdir -p "$OUT"
cp -R build/Products/Release/BlueToolFixup.kext "$OUT/"
echo "BlueToolFixup $(plutil -extract CFBundleVersion raw "$OUT/BlueToolFixup.kext/Contents/Info.plist") (BrcmPatchRAM $(git rev-parse --short HEAD), Lilu $(git -C "$DEPS/Lilu" rev-parse --short HEAD)) -> $OUT/BlueToolFixup.kext"
