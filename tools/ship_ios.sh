#!/bin/bash
# Build one plug-in's iPad/iPhone app (AUv3 inside) and send it to TestFlight.
#   ship_ios.sh ContourEQ|CaptureRack|Omnisampler
# Logs: tools/generate-<Plugin>.log (CMake) and build-ios-<Plugin>/tools/testflight.log (archive + upload)
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
P="$1"; cd "$(dirname "$0")/.." || exit 1
case "$P" in
  ContourEQ)   OPT=-DBUILD_CONTOUR_EQ=ON;   B=contoureq ;;
  CaptureRack) OPT=-DBUILD_CAPTURE_RACK=ON; B=capturerack ;;
  Omnisampler) OPT=-DBUILD_OMNISAMPLER=ON;  B=omnisampler ;;
  *) echo "usage: ship_ios.sh ContourEQ|CaptureRack|Omnisampler"; exit 1 ;;
esac
WB="$HOME/Documents/Claude Workbench/scripts/testflight"; source "$WB/asc.env"
OUT="build-ios-$P"; mkdir -p tools "$OUT/tools"; GEN="tools/generate-$P.log"; : > "$GEN"; : > "$OUT/tools/testflight.log"
command -v cmake >/dev/null || brew install cmake >> "$GEN" 2>&1
BUILD=$(date +%Y%m%d%H%M)
echo "Generating the iOS Xcode project for $P (build $BUILD)..." | tee -a "$GEN"
cmake -S . -B "$OUT" -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 \
  -DBUILD_CONTOUR_EQ=OFF -DBUILD_CAPTURE_RACK=OFF -DBUILD_OMNISAMPLER=OFF $OPT \
  -DJUCE_DIR_LOCAL="$HOME/Documents/Claude Workbench/JUCE" \
  -DEVAN_TEAM_ID="$TEAM_ID" -DEVAN_BUILD="$BUILD" \
  -DCMAKE_XCODE_ATTRIBUTE_DEVELOPMENT_TEAM="$TEAM_ID" -DCMAKE_XCODE_ATTRIBUTE_CODE_SIGN_STYLE=Automatic \
  -DCMAKE_XCODE_GENERATE_SCHEME=ON >> "$GEN" 2>&1
if [ $? -ne 0 ]; then echo "GENERATE FAILED — see $GEN" | tee -a "$OUT/tools/testflight.log"; exit 1; fi
echo "GENERATED" >> "$GEN"
SHIP_BUNDLE="com.evanmcintosh.$B" bash "$WB/ship_testflight.sh" "$PWD/$OUT" "${P}_Standalone"
