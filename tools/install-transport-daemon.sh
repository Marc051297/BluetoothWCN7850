#!/bin/bash
# install-transport-daemon.sh [--remove]
# Installs /Library/LaunchDaemons/com.razvanccr.qcabt-transport.plist (see the comment inside it) and applies it now.
# Run with sudo. --remove takes it out again.
set -euo pipefail
SRC="$(cd "$(dirname "$0")" && pwd)/com.razvanccr.qcabt-transport.plist"
DST=/Library/LaunchDaemons/com.razvanccr.qcabt-transport.plist
if [ "${1:-}" = --remove ]; then
  launchctl bootout system "$DST" 2>/dev/null || true
  rm -f "$DST"; launchctl unsetenv BT_HCI_TRANSPORT || true
  echo "removed; bluetoothd falls back to the board-id transport at its next start"; exit 0
fi
install -o root -g wheel -m 644 "$SRC" "$DST"
launchctl bootout system "$DST" 2>/dev/null || true
launchctl bootstrap system "$DST"
echo "installed $DST; waiting for bluetoothd"; sleep 6
/usr/bin/log show --last 10s --predicate 'process == "bluetoothd" AND eventMessage CONTAINS "Transport is set"' --style compact | tail -2
