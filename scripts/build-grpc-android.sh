#!/usr/bin/env bash
# Build gRPC C++ from source for Android
# NDK 30 + gRPC v1.80.0
# Target ABIs: arm64-v8a, armeabi-v7a
#
# Usage: bash scripts/build-grpc-android.sh
# Env overrides:
#   NDK_VERSION=30.0.14904198
#   GRPC_VERSION=v1.80.0
#   ANDROID_PLATFORM=android-24
#   ABIS="arm64-v8a armeabi-v7a"

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# ─── Config ────────────────────────────────────────────────
ANDROID_HOME="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
NDK_VERSION="${NDK_VERSION:-30.0.14904198}"
GRPC_VERSION="${GRPC_VERSION:-v1.80.0}"
ANDROID_PLATFORM="${ANDROID_PLATFORM:-android-24}"
ABIS="${ABIS:-arm64-v8a armeabi-v7a}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)}"

NDK_TOOLCHAIN="$ANDROID_HOME/ndk/$NDK_VERSION/build/cmake/android.toolchain.cmake"
BUILD_DIR="$REPO_ROOT/.grpc-build"
GRPC_SRC="$BUILD_DIR/grpc-src"
INSTALL_BASE="$REPO_ROOT/android/grpc-android-prebuilt"

# ─── Verify prerequisites ─────────────────────────────────
echo "==> NDK toolchain: $NDK_TOOLCHAIN"
if [ ! -f "$NDK_TOOLCHAIN" ]; then
    echo "ERROR: NDK toolchain not found. Install NDK $NDK_VERSION first."
    echo "  sdkmanager --install 'ndk;$NDK_VERSION'"
    exit 1
fi

command -v cmake >/dev/null || { echo "ERROR: cmake not found"; exit 1; }
command -v git >/dev/null || { echo "ERROR: git not found"; exit 1; }

# ─── Clone gRPC source ────────────────────────────────────
mkdir -p "$BUILD_DIR"
if [ ! -d "$GRPC_SRC" ]; then
    echo "==> Cloning gRPC $GRPC_VERSION (with submodules)..."
    echo "    This will take a few minutes..."
    git clone --recurse-submodules --depth 1 -b "$GRPC_VERSION" \
        https://github.com/grpc/grpc.git "$GRPC_SRC"
    echo "==> Updating submodules..."
    cd "$GRPC_SRC" && git submodule update --init --recursive --depth 1 && cd -
else
    echo "==> gRPC source already exists at $GRPC_SRC"
    CURRENT_TAG=$(cd "$GRPC_SRC" && git describe --tags 2>/dev/null || echo "unknown")
    if [ "$CURRENT_TAG" != "$GRPC_VERSION" ]; then
        echo "WARNING: Existing source is $CURRENT_TAG, need $GRPC_VERSION"
        echo "==> Removing and re-cloning..."
        rm -rf "$GRPC_SRC"
        git clone --recurse-submodules --depth 1 -b "$GRPC_VERSION" \
            https://github.com/grpc/grpc.git "$GRPC_SRC"
        cd "$GRPC_SRC" && git submodule update --init --recursive --depth 1 && cd -
    fi
fi

# ─── Patch zlib version script (gz_intmax removed in newer zlib) ──
for ABI in $ABIS; do
    ZLIB_MAP="$GRPC_SRC/third_party/zlib/zlib.map"
    if [ -f "$ZLIB_MAP" ] && grep -q 'gz_intmax' "$ZLIB_MAP"; then
        echo "==> Patching zlib.map to remove gz_intmax..."
        sed -i '' '/gz_intmax;/d' "$ZLIB_MAP"
    fi
done

