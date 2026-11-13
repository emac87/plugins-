#!/bin/bash
# Build Omnisampler and Omnisampler FX on this Mac (Apple silicon only), run the tests (they also load
# Apple's DLS synth and the built VST3 inside Omnisampler and auto-sample them), package the result
# like the GitHub build does, install it (AU, VST3, app) and run Apple's AU validation on both.
#   Log: tools/build_mac.log     Last line: BUILD_MAC_RESULT=...
# Uses the JUCE copy in Claude Workbench (the one the iPad/iPhone builds use), so nothing is downloaded.
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
cd "$(dirname "$0")/.." || exit 1
ROOT="$PWD"; WB="$HOME/Documents/Claude Workbench"
LOG="$ROOT/tools/build_mac.log"; : > "$LOG"
say () { echo "$@" | tee -a "$LOG"; }
fail () { say "BUILD_MAC_RESULT=FAILED at: $1"; exit 1; }
say "== Omnisampler Mac build, $(date) =="
{ sw_vers; xcodebuild -version; uname -m; } >> "$LOG" 2>&1
command -v cmake >/dev/null || brew install cmake >> "$LOG" 2>&1
command -v cmake >/dev/null || fail "cmake is missing and Homebrew couldn't install it"
GEN="Unix Makefiles"; command -v ninja >/dev/null && GEN="Ninja"
B="$ROOT/build-mac"
say "Configuring ($GEN)..."
cmake -S . -B "$B" -G "$GEN" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON -DBUILD_CONTOUR_EQ=OFF -DBUILD_CAPTURE_RACK=OFF \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DJUCE_DIR_LOCAL="$WB/JUCE" >> "$LOG" 2>&1 || fail "configure"
say "Building (about ten minutes)..."
cmake --build "$B" --config Release -j "$(sysctl -n hw.ncpu)" >> "$LOG" 2>&1 || fail "build"
say "BUILT"
A="$B/Omnisampler/Omnisampler_artefacts/Release"; F="$B/Omnisampler/OmnisamplerFX_artefacts/Release"

say "Testing (details in tools/test_mac.log)..."
TESTS=passed
bash "$ROOT/tools/test_mac_omnisampler.command" >> "$LOG" 2>&1 || TESTS=failed
say "TESTS $TESTS"

say "Packaging..."
PKG="$B/pkg/Omnisampler"; rm -rf "$B/pkg"; mkdir -p "$PKG"
cp -R "$A/AU/Omnisampler.component" "$A/VST3/Omnisampler.vst3" "$A/Standalone/Omnisampler.app" \
      "$F/AU/Omnisampler FX.component" "$F/VST3/Omnisampler FX.vst3" "$PKG/" >> "$LOG" 2>&1 || fail "packaging (a built plug-in is missing)"
for b in "$PKG"/*.component "$PKG"/*.vst3 "$PKG"/*.app; do codesign --force --deep --sign - "$b" >> "$LOG" 2>&1; done
ZIP="$WB/installers/plugins/Omnisampler-macos.zip"
[ -f "$ZIP" ] && mv -f "$ZIP" "$WB/installers/plugins/Omnisampler-macos.previous.zip"
(cd "$B/pkg" && ditto -c -k --keepParent "Omnisampler" "$ZIP") >> "$LOG" 2>&1 || fail "zip"

say "Installing..."
bash "$WB/scripts/plugins/install_plugin.sh" Omnisampler >> "$LOG" 2>&1 || fail "install"
say "INSTALLED"

say "Validating the Audio Units with auval (a minute or two)..."
AUVAL=passed
for spec in "aumu Omns Emac" "aufx OmFx Emac"; do
  code=$(echo "$spec" | cut -d' ' -f2)
  if auval -v $spec > "$ROOT/tools/auval_$code.log" 2>&1; then say "AUVAL PASSED: $spec"; else say "AUVAL FAILED: $spec (tools/auval_$code.log)"; AUVAL=failed; fi
done
say "BUILD_MAC_RESULT=OK tests=$TESTS auval=$AUVAL"
