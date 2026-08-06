#!/usr/bin/env bash
#
# Fetch and build DXVK Native's d3d9 as a plain Linux shared object, for
# linking straight into our ELF. No mingw, no Wine at runtime.
#
#   native/tools/build_dxvk_native.sh [32|64]
#
# Result: native/third_party/dxvk-native/lib<bits>/libdxvk_d3d9.so
#
# Only d3d9 is built (dxgi/d3d10/d3d11/d3d8 are switched off) since that is
# the only API the game uses - see DIRECTX_SCOPE.md.
set -euo pipefail

BITS="${1:-32}"
if [[ "$BITS" != "32" && "$BITS" != "64" ]]; then
    echo "usage: $0 [32|64]" >&2
    exit 2
fi

# Pinned so a rebuild is reproducible and an upstream ABI change cannot land
# silently. Bump deliberately, and re-run the abi-layout-match test after.
DXVK_TAG="${DXVK_TAG:-v3.0.2}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NATIVE_DIR="$(dirname "$SCRIPT_DIR")"
SRC_DIR="$NATIVE_DIR/third_party/dxvk"
BUILD_DIR="$SRC_DIR/build-native-$BITS"
OUT_DIR="$NATIVE_DIR/third_party/dxvk-native/lib$BITS"

for tool in git meson ninja glslang glslangValidator; do
    command -v "$tool" >/dev/null || missing="${missing:-} $tool"
done
if [[ -n "${missing:-}" ]]; then
    # glslang and glslangValidator are alternative names for the same tool;
    # only complain if neither is present.
    if [[ "$missing" == " glslang" || "$missing" == " glslangValidator" ]]; then
        :
    else
        echo "missing required tool(s):$missing" >&2
        exit 1
    fi
fi

if [[ -e "$SRC_DIR" && ! -d "$SRC_DIR/.git" ]]; then
    echo "$SRC_DIR exists but is not a git checkout." >&2
    echo "Remove it (or point DXVK at a real checkout) and re-run." >&2
    exit 1
fi

if [[ ! -d "$SRC_DIR/.git" ]]; then
    echo ">>> cloning dxvk $DXVK_TAG"
    git clone --branch "$DXVK_TAG" --depth 1 --recurse-submodules --shallow-submodules \
        https://github.com/doitsujin/dxvk.git "$SRC_DIR"
else
    echo ">>> using existing checkout in $SRC_DIR ($(git -C "$SRC_DIR" describe --tags --always))"
fi

MESON_ARGS=(
    --buildtype release
    -Denable_dxgi=false
    -Denable_d3d8=false
    -Denable_d3d10=false
    -Denable_d3d11=false
)

if [[ "$BITS" == "32" ]]; then
    MESON_ARGS+=(--cross-file "$NATIVE_DIR/cross/linux32.txt")
fi

if [[ ! -d "$BUILD_DIR" ]]; then
    echo ">>> configuring ($BITS-bit)"
    if ! meson setup "${MESON_ARGS[@]}" "$BUILD_DIR" "$SRC_DIR"; then
        cat >&2 <<EOF

DXVK Native needs SDL3, SDL2 or GLFW for its window-system layer, matching the
target word size. If that is what failed above:

  Arch, 32-bit: sudo pacman -S lib32-sdl2-compat lib32-vulkan-icd-loader
  Arch, 64-bit: sudo pacman -S sdl2-compat vulkan-icd-loader

EOF
        exit 1
    fi
fi

echo ">>> building"
ninja -C "$BUILD_DIR" src/d3d9/all 2>/dev/null || ninja -C "$BUILD_DIR"

mkdir -p "$OUT_DIR"
found=0
for so in "$BUILD_DIR"/src/d3d9/libdxvk_d3d9.so*; do
    [[ -e "$so" ]] || continue
    cp -P "$so" "$OUT_DIR/"
    found=1
done
if [[ "$found" != "1" ]]; then
    echo "build finished but libdxvk_d3d9.so was not produced" >&2
    exit 1
fi

echo
echo ">>> installed into $OUT_DIR:"
ls -l "$OUT_DIR"
echo
echo "next: meson setup native/build$BITS$([[ $BITS == 32 ]] && echo ' --cross-file native/cross/linux32.txt')"
