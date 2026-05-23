#!/bin/bash
# ============================================================
# 统一 Proto 代码生成脚本
#
# 因为 Android gRPC prebuilt（protobuf v31.1）和桌面 Homebrew（v34.1）
# 版本不同，无法用一个 protoc 生成兼容两边的代码。
#
# 此脚本用各自的 protoc 生成，但保证 proto 定义一致。
# 改 proto 后运行此脚本即可。
#
# 默认工具路径（可用环境变量覆盖）：
# - protoc v34.1: /Users/qf/Software/protobuf/34.1/bin/protoc（desktop stub）
# - protoc v31.1: /Users/qf/Software/protobuf/31.1/bin/protoc（Android stub）
# ============================================================
set -e
cd "$(dirname "$0")/.."

DEFAULT_PROTOC_DESKTOP="/Users/qf/Software/protobuf/34.1/bin/protoc"
DEFAULT_PROTOC_ANDROID="/Users/qf/Software/protobuf/31.1/bin/protoc"

PROTOC_DESKTOP="${PROTOC_DESKTOP:-${PROTOC:-$DEFAULT_PROTOC_DESKTOP}}"
PROTOC_ANDROID="${PROTOC_ANDROID:-$DEFAULT_PROTOC_ANDROID}"

require_protoc_version() {
  local binary="$1"
  local expected="$2"
  local role="$3"
  local actual

  actual="$("$binary" --version 2>/dev/null || true)"
  if [ "$actual" != "libprotoc $expected" ]; then
    echo "wrong $role protoc version: expected libprotoc $expected, got '${actual:-<not executable>}'" >&2
    echo "set PROTOC_${role^^} to a matching protoc binary" >&2
    exit 1
  fi
}

if [ ! -x "$PROTOC_DESKTOP" ]; then
  echo "missing desktop protoc; set PROTOC_DESKTOP to protoc v34.1" >&2
  exit 1
fi

if [ ! -x "$PROTOC_ANDROID" ]; then
  echo "missing Android protoc; set PROTOC_ANDROID to protoc v31.1" >&2
  exit 1
fi

require_protoc_version "$PROTOC_DESKTOP" "34.1" "DESKTOP"
require_protoc_version "$PROTOC_ANDROID" "31.1" "ANDROID"

GRPC_CPP_PLUGIN="${GRPC_CPP_PLUGIN:-$(command -v grpc_cpp_plugin || true)}"
if [ -z "$GRPC_CPP_PLUGIN" ]; then
  GRPC_CPP_PLUGIN="android/grpc-android-prebuilt/host-tools/grpc_cpp_plugin"
fi

if [ ! -x "$GRPC_CPP_PLUGIN" ]; then
  echo "missing grpc_cpp_plugin; set GRPC_CPP_PLUGIN or install grpc_cpp_plugin" >&2
  exit 1
fi

echo "=== 1/2 生成 gen-desktop（protoc, $("$PROTOC_DESKTOP" --version)) ==="
"$PROTOC_DESKTOP" \
  -I proto \
  -I /opt/homebrew/include \
  --cpp_out=gen-desktop \
  --grpc_out=gen-desktop \
  --plugin=protoc-gen-grpc="$GRPC_CPP_PLUGIN" \
  proto/terminal_agent/v1/service.proto \
  proto/terminal_agent/v1/device.proto

echo "=== 2/2 生成 gen-android（protoc, $("$PROTOC_ANDROID" --version)) ==="
"$PROTOC_ANDROID" \
  -I proto \
  -I android/grpc-android-prebuilt/arm64-v8a/include \
  --cpp_out=gen-android \
  --grpc_out=gen-android \
  --plugin=protoc-gen-grpc="$GRPC_CPP_PLUGIN" \
  proto/terminal_agent/v1/service.proto \
  proto/terminal_agent/v1/device.proto

echo ""
echo "✅ 完成"
echo "   gen-desktop: $(grep -c ReportReleaseStatus gen-desktop/terminal_agent/v1/service.grpc.pb.h) ReportReleaseStatus refs"
echo "   gen-android: $(grep -c ReportReleaseStatus gen-android/terminal_agent/v1/service.grpc.pb.h) ReportReleaseStatus refs"
