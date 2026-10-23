#!/bin/sh
# Explicitly invoked only; profile generation never executes this script.
set -eu
if [ "$#" -ne 1 ]; then echo "usage: $0 NEW_OUTPUT_DIRECTORY (macOS + iPhoneOS SDK)" >&2; exit 2; fi
mkdir "$1"
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
xcrun --sdk iphoneos clang -arch arm64 -miphoneos-version-min=16.0 -O2 \
  -Wall -Wextra -Werror -I "$here/../common" -I "$here/.." "$here/giulia-debugd.c" -o "$1/giulia-debugd"
codesign --force --sign - "$1/giulia-debugd"
codesign --verify --strict "$1/giulia-debugd"
