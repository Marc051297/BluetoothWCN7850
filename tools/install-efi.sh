#!/bin/bash
# Install QcaBluetooth.kext + BlueToolFixup.kext into the OpenCore EFI and
# patch config.plist as described in docs/04-bringup.md.  Run with sudo:
#
#   sudo bash tools/install-efi.sh [disk1s2]
#
# Idempotent: re-running replaces the kexts and leaves an already patched
# config.plist alone.  A backup of config.plist is kept next to it.
set -euo pipefail

DISK="${1:-disk1s2}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$REPO/build"

for k in QcaBluetooth BlueToolFixup; do
    [ -x "$BUILD/$k.kext/Contents/MacOS/$k" ] || { echo "missing $BUILD/$k.kext"; exit 1; }
done

if [ "$(diskutil info "$DISK" | awk -F': *' '/Mounted/{print $2}')" != "Yes" ]; then
    diskutil mount "$DISK"
fi
EFI="$(diskutil info "$DISK" | awk -F': *' '/Mount Point/{print $2}')"
OC="$EFI/EFI/OC"
CFG="$OC/config.plist"
[ -f "$CFG" ] || { echo "no config.plist at $CFG"; exit 1; }
echo "EFI at $EFI"

STAMP="$(date +%Y%m%d-%H%M%S)"
cp "$CFG" "$OC/config.plist.bak-$STAMP"
cp "$CFG" "$REPO/build/config.plist.before-$STAMP"
echo "backup: $OC/config.plist.bak-$STAMP"

for k in QcaBluetooth BlueToolFixup; do
    rm -rf "$OC/Kexts/$k.kext"
    cp -R "$BUILD/$k.kext" "$OC/Kexts/$k.kext"
    echo "copied $k.kext"
done

python3 - "$CFG" <<'PY'
import plistlib, sys
path = sys.argv[1]
with open(path, 'rb') as f:
    cfg = plistlib.load(f)

add = cfg['Kernel']['Add']
names = [e['BundlePath'] for e in add]
if 'Lilu.kext' not in names:
    sys.exit('Lilu.kext is not in Kernel->Add; BlueToolFixup needs it')

def entry(name, comment):
    return {'Arch': 'Any', 'BundlePath': f'{name}.kext', 'Comment': comment,
            'Enabled': True, 'ExecutablePath': f'Contents/MacOS/{name}',
            'MaxKernel': '', 'MinKernel': '', 'PlistPath': 'Contents/Info.plist'}

changed = []
if 'BlueToolFixup.kext' not in names:
    i = names.index('Lilu.kext') + 1
    add.insert(i, entry('BlueToolFixup', 'Lets bluetoothd use the WCN7850 USB controller (2.7.3, built from source)'))
    names.insert(i, 'BlueToolFixup.kext')
    changed.append('Kernel->Add BlueToolFixup after Lilu')
if 'QcaBluetooth.kext' not in names:
    i = names.index('BlueToolFixup.kext') + 1
    add.insert(i, entry('QcaBluetooth', 'WCN7850 Bluetooth firmware loader (0.1.0)'))
    names.insert(i, 'QcaBluetooth.kext')
    changed.append('Kernel->Add QcaBluetooth after BlueToolFixup')
for e in add:
    if e['BundlePath'] in ('BlueToolFixup.kext', 'QcaBluetooth.kext') and not e.get('Enabled'):
        e['Enabled'] = True
        changed.append(f"enabled {e['BundlePath']}")

GUID = '7C436110-AB2A-4BBB-A880-FE41995C9F82'
nv_add = cfg['NVRAM'].setdefault('Add', {}).setdefault(GUID, {})
nv_del = cfg['NVRAM'].setdefault('Delete', {}).setdefault(GUID, [])

args = nv_add.get('boot-args', '')
if '-btlfxboardid' not in args.split():
    nv_add['boot-args'] = (args + ' -btlfxboardid').strip()
    changed.append('boot-args += -btlfxboardid')
if nv_add.get('bluetoothExternalDongleFailed') != b'\x00':
    nv_add['bluetoothExternalDongleFailed'] = b'\x00'
    changed.append('NVRAM Add bluetoothExternalDongleFailed=00')
if nv_add.get('bluetoothInternalControllerInfo') != bytes(14):
    nv_add['bluetoothInternalControllerInfo'] = bytes(14)
    changed.append('NVRAM Add bluetoothInternalControllerInfo=14x00')
for key in ('bluetoothExternalDongleFailed', 'bluetoothInternalControllerInfo'):
    if key not in nv_del:
        nv_del.append(key)
        changed.append(f'NVRAM Delete {key}')

if changed:
    with open(path, 'wb') as f:
        plistlib.dump(cfg, f, sort_keys=False)
    print('config.plist changed:')
    for c in changed:
        print('  ' + c)
else:
    print('config.plist already patched, nothing changed')

print('Kernel->Add order now:')
for e in add:
    print(f"  {'on ' if e.get('Enabled') else 'off'} {e['BundlePath']}")
print('boot-args:', nv_add.get('boot-args'))
PY

plutil -lint "$CFG"
if OCV="$(command -v ocvalidate 2>/dev/null)"; then "$OCV" "$CFG" || true; fi
ls -la "$OC/Kexts" | grep -E 'Lilu|BlueTool|QcaBluetooth'
echo "done; reboot to test"
