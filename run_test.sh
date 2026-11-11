#!/bin/sh
# Background test run: hidden window, killed after $1 seconds (default 30).
# Audio goes to the real device; mute it in the game's options for tests.
cd "$(dirname "$0")"
T=${1:-30}; shift
ST_HIDDEN=1 ./sttng.exe "$@" &
PID=$!
i=0
while kill -0 $PID 2>/dev/null && [ $i -lt $T ]; do sleep 1; i=$((i+1)); done
kill $PID 2>/dev/null && echo "(killed after ${T}s)"
tail -c 3000 sttng.log
