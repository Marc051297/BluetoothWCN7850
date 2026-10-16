#!/bin/bash
# build.sh [out dir]  ->  <out>/BluetoothWCN7850.kext
# Plain clang against acidanthera's MacKernelSDK (no Xcode project). MacKernelSDK is cloned into deps/ on first run.
set -euo pipefail
cd "$(dirname "$0")"
ROOT=$PWD
OUT=${1:-$ROOT/build}
MKSDK=$ROOT/deps/MacKernelSDK
VERSION=$(cat VERSION)
[ -d "$MKSDK/Headers" ] || git clone -q --depth 1 https://github.com/acidanthera/MacKernelSDK "$MKSDK"
[ -f kext/FwData.cpp ] || { echo "STOP: run scripts/fetch-firmware.sh first"; exit 1; }

OBJ=$OUT/obj; KEXT=$OUT/BluetoothWCN7850.kext
mkdir -p "$OBJ" "$KEXT/Contents/MacOS"

COMMON=(-arch x86_64 -mmacosx-version-min=12.0 -fapple-kext -mkernel -nostdinc
  -isystem "$MKSDK/Headers" -I kext
  -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT
  -DQCABT_VERSION=\"$VERSION\" -DGIT_COMMIT=\"$(git rev-parse --short HEAD 2>/dev/null || echo none)\"
  -x c++ -std=gnu++14 -fno-rtti -fno-exceptions -fno-builtin -fno-common -fno-cxx-exceptions
  -O2 -g -Wall -Wno-unused-parameter)
[ "${DEBUG:-0}" = 1 ] && COMMON+=(-DDEBUG=1 -O0)

for s in kext/QcaBluetooth.cpp kext/FwData.cpp; do
  o=$OBJ/$(basename "$s").o
  if [ ! -f "$o" ] || [ "$s" -nt "$o" ] || [ kext/QcaBluetooth.hpp -nt "$o" ] || [ kext/FwData.h -nt "$o" ]; then
    xcrun clang++ "${COMMON[@]}" -c "$s" -o "$o"
  fi
done
# kmod_info (what Xcode generates for kext targets) must keep C symbol names.
xcrun clang -arch x86_64 -mmacosx-version-min=12.0 -mkernel -nostdinc -isystem "$MKSDK/Headers" \
  -DKERNEL -DQCABT_VERSION=\"$VERSION\" -x c -c kext/kmod_info.c -o "$OBJ/kmod_info.o"

xcrun clang++ -arch x86_64 -mmacosx-version-min=12.0 -fapple-kext -nostdlib -Xlinker -kext -Xlinker -no_data_const -Xlinker -ld_classic \
  -L"$MKSDK/Library/x86_64" -lkmodc++ -lkmod -lcc_kext "$OBJ"/*.o -o "$KEXT/Contents/MacOS/BluetoothWCN7850"
strip -S "$KEXT/Contents/MacOS/BluetoothWCN7850"
for sym in _kmod_info __start __stop __realmain; do
  nm "$KEXT/Contents/MacOS/BluetoothWCN7850" | grep -q " $sym\$" || { echo "STOP: $sym missing from the kext"; exit 1; }
done
KERNEL_BIN=/System/Library/Kernels/kernel
if [ -f "$KERNEL_BIN" ]; then
  clash=$(comm -12 <(nm -gU "$KEXT/Contents/MacOS/BluetoothWCN7850" | awk '{print $3}' | sort -u) \
                   <(nm -gU "$KERNEL_BIN" | awk '{print $3}' | sort -u) \
          | grep -vE '^(_kmod_info|__start|__stop|__realmain|__antimain|_kext_apple_cc)$' || true)
  [ -z "$clash" ] || { echo "STOP: the kext defines kernel symbols: $clash"; exit 1; }
fi
sed "s/@VERSION@/$VERSION/g" Info.plist > "$KEXT/Contents/Info.plist"
echo "BluetoothWCN7850 $VERSION $(shasum -a 256 "$KEXT/Contents/MacOS/BluetoothWCN7850" | cut -c1-8) $(stat -f %z "$KEXT/Contents/MacOS/BluetoothWCN7850") B -> $KEXT"
