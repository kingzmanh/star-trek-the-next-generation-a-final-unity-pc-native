#!/bin/sh
# Dev build helper: ./build.sh [gen [tag...]] - regenerate (optional) and build.
# Windows Python (with capstone) for generation, MSYS2 MINGW64 for the build.
# -j16: about half the threads of a 32-thread PC; lower it on smaller ones.
cd "$(dirname "$0")"
WPY=${WPY:-/c/Users/Msi/AppData/Local/Programs/Python/Python310/python.exe}
if [ "$1" = gen ]; then
  shift
  "$WPY" dosport.py gen "$@" | grep -v "^unimplemented: {}" || exit 1
fi
export PATH=/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH
# dev icon: the "A Final Unity" logo (two lines) cut from the game's pause panel (gamedata, not in git)
ICON=; [ -f gamedata/sttng_logo2.png ] && ICON="ICON_SRC=gamedata/sttng_logo2.png ICON_BG=black"
make -j${JOBS:-16} PY="$WPY" $ICON 2>&1 | grep -E "error|undefined reference" | head -30
ls -la *.exe 2>/dev/null | grep -v Setup | awk '{print $5, $6, $7, $8, $9}'
