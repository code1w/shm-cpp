# 核心代码第三轮审核（2026-11-21）

当前调用契约更新：用户选择不支持codec内的同线程连接管理/收发重入，也不支持执行中的回调自替换。ShmConnection热路径已移除为这些用法建立的连接/codec/回调强引用快照及编码缓冲所有权转移；Debug检查本连接的常见违约并assert，Release依赖契约。codec及配置只在收发操作之外修改，调用方在整个操作期间保持连接有效；普通消息回调回复和Close由owner已有强引用安全收尾。下文原UAF复现属于当前契约之外的用法，不能将历史修复描述理解为当前仍支持任意重入。


修复状态：以下三项问题均已修复，下文保留修复前审核记录。解码与心跳调用保活连接和 codec；Decode 内更换 codec 时关闭连接，不投递或重新解析当前帧。心跳和普通 SendCmd 共享 EncodeGuard，借用缓冲独立保活，禁止编码重入，并在用户调用返回后检查连接/codec 状态。listen 失败时按 bind 后记录的节点身份清理路径，并保留原始错误码。

新增 `test/test_audit_round3_fixes.cpp`，覆盖 Decode 替换/清空 codec、关闭连接、心跳扩容与发送重入、心跳关闭/替换/异常、启动失败重试和其他服务入口保护。独立 Debug 构建的 14 个测试全部通过；同样 14 个测试在 ASan/UBSan 与泄漏检查下全部通过，无 sanitizer 报告。构建无 warning/error，本次改动的核心文件与 CMake 通过 diff 空白检查。测试结果仅证明所覆盖行为，不代表完整工业认证。


审核对象：当前工作区，包含已有未提交修改；前两轮报告中已修复的问题不计入本轮发现。初次审核时仅新增报告；后续按用户要求修改实现并补充回归。

结论：发现两个 P1 内存安全问题和一个 P2 启动失败恢复问题，均经独立探针复现。修复前不能按工业级交付标准放行。P1 表示需优先修复的释放后使用；本轮未证明代码执行或远程利用能力。

## 1. P1：Decode 重入 SetCodec 销毁正在执行的 codec

位置：`include/shm_ipc/shm_connection.hpp:563`；`SetCodec` 直接替换成员 shared_ptr。

DrainRingOnce 通过 `codec_->Decode(...)` 调用用户虚函数，没有复制 codec 的强引用。当连接是 codec 的唯一所有者，Decode 在同一线程重入 `conn->SetCodec(new_codec)`，旧 codec 立即析构。Decode 随后访问自身成员即为释放后使用。保持连接存活不能保护被替换的 codec。

独立探针只使用正常 socketpair/fd 握手、一个输入字节和公开 SetCodec 方法：

```cpp
int32_t Decode(const comm::byte*, size_t, std::any& pkg, size_t*) override {
    conn->SetCodec(std::make_shared<comm::codec::Codec>());
    pkg = value;  // value 是旧 codec 的成员；此处已经释放
    return 1;
}
```

ASan/UBSan 构建检出 `AddressSanitizer: heap-use-after-free`，调用链包含 Probe::Decode、DrainRingOnce（563）、DrainRing（459）。不依赖多线程竞争、不依赖恶意共享内存修改，也未违反 SPSC 的单读者约束。

建议：在每次用户 codec 调用前保活 codec，并在返回后复查 codec 身份及连接状态。明确当前帧的提交与替换后的处理语义，避免旧调用返回后继续使用已关闭连接或空 codec。SendKeepAlive 等虚函数也应纳入同一规则。回归至少覆盖 Decode 替换唯一持有的 codec、设为空、关闭连接及异常退出。

## 2. P1：心跳编码重入 ResizeBuffer 使借用缓冲失效

位置：`include/shm_ipc/shm_connection.hpp:732`；心跳路径由 `DrainRingOnce:596` 调用。

