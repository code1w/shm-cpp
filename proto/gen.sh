#!/bin/bash
# 使用 protoc 生成 C++ 协议代码
# 用法: cd proto && ./gen.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
PROTOC="$PROJECT_ROOT/3rd/protobuf/bin/protoc"

if [ ! -x "$PROTOC" ]; then
    echo "error: protoc not found at $PROTOC" >&2
    exit 1
fi

"$PROTOC" --cpp_out="$SCRIPT_DIR" --proto_path="$SCRIPT_DIR" "$SCRIPT_DIR"/*.proto

echo "generated:"
ls -1 "$SCRIPT_DIR"/*.pb.h "$SCRIPT_DIR"/*.pb.cc
