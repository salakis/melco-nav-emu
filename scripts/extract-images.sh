#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (c) 2026 melco-nav-emu contributors
# Extract the firmware/install layer from the two Volvo RTI update ISOs.
#
# The MAP/ directory is left on the ISO by default: the VM uses the ISO as a
# virtual DVD. Use --extract-map if you need a host copy for HDD building.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Another disc set (e.g. NA-2450-00): export CUP2006_DISC1/CUP2006_DISC2, or
# pass --dvd1/--dvd2 (or DVD1/DVD2) — the same variables tools/profile.py reads.
DVD1="${DVD1:-${CUP2006_DISC1:-$PROJECT_ROOT/images/dvd/disc1/EU-3350-00_1.ISO}}"
DVD2="${DVD2:-${CUP2006_DISC2:-$PROJECT_ROOT/images/dvd/disc2/EU-3350-00_2.ISO}}"
OUT="${OUT:-$PROJECT_ROOT/images}"
EXTRACT_MAP=0

usage() {
  cat <<EOF
usage: $0 [--dvd1 ISO] [--dvd2 ISO] [--out DIR] [--extract-map]

Extracts the ISO root firmware files into DIR/dvd1 and DIR/dvd2.
The default ISOs are:
  $DVD1
  $DVD2

If a disc is already mounted at /Volumes/EU-3350-00_N (or the underscore
variant), that mount is reused instead of attaching it again.
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --dvd1) DVD1="$2"; shift 2 ;;
    --dvd2) DVD2="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --extract-map) EXTRACT_MAP=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage; exit 2 ;;
  esac
done

ROOT_FILES=(
  LOADING.KWI
  SUBCPU.ME
  MMM02400.SUB
  P2006.SCE
  DKLABEL.DAT
  ALLDATA.KWI
)

mkdir -p "$OUT/dvd1" "$OUT/dvd2" "$PROJECT_ROOT/build"

sha1_of() {
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 1 "$1" | awk '{print $1}'
  else
    sha1sum "$1" | awk '{print $1}'
  fi
}

find_mounted() {
  local iso="$1"
  local label
  base="$(basename "$iso" .ISO)"
  for label in "$base" "${base//-/_}"; do
    if [ -d "/Volumes/$label" ]; then
      printf '%s\n' "/Volumes/$label"
      return 0
    fi
  done
  return 1
}

detach_mount() {
  local mnt="$1"
  if command -v diskutil >/dev/null 2>&1; then
    diskutil eject "$mnt" >/dev/null 2>&1 || true
  else
    hdiutil detach "$mnt" >/dev/null 2>&1 || true
  fi
}

extract_from_mount() {
  local mnt="$1" dest="$2"
  for f in "${ROOT_FILES[@]}"; do
    if [ -f "$mnt/$f" ]; then
      # The ISO mount is read-only and cp -p preserves mode 0555, which makes
      # a second run fail with EACCES. Replace and make the copy writable.
      rm -f "$dest/$f"
      cp -p "$mnt/$f" "$dest/$f"
      chmod u+w "$dest/$f"
    else
      echo "warning: $f not found in $mnt" >&2
    fi
  done
  if [ "$EXTRACT_MAP" = 1 ] && [ -d "$mnt/MAP" ]; then
    echo "copying MAP/ from $mnt (this is large)"
    mkdir -p "$dest/MAP"
    rsync -a "$mnt/MAP/" "$dest/MAP/"
  fi
}

extract_with_7z() {
  local iso="$1" dest="$2"
  local seven=""
  for candidate in 7z 7zz 7za; do
    if command -v "$candidate" >/dev/null 2>&1; then seven="$candidate"; break; fi
  done
  if [ -z "$seven" ]; then
    echo "error: neither diskutil/hdiutil nor 7z is available" >&2
    return 1
  fi
  local failed=0
  for f in "${ROOT_FILES[@]}"; do
    if ! "$seven" x -y -o"$dest" "$iso" "$f" >/dev/null; then
      echo "error: $seven could not extract $f from $iso" >&2
      failed=1
    elif [ ! -f "$dest/$f" ]; then
      echo "warning: $f not found in $iso" >&2
    fi
  done
  if [ "$EXTRACT_MAP" = 1 ]; then
    echo "extracting MAP/ from $iso (this is large)"
    if ! "$seven" x -y -o"$dest" "$iso" MAP >/dev/null || [ ! -d "$dest/MAP" ]; then
      echo "error: $seven could not extract MAP/ from $iso" >&2
      failed=1
    fi
  fi
  return "$failed"
}

for pair in "1:$DVD1" "2:$DVD2"; do
  idx="${pair%%:*}"
  iso="${pair#*:}"
  dest="$OUT/dvd$idx"
  if [ ! -f "$iso" ]; then
    echo "error: ISO not found: $iso" >&2
    exit 1
  fi

  if mnt="$(find_mounted "$iso")"; then
    echo "using existing mount: $mnt"
    extract_from_mount "$mnt" "$dest"
  elif command -v diskutil >/dev/null 2>&1 && [ "$(uname -s)" = "Darwin" ]; then
    mnt="$PROJECT_ROOT/build/mnt-$idx"
    rm -rf "$mnt"
    mkdir -p "$mnt"
    echo "attaching $iso -> $mnt"
    diskutil image attach --readOnly --nobrowse --mountPoint "$mnt" "$iso" >/dev/null
    extract_from_mount "$mnt" "$dest"
    detach_mount "$mnt"
  elif command -v hdiutil >/dev/null 2>&1; then
    mnt="$PROJECT_ROOT/build/mnt-$idx"
    rm -rf "$mnt"
    mkdir -p "$mnt"
    echo "attaching $iso -> $mnt"
    hdiutil attach -readonly -nobrowse -mountpoint "$mnt" "$iso" >/dev/null
    extract_from_mount "$mnt" "$dest"
    detach_mount "$mnt"
  else
    if ! extract_with_7z "$iso" "$dest"; then
      echo "error: extracting $iso failed; $dest is incomplete" >&2
      exit 1
    fi
  fi

  for f in "${ROOT_FILES[@]}"; do
    if [ -f "$dest/$f" ]; then
      size="$(stat -f%z "$dest/$f" 2>/dev/null || stat -c%s "$dest/$f")"
      printf '  %-14s %12d B  %s\n' "$f" "$size" "$(sha1_of "$dest/$f")"
    else
      printf '  %-14s missing\n' "$f"
    fi
  done
done

echo "done: $OUT/dvd1 and $OUT/dvd2"
