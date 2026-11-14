#!/bin/bash
# Omnisampler -> TestFlight (iPad/iPhone app with the AUv3 inside)
bash "$(dirname "$0")/ship_ios.sh" Omnisampler
echo "Finished."
osascript -e 'tell application "Terminal" to close (every window whose name contains "ship_Omnisampler")' >/dev/null 2>&1 &
exit 0
