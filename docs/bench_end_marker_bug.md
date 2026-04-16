# Bench 模式结束标记残留 Bug

## 现象

`client --bench` 和 `server --bench` 进行多轮吞吐量测试时，第一轮（payload=64）正常完成，第二轮（payload=256）起 server 端收到 0 条消息，输出 `rounds=0`。Client 端挂起，等待永远不会到来的 ack。

## 根因

Client 每轮发送 `rounds` 条正常消息 + 1 条结束标记（`seq=-1`）。Server 端原始接收循环：

```cpp
while (received < cmd.rounds)  // rounds = 500000
{
    if (reader.TryRecv(ch, &tag, &payload, &payload_len) == 0)
    {
        ClientMsg msg{};
        shm_ipc::DecodePod<ClientMsg>(payload, payload_len, &msg);
        if (msg.seq == -1)
            break;
        ++received;
    }
}
```

当 `received` 达到 `cmd.rounds`（500000）时，`while` 条件不满足，循环正常退出。**结束标记是第 500001 条消息，未被消费，残留在 ring buffer 中。**

第二轮开始时，server 的 `FrameReader::TryRecv` 立即读到上一轮残留的结束标记，`msg.seq == -1` 触发 break，`received = 0`。

## 时序

```
Round 1:
  Client: write BenchCmd1 → socket
  Client: SendPod × 500000 → ring
  Client: SendPod(seq=-1) → ring        ← 结束标记
  Server: read BenchCmd1 ← socket
  Server: TryRecv × 500000              ← 循环条件 received < 500000 退出
  Server: 结束标记未消费，残留在 ring
  Server: write ack → socket
  Client: read ack ← socket

Round 2:
  Client: write BenchCmd2 → socket
  Server: read BenchCmd2 ← socket
  Server: TryRecv → 读到 Round 1 残留的 seq=-1 → break
  Server: received = 0, 输出异常结果
  Server: write ack → socket
  Client: 开始 SendPod，但 server 已进入下一轮/退出
```

## 修复

将接收循环改为「收到结束标记才退出」，不依赖计数：

```cpp
bool got_end = false;
while (!got_end)
{
    if (reader.TryRecv(ch, &tag, &payload, &payload_len) == 0)
    {
        ClientMsg msg{};
        shm_ipc::DecodePod<ClientMsg>(payload, payload_len, &msg);
        if (msg.seq == -1)
        {
            got_end = true;
            break;
        }
        ++received;
    }
}
```

这确保每轮结束标记一定被消费，不会泄漏到下一轮。

## 教训

在生产者-消费者协议中，如果使用带外终止标记（sentinel），消费者的退出条件必须以标记为准，不能仅依赖预期消息数量。否则标记会残留在缓冲区中，污染后续会话。
