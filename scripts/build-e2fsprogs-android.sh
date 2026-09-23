#!/usr/bin/env bash
set -euo pipefail

# Build the small e2fsprogs libraries VineOS needs with the Android NDK.
# Usage: ANDROID_NDK_HOME=~/Android/Sdk/ndk/28.2.13676358 ./scripts/build-e2fsprogs-android.sh [arm64-v8a]

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/third_party/e2fsprogs"
OUT="$ROOT/third_party/e2fsprogs-android"
ABI="${1:-arm64-v8a}"
API="${ANDROID_API:-26}"
NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"

if [[ -z "$NDK" ]]; then
  echo "Set ANDROID_NDK_HOME (or ANDROID_NDK_ROOT) to your Android NDK." >&2
  exit 2
fi

HOST_TAG=linux-x86_64
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/$HOST_TAG"
case "$ABI" in
  arm64-v8a) TARGET=aarch64-linux-android ;;
  armeabi-v7a) TARGET=armv7a-linux-androideabi ;;
  x86_64) TARGET=x86_64-linux-android ;;
  x86) TARGET=i686-linux-android ;;
  *) echo "Unsupported ABI: $ABI" >&2; exit 2 ;;
esac

BUILD="$ROOT/.build/e2fsprogs-$ABI"
PREFIX="$OUT/$ABI"
rm -rf "$BUILD" "$PREFIX"
mkdir -p "$BUILD" "$PREFIX"

# e2fsprogs' generated configure script is intentionally used here instead of
# trying to duplicate its feature checks in VineOS CMake.
cd "$BUILD"
export CC="$TOOLCHAIN/bin/${TARGET}${API}-clang"
export AR="$TOOLCHAIN/bin/llvm-ar"
export RANLIB="$TOOLCHAIN/bin/llvm-ranlib"
export STRIP="$TOOLCHAIN/bin/llvm-strip"
export CFLAGS="-fPIC -O2"

"$SRC/configure" \
  --host="$TARGET" \
  --prefix="$PREFIX" \
  --disable-shared \
  --enable-static \
  --disable-nls \
  --disable-tdb \
  --disable-debugfs \
  --disable-imager \
  --disable-resizer \
  --disable-defrag \
  --disable-fsck \
  --disable-e2initrd-helper \
  --without-systemd-unit-dir

make -j"$(nproc)" libs
make install-libs

echo "e2fsprogs Android libraries installed to: $PREFIX"
find "$PREFIX" -maxdepth 2 -type f \( -name '*.a' -o -name '*.h' \) -print