# ─── Build for each ABI ───────────────────────────────────
for ABI in $ABIS; do
    echo ""
    echo "══════════════════════════════════════════════════════"
    echo "  Building gRPC for $ABI"
    echo "══════════════════════════════════════════════════════"

    ABI_BUILD_DIR="$BUILD_DIR/build-$ABI"
    INSTALL_DIR="$INSTALL_BASE/$ABI"

    rm -rf "$ABI_BUILD_DIR"
    mkdir -p "$ABI_BUILD_DIR"
    mkdir -p "$INSTALL_DIR"

    cmake -G Ninja -S "$GRPC_SRC" -B "$ABI_BUILD_DIR" \
        -DCMAKE_TOOLCHAIN_FILE="$NDK_TOOLCHAIN" \
        -DANDROID_ABI="$ABI" \
        -DANDROID_PLATFORM="$ANDROID_PLATFORM" \
        -DANDROID_STL=c++_shared \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
        -DBUILD_SHARED_LIBS=OFF \
        -DgRPC_BUILD_TESTS=OFF \
        -DgRPC_BUILD_GRPC_CSHARP_PLUGIN=OFF \
        -DgRPC_BUILD_GRPC_NODE_PLUGIN=OFF \
        -DgRPC_BUILD_GRPC_OBJECTIVE_C_PLUGIN=OFF \
        -DgRPC_BUILD_GRPC_PHP_PLUGIN=OFF \
        -DgRPC_BUILD_GRPC_PYTHON_PLUGIN=OFF \
        -DgRPC_BUILD_GRPC_RUBY_PLUGIN=OFF \
        -DgRPC_BUILD_CODEGEN=OFF \
        -DgRPC_BUILD_PROTOBUF_PROVIDER=module \
        -DgRPC_BUILD_ZLIB_PROVIDER=module \
        -DgRPC_BUILD_CARES_PROVIDER=module \
        -DgRPC_BUILD_RE2_PROVIDER=module \
        -DgRPC_BUILD_ABSL_PROVIDER=module \
        -DgRPC_BUILD_SSL_PROVIDER=module \
        -Dprotobuf_BUILD_TESTS=OFF \
        -Dprotobuf_BUILD_PROTOC_BINARIES=OFF \
        -DABSL_BUILD_TESTING=OFF \
        -DABSL_USE_GOOGLETEST_HEAD=OFF \
        -DRE2_BUILD_TESTING=OFF \
        -Dc-ares_BUILD_TESTS=OFF

    echo "==> Building ($ABI, $JOBS parallel jobs)..."
    cmake --build "$ABI_BUILD_DIR" -j "$JOBS"

    echo "==> Installing to $INSTALL_DIR..."
    cmake --install "$ABI_BUILD_DIR"

    echo "==> ✅ $ABI done"
done

# ─── Verify ────────────────────────────────────────────────
echo ""
echo "══════════════════════════════════════════════════════"
echo "  Build complete! Verifying..."
echo "══════════════════════════════════════════════════════"

for ABI in $ABIS; do
    INSTALL_DIR="$INSTALL_BASE/$ABI"
    echo ""
    echo "--- $ABI ---"
    echo "Libraries:"
    ls "$INSTALL_DIR/lib/"*.a 2>/dev/null | wc -l | xargs echo "  static libs:"
    ls "$INSTALL_DIR/lib/"*.so 2>/dev/null | wc -l | xargs echo "  shared libs:"
    echo "  gRPC config: $(ls "$INSTALL_DIR"/lib/cmake/grpc/gRPCConfig.cmake 2>/dev/null && echo 'OK' || echo 'MISSING')"
    echo "  protobuf config: $(ls "$INSTALL_DIR"/lib/cmake/protobuf/protobuf-config.cmake 2>/dev/null && echo 'OK' || echo 'MISSING')"
done

echo ""
echo "==> Backup old prebuilt and switch..."
OLD_BACKUP="$REPO_ROOT/android/grpc-android-prebuilt-OLD"
if [ -d "$OLD_BACKUP" ]; then
    rm -rf "$OLD_BACKUP"
fi

# Move old prebuilt to backup (if it's not already our new build)
for ABI in $ABIS; do
    if [ -d "$REPO_ROOT/android/grpc-android-prebuilt-OLD/$ABI" ]; then
        continue
    fi
done

echo ""
echo "==> Done! Next steps:"
echo "  1. Rebuild device-agent APK:"
echo "     cd android && ./gradlew :app:assembleDebug"
echo "  2. Install and test on device"
echo "  3. If proto files need regeneration (protobuf version mismatch):"
echo "     bash scripts/gen-proto.sh"
