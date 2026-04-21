#!/usr/bin/env bash
# 一键生成 Android + 桌面两套 protobuf/gRPC 代码
# Android 用 protoc v25.1 (host-tools 里的 libprotoc 25.1)
# 桌面   用 protoc v34.1 (Homebrew)
#
# 环境变量覆盖（可选）：
#   PROTOC_ANDROID=/path/to/protoc
#   PROTOC_DESKTOP=/path/to/protoc
#   GRPC_CPP_PLUGIN_ANDROID=/path/to/grpc_cpp_plugin
#   GRPC_CPP_PLUGIN_DESKTOP=/path/to/grpc_cpp_plugin

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

PROTO_DIR="$REPO_ROOT/proto"
GEN_ANDROID="$REPO_ROOT/gen-android"
GEN_DESKTOP="$REPO_ROOT/gen-desktop"

PROTOC_ANDROID="${PROTOC_ANDROID:-$REPO_ROOT/android/grpc-android-prebuilt/host-tools/protoc}"
PROTOC_DESKTOP="${PROTOC_DESKTOP:-/opt/homebrew/bin/protoc}"
GRPC_CPP_PLUGIN_ANDROID="${GRPC_CPP_PLUGIN_ANDROID:-$REPO_ROOT/android/grpc-android-prebuilt/host-tools/grpc_cpp_plugin}"
GRPC_CPP_PLUGIN_DESKTOP="${GRPC_CPP_PLUGIN_DESKTOP:-/opt/homebrew/bin/grpc_cpp_plugin}"

PROTO_FILES=$(find "$PROTO_DIR" -name "*.proto")

echo "==> generating Android protos into $GEN_ANDROID (protoc=$PROTOC_ANDROID)"
mkdir -p "$GEN_ANDROID"
"$PROTOC_ANDROID" \
    --proto_path="$PROTO_DIR" \
    --cpp_out="$GEN_ANDROID" \
    --grpc_out="$GEN_ANDROID" \
    --plugin=protoc-gen-grpc="$GRPC_CPP_PLUGIN_ANDROID" \
    $PROTO_FILES

echo "==> generating Desktop protos into $GEN_DESKTOP (protoc=$PROTOC_DESKTOP)"
mkdir -p "$GEN_DESKTOP"
"$PROTOC_DESKTOP" \
    --proto_path="$PROTO_DIR" \
    --cpp_out="$GEN_DESKTOP" \
    --grpc_out="$GEN_DESKTOP" \
    --plugin=protoc-gen-grpc="$GRPC_CPP_PLUGIN_DESKTOP" \
    $PROTO_FILES

echo "==> done"
