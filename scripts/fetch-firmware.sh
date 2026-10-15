#!/bin/bash
# fetch-firmware.sh -> kext/FwData.cpp
# Downloads the WCN785x Bluetooth firmware (RAM patch + every NVM variant for ROM 0x00190200)
# from linux-firmware, checks it against firmware/SHA256SUMS, and embeds it into the kext.
# The firmware is Qualcomm's (LICENSE.QualcommAtheros_ath10k): redistributable in binary form,
# so it is fetched at build time and never committed.
set -euo pipefail
cd "$(dirname "$0")/.."
TAG=20260309
BASE=https://gitlab.com/kernel-firmware/linux-firmware/-/raw/$TAG
DL=firmware/dl
mkdir -p "$DL"

fetch() { [ -s "$DL/$2" ] || curl -fsSL -o "$DL/$2" "$BASE/$1"; }
fetch qca/rampatch_usb_00190200.bin rampatch_usb_00190200.bin
for v in "" _0104 _0106 _0108 _0109 _010a _010c _010e _0110 _0111 _0112; do
  fetch qca/nvm_usb_00190200$v.bin nvm_usb_00190200$v.bin
done
fetch qca/NOTICE.txt NOTICE.txt
fetch LICENSE.QualcommAtheros_ath10k LICENSE.QualcommAtheros_ath10k

if [ -f firmware/SHA256SUMS ]; then
  (cd "$DL" && shasum -a 256 -c ../SHA256SUMS)
else
  (cd "$DL" && shasum -a 256 * > ../SHA256SUMS)
  echo "recorded firmware/SHA256SUMS (commit it)"
fi

python3 -I scripts/fw_gen.py kext/FwData.cpp "$DL"/rampatch_usb_*.bin "$DL"/nvm_usb_*.bin
