#!/usr/bin/env bash
# @file bench.sh
# @brief shm vs socket 端到端吞吐对比基准
#
# 绑核 + 多轮取中位数，输出对比表。单轮数字在共享机器上噪声很大
# （大包档可达 2 倍方差），多轮中位数 + 绑核才能给出可复现的结论。
#
# 用法：
#   ./bench.sh               # 3 轮，server 绑核 2 / client 绑核 4
#   ./bench.sh -r 5          # 5 轮
#   ./bench.sh -s 2 -c 4     # 指定 server/client CPU 核
#   ./bench.sh -h            # 帮助

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"

ROUNDS=3
SERVER_CORE=2
CLIENT_CORE=4

# 只打印文件头部的注释块（第一个非注释行即停止），避免泄露函数内的注释
usage() { awk '/^#!/{next} /^#/{sub(/^# ?/,""); print; next} {exit}' "$0"; exit 0; }

while [ $# -gt 0 ]; do
    case "$1" in
        -r|--rounds)       ROUNDS="$2"; shift 2 ;;
        -s|--server-core)  SERVER_CORE="$2"; shift 2 ;;
        -c|--client-core)  CLIENT_CORE="$2"; shift 2 ;;
        -h|--help)         usage ;;
        *) echo "未知参数: $1（-h 查看帮助）" >&2; exit 1 ;;
    esac
done

for bin in server client socket_server socket_client; do
    [ -x "${BUILD_DIR}/${bin}" ] || { echo "缺少 ${bin}，请先运行 ./build.sh" >&2; exit 1; }
done

WORKDIR="$(mktemp -d /tmp/shm-bench.XXXXXX)"
trap 'rm -rf "${WORKDIR}"' EXIT

# 运行一组 bench：$1=tag  $2=server 命令  $3=client 命令
# 每轮从 client 输出提取数据行（payload msg/s MB/s）追加到 ${WORKDIR}/$1.dat
run_group() {
    local tag="$1" server_cmd="$2" client_cmd="$3"
    local i
    for i in $(seq 1 "${ROUNDS}"); do
        rm -f /tmp/fd-pass.socket /tmp/shm-ipc-sock-bench.socket
        # server_cmd/client_cmd 含参数，依赖分词展开
        taskset -c "${SERVER_CORE}" ${server_cmd} > "${WORKDIR}/${tag}_srv.log" 2>&1 &
        local srv_pid=$!
        sleep 0.3
        taskset -c "${CLIENT_CORE}" ${client_cmd} > "${WORKDIR}/${tag}_cli.log" 2>&1
        wait "${srv_pid}"
        grep -E '^[[:space:]]*[0-9]+[[:space:]]+[0-9]+' "${WORKDIR}/${tag}_cli.log" \
            | awk '{print $1, $4, $5}' >> "${WORKDIR}/${tag}.dat"
        echo "  ${tag} 第 ${i}/${ROUNDS} 轮完成"
    done
}

# stdin 数字流 → 中位数
median_of() { sort -n | awk '{a[NR]=$1} END {print a[int((NR+1)/2)]}'; }

# $1=dat 文件  $2=payload  $3=列号（2=msg/s 3=MB/s）→ 该档位中位数
field_median() { awk -v p="$2" -v c="$3" '$1==p {print $c}' "$1" | median_of; }

# payload 字节数 → 可读标签（64B / 1KB / 1MB）
human_size() {
    awk -v p="$1" 'BEGIN {
        if (p >= 1048576) printf "%dMB", p/1048576;
        else if (p >= 1024) printf "%dKB", p/1024;
        else printf "%dB", p;
    }'
}

echo ">>> [1/2] shm bench（${ROUNDS} 轮，server 核 ${SERVER_CORE} / client 核 ${CLIENT_CORE}）"
run_group shm "${BUILD_DIR}/server --bench" "${BUILD_DIR}/client --bench"

echo ">>> [2/2] socket bench（${ROUNDS} 轮）"
run_group sock "${BUILD_DIR}/socket_server" "${BUILD_DIR}/socket_client"

echo
echo "=== 对比结果（${ROUNDS} 轮中位数）==="
echo
printf '%8s %14s %14s %12s %12s %9s\n' \
       "payload" "shm_msg/s" "sock_msg/s" "shm_MB/s" "sock_MB/s" "shm倍率"

awk '{print $1}' "${WORKDIR}/shm.dat" | sort -n | uniq | while read -r p; do
    s_ms=$(field_median "${WORKDIR}/shm.dat"  "$p" 2)
    k_ms=$(field_median "${WORKDIR}/sock.dat" "$p" 2)
    s_mb=$(field_median "${WORKDIR}/shm.dat"  "$p" 3)
    k_mb=$(field_median "${WORKDIR}/sock.dat" "$p" 3)
    ratio=$(awk -v a="${s_mb}" -v b="${k_mb}" 'BEGIN{printf "%.1f", a/b}')
    printf '%8s %14.0f %14.0f %12.1f %12.1f %8sx\n' \
           "$(human_size "$p")" "${s_ms}" "${k_ms}" "${s_mb}" "${k_mb}" "${ratio}"
done

echo
echo "原始日志：${WORKDIR}（退出后自动清理）"
