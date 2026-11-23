#!/usr/bin/env bash
# Builds a dedicated i686-elf cross-compiler (binutils + GCC, C only,
# no host libc linkage), per docs/FOUNDING-PROPOSAL.md §4.2.
#
# This deliberately does NOT use the host's own gcc -m32 to build ManiOS:
# a real i686-elf cross-compiler has no hosted-environment assumptions
# baked into its default specs (no PIE, no stack-protector runtime
# dependency, no host libc/headers reachable by accident), which matters
# more as the kernel grows than it does for a single boot.S file.
#
# Usage: tools/toolchain/build-i686-elf-toolchain.sh
# Installs into tools/toolchain/i686-elf/ (git-ignored). Re-run is safe;
# it skips steps whose output already exists.

set -euo pipefail

BINUTILS_VERSION=2.42
GCC_VERSION=13.2.0
TARGET=i686-elf

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="$SCRIPT_DIR/i686-elf"
BUILD_DIR="$SCRIPT_DIR/build"
SRC_DIR="$BUILD_DIR/src"

export PATH="$PREFIX/bin:$PATH"

mkdir -p "$SRC_DIR"
cd "$SRC_DIR"

# GNU mirrors, tried in turn: ftp.gnu.org alone has been unreachable from
# CI (a release build failed on 2026-11-23). Every tarball must match its
# pinned SHA-256, whichever mirror served it.
GNU_MIRRORS="https://ftpmirror.gnu.org/gnu https://mirrors.kernel.org/gnu https://ftp.gnu.org/gnu"

fetch() {
  local path="$1" out="$2" sha="$3" m
  if [ -f "$out" ] && echo "$sha  $out" | sha256sum -c --quiet - 2>/dev/null; then
    return 0
  fi
  for m in $GNU_MIRRORS; do
    echo "==> Fetching $out from $m"
    rm -f "$out"
    if curl -fL --retry 3 --retry-delay 5 --connect-timeout 30 -o "$out" "$m/$path" &&
       echo "$sha  $out" | sha256sum -c --quiet -; then
      return 0
    fi
    echo "==> $m failed or gave a wrong checksum" >&2
  done
  rm -f "$out"
  echo "error: could not fetch $out from any GNU mirror" >&2
  exit 1
}

fetch "binutils/binutils-${BINUTILS_VERSION}.tar.xz" "binutils-${BINUTILS_VERSION}.tar.xz" \
      f6e4d41fd5fc778b06b7891457b3620da5ecea1006c6a4a41ae998109f85a800
fetch "gcc/gcc-${GCC_VERSION}/gcc-${GCC_VERSION}.tar.xz" "gcc-${GCC_VERSION}.tar.xz" \
      e275e76442a6067341a27f04c5c6b83d8613144004c0413528863dc6b5c743da

[ -d "binutils-${BINUTILS_VERSION}" ] || tar xf "binutils-${BINUTILS_VERSION}.tar.xz"
[ -d "gcc-${GCC_VERSION}" ] || tar xf "gcc-${GCC_VERSION}.tar.xz"

# --- binutils ---
if [ ! -x "$PREFIX/bin/${TARGET}-ld" ]; then
  echo "==> Building binutils ${BINUTILS_VERSION} for ${TARGET}"
  mkdir -p "$BUILD_DIR/binutils-build"
  cd "$BUILD_DIR/binutils-build"
  "$SRC_DIR/binutils-${BINUTILS_VERSION}/configure" \
    --target="$TARGET" --prefix="$PREFIX" \
    --with-sysroot --disable-nls --disable-werror
  make -j"$(nproc)"
  make install
fi

# --- gcc (C only, no libc — matches --without-headers freestanding target) ---
if [ ! -x "$PREFIX/bin/${TARGET}-gcc" ]; then
  echo "==> Building gcc ${GCC_VERSION} for ${TARGET}"
  cd "$SRC_DIR/gcc-${GCC_VERSION}"
  ./contrib/download_prerequisites
  mkdir -p "$BUILD_DIR/gcc-build"
  cd "$BUILD_DIR/gcc-build"
  "$SRC_DIR/gcc-${GCC_VERSION}/configure" \
    --target="$TARGET" --prefix="$PREFIX" \
    --disable-nls --enable-languages=c --without-headers \
    --disable-shared --disable-threads --disable-libssp \
    --disable-libgomp --disable-libquadmath
  make -j"$(nproc)" all-gcc all-target-libgcc
  make install-gcc install-target-libgcc
fi

echo "==> ${TARGET} cross-compiler ready at $PREFIX/bin"
"$PREFIX/bin/${TARGET}-gcc" --version | head -1
