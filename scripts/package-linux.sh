#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -s)" != Linux ]]; then
    echo "Run this script on linux." >&2
    exit 1
fi
if [[ -z "${VCPKG_ROOT:-}" || ! -f "$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" ]]; then
    echo "Set VCPKG_ROOT to a bootstrapped vcpkg checkout." >&2
    exit 1
fi

case "$(uname -m)" in
    x86_64) arch=x64 ;;
    arm64|aarch64) arch=arm64 ;;
    *) echo "Unsupported architecture: $(uname -m)" >&2; exit 1 ;;
esac

cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.."
preset=linux-release
cmake --preset "$preset"
cmake --build --preset "$preset"

dist="$PWD/dist/linux-$arch"
exe="$PWD/build/$preset/updater"
dynamic="$(readelf -d "$exe")"
headers="$(readelf -l "$exe")"
if [[ "$dynamic" == *"(NEEDED)"* || "$headers" == *"INTERP"* ]]; then
    echo "Linux executable is dynamically linked: $exe" >&2
    exit 1
fi
"$exe" --help
cmake --install "build/$preset" --config Release --prefix "$dist"
printf 'Packaged %s\n' "$dist/updater"
