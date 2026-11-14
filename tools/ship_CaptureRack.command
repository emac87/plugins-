#!/bin/bash
# CaptureRack -> TestFlight (iPad/iPhone app with the AUv3 inside)
bash "$(dirname "$0")/ship_ios.sh" CaptureRack
echo "Finished."
osascript -e 'tell application "Terminal" to close (every window whose name contains "ship_CaptureRack")' >/dev/null 2>&1 &
exit 0
