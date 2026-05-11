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
# protoc v31.1: 为 Android 编译的，放在 tools/protoc-31.1
# protoc v34.1: Homebrew 安装的，路径 /opt/homebrew/bin/protoc
# ============================================================
set -e
cd "$(dirname "$0")/.."

PROTOC_DESKTOP="${PROTOC_DESKTOP:-${PROTOC:-$(command -v protoc || true)}}"
PROTOC_ANDROID="${PROTOC_ANDROID:-tools/protoc-31.1}"

if [ ! -x "$PROTOC_DESKTOP" ]; then
  echo "missing desktop protoc; set PROTOC_DESKTOP to protoc v34.1" >&2
  exit 1
fi

if [ ! -x "$PROTOC_ANDROID" ]; then
  echo "missing Android protoc; set PROTOC_ANDROID to protoc v31.1" >&2
  exit 1
fi

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
