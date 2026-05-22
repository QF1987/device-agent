#!/usr/bin/env bash
# Build libtorrent for Android as static prebuilt libraries.
#
# Defaults intentionally match android/app/build.gradle and the existing gRPC
# prebuilt bundle:
#   NDK 30.0.14904198, minSdk 24, STL c++_shared
#
# Usage:
#   bash scripts/build-libtorrent-android.sh
#
# Env overrides:
#   ANDROID_HOME=/path/to/Android/SDK
#   NDK_VERSION=30.0.14904198
#   ANDROID_PLATFORM=android-24
#   ABIS="arm64-v8a armeabi-v7a"
#   LIBTORRENT_VERSION=v2.0.11
#   BOOST_VERSION=1.84.0
#   BUILD_ROOT=/private/tmp/device-agent-libtorrent-android
#   JOBS=8

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

ANDROID_HOME="${ANDROID_HOME:-$HOME/Software/Android/SDK}"
NDK_VERSION="${NDK_VERSION:-30.0.14904198}"
ANDROID_PLATFORM="${ANDROID_PLATFORM:-android-24}"
ABIS="${ABIS:-arm64-v8a armeabi-v7a}"
LIBTORRENT_VERSION="${LIBTORRENT_VERSION:-v2.0.11}"
BOOST_VERSION="${BOOST_VERSION:-1.84.0}"
BUILD_ROOT="${BUILD_ROOT:-/private/tmp/device-agent-libtorrent-android}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)}"

NDK_ROOT="$ANDROID_HOME/ndk/$NDK_VERSION"
NDK_TOOLCHAIN="$NDK_ROOT/build/cmake/android.toolchain.cmake"
HOST_TAG="darwin-x86_64"
if [ "$(uname -m)" = "arm64" ] && [ -d "$NDK_ROOT/toolchains/llvm/prebuilt/darwin-arm64" ]; then
    HOST_TAG="darwin-arm64"
fi
LLVM_BIN="$NDK_ROOT/toolchains/llvm/prebuilt/$HOST_TAG/bin"

BOOST_UNDERSCORE="${BOOST_VERSION//./_}"
BOOST_TARBALL="boost_${BOOST_UNDERSCORE}.tar.gz"
BOOST_URL="https://archives.boost.io/release/${BOOST_VERSION}/source/${BOOST_TARBALL}"
BOOST_SRC="$BUILD_ROOT/boost_${BOOST_UNDERSCORE}"
LIBTORRENT_SRC="$BUILD_ROOT/libtorrent-src"
INSTALL_BASE="$REPO_ROOT/android/libtorrent-android-prebuilt"
SHARED_INCLUDE_DIR="$INSTALL_BASE/include"
SHARED_BOOST_INCLUDE="$SHARED_INCLUDE_DIR/boost"

echo "==> Android SDK: $ANDROID_HOME"
echo "==> NDK toolchain: $NDK_TOOLCHAIN"
echo "==> libtorrent: $LIBTORRENT_VERSION"
echo "==> Boost: $BOOST_VERSION"
echo "==> ABIs: $ABIS"

if [ ! -f "$NDK_TOOLCHAIN" ]; then
    echo "ERROR: NDK toolchain not found. Install NDK $NDK_VERSION first."
    echo "  sdkmanager --install 'ndk;$NDK_VERSION'"
    exit 1
fi

