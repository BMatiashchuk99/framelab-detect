#!/usr/bin/env sh
# Cross-builds FrameLab Detect for 64-bit Windows with mingw-w64.
#   Debian/Ubuntu: sudo apt install gcc-mingw-w64-x86-64    macOS: brew install mingw-w64
# Output: dist/FrameLab-Detect.exe. Releases are built by .github/workflows/build.yml, not on a developer PC.
set -eu
cd "$(dirname "$0")"
CC=${CC:-x86_64-w64-mingw32-gcc}
WINDRES=${WINDRES:-x86_64-w64-mingw32-windres}
OUT=dist/FrameLab-Detect.exe
mkdir -p build dist
"$WINDRES" -O coff -i res/app.rc -o build/res.o
# --no-insert-timestamp keeps the build reproducible: the same source and compiler give the same bytes.
"$CC" -std=c11 -Os -s -municode -mwindows -Wall -Wextra -Werror ${CFLAGS:-} \
  -D_WIN32_WINNT=0x0601 -DUNICODE -D_UNICODE \
  -Wl,--no-insert-timestamp -Wl,--dynamicbase -Wl,--nxcompat -Wl,--high-entropy-va \
  src/main.c build/res.o -o "$OUT" \
  -lcomctl32 -lshell32 -lole32 -ldxgi -luuid -ladvapi32 -luser32 -lwinhttp
ls -l "$OUT"
sha256sum "$OUT" 2>/dev/null || shasum -a 256 "$OUT"
