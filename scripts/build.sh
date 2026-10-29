#!/usr/bin/env bash
# Compila o DesignThumbs para Windows x64 a partir do Linux (mingw-w64).
# Uso: scripts/build.sh            -> gera dist/DesignThumbs/ e dist/DesignThumbs-<versao>.zip
# Requisitos (Ubuntu/Debian): sudo apt install mingw-w64 git curl zip
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEPS="$ROOT/.deps"
OUT="$ROOT/build"
DIST="$ROOT/dist"

LUNASVG_TAG="v3.5.0"
PDFIUM_TAG="chromium/8111"     # https://github.com/bblanchon/pdfium-binaries/releases

CXX=x86_64-w64-mingw32-g++
CC=x86_64-w64-mingw32-gcc
AR=x86_64-w64-mingw32-ar
WINDRES=x86_64-w64-mingw32-windres

VERSION="$(sed -n 's/.*DT_VERSION_W L"\([^"]*\)".*/\1/p' "$ROOT/src/common.h")"
echo "DesignThumbs $VERSION"

mkdir -p "$DEPS" "$OUT" "$DIST"

# ---- dependencias
if [ ! -d "$DEPS/lunasvg" ]; then
  git clone --depth 1 --branch "$LUNASVG_TAG" https://github.com/sammycage/lunasvg.git "$DEPS/lunasvg"
fi
if [ ! -f "$DEPS/pdfium/bin/pdfium.dll" ]; then
  mkdir -p "$DEPS/pdfium"
  curl -fsSL "https://github.com/bblanchon/pdfium-binaries/releases/download/${PDFIUM_TAG/\//%2F}/pdfium-win-x64.tgz" -o "$DEPS/pdfium.tgz"
  tar xzf "$DEPS/pdfium.tgz" -C "$DEPS/pdfium"
fi
L="$DEPS/lunasvg"; P="$DEPS/pdfium"

# ---- lunasvg + plutovg (estatico)
if [ ! -f "$OUT/liblunasvg.a" ]; then
  mkdir -p "$OUT/luna"
  for f in "$L"/plutovg/source/*.c; do
    $CC -O2 -DPLUTOVG_BUILD -DPLUTOVG_BUILD_STATIC -I"$L/plutovg/include" -c "$f" -o "$OUT/luna/p_$(basename "$f").o"
  done
  for f in "$L"/source/*.cpp; do
    $CXX -O2 -std=c++17 -DLUNASVG_BUILD -DLUNASVG_BUILD_STATIC -DPLUTOVG_BUILD_STATIC -I"$L/include" -I"$L/plutovg/include" -c "$f" -o "$OUT/luna/l_$(basename "$f").o"
  done
  $AR rcs "$OUT/liblunasvg.a" "$OUT"/luna/*.o
fi

COMMON="-O2 -std=c++17 -municode -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0A00 -Wall -Wno-unknown-pragmas -Wno-cast-function-type"
STATIC="-static -static-libgcc -static-libstdc++"
S="$ROOT/src"

# ---- DLL de miniaturas
$CXX $COMMON -DLUNASVG_BUILD_STATIC -DPLUTOVG_BUILD_STATIC -I"$L/include" -I"$P/include" -I"$S" \
  "$S/provider.cpp" "$S/util.cpp" "$S/psd.cpp" "$S/pdf.cpp" "$S/svg.cpp" "$S/eps.cpp" "$S/xmp.cpp" "$S/reg.cpp" "$S/settings.cpp" \
  "$S/designthumbs.def" -shared -o "$OUT/DesignThumbs.dll" "$OUT/liblunasvg.a" $STATIC \
  -lole32 -loleaut32 -luuid -lshlwapi -lwindowscodecs -lgdi32 -lshell32 -ladvapi32

# ---- app de configuracao
(cd "$S" && $WINDRES -O coff -i app.rc -o "$OUT/app.res.o")
$CXX $COMMON -mwindows -I"$S" "$S/app.cpp" "$S/reg.cpp" "$S/settings.cpp" "$OUT/app.res.o" -o "$OUT/DesignThumbs.exe" $STATIC \
  -lole32 -luuid -lshell32 -lshlwapi -lcomctl32 -lgdi32 -luser32 -ladvapi32

# ---- ferramenta de teste (render.exe gera PNG das miniaturas)
$CXX $COMMON -I"$S" "$ROOT/test/render.cpp" -o "$OUT/render.exe" $STATIC -lole32 -luuid -lwindowscodecs -lgdi32 -lshlwapi

# ---- pacote
PKG="$DIST/DesignThumbs"
rm -rf "$PKG" && mkdir -p "$PKG/licencas"
cp "$OUT/DesignThumbs.exe" "$OUT/DesignThumbs.dll" "$P/bin/pdfium.dll" "$PKG/"
cp "$ROOT/LICENSE" "$PKG/licencas/DesignThumbs-LICENSE.txt"
cp "$ROOT/THIRD_PARTY_NOTICES.md" "$PKG/licencas/"
cp "$P/LICENSE" "$PKG/licencas/PDFium-LICENSE.txt"
[ -d "$P/licenses" ] && cp -r "$P/licenses" "$PKG/licencas/pdfium-third-party"
cp "$L/LICENSE" "$PKG/licencas/lunasvg-LICENSE.txt"
cp "$L/plutovg/LICENSE" "$PKG/licencas/plutovg-LICENSE.txt"
cp "$ROOT/docs/LEIA-ME.txt" "$PKG/"
(cd "$DIST" && rm -f "DesignThumbs-$VERSION.zip" && zip -qr "DesignThumbs-$VERSION.zip" DesignThumbs)
echo "Pacote: $DIST/DesignThumbs-$VERSION.zip"