if ! command -v cmake >/dev/null; then
    for sdk_cmake in "$ANDROID_HOME/cmake/3.22.1/bin/cmake" "$ANDROID_HOME"/cmake/*/bin/cmake; do
        if [ -x "$sdk_cmake" ]; then
            export PATH="$(dirname "$sdk_cmake"):$PATH"
            break
        fi
    done
fi

for tool in cmake git curl tar; do
    command -v "$tool" >/dev/null || { echo "ERROR: $tool not found"; exit 1; }
done

mkdir -p "$BUILD_ROOT" "$INSTALL_BASE"
rm -rf "$SHARED_BOOST_INCLUDE"
mkdir -p "$SHARED_INCLUDE_DIR"

if [ ! -d "$LIBTORRENT_SRC/.git" ]; then
    echo "==> Cloning libtorrent $LIBTORRENT_VERSION"
    git clone --recurse-submodules --depth 1 -b "$LIBTORRENT_VERSION" \
        https://github.com/arvidn/libtorrent.git "$LIBTORRENT_SRC"
    (cd "$LIBTORRENT_SRC" && git submodule update --init --recursive --depth 1)
else
    echo "==> libtorrent source already exists at $LIBTORRENT_SRC"
    CURRENT_TAG="$(cd "$LIBTORRENT_SRC" && git describe --tags --exact-match 2>/dev/null || true)"
    if [ "$CURRENT_TAG" != "$LIBTORRENT_VERSION" ]; then
        echo "ERROR: existing libtorrent source is '$CURRENT_TAG', expected '$LIBTORRENT_VERSION'."
        echo "       Remove $LIBTORRENT_SRC or set BUILD_ROOT to a clean directory."
        exit 1
    fi
    (cd "$LIBTORRENT_SRC" && git submodule update --init --recursive --depth 1)
fi

if [ ! -d "$BOOST_SRC" ]; then
    echo "==> Downloading Boost $BOOST_VERSION"
    curl -L --fail "$BOOST_URL" -o "$BUILD_ROOT/$BOOST_TARBALL"
    tar -xzf "$BUILD_ROOT/$BOOST_TARBALL" -C "$BUILD_ROOT"
else
    echo "==> Boost source already exists at $BOOST_SRC"
fi

boost_toolset_for_abi() {
    case "$1" in
        arm64-v8a) echo "androidarm64" ;;
        armeabi-v7a) echo "androidarmv7" ;;
        *) echo "ERROR: unsupported ABI '$1'" >&2; return 1 ;;
    esac
}

boost_triple_for_abi() {
    case "$1" in
        arm64-v8a) echo "aarch64-linux-android24" ;;
        armeabi-v7a) echo "armv7a-linux-androideabi24" ;;
        *) echo "ERROR: unsupported ABI '$1'" >&2; return 1 ;;
    esac
}

boost_arch_for_abi() {
    case "$1" in
        arm64-v8a) echo "architecture=arm address-model=64 abi=aapcs" ;;
        armeabi-v7a) echo "architecture=arm address-model=32 abi=aapcs" ;;
        *) echo "ERROR: unsupported ABI '$1'" >&2; return 1 ;;
    esac
}

copy_libtorrent_boost_headers() {
    local abi="$1"
    local install_dir="$2"
    local boost_install="$3"
    local boost_include="$boost_install/include"
    local scan_dir="$BUILD_ROOT/boost-header-scan/$abi"
    local scan_tu="$scan_dir/libtorrent_public_headers.cc"
    local deps_raw="$scan_dir/deps.raw"
    local deps_list="$scan_dir/boost-headers.txt"
    local triple

    triple="$(boost_triple_for_abi "$abi")"
    rm -rf "$scan_dir"
    mkdir -p "$scan_dir"

    {
        echo "#include <libtorrent/version.hpp>"
        echo "#include <libtorrent/session.hpp>"
        echo "#include <libtorrent/add_torrent_params.hpp>"
        echo "#include <libtorrent/torrent_info.hpp>"
        echo "#include <libtorrent/alert_types.hpp>"
        echo "#include <libtorrent/settings_pack.hpp>"
        echo "#include <libtorrent/magnet_uri.hpp>"
        echo "#include <libtorrent/create_torrent.hpp>"
    } > "$scan_tu"

    "$LLVM_BIN/${triple}-clang++" \
        -std=c++17 \
        -D__ANDROID_API__=24 \
        -I"$install_dir/include" \
        -I"$boost_include" \
        -M -MT libtorrent_public_headers "$scan_tu" > "$deps_raw"

    tr ' \\' '\n\n' < "$deps_raw" \
        | sed '/^$/d' \
        | grep "^$boost_include/boost/" \
        | sort -u > "$deps_list"

    while IFS= read -r header; do
        local rel="${header#$boost_include/}"
        mkdir -p "$SHARED_INCLUDE_DIR/$(dirname "$rel")"
        cp "$header" "$SHARED_INCLUDE_DIR/$rel"
    done < "$deps_list"

    echo "==> Copied $(wc -l < "$deps_list" | tr -d ' ') Boost public dependency headers for $abi"
}

if [ ! -x "$BOOST_SRC/b2" ]; then
    echo "==> Bootstrapping Boost build engine"
    (cd "$BOOST_SRC" && ./bootstrap.sh)
fi

for ABI in $ABIS; do
    echo ""
    echo "══════════════════════════════════════════════════════"
    echo "  Building libtorrent for $ABI"
    echo "══════════════════════════════════════════════════════"

    BOOST_INSTALL="$BUILD_ROOT/boost-install/$ABI"
    LIBTORRENT_BUILD="$BUILD_ROOT/libtorrent-build/$ABI"
    INSTALL_DIR="$INSTALL_BASE/$ABI"
    TOOLSET="$(boost_toolset_for_abi "$ABI")"
    TRIPLE="$(boost_triple_for_abi "$ABI")"
    ARCH_OPTS="$(boost_arch_for_abi "$ABI")"
    USER_CONFIG="$BUILD_ROOT/user-config-$ABI.jam"

    cat > "$USER_CONFIG" <<EOF
using clang : $TOOLSET : $LLVM_BIN/${TRIPLE}-clang++ :
    <archiver>$LLVM_BIN/llvm-ar
    <ranlib>$LLVM_BIN/llvm-ranlib
    <compileflags>--target=$TRIPLE
    <compileflags>--sysroot=$NDK_ROOT/toolchains/llvm/prebuilt/$HOST_TAG/sysroot
    <compileflags>-D__ANDROID_API__=24
    <compileflags>-fPIC
    <cxxflags>-std=c++17
    <linkflags>--target=$TRIPLE
    <linkflags>--sysroot=$NDK_ROOT/toolchains/llvm/prebuilt/$HOST_TAG/sysroot
;
EOF

    rm -rf "$BOOST_INSTALL" "$LIBTORRENT_BUILD" "$INSTALL_DIR"
    mkdir -p "$BOOST_INSTALL" "$LIBTORRENT_BUILD" "$INSTALL_DIR"

    echo "==> Building Boost.System for $ABI"
    (cd "$BOOST_SRC" && ./b2 \
        --user-config="$USER_CONFIG" \
        --prefix="$BOOST_INSTALL" \
        --with-system \
        toolset=clang-$TOOLSET \
        target-os=android \
        $ARCH_OPTS \
        variant=release \
        link=static \
        threading=multi \
        runtime-link=shared \
        cxxstd=17 \
        install \
        -j "$JOBS")

    echo "==> Configuring libtorrent for $ABI"
    cmake -G Ninja -S "$LIBTORRENT_SRC" -B "$LIBTORRENT_BUILD" \
        -DCMAKE_TOOLCHAIN_FILE="$NDK_TOOLCHAIN" \
        -DANDROID_ABI="$ABI" \
        -DANDROID_PLATFORM="$ANDROID_PLATFORM" \
        -DANDROID_STL=c++_shared \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DBUILD_SHARED_LIBS=OFF \
        -DBoost_NO_SYSTEM_PATHS=ON \
        -DBoost_USE_STATIC_LIBS=ON \
        -DBoost_INCLUDE_DIR="$BOOST_INSTALL/include" \
        -DBoost_LIBRARY_DIR="$BOOST_INSTALL/lib" \
        -Dbuild_tests=OFF \
        -Dbuild_examples=OFF \
        -Dbuild_tools=OFF \
        -Dpython-bindings=OFF \
        -Ddeprecated-functions=OFF \
        -Dencryption=ON

    echo "==> Building libtorrent for $ABI"
    cmake --build "$LIBTORRENT_BUILD" -j "$JOBS"

    echo "==> Installing libtorrent to $INSTALL_DIR"
    cmake --install "$LIBTORRENT_BUILD"

    echo "==> Copying Boost static libs into $INSTALL_DIR"
    mkdir -p "$INSTALL_DIR/lib"
    rsync -a "$BOOST_INSTALL/lib/" "$INSTALL_DIR/lib/"
    copy_libtorrent_boost_headers "$ABI" "$INSTALL_DIR" "$BOOST_INSTALL"

    test -f "$INSTALL_DIR/lib/libtorrent-rasterbar.a" || {
        echo "ERROR: missing $INSTALL_DIR/lib/libtorrent-rasterbar.a"
        exit 1
    }
    test -d "$INSTALL_DIR/include/libtorrent" || {
        echo "ERROR: missing libtorrent headers in $INSTALL_DIR/include/libtorrent"
        exit 1
    }
    test -d "$SHARED_BOOST_INCLUDE" || {
        echo "ERROR: missing shared Boost subset in $SHARED_BOOST_INCLUDE"
        exit 1
    }

    echo "==> $ABI done"
done

echo ""
echo "══════════════════════════════════════════════════════"
echo "  Build complete"
echo "══════════════════════════════════════════════════════"
for ABI in $ABIS; do
    INSTALL_DIR="$INSTALL_BASE/$ABI"
    echo "--- $ABI ---"
    ls -lh "$INSTALL_DIR/lib/libtorrent-rasterbar.a"
    ls "$INSTALL_DIR/lib"/libboost_system*.a 2>/dev/null | xargs -n 1 basename
    echo "  headers: $INSTALL_DIR/include"
done
echo "Shared Boost subset:"
du -sh "$SHARED_BOOST_INCLUDE"

echo ""
echo "Next:"
echo "  cmake -B build && cmake --build build"
echo "  cd android && ./gradlew :app:assembleDebug"
