#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -s)" != Darwin ]]; then
    echo "Run this script on macos." >&2
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
preset=macos-release
cmake --preset "$preset"
cmake --build --preset "$preset"

dist="$PWD/dist/macos-$arch"
exe="$PWD/build/$preset/updater"
dependencies="$(otool -L "$exe")"
while read -r library _; do
    case "$library" in
        /usr/lib/*|/System/Library/*) ;;
        *) echo "Unexpected dynamic dependency: $library" >&2; exit 1 ;;
    esac
done <<< "$(tail -n +2 <<< "$dependencies")"
"$exe" --help
cmake --install "build/$preset" --config Release --prefix "$dist"
printf 'Packaged %s\n' "$dist/updater"
