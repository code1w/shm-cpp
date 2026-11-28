# EventLoop 注册身份与 RAII 句柄

`EventLoop::Registration` 管理一次监听的生命周期。它持有 EventLoop 地址和本次注册 ID，不以裸 fd 号码作为取消身份。

## 普通 fd

```cpp
shm::EventLoop loop;
shm::UniqueFd socket; // 由应用完成创建/连接
shm::EventLoop::Registration watch;

const auto ec = loop.WatchFd(
    socket.Get(),
    [](int fd, short events) { /* 在 owner 线程处理事件 */ },
    &watch);
if (ec != shm::ErrorCode::kOk) {
    return ec;
}

watch.Reset(); // 仅取消这一项监听，不关闭 socket
```

句柄不可复制，可以移动。`Reset()` 幂等；析构自动 Reset。成功替换输出句柄会取消旧注册；参数或分配失败保持输出句柄原状。

`Id()` 返回句柄保存的注册身份，0 表示空句柄。外部取消或一次性到期后，句柄可以仍保存旧 ID；`operator bool()` 检查注册是否仍活跃，`Fd()` 在不活跃时返回 -1。最后 Reset 清空句柄。

fd 可以复用，而注册 ID 在正常调用范围内不复用：旧监听 ID=100 的延迟取消，不会影响同一 fd 号码上的新监听 ID=101。

```cpp
struct ConnectionResources {
    shm::UniqueFd socket;
    shm::EventLoop::Registration socket_watch;
};
```

成员逆序析构，因此先取消监听、再关闭 fd。主动关闭也应使用这个顺序。普通 WatchFd/WatchBusyFd 都不接管 fd 所有权。

## 定时器

```cpp
shm::EventLoop::Registration timeout;
const auto ec = loop.WatchOneShotTimer(
    1000, [](int fd, short events) { /* 到期处理 */ }, &timeout);
if (ec != shm::ErrorCode::kOk) {
    return ec;
}
// 在到期之前取消：同时取消监听并关闭 timerfd。
timeout.Reset();
```

- `WatchTimer(interval_ms, callback, out)` 创建周期定时器，interval_ms 必须大于 0；回调仍需调用 `DrainTimerfd(fd)`。
- `WatchOneShotTimer(delay_ms, callback, out)` 创建一次性定时器；非正 delay_ms 按立即触发处理，包装层自动排空计数。
- 两个接口的句柄取消同时释放该定时器 fd，调用方不能再自行 close 它。
- 一次性定时器在调用业务回调前标记失效；回调内收到的 fd 仍有效，直到回调返回后关闭。
- 回调内 Reset 自己的定时器也不会提前关闭当前回调的 fd。
- 回调内可以在同一个句柄上安装新的定时器。旧回调的收尾只操作旧节点，不取消或关闭新的定时器。

## Busy 模式

`WatchBusyFd(fd, callback, out)` 要求预先启用 busy polling，回调签名为 `ErrorCode(int, short)`，遵循既有 busy 回调契约。回调收到的是 `(-1, 0)`，fd 参数只是登记和兼容 RemoveFd 的标识；Reset 不关闭它。

## 线程、失败与分发契约

- EventLoop 必须比关联的非空句柄活得更久；Watch、Reset、状态查询与资源关闭都在 owner 线程执行，Stop 保留原有跨线程语义。
- Watch 接口返回 ErrorCode。回调直接存入一次 `new(std::nothrow)` 分配的节点，注册失败返回 kNoMemory，不通过捕获异常实现失败转换。
- 回调类型必须能不抛异常地构造、析构。移动已有 std::function 可避免在接口内部复制它；调用方在创建回调对象之前发生的分配不属于注册接口。
- 普通 fd/定时器回调返回 void；busy 回调返回 ErrorCode。回调执行沿用各自的原有约定，Registration 不触发连接的业务关闭回调。
- 普通 fd 的新注册在下一轮 poll 生效，busy 的新注册在下一轮 spin 生效。取消立即打墓碑，本轮尚未执行的旧事件会被跳过；活动分发期间延迟回收回调存储。
- 新旧注册 API 混用时，普通事件仍按注册先后顺序构建快照。Run 的独占、Stop、半帧等待和消息收发协议没有改变。
- 旧 AddFd、RemoveFd、AddTimer、RemoveTimer 等保留兼容。裸 fd 的 RemoveFd 仍取消调用时所有匹配监听；需要防止延迟取消误伤复用 fd 时，应持有 Registration。

## 当前接入范围

ShmConnection 内部持有数据通知、断连检测和验证超时的注册句柄；ShmServer 持有监听与握手完成通知的句柄。Server/AsyncClient 通过连接的 WatchEvents 安装两项监听，第二项失败时回滚第一项，成功后才触发建连回调。

连接移除、AsyncClient 析构和服务端 Stop 都先取消注册。验证成功、拒绝、关闭、析构都通过验证句柄取消超时，避免用旧 timerfd 号码取消新的资源。

这轮只改监听生命周期：不增加 codec 层的锁、逐消息分配或 shared_ptr 快照，不改变 eventfd 通知协议，也不引入读取预算。

## 验证

```sh
cmake --build build --target test_registration test_registration_app test_eventloop test_shm_app -j3
./build/test_registration
./build/test_registration_app
./build/test_eventloop
./build/test_shm_app
./build/test_shm_app --busy-server --busy-client
```

新增测试包括句柄移动/析构、借用 fd 不被关闭、nothrow 分配失败回滚、旧句柄与 fd/timerfd 复用、混合 API 顺序、同轮取消、定时器回调内取消/替换、busy 取消和错误返回，以及服务端/客户端两项注册失败后的恢复。
