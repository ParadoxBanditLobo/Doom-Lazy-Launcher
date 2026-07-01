#!/bin/sh
set -eu

APP_NAME="DoomLazyLauncher"
SRC="src/doom_lazy_launcher_v0_3.c"
BUILD_DIR="build"
OUT="$BUILD_DIR/$APP_NAME"

if ! command -v gcc >/dev/null 2>&1; then
  echo "Error: gcc is not installed."
  echo "Install it with your distro package manager, for example:"
  echo "  Debian/Ubuntu: sudo apt install build-essential"
  echo "  Fedora:        sudo dnf install gcc"
  echo "  Arch:          sudo pacman -S gcc"
  exit 1
fi

if [ ! -f "$SRC" ]; then
  echo "Error: source file not found: $SRC"
  echo "Run this from the repository root."
  exit 1
fi

mkdir -p "$BUILD_DIR"

gcc -std=c99 -O2 -Wall -Wextra -pedantic "$SRC" -o "$OUT"
chmod +x "$OUT"

echo "Built: $OUT"
