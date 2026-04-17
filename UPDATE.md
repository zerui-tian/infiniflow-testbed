# InfiniFlow流控机制

## 总览

当前Sender Reciever Switch需要添加一种新的流控机制，InfiniFlow。区别于之前的无流控机制和CBFC机制。


## 算法机制
InfiniFlow 通过以下两个机制解耦 VC 数量与缓冲需求：

1. **UABD（Upstream Allocates Buffer for Downstream）**：上游端口代替下游端口
   主动管理共享缓冲，维护一个 per-port 信用池（credit_pool）。
2. **BUCP（Buffer Usage Control Protocol）**：通过实时积压反馈动态调整每个 VC
   的信用阈值（threshold），防止拥塞 VC 垄断共享缓冲。

### 关键数学关系

- 上游的信用池：`credit_pool = FCCL - FCTBS`
  其中 `FCCL = 端口累计接收量 + Buffer剩余空间`（下游计算，通过 Feedback 回传获取）
- 在途字节：`inflight_pkgs[i] = VC_TX[i] - VC_DR[i]`
  其中`VC_DR[i]` 代表VC i的下游累计已排出（drained）数据包个数，通过Feedback回传给上游。
- 下游积压：`VC_BKLG[i] = VC_RX[i] - VC_DR[i]`
  其中`VC_RX[i]`代表VC i累计接收数据包个数，`VC_BKLG[i]`通过Feedback回传给上游。
- 调度条件（必须同时满足）：
  - Per-Port 约束：`pkt.len <= credit_pool`
  - Per-VC 约束：`pkt.len < threshold[i] - inflight_pkgs[i]`
    其中`threshold[i]`代表VC i动态信用上限

### 阈值调整逻辑

```python
if (feedback.VC_BKLG >= qmax):              # 拥塞抑制
    threshold[i] = max(PACKET_SIZE,
                       inflight_pkgs[i] - (feedback.VC_BKLG - qmin))
    state[i] = TA
elif (feedback.VC_BKLG < qmin):            # 吞吐提升
    threshold[i] = min(下游剩余共享缓冲大小,
                       threshold[i] + (qmin - feedback.VC_BKLG))
    state[i] = TA
```
这里的qmax和qmin是超参数，分别代表最大容忍积压量和目标最小积压量。
这里的PACKET_SIZE是数据包大小。

### 振荡避免 FSM（精确实现，不得简化）

上游 FSM（3 状态）：
- `UN`（Upstream Normal）→ `TA`（Threshold Adjusting）：触发阈值调整时
- `TA` → `WT`（Feedback Waiting）：发送 TA 标记数据包后
- `WT` → `UN`：收到下游 TA 标记 feedback 后（解锁）

下游 FSM（2 状态）：
- `DN`（Downstream Normal）→ `FM`（Feedback Marking）：收到 TA 标记数据包时
- `FM` → `DN`：发出 TA 标记 feedback 后

**锁定语义**：仅在 `state[i] == UN` 时才允许执行阈值调整逻辑。

## 字段定义

可以扩充fc_data_header_s来添加新的字段，比如TA标记。
将InfiniFlow使用的Feedback和CBFC Feedback进行区分，另起一个数据结果来定义所需要的字段。

## 初始化与配置

qmin、qmax、初始 threshold[i]、port缓冲大小，需要新增 CLI 参数/脚本参数。




