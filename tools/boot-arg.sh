#!/bin/bash
# boot-arg.sh <diskNsM> add|remove <arg>...   (run with sudo; edits boot-args in that EFI's OpenCore config.plist)
set -euo pipefail
DISK=$1; OP=$2; shift 2
diskutil info "$DISK" | grep -q 'Mounted: *Yes' || diskutil mount "$DISK" >/dev/null
EFI=$(diskutil info "$DISK" | awk -F': *' '/Mount Point/{print $2}')
CFG="$EFI/EFI/OC/config.plist"
cp "$CFG" "$CFG.bak-$(date +%Y%m%d-%H%M%S)"
python3 - "$CFG" "$OP" "$@" <<'PY'
import plistlib, sys
path, op, args = sys.argv[1], sys.argv[2], sys.argv[3:]
cfg = plistlib.load(open(path, 'rb'))
nv = cfg['NVRAM']['Add']['7C436110-AB2A-4BBB-A880-FE41995C9F82']
cur = nv.get('boot-args', '').split()
for a in args:
    if op == 'add' and a not in cur: cur.append(a)
    if op == 'remove' and a in cur: cur.remove(a)
nv['boot-args'] = ' '.join(cur)
plistlib.dump(cfg, open(path, 'wb'), sort_keys=False)
print('boot-args:', nv['boot-args'])
PY
plutil -lint "$CFG"
