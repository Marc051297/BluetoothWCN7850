#!/bin/bash
# Install BluetoothWCN7850.kext, BlueToolFixup.kext and IntelBTPatcher.kext into an OpenCore EFI
# and patch config.plist as described in docs/04-bringup.md.  Run with sudo:
#
#   sudo bash tools/install-efi.sh <diskNsM>        (the USB stick's EFI; find it by GUID 1623DE29-…, numbers move)
#   QCABT_PATCHER=debug sudo -E bash tools/install-efi.sh <diskNsM>   (debug IntelBTPatcher + -ibtcompatdbg)
#
# Idempotent: re-running replaces the kexts and leaves an already patched
# config.plist alone.  A backup of config.plist is kept next to it.
set -euo pipefail

DISK="${1:-disk1s2}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$REPO/build"

# IntelBTPatcher (OpenIntelWireless, built by scripts/build-intelbtpatcher.sh) is installed too: bluetoothd's LE scan
# setup fails on this controller without its LE Set Random Address shim. QCABT_PATCHER=debug installs the debug build
# (and adds -ibtcompatdbg, which hex-dumps every HCI command into the kernel log, sudo dmesg); default is the release build.
PATCHER_SRC="$BUILD/IntelBTPatcher${QCABT_PATCHER:+-$QCABT_PATCHER}.kext"
[ "${QCABT_PATCHER:-}" = release ] && PATCHER_SRC="$BUILD/IntelBTPatcher.kext"
for k in BluetoothWCN7850 BlueToolFixup; do
    [ -x "$BUILD/$k.kext/Contents/MacOS/$k" ] || { echo "missing $BUILD/$k.kext"; exit 1; }
done
[ -x "$PATCHER_SRC/Contents/MacOS/IntelBTPatcher" ] || { echo "missing $PATCHER_SRC (run scripts/build-intelbtpatcher.sh)"; exit 1; }

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

rm -rf "$OC/Kexts/QcaBluetooth.kext"   # the kext's name before 1.0.0
for k in BluetoothWCN7850 BlueToolFixup; do
    rm -rf "$OC/Kexts/$k.kext"
    cp -R "$BUILD/$k.kext" "$OC/Kexts/$k.kext"
    echo "copied $k.kext"
done
rm -rf "$OC/Kexts/IntelBTPatcher.kext"
cp -R "$PATCHER_SRC" "$OC/Kexts/IntelBTPatcher.kext"
echo "copied IntelBTPatcher.kext from $PATCHER_SRC"

QCABT_VERSION=$(cat "$REPO/VERSION")
python3 - "$CFG" "${QCABT_PATCHER:-release}" "$QCABT_VERSION" <<'PY'
import plistlib, sys
path = sys.argv[1]
patcher_debug = sys.argv[2] == 'debug'
version = sys.argv[3]
comments = {
    'BlueToolFixup.kext': 'BlueToolFixup 2.7.3 with the Tahoe board-id fix (QcaBluetooth build): lets bluetoothd use a third-party USB controller',
    'IntelBTPatcher.kext': 'IntelBTPatcher 2.5.0 (OpenIntelWireless, built from source): LE Set Random Address shim, needed by any third-party controller',
    'BluetoothWCN7850.kext': f'BluetoothWCN7850 {version}: WCN7850 Bluetooth firmware loader (0489:e10a)',
}
with open(path, 'rb') as f:
    cfg = plistlib.load(f)

add = cfg['Kernel']['Add']
for e in add:   # the kext's name before 1.0.0
    if e['BundlePath'] == 'QcaBluetooth.kext':
        e['BundlePath'] = 'BluetoothWCN7850.kext'; e['ExecutablePath'] = 'Contents/MacOS/BluetoothWCN7850'
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
    add.insert(i, entry('BlueToolFixup', comments['BlueToolFixup.kext']))
    names.insert(i, 'BlueToolFixup.kext')
    changed.append('Kernel->Add BlueToolFixup after Lilu')
if 'BluetoothWCN7850.kext' not in names:
    i = names.index('BlueToolFixup.kext') + 1
    add.insert(i, entry('BluetoothWCN7850', comments['BluetoothWCN7850.kext']))
    names.insert(i, 'BluetoothWCN7850.kext')
    changed.append('Kernel->Add BluetoothWCN7850 after BlueToolFixup')
if 'IntelBTPatcher.kext' not in names:
    i = names.index('BlueToolFixup.kext') + 1
    add.insert(i, entry('IntelBTPatcher', comments['IntelBTPatcher.kext']))
    names.insert(i, 'IntelBTPatcher.kext')
    changed.append('Kernel->Add IntelBTPatcher after BlueToolFixup')
for e in add:
    if e['BundlePath'] in comments:
        if not e.get('Enabled'):
            e['Enabled'] = True
            changed.append(f"enabled {e['BundlePath']}")
        if e.get('Comment') != comments[e['BundlePath']]:
            e['Comment'] = comments[e['BundlePath']]
            changed.append(f"comment {e['BundlePath']}")

GUID = '7C436110-AB2A-4BBB-A880-FE41995C9F82'
nv_add = cfg['NVRAM'].setdefault('Add', {}).setdefault(GUID, {})
nv_del = cfg['NVRAM'].setdefault('Delete', {}).setdefault(GUID, [])

args = nv_add.get('boot-args', '')
if '-btlfxboardid' not in args.split():
    nv_add['boot-args'] = (args + ' -btlfxboardid').strip()
    changed.append('boot-args += -btlfxboardid')
args = nv_add.get('boot-args', '')
if patcher_debug and '-ibtcompatdbg' not in args.split():
    nv_add['boot-args'] = (args + ' -ibtcompatdbg').strip()
    changed.append('boot-args += -ibtcompatdbg')
if not patcher_debug and '-ibtcompatdbg' in args.split():
    nv_add['boot-args'] = ' '.join(a for a in args.split() if a != '-ibtcompatdbg')
    changed.append('boot-args -= -ibtcompatdbg')
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
ls -la "$OC/Kexts" | grep -E 'Lilu|BlueTool|BluetoothWCN7850|IntelBTPatcher'
echo "done; reboot to test"
