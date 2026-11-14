#!/bin/bash
# Double-click: makes this Mac folder match GitHub (see tools/sync.sh). Result in tools/sync.log.
bash "$(dirname "$0")/sync.sh"
rc=$?
case $rc in
  0) osascript -e 'display notification "EvanPlugins matches GitHub" with title "Sync from GitHub"' ;;
  2) osascript -e 'display notification "Local changes - not synced. Tell Claude." with title "Sync from GitHub"' ;;
  *) osascript -e 'display notification "Could not sync - see tools/sync.log" with title "Sync from GitHub"' ;;
esac
osascript -e 'tell application "Terminal" to close (every window whose name contains "Sync from GitHub")' >/dev/null 2>&1 &
exit $rc
