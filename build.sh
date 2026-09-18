#!/usr/bin/env bash
# @file build.sh
# @brief shm-cpp 一键构建脚本
#
# 用法：
#   ./build.sh                 # Release 构建（默认）
#   ./build.sh -t Debug        # Debug 构建
#   ./build.sh -c              # 清理后重新构建
#   ./build.sh --test          # 构建并运行全部测试
#   ./build.sh -j 4            # 指定并行编译任务数
#   ./build.sh -h              # 帮助

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"

BUILD_TYPE=Release
CLEAN=0
RUN_TEST=0
JOBS="$(nproc 2>/dev/null || echo 4)"

usage() {
    grep '^#' "$0" | grep -v '^#!' | sed 's/^# \{0,1\}//'
    exit 0
}

while [ $# -gt 0 ]; do
    case "$1" in
        -t|--type)   BUILD_TYPE="$2"; shift 2 ;;
        -c|--clean)  CLEAN=1; shift ;;
        --test)      RUN_TEST=1; shift ;;
        -j)          JOBS="$2"; shift 2 ;;
        -h|--help)   usage ;;
        *) echo "未知参数: $1（-h 查看帮助）" >&2; exit 1 ;;
    esac
done

if [ "${CLEAN}" -eq 1 ]; then
    echo ">>> 清理 ${BUILD_DIR}"
    rm -rf "${BUILD_DIR}"
fi

mkdir -p "${BUILD_DIR}"

# 调用者可能正位于刚被清理的 build/ 目录中，cwd 失效会导致 cmake 报错
cd "${SCRIPT_DIR}"

echo ">>> 配置 (CMAKE_BUILD_TYPE=${BUILD_TYPE})"
cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" \
      -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" -Wno-dev

echo ">>> 编译 (-j${JOBS})"
cmake --build "${BUILD_DIR}" -j "${JOBS}"

if [ "${RUN_TEST}" -eq 1 ]; then
    echo ">>> 运行测试"
    # protobuf 存在时才生成 proto 测试目标
    TESTS=(test_codec test_ringbuf_wrap test_ringbuf_boundary test_eventloop)
    [ -x "${BUILD_DIR}/test_proto_codec" ] && TESTS+=(test_proto_codec)
    [ -x "${BUILD_DIR}/test_proto_wrap" ]  && TESTS+=(test_proto_wrap)

    for t in "${TESTS[@]}"; do
        echo "--- ${t} ---"
        # 成功时丢弃输出（死亡测试子进程会打印预期的 assert 信息），
        # 失败时回显完整输出便于诊断
        if out="$("${BUILD_DIR}/${t}" 2>&1)"; then
            echo "✔ ${t}"
        else
            echo "✘ ${t} 失败：" >&2
            echo "${out}" >&2
            exit 1
        fi
    done
    echo ">>> 全部测试通过"
fi

echo ">>> 完成，产物位于 ${BUILD_DIR}"
