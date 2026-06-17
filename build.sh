#!/bin/sh
# Build a Vanta game to a native binary:  Vanta --(vc)--> C --(cc+SDL2)--> native.
# Usage: ./build.sh [gamename]   (default: bounce)   then: ./gamename
set -e
GAME="${1:-bounce}"
python3 "../vanta/vanta.py" "../vanta/vc.va" "$GAME.va" -k
cat sdlrt.c "$GAME.va.c" > "/tmp/${GAME}_full.c"
cc "/tmp/${GAME}_full.c" $(sdl2-config --cflags --libs) -O2 -w -o "$GAME"
echo "built ./$GAME  (native, no python)"
