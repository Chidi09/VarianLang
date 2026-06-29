#!/bin/sh
# build_tre.sh — bootstrap the static TRE regex library used by the Windows
# build (MinGW has no POSIX <regex.h>; Linux/macOS use libc's regex instead).
#
# The Makefile's Windows branch links `deps/libtre.a` and adds
# `-Ideps/tre-0.9.0/local_includes -DUSE_LOCAL_TRE_H`, and runs this script via
# the `deps/libtre.a` target. This script fetches the TRE 0.9.0 source, builds a
# static library, and stages its public headers so the build is reproducible
# without checking the upstream source tree into version control.
#
# Idempotent: if deps/libtre.a already exists it does nothing.
#
# NOTE: validated path is the AppVeyor MinGW environment. If you already have a
# working deps/ tree, prefer committing it over relying on a fresh fetch.
set -e

DEPS_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$DEPS_DIR/tre-0.9.0"
LIB="$DEPS_DIR/libtre.a"
INC="$SRC_DIR/local_includes"
TRE_URL="https://github.com/laurikari/tre/archive/refs/tags/v0.9.0.tar.gz"

if [ -f "$LIB" ] && [ -f "$INC/tre/regex.h" ]; then
    echo "deps: libtre.a + headers already present — nothing to do."
    exit 0
fi

echo "deps: fetching TRE 0.9.0 source ..."
mkdir -p "$DEPS_DIR"
if [ ! -d "$SRC_DIR/.git" ] && [ ! -f "$SRC_DIR/configure.ac" ]; then
    if command -v curl >/dev/null 2>&1; then
        curl -sL "$TRE_URL" -o "$DEPS_DIR/tre.tar.gz"
    else
        wget -q "$TRE_URL" -O "$DEPS_DIR/tre.tar.gz"
    fi
    tar -xzf "$DEPS_DIR/tre.tar.gz" -C "$DEPS_DIR"
    rm -f "$DEPS_DIR/tre.tar.gz"
    # the tarball unpacks as tre-0.9.0/ already; normalize just in case
    [ -d "$DEPS_DIR/tre-0.9.0" ] || mv "$DEPS_DIR"/tre-* "$SRC_DIR"
fi

echo "deps: building static libtre.a ..."
cd "$SRC_DIR"
# TRE ships an autotools build; generate configure if needed.
if [ ! -x ./configure ]; then
    ./utils/autogen.sh || autoreconf -fi
fi
./configure --enable-static --disable-shared --disable-agrep --without-libutf8 \
            --prefix="$SRC_DIR/_install" CFLAGS="-O2 -DTRE_USE_SYSTEM_REGEX_H=0"
make -j"$(nproc 2>/dev/null || echo 2)"
make install

# Stage the static archive and the public headers the Varian build expects.
cp "$SRC_DIR/_install/lib/libtre.a" "$LIB"
mkdir -p "$INC/tre"
cp "$SRC_DIR/_install/include/tre/"*.h "$INC/tre/" 2>/dev/null || \
    cp "$SRC_DIR/_install/include/"*.h "$INC/tre/"

echo "deps: done -> $LIB"
