# SHM 忙轮询接收模式

默认接收方式仍是 eventfd 通知。可在事件循环注册任何 fd、创建 server/client 之前启用忙轮询：

```cpp
shm::EventLoop loop;
const auto ec = loop.SetBusyPolling(true); // 默认每256轮检查一次控制fd
if (ec != shm::ErrorCode::kOk) return ec;
shm::ShmAsyncClient client(&loop);
// 设置codec、回调并Connect，随后loop.Run()
```

服务端同样在创建 `ShmServer` 前配置其 `EventLoop`。两端均启用可测试完整忙轮询；也支持只启用一端，与默认异步客户端/服务端或同步客户端互通。配置、注册和连接操作遵循原有单线程调用契约，配置不得与 Run 并发。

轮询接收方在连接安装时将共享环的 `reader_wait` 设为已有的 awake 状态，并且不再武装睡眠。发送方沿用原来的 `ShouldNotify` 协议，发现对端 awake 就跳过 eventfd 写入。轮询方直接 Peek/Decode/CommitRead，不读取通知fd。对端若仍使用阻塞接收，其等待标志会正常触发通知。共享内存布局和握手协议均未改变，不引入新的等待状态。

事件循环每轮调用存活的轮询条目；默认每256轮以非阻塞 poll 检查 UDS断连、监听socket、握手完成、timerfd和停止fd。`SetBusyPolling(true, N)` 可以配置这个轮数，N必须大于零。轮询条目以通知fd作为生命周期标识，移除和同轮替换沿用墓碑语义；回调收到 `fd=-1, revents=0`，不得尝试读取该fd。轮询回调必须返回 `ErrorCode` 且不得抛异常；非 `kOk` 返回值由 `Run()` 原样返回，退出前清理墓碑并释放运行所有权。

运行期间，即使没有连接或消息，每个事件循环也会持续占用一个逻辑CPU。应绑定两个不同的物理核心，并保证可信SPSC、有限连接和可控处理量。业务消息回调必须及时返回；控制检查间隔以循环轮数计，不是时间上界，当前无限制排空策略仍可能延后定时器/断连处理。这里没有新增实时性保证。

“不使用fd通知”仅指稳定运行后的消息通知路径。握手仍创建/传递eventfd，保留UDS生命周期检测，事件循环仍有控制fd及周期性poll系统调用；连接安装前的发送也可能产生一次通知。不应将其描述为完全无fd或完全无系统调用。

## 测试程序

```sh
cmake -S . -B /tmp/shm-busy-poll -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/shm-busy-poll --target bench_acceptance test_eventloop test_shm_app -j4
/tmp/shm-busy-poll/test_eventloop
/tmp/shm-busy-poll/test_shm_app --busy-server --busy-client
```

基准双方增加 `--busy-poll 1`，例如在两个终端运行：

```sh
taskset -c 1 /tmp/shm-busy-poll/bench_acceptance server /tmp/shm-poll-demo.sock --transport shm --bytes 16 --batch 1 --single-type 1 --busy-poll 1
taskset -c 3 /tmp/shm-busy-poll/bench_acceptance client /tmp/shm-poll-demo.sock --transport shm --bytes 16 --batch 1 --single-type 1 --busy-poll 1 --stride 1 --warmup 1 --seconds 10
```

客户端测量完成自动退出，服务端需发送SIGTERM停止。`--busy-poll 0` 为默认eventfd模式。此选项用于SHM；Socket对照应保留默认模式。

## 错误码契约

`SetBusyPolling` 返回 `kOk` 或 `kInvalidArgument`（已有注册、正在运行或检查间隔为零）；失败不改变配置。

`AddBusyPollFd` 返回 `kOk`、`kInvalidArgument`（未启用轮询、负fd标识或空函数指针）或 `kNoMemory`。注册使用 `new (std::nothrow)` 存储回调节点，无异常转换逻辑，也不通过 `std::function` 或STL容器分配。传入的可调用对象必须支持不抛异常的复制/移动构造，编译期检查；对象自身在调用注册之前的构造由调用者负责。失败不添加条目。库内客户端将注册错误交给连接失败路径，服务端关闭并移除尚未完成安装的连接。
