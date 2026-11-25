#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
# Build the pinned QEMU (qemu/upstream.json, tarball cached in qemu/cache/)
# with qemu/core-patches/* and the machine/*.c overlay symlinked into
# qemu/src/hw/sh4/.  Rerun after editing; if core-patches change the tree is
# re-extracted (the old one moves to qemu/previous-src).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TARGET="${QEMU_TARGET:-sh4-softmmu}"
BINARY="qemu-system-${TARGET%-softmmu}"
SRC="$ROOT/qemu/src"
upstream() {
  python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]])' \
    "$ROOT/qemu/upstream.json" "$1"
}
VERSION="$(upstream version)"
URL="$(upstream url)"
SHA="$(upstream sha256)"
TARBALL="$ROOT/qemu/cache/qemu-$VERSION.tar.xz"
STAMP="$SRC/.core-patches.sha256"

sha256() {
  if command -v shasum >/dev/null 2>&1; then shasum -a 256 "$@"; else sha256sum "$@"; fi
}
# True when file $1 has the release's sha256 from qemu/upstream.json.
sha_ok() {
  echo "$SHA  $1" | sha256 -c - >/dev/null 2>&1
}

# The stamp names the release too, so a version bump re-extracts the tree.
PATCHES_SHA="$VERSION $(cat "$ROOT"/qemu/core-patches/*.patch | sha256 | cut -d' ' -f1)"

if [ -f "$TARBALL" ] && ! sha_ok "$TARBALL"; then
  echo "qemu.sh: cached $TARBALL does not match qemu/upstream.json; fetching it again" >&2
  rm -f "$TARBALL"
fi
if [ ! -f "$TARBALL" ]; then
  mkdir -p "$ROOT/qemu/cache"
  rm -f "$TARBALL.part"
  curl -fL --retry 3 -o "$TARBALL.part" "$URL"
  # Check the download before it enters the cache: a bad one is removed
  # instead of failing every later build.
  if ! sha_ok "$TARBALL.part"; then
    rm -f "$TARBALL.part"
    echo "qemu.sh: error: $URL does not match the sha256 in qemu/upstream.json (download removed)" >&2
    exit 1
  fi
  mv "$TARBALL.part" "$TARBALL"
fi

if [ -d "$SRC" ] && [ "$(cat "$STAMP" 2>/dev/null)" != "$PATCHES_SHA" ]; then
  # QEMU tarballs ship read-only directories (roms/edk2); make them removable.
  [ -d "$ROOT/qemu/previous-src" ] && chmod -R u+w "$ROOT/qemu/previous-src"
  rm -rf "$ROOT/qemu/previous-src"
  mv "$SRC" "$ROOT/qemu/previous-src"
fi
if [ ! -d "$SRC" ]; then
  tmp="$ROOT/qemu/.extract"
  [ -d "$tmp" ] && chmod -R u+w "$tmp"
  rm -rf "$tmp" && mkdir -p "$tmp"
  tar -xJf "$TARBALL" -C "$tmp"
  mv "$tmp/qemu-$VERSION" "$SRC" && rm -rf "$tmp"
  for p in "$ROOT"/qemu/core-patches/*.patch; do
    patch -d "$SRC" -p1 --forward --quiet < "$p"
  done
  echo "$PATCHES_SHA" > "$STAMP"
fi

for f in "$ROOT"/machine/*.c "$ROOT"/machine/*.c.inc "$ROOT"/machine/*.h; do
  [ -e "$f" ] || continue
  ln -sfn "../../../../machine/$(basename "$f")" "$SRC/hw/sh4/$(basename "$f")"
done
# The vendored PowerVR MBX1 model keeps its own directory so that its MIT
# headers stay beside their sources (machine/mbx/README.melco.md).
ln -sfn ../../../../machine/mbx "$SRC/hw/sh4/mbx"

# QEMU's configure wants tomli (Python < 3.11) and ninja: use the project venv.
VENV="$ROOT/.venv"
if [ ! -x "$VENV/bin/ninja" ]; then
  python3 -m venv "$VENV"
  "$VENV/bin/pip" install --quiet ninja tomli
fi
export PATH="$VENV/bin:$PATH"

# The build directory remembers the configured targets: a different
# QEMU_TARGET needs its own configuration (or its binary is never built).
TARGET_STAMP="$SRC/build/.melco-targets"
if [ -f "$SRC/build/build.ninja" ] && \
   [ "$(cat "$TARGET_STAMP" 2>/dev/null)" != "$TARGET" ]; then
  echo "qemu.sh: configured for '$(cat "$TARGET_STAMP" 2>/dev/null)', rebuilding for '$TARGET'" >&2
  rm -rf "$SRC/build"
fi
if [ ! -f "$SRC/build/build.ninja" ]; then
  (cd "$SRC" && ./configure --target-list="$TARGET" --disable-werror --disable-docs \
     --disable-gtk --ninja="$VENV/bin/ninja" --python="$VENV/bin/python")
  printf '%s\n' "$TARGET" > "$TARGET_STAMP"
fi
if [ "$(uname -s)" = "Darwin" ]; then
  # The "-unsigned" ninja target exists only on macOS (ad-hoc signing).
  ninja -C "$SRC/build" "$BINARY-unsigned"
  # The signed copy needs an entitlement step that sometimes fails; the
  # unsigned binary is functionally identical, so fall back to it.
  ninja -C "$SRC/build" "$BINARY" 2>/dev/null || cp "$SRC/build/$BINARY-unsigned" "$SRC/build/$BINARY"
else
  ninja -C "$SRC/build" "$BINARY"
fi
echo "built $SRC/build/$BINARY"
