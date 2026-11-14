#!/bin/bash
# Ships Omnisampler, Contour EQ and Capture Rack (iPad/iPhone apps with the AUv3 inside) to TestFlight, one after another.
# Each writes its own log: build-ios-<Plugin>/tools/testflight.log. Summary: tools/ship_all_ios.log
cd "$(dirname "$0")" || exit 1
: > ship_all_ios.log
for P in Omnisampler ContourEQ CaptureRack; do
  echo "$(date '+%H:%M') start $P" >> ship_all_ios.log
  bash ./ship_ios.sh "$P"; rc=$?
  echo "$(date '+%H:%M') $P exit $rc" >> ship_all_ios.log
done
echo "$(date '+%H:%M') ALL DONE" >> ship_all_ios.log
osascript -e 'tell application "Terminal" to close (every window whose name contains "ship_all_ios")' >/dev/null 2>&1 &
exit 0
