# SHM 7 天稳定性测试

专用入口 `run_soak.py` 持续运行两个真实进程，使用 ShmServer/ShmAsyncClient、SimplePodCodec 和 EventLoop。默认为 16B、batch256 满速 echo，服务端 CPU2、客户端 CPU4。7 天是正式测量 604800 秒，另有默认5秒预热及启动/清理时间。

资源采样直接写 CSV，日志只读取末尾16KiB，直方图由 benchmark 使用固定内存；监控内存不随天数累积。默认每10秒采样RSS、匿名/共享RSS、fd、memfd映射、线程数，每60秒打印控制进度。状态文件每次资源采样更新。

## 构建和后台启动

在仓库根目录可直接执行启动脚本：

```sh
./start_soak7d.sh
```

脚本完成Release构建和nohup后台启动，并打印结果目录、状态/日志/停止命令。支持从任意目录通过绝对路径调用。可透传负载选项，例如 `./start_soak7d.sh --seconds 60` 做短跑，或 `./start_soak7d.sh --bytes 1048576 --batch 4`。`SOAK_OUTPUT_DIR` 可指定新结果目录，`SOAK_BUILD_JOBS` 可设置编译并行度。`--help` 只显示帮助。

等价的手工启动命令，在仓库根目录执行：

```sh
cmake -S . -B build/soak7d -DCMAKE_BUILD_TYPE=Release \
  "-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG" -DSHM_IPC_SANITIZE=
cmake --build build/soak7d --target bench_acceptance -j4

SOAK_DIR="$PWD/bench/results/shm_soak_7d_$(date +%Y%m%d_%H%M%S)"
nohup python3 -u bench/run_soak.py "$PWD/build/soak7d/bench_acceptance" \
  "$SOAK_DIR" --seconds 604800 --batch 256 --bytes 16 \
  --server-cpu 2 --client-cpu 4 --sample-interval 10 \
  > "${SOAK_DIR}.console.log" 2>&1 &
printf '结果目录：%s\n' "$SOAK_DIR"
```

保存打印的结果目录。脚本要求新目录，避免覆盖原有结果。nohup 可脱离SSH会话；机器重启会中断运行，`--status` 会检查控制进程是否仍存在。测试占用两个指定核心，推荐在计划的生产硬件上运行并保持工作负载/电源策略记录。预计日志为数百MB量级，建议预留至少1GB。

## 看进度和停止

同一shell可直接用变量；重新登录后把SOAK_DIR设为实际结果目录：

```sh
python3 bench/run_soak.py --status "$SOAK_DIR"
tail -f "${SOAK_DIR}.console.log"
tail -f "$SOAK_DIR/client.jsonl"
```

手动停止：

```sh
python3 bench/run_soak.py --stop "$SOAK_DIR"
```

停止命令验证控制进程的PID、启动时间和boot_id，防止对复用PID发送信号。控制程序先停止客户端，再停止服务端，必要时超时终止自己的子进程组，记录forced_kills。手动停止标为interrupted，不会把未跑满7天的结果标成passed。

## 结果判读

- `status.json`：running、passed、failed或interrupted，包含退出码和原因；`--status`另检测控制进程缺失。
- `result.json`：正常完成后的正式测量回复数、RPC/s、采样RTT的P50/P99/P99.9和最大值。
- `resources.csv`：持续资源变化，重点比较预热后RSS、fd、映射和线程是否累积增长。
- `server.jsonl` / `client.jsonl`：每秒应用进度；`*.stderr`保留错误。
- `pids.json` / `*.command.json` / `binary.sha256`：进程身份、配置和实际可执行文件指纹。

连接异常、进程异常退出或默认60秒无回复进展即判失败并保留日志。正式运行满时长、客户端正常结束、两端fd及映射回到基线、服务端建立/关闭数量匹配，才标为passed。测试不自动重连来掩盖连接故障；故障注入和重连验收由run_acceptance.py单独执行。

RTT是closed-loop已经发送请求的往返时延，发送前上游排队不在统计内；7天累计分位数不能替代每日/每窗口尾延迟。每秒回复日志可计算窗口吞吐，资源CSV可计算增长趋势。通过7天是针对该配置和该机器的证据，不表示覆盖所有业务负载。

其他负载另开结果目录，例如4KB batch64加`--bytes 4096 --batch 64`，1MB batch4加`--bytes 1048576 --batch 4`。CPU编号按实际机器调整，不同物理核心/NUMA拓扑需记录。

## 本轮入口验证

已做短时正常完成、人工停止、杀死服务端导致失败、状态查询验证。它们验证启动/采样/收尾/错误分类，不意味着本轮已实际跑完7天。
