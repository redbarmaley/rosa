#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

die() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

if [[ -n "${CMAKE_BIN:-}" ]]; then
    CMAKE="$CMAKE_BIN"
else
    CMAKE="$(command -v cmake || true)"
fi
[[ -n "$CMAKE" && -x "$CMAKE" ]] || die "cmake not found: set CMAKE_BIN or add cmake to PATH"

command -v python3 >/dev/null 2>&1 || die "python3 not found in PATH"

CROSS_TOOLCHAIN_ROOT="${CROSS_TOOLCHAIN_ROOT:-/opt/gcc-arm-none-eabi-10.3-2021.10}"
[[ -x "$CROSS_TOOLCHAIN_ROOT/bin/arm-none-eabi-gcc" ]] \
    || die "ARM GCC not found or not executable: $CROSS_TOOLCHAIN_ROOT/bin/arm-none-eabi-gcc (set CROSS_TOOLCHAIN_ROOT)"
export CROSS_TOOLCHAIN_ROOT

SDK_DIR="$ROOT_DIR/OpenBK7231T_App/sdk/OpenLN882H"
PROJECT_LINK_DIR="$SDK_DIR/project"
PROJECT_LINK="$PROJECT_LINK_DIR/PowerSocket"
PROJECT_DIR="$ROOT_DIR/ln882h_power_socket"
BUILD_DIR="$ROOT_DIR/build_power_socket"
OUTPUT_DIR="$PROJECT_DIR/output"

[[ -d "$PROJECT_LINK_DIR" ]] || die "SDK project directory not found: $PROJECT_LINK_DIR"
[[ -d "$PROJECT_DIR" ]] || die "Power socket project directory not found: $PROJECT_DIR"

if [[ -e "$PROJECT_LINK" && ! -L "$PROJECT_LINK" ]]; then
    die "$PROJECT_LINK exists and is not a symlink; refusing to replace it"
fi
ln -sfn "$PROJECT_DIR" "$PROJECT_LINK"
printf 'Symlink: %s -> %s\n' "$PROJECT_LINK" "$PROJECT_DIR"

cd "$ROOT_DIR"
"$CMAKE" -S OpenBK7231T_App/sdk/OpenLN882H -B build_power_socket -DUSER_PROJECT=PowerSocket
"$CMAKE" --build build_power_socket -j"$(nproc)"

mkdir -p "$OUTPUT_DIR"

copy_artifact() {
    local name="$1"
    local src="$BUILD_DIR/bin/$name"
    [[ -f "$src" ]] || die "Build artifact not found: $src"
    cp -f "$src" "$OUTPUT_DIR/"
    printf 'Copied: %s -> %s/\n' "$src" "$OUTPUT_DIR"
}

copy_artifact flashimage.bin
copy_artifact flashimage-ota-xz-v0.1.bin

printf 'Build complete. Artifacts are in %s\n' "$OUTPUT_DIR"
