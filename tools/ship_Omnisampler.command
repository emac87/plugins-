#!/bin/bash
# Omnisampler -> TestFlight (iPad/iPhone app with the AUv3 inside)
bash "$(dirname "$0")/ship_ios.sh" Omnisampler
echo "Finished. You can close this window."
