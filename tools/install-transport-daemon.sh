#!/bin/bash
# install-transport-daemon.sh [--remove]
# Installs the per-user LaunchAgent that tells bluetoothd to use the USB transport (see the comment in the plist), as the
# logged-in user, no sudo. It also removes the earlier system-daemon variant if present (that part asks for sudo).
# --remove takes the agent out again.
set -euo pipefail
SRC="$(cd "$(dirname "$0")" && pwd)/com.razvanccr.qcabt-transport.plist"
AGENT="$HOME/Library/LaunchAgents/com.razvanccr.qcabt-transport.plist"
OLD=/Library/LaunchDaemons/com.razvanccr.qcabt-transport.plist
if [ -f "$OLD" ]; then
  echo "removing the old system daemon $OLD (sudo)"
  sudo launchctl bootout system "$OLD" 2>/dev/null || true
  sudo rm -f "$OLD"
  sudo launchctl unsetenv BT_HCI_TRANSPORT 2>/dev/null || true
fi
if [ "${1:-}" = --remove ]; then
  launchctl bootout "gui/$(id -u)" "$AGENT" 2>/dev/null || true
  rm -f "$AGENT"; launchctl unsetenv BT_HCI_TRANSPORT || true
  echo "removed; bluetoothd falls back to the board-id transport at its next start"; exit 0
fi
mkdir -p "$(dirname "$AGENT")"
install -m 644 "$SRC" "$AGENT"
launchctl bootout "gui/$(id -u)" "$AGENT" 2>/dev/null || true
launchctl bootstrap "gui/$(id -u)" "$AGENT"
echo "installed $AGENT; gui domain has BT_HCI_TRANSPORT: $(launchctl print "gui/$(id -u)" | grep -c BT_HCI)"
