#!/bin/bash
# Builds hipiview.exe for Windows on Linux (MinGW-w64): one file, with
# SDL2 and zlib built in. Used by the GitHub workflow; runs the same at
# home (Ubuntu):
#
#   sudo apt install g++-mingw-w64-x86-64-posix curl zip
#   tools/hipiview/ci/build-windows.sh [version]
#
# Leaves tools/hipiview/hipiview-windows[-version].zip (hipiview.exe and a
# short note). SDL2 and zlib are downloaded into tools/hipiview/build-win/.
set -euo pipefail

SDL_VERSION=2.30.9
ZLIB_VERSION=1.3.1
VERSION="${1:-}"

HERE="$(cd "$(dirname "$0")/.." && pwd)"            # tools/hipiview
WORK="$HERE/build-win"
mkdir -p "$WORK"
cd "$WORK"

# SDL2, the MinGW development package (headers + static library)
if [ ! -d "SDL2-$SDL_VERSION" ]; then
    curl -sSLf -o sdl2.tar.gz \
        "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL2-devel-$SDL_VERSION-mingw.tar.gz"
    tar xzf sdl2.tar.gz
fi

# zlib, built as a static MinGW library
if [ ! -f zlib/lib/libz.a ]; then
    curl -sSLf -o zlib.tar.gz \
        "https://github.com/madler/zlib/releases/download/v$ZLIB_VERSION/zlib-$ZLIB_VERSION.tar.gz"
    tar xzf zlib.tar.gz
    make -s -C "zlib-$ZLIB_VERSION" -f win32/Makefile.gcc PREFIX=x86_64-w64-mingw32- libz.a
    mkdir -p zlib/include zlib/lib
    cp "zlib-$ZLIB_VERSION/zlib.h" "zlib-$ZLIB_VERSION/zconf.h" zlib/include/
    cp "zlib-$ZLIB_VERSION/libz.a" zlib/lib/
fi

cd "$HERE"
rm -f hipiview.exe
make windows SDL2_MINGW="$WORK/SDL2-$SDL_VERSION/x86_64-w64-mingw32" ZLIB_MINGW="$WORK/zlib"
x86_64-w64-mingw32-strip hipiview.exe

# The package
NAME="hipiview-windows${VERSION:+-$VERSION}"
PKG="$WORK/$NAME"
rm -rf "$PKG" && mkdir -p "$PKG"
cp hipiview.exe "$PKG/"
cat > "$PKG/README.txt" <<NOTE
hipiview${VERSION:+ $VERSION} -- HIPI's 7" display on a Windows PC

On HIPI: More > Settings > Screen > Mirror to PC < View > (or < Control >,
to use HIPI with the mouse and keyboard too). Then, in a command prompt:

  hipiview.exe              finds HIPI's mirror port by itself
  hipiview.exe COM7         or name it
  hipiview.exe --help       all options and keys

Recording needs ffmpeg.exe in the PATH or next to hipiview.exe.
More: tools/hipiview/README.md in the HIPI project.
NOTE
rm -f "$HERE/$NAME.zip"
(cd "$WORK" && zip -qr "$HERE/$NAME.zip" "$NAME")
echo "built $HERE/$NAME.zip"
