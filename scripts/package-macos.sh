#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -s)" != Darwin ]]; then
    echo "Run this script on macOS." >&2
    exit 1
fi
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ $# -gt 1 ]]; then
    echo "Usage: bash scripts/package-macos.sh [vcpkg-root]" >&2
    exit 1
fi
requested_root="${1:-${VCPKG_ROOT:-}}"
if [[ -n "$requested_root" ]]; then
    candidates=("$requested_root")
else
    candidates=("$root/vcpkg" "$HOME/vcpkg" "$HOME/.local/share/vcpkg" /opt/vcpkg /opt/homebrew/share/vcpkg /usr/local/share/vcpkg)
    if command -v vcpkg >/dev/null 2>&1; then
        candidates+=("$(dirname -- "$(command -v vcpkg)")")
    fi
fi
resolved_root=""
for candidate in "${candidates[@]}"; do
    if [[ -x "$candidate/vcpkg" && -f "$candidate/scripts/buildsystems/vcpkg.cmake" ]]; then
        resolved_root="$(cd -- "$candidate" && pwd)"
        break
    fi
done
if [[ -z "$resolved_root" ]]; then
    echo "Bootstrapped vcpkg not found. Pass its directory as the first argument or set VCPKG_ROOT. See README.md." >&2
    exit 1
fi
export VCPKG_ROOT="$resolved_root"
for tool in cmake ninja git xcrun otool; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "Required tool missing: $tool. See README.md." >&2
        exit 1
    fi
done
xcrun --find clang++ >/dev/null
case "$(uname -m)" in
    x86_64) arch=x64; osx_arch=x86_64 ;;
    arm64|aarch64) arch=arm64; osx_arch=arm64 ;;
    *) echo "Unsupported architecture: $(uname -m)" >&2; exit 1 ;;
esac

cd -- "$root"
preset=macos-release
# Separate architecture caches so Intel/Rosetta and arm64 builds cannot mix.
build="$root/build/$preset-$arch"
echo "Using vcpkg: $VCPKG_ROOT ($arch-osx)"
cmake --preset "$preset" -B "$build" \
    -DVCPKG_TARGET_TRIPLET="$arch-osx" -DCMAKE_OSX_ARCHITECTURES="$osx_arch"
cmake --build "$build" --parallel

dist="$root/dist/macos-$arch"
exe="$build/updater"
dependencies="$(otool -L "$exe")"
while read -r library _; do
    case "$library" in
        /usr/lib/*|/System/Library/*) ;;
        *) echo "Unexpected dynamic dependency: $library" >&2; exit 1 ;;
    esac
done <<< "$(tail -n +2 <<< "$dependencies")"
"$exe" --help
cmake --install "$build" --config Release --prefix "$dist"
printf 'Packaged %s\n' "$dist/updater"
