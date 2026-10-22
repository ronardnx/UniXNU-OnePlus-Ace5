#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_dir="$(mktemp -d -t giulia-wifi-tests.XXXXXXXX)"
trap 'rm -f "$test_dir"/*; rmdir "$test_dir"' EXIT
for transport in pci smmu mhi qmi ce wmi apple; do
  clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    "tests/ios16_wifi_${transport}.c" -o "$test_dir/$transport"
  "$test_dir/$transport"
done
clang -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
  tests/ios16_display.c -o "$test_dir/display"
"$test_dir/display"
clang++ -std=c++14 -Wall -Wextra -Werror -c tests/ios16_wifi_headers.cpp -o "$test_dir/headers.o"
