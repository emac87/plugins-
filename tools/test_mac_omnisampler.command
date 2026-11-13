#!/bin/bash
# Run the Omnisampler tests from the Mac build (build-mac). They also load Apple's DLS synth (an Audio
# Unit) and the built Omnisampler.vst3 inside Omnisampler and auto-sample them.
#   Log: tools/test_mac.log     Last line: TEST_MAC_RESULT=...
cd "$(dirname "$0")/.." || exit 1
ROOT="$PWD"; B="$ROOT/build-mac"; LOG="$ROOT/tools/test_mac.log"; : > "$LOG"
A="$B/Omnisampler/Omnisampler_artefacts/Release"
T="$B/Tests/omni_tests"; [ -x "$T" ] || T="$B/Tests/Release/omni_tests"
[ -x "$T" ] || { echo "TEST_MAC_RESULT=FAILED: no test program (run build_mac_omnisampler.command first)" | tee -a "$LOG"; exit 1; }
echo "== Omnisampler tests, $(date) ==" | tee -a "$LOG"
OMNI_HOST_VST3="$A/VST3/Omnisampler.vst3" OMNI_HOST_AU="AudioUnit:Synths/aumu,dls ,appl" "$T" "$B/test-output" >> "$LOG" 2>&1 &
PID=$!
( sleep 900; kill -9 $PID 2>/dev/null ) & WATCH=$!
wait $PID; RC=$?
kill $WATCH 2>/dev/null
if [ $RC -eq 0 ]; then echo "TEST_MAC_RESULT=PASSED" | tee -a "$LOG"; else echo "TEST_MAC_RESULT=FAILED (exit $RC)" | tee -a "$LOG"; exit 1; fi