SendKeepaliveResponse 直接把 `encode_buf_.data()` 传入用户 SendKeepAlive。SendKeepAlive 在同一线程调用 ResizeBuffer 扩容，vector 重分配并释放旧内存；随后向传入的 buf 写一个字节即发生释放后使用。

复现 codec 的 Decode 返回心跳标志（-2），*out=1；开启 SetKeepAlive(true)：

```cpp
int32_t SendKeepAlive(comm::byte* buf, size_t, size_t*) override {
    conn->ResizeBuffer(1024 * 1024, 1024 * 1024);
    buf[0] = 42;
    return 1;
}
```

ASan 检出 `heap-use-after-free / WRITE of size 1`；调用链是 Probe::SendKeepAlive → SendKeepaliveResponse（732）→ DrainRingOnce（596）。借用缓冲初始为 128 KiB，扩到 1 MiB 时释放。

普通 SendCmd 已采用独立 working_buffer 与发送作用域保护，但心跳路径未采用。心跳函数也没有在用户回调返回后复查连接状态，且直接通过成员 codec 调用，存在与第一项相同的生命周期缺口。

建议：将普通消息与心跳编码收敛到共同的借用缓冲保活、codec 保活、重入保护和返回后状态检查机制。回归覆盖心跳内 ResizeBuffer、SendCmd、SetCodec 与 Close；断言不发生内存错误，也不向已经关闭的连接发布响应。

## 3. P2：bind 成功、listen 失败后遗留 socket 路径，阻止重试

位置：`include/shm_ipc/common.hpp:813`、`:819`；ShmServer::Listen 在 ListenUnixSocket 成功返回后才记录路径所有权。

bind 已创建文件系统 socket 节点，listen 若失败，局部 UniqueFd 只关闭 fd，未删除刚创建的路径。调用者收到失败，没有取得 fd，也没有建立路径所有权；Stop 因尚未 started 而直接返回。临时资源故障恢复后，再次 Start 仍在 bind 处失败。

用链接参数 `-Wl,--wrap=listen` 仅为第一次 listen 注入 ENOMEM，随后调用真正的 listen；同一 server 连续 Start 两次，结果：

```text
first=0 errno=12 retry=0 errno=98 path_exists=1
```

12 为 ENOMEM，98 为 EADDRINUSE。探针退出前自行清理唯一的临时路径。此问题需要 listen 失败这一低频条件，因此列 P2；不应把它误报为正常启动必现故障。

建议：在 bind 成功后立即建立路径回滚守卫，只有监听建立并移交所有权后解除；删除前校验节点身份，保持项目已有的“不替换其他服务入口”约束。回归验证 bind 后 listen 失败、重试成功，以及不误删替换后的其他节点。

## 验证与边界

- 独立 Debug 构建：`/tmp/shm-industrial-review`，构建成功，日志未出现 warning/error。
- 13 个现有测试程序全部退出 0（包含 protobuf 两项及六个 audit 程序）；assert 保持启用，单程序超时 45 秒。
- 两项内存安全探针使用 `-fsanitize=address,undefined -fno-omit-frame-pointer`，分别检出上述 UAF；现有测试通过不覆盖这些新触发条件。
- 探针源码：`/tmp/shm-industrial-probe.cpp`、`/tmp/shm-industrial-listen.cpp`；日志：`/tmp/shm-industrial-probe-1.log`、`/tmp/shm-industrial-probe-2.log`。临时目录不是持久归档，关键触发代码与观测结果已记录于本报告。

静态审核重点包括环游标/边界、批次提交、镜像映射与尺寸封印、fd 交换与期限、通知、事件循环、服务端握手、连接编解码及重入生命周期。本轮没有完成全部 syscall 故障注入、长期负载/资源耗尽压力测试或 ARM 等目标上的跨进程内存序验证；因此不能将未列出问题的模块视为已通过完整认证。现有 SPSC 和 loop 单线程契约是分析前提；本轮三项发现均不要求违反这些契约。
