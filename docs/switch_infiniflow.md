# Switch 侧 InfiniFlow 机制说明

本文描述本仓库中 **switch** 在 `FC_MODE_INFINIFLOW` 下如何配合数据报文与反馈报文完成基于 **端口共享信用 + VC 动态阈值** 的流控（实现见 `src/switch/`）。

## 1. 角色与报文类型

- **数据面**：识别以太网类型 `FC_ETHER_TYPE`（`0x88B5`）的 FC 数据报文，载荷头中含 `flow_id`、`vc_id`、`flags`（`fc_data_header_t`，见 `src/core/fc_header.h`）。
- **反馈面**：识别 `INFINIFLOW_FEEDBACK_ETHER_TYPE`（`0x88B7`）的 InfiniFlow 反馈报文，载荷为 `vc_id`、`flags`、`vc_dr`、`vc_bklg`、`fccl`（`infiniflow_feedback_header_t`）。
- **路由面**：通过 `route_csv` 把 `flow_id` 映射到某个 egress 端口（`route_table.c`），决定每个数据包进入哪个 `(egress, vc)` 队列。

Switch 在 InfiniFlow 模式下同时承担：

1. **从 ingress 收 FC 数据并进入 egress VC 队列**；
2. **按 egress 端口共享信用和 VC 阈值决定是否可继续向下游发送**；
3. **把本地 ingress 侧的 `fccl/vc_dr/vc_bklg/flags` 经原 ingress 口反馈回上游**；
4. **在 egress 口接收下游反馈，更新本地端口信用和 VC 阈值状态**；
5. **在下游方向通过 TA 标志传播阈值变化，并在上游方向对收到的 TA 做回执**。

## 2. 关键状态

InfiniFlow 在 switch 中同时维护 **ingress 侧共享缓存状态** 和 **egress 侧发送控制状态**。

### 2.1 ingress 侧状态

#### `switch_ingress_port_state_t`

每个 ingress 口维护一份共享缓冲状态：

| 字段 | 含义 |
|------|------|
| `occupancy` | 该 ingress 口当前已占用的共享缓存包数。InfiniFlow 入向准入按它与 `port_buffer_pkts` 比较。 |
| `total_received` | 该 ingress 口累计成功入队的包数。 |
| `total_drained` | 该 ingress 口累计从 switch 内部被“释放信用”的包数。 |

#### `switch_ingress_vc_state_t`

每个 `(ingress, vc)` 维护一份 VC 局部状态：

| 字段 | 含义 |
|------|------|
| `occupancy` | 该 ingress 的该 VC 当前在 switch 内尚未释放信用的包数。 |
| `total_received` | 该 `(ingress, vc)` 累计成功入队的包数。 |
| `total_drained` | 该 `(ingress, vc)` 累计被释放信用的包数，作为反馈里的 `vc_dr`。 |
| `state` | ingress 方向的 TA 账本状态，代码中只在收到带 TA 的数据包时置 1，在反馈发回时清 0。 |
| `pending_feedback_flags` | 待随下一次反馈报文回传给上游的标志位；当前主要是 `INFINIFLOW_FEEDBACK_FLAG_TA`。 |

说明：结构里还有 `capacity` 字段，但在 `FC_MODE_INFINIFLOW` 下，实际入向门控使用的是 **端口级共享缓存** `port_buffer_pkts`，而不是该 VC 的固定容量。

### 2.2 egress 侧状态

#### `switch_egress_port_state_t`

每个 egress 口维护一份端口级发送信用状态：

| 字段 | 含义 |
|------|------|
| `fccl` | 下游反馈给该 egress 口的累计信用上限。初值为配置项 `initial_fccl`。 |
| `fctbs` | 本 egress 口累计成功发送的数据包数。 |

#### `switch_egress_vc_state_t`

每个 `(egress, vc)` 维护一份 VC 发送控制状态：

| 字段 | 含义 |
|------|------|
| `tx_pkts` | 本 switch 在该 `(egress, vc)` 上累计成功发送的包数。 |
| `vc_dr` | 下游反馈的该 VC 已 drain 计数。 |
| `vc_bklg` | 下游反馈的该 VC 当前 backlog。 |
| `threshold` | 本地对该 VC 的动态发送阈值，初值为 `initial_threshold`。 |
| `state` | egress 方向的 TA 状态。按实现可理解为：`0=稳定态`、`1=阈值已变化，等待在下一包上带 TA`、`2=TA 已发出，等待下游反馈回执`。 |

虽然结构里也保留了 `fccl/fctbs` 字段，但 InfiniFlow 的实际发送控制使用的是 **egress 端口级** `fccl/fctbs`，以及 **VC 级** `tx_pkts/vc_dr/vc_bklg/threshold/state`。

## 3. 入向路径：scheduler（`scheduler.c`）

每个 ingress 口对应一个 `switch_scheduler_run_tick` 循环，职责是 **收包、做共享缓存准入、按路由分流到 egress VC 队列**。

### 3.1 收包与基础校验

调度器从 ingress 口 RX 队列收包后：

1. 仅接受 `FC_ETHER_TYPE` 数据报；
2. 校验长度至少覆盖以太网头和 `fc_data_header_t`；
3. 解析 `vc_id`、`flow_id`、`flags`；
4. `vc_id` 越界则丢包；
5. 用 `flow_id` 查 `route_csv`，若找不到合法 egress 端口则丢包。

### 3.2 共享缓存准入

在 `FC_MODE_INFINIFLOW` 下，`try_reserve_ingress_slot` 不看 VC 固定容量，而是：

- 读取该 ingress 口的 `port_state->occupancy`；
- 仅当 `occupancy < port_buffer_pkts` 时，CAS 把 `occupancy` 加 1；
- 成功后再把该 `(ingress, vc)` 的 `vc_state->occupancy` 加 1；
- 若共享缓存已满，则直接丢包并记 `drop_capacity_pkts`。

因此，InfiniFlow 的 ingress 侧本质上是 **一个端口级共享 buffer**，VC 只记录自己占了多少，不单独决定是否还能继续收。

### 3.3 TA 输入处理

若收到的数据包 `flags` 中带有 `FC_DATA_FLAG_TA`：

- 将 ingress 侧该 VC 的 `state` 置为 1；
- 把 `pending_feedback_flags` OR 上 `INFINIFLOW_FEEDBACK_FLAG_TA`。

这表示：**上游发送过来的 TA 需要被 switch 通过反馈回传给本 ingress 对应的上游发送者**。

### 3.4 入 egress VC 队列

路由确定后，数据包会被送入目标 `(egress, vc)` 对应的无锁环：

- 队列索引为 `switch_egress_vc_state_index(ctx, egress_idx, vc_id)`；
- 若入环失败，会回滚前面预留的 `vc_state->occupancy` 和 `port_state->occupancy`；
- 成功后累计 `total_received`，其中 InfiniFlow 还会同步增加 ingress 口的 `total_received`。

到这里，数据已经从 “某个 ingress 的接收视角” 转化为 “某个 egress 的某个 VC 等待发送”。

## 4. 出向路径：forward（`forward.c` + `main.c` 中 `infiniflow_*`）

每个 egress 口对应一个 `switch_forward_run_tick` 循环，按 VC 轮询执行。

### 4.1 发送门控：端口信用池 + VC 阈值双重限制

在真正 `dequeue` 前，若 `fc_mode != none` 且已注册 `fc_ops`，会调用 `infiniflow_calc_deq_limit` 计算本次该 VC 最多允许发送多少包：

- **端口级信用池**
  - `credit_pool = max(port_fccl - port_fctbs, 0)`
  - 其中 `port_fccl` 来自下游反馈，`port_fctbs` 是本 egress 口已发送总数
- **VC 级信用**
  - `inflight = max(tx_pkts - vc_dr, 0)`
  - `vc_credit = max(threshold - inflight, 0)`
  - 其中 `tx_pkts` 是本 switch 在该 VC 上已发出的包数，`vc_dr` 是下游声称已 drain 的包数

最终：

```text
deq_limit = min(tx_burst_size, credit_pool, vc_credit)
```

只要 **端口级没有共享 headroom**，或者 **该 VC 自己的 inflight 已达到 threshold**，这个 VC 在本 tick 就不会被继续发出。

### 4.2 出队、改写 MAC、选择候选发送包

当 `deq_limit > 0` 时，forward 从 `(egress, vc)` 的环上批量出队。

对每个出队包：

- 记录原始 `src_mac`，后续反馈要发回这个地址；
- 把以太网 **源 MAC** 改写成该 egress 口 MAC；
- 不改动 `dst_mac`；
- 记录该包最初来自哪个 ingress 口（通过 `mbuf->port` 反查 `ingress_idx`）。

### 4.3 TA 下发：只在该批次首包上打标

对一个 VC 的本轮候选发送包，只有 **第一个候选包** 会调用 `infiniflow_on_tx_prepare`：

- 若 `egress_vc_state.state == 1`，则给该包的 `fc_data_header.flags` OR 上 `FC_DATA_FLAG_TA`。

这表示：**本地阈值刚发生变化，需要把这一事实沿数据方向通知下游**。实现上并不会给整批所有包都打 TA，只 piggyback 在该 VC 本次首个可发数据包上。

### 4.4 发送成功后的状态推进

`rte_eth_tx_burst` 返回成功发送数量 `n_tx` 后：

1. 调用 `infiniflow_on_tx_success`：
   - `egress_port_state.fctbs += n_tx`
   - `egress_vc_state.tx_pkts += n_tx`
   - 如果这批首包带了 TA，则把 `egress_vc_state.state` 置为 2，表示 **TA 已发出，等待下游反馈确认**
2. 对每个成功发出的包，都会调用 `release_ingress_credit`，把它在原 ingress 侧占用的共享缓存/VC 占用释放掉，并生成一条反馈消息发回上游。

### 4.5 释放 ingress 信用并生成回上游反馈

`release_ingress_credit` 是 switch 里连接 “forward 成功发送” 和 “给上游反馈” 的核心函数。对每个已成功转出的包：

1. 将原 ingress 的该 VC `occupancy` 减 1；
2. 将原 ingress 口共享缓存 `occupancy` 减 1；
3. `vc_state->total_drained` 加 1，并把结果作为反馈中的 `vc_dr`；
4. 把新的 VC 当前占用作为反馈中的 `vc_bklg`；
5. 取出并清空 `pending_feedback_flags` 作为反馈中的 `flags`；
6. 若这次带回了 `INFINIFLOW_FEEDBACK_FLAG_TA`，把 ingress 侧该 VC `state` 清 0；
7. 计算回给上游的 `fccl`：

```text
fccl = ingress_port.total_received + max(port_buffer_pkts - port_occupancy, 0)
```

这里的 `fccl` 不是 VC 私有窗口，而是 **该 ingress 口共享缓存视角下的累计信用指示**：

- `total_received` 表示该 ingress 已经被 switch 接纳过多少包；
- `port_buffer_pkts - port_occupancy` 表示当前共享缓存还剩多少空位。

二者相加后，发送者可据此推导自己的端口级 headroom。

### 4.6 发送失败和异常路径

- 若出队包长度异常，会直接释放 ingress 信用并给上游发反馈，然后丢弃 mbuf。
- 若 `tx_burst` 没把某个候选包发出去，代码会优先尝试把它重新塞回 egress VC 环。
  - **回塞成功**：包仍在 switch 内缓存中，不释放 ingress 信用，也不回上游反馈。
  - **回塞失败**：视为彻底丢失，释放 ingress 信用、回发反馈，并释放 mbuf。

这保证了：只有包真正离开 switch，或者在 switch 内已无法继续保留时，ingress 侧占用才会被释放。

## 5. 向上游发反馈：`feedback_gen.c`

每个 ingress 对应一个 `switch_feedback_gen_run_tick` 循环，从该 ingress 自己的反馈队列中取出消息并封装成 InfiniFlow 反馈报文。

封装格式如下：

- `ether_type = INFINIFLOW_FEEDBACK_ETHER_TYPE (0x88B7)`
- `vc_id = msg->vc_id`
- `flags = msg->flags`
- `vc_dr = msg->vc_dr`
- `vc_bklg = msg->vc_bklg`
- `fccl = msg->fccl`
- 目的 MAC = 原始数据包的 `src_mac`
- 源 MAC = 当前 ingress 口 MAC

反馈报文通过 **同一个 ingress 口的 TX 队列** 发回上游。

## 6. 接收下游反馈：`feedback_handler.c`

`switch_feedback_handler_run_tick` 在一个独立 worker 上轮询所有 egress 口的 RX 队列。

对 InfiniFlow 反馈：

1. 识别 `0x88B7`；
2. 解析 `vc_id`、`flags`、`vc_dr`、`vc_bklg`、`fccl`；
3. 调用 `infiniflow_on_feedback_rx` 更新本地 egress 侧流控状态；
4. 释放 mbuf。

这条路径代表：**下游告诉 switch 当前端口还有多少共享 headroom、某个 VC 已 drain 到哪里、某个 VC 当前 backlog 多大，以及这次反馈是否在确认 TA**。

## 7. 阈值更新与 TA 握手（`infiniflow_on_feedback_rx`）

InfiniFlow 的核心不是固定信用，而是根据下游 backlog 动态调整 `threshold`。`infiniflow_on_feedback_rx` 的逻辑可以拆成三步。

### 7.1 先同步下游观测值

每次收到下游反馈，先无条件写入：

- `egress_port_state.fccl = fccl`
- `egress_vc_state.vc_dr = vc_dr`
- `egress_vc_state.vc_bklg = vc_bklg`

这些值会立刻影响后续 `calc_deq_limit`。

### 7.2 处理 TA 回执

若反馈 `flags` 带 `INFINIFLOW_FEEDBACK_FLAG_TA`，并且本地 `state == 2`，则把 `state` 清回 0。

这表示：**此前 switch 已经把“阈值变化”通过 TA piggyback 在数据包上发给了下游，而下游现在通过反馈显式确认已经看到这次变化**。

紧接着，如果 `state != 0`，函数会直接返回，不继续改阈值。也就是说：

- `state == 1`：阈值刚改，等下一包携带 TA；
- `state == 2`：TA 已发，还没收到确认；
- 只有 `state == 0` 的稳定态，才允许继续根据新反馈调阈值。

### 7.3 根据 backlog 调整 threshold

当 `state == 0` 时：

- `inflight = max(tx_pkts - vc_dr, 0)`
- 读取当前 `threshold`
- 读取 `port_fctbs`

然后按 `vc_bklg` 分三种情况：

#### 情况 A：`vc_bklg >= qmax`

说明下游该 VC backlog 偏大，需要收缩本地阈值：

```text
reduction = vc_bklg - qmin
new_threshold = max(inflight - reduction, 1)
```

随后：

- `threshold = new_threshold`
- `state = 1`

即阈值一旦被收缩，就准备通过下一次发送的 TA 把变化通知下游。

#### 情况 B：`vc_bklg < qmin`

说明下游该 VC backlog 偏小，可以扩大本地阈值：

```text
headroom = max(fccl - port_fctbs, 1)
increase = qmin - vc_bklg
new_threshold = threshold + increase
new_threshold = min(new_threshold, headroom)
new_threshold = max(new_threshold, 1)
```

随后同样：

- `threshold = new_threshold`
- `state = 1`

也就是说，阈值增长不会超过当前端口级剩余共享 headroom。

#### 情况 C：`qmin <= vc_bklg < qmax`

不调整 `threshold`，也不触发新的 TA。

## 8. 两条 TA 闭环

Switch 在 InfiniFlow 中同时参与两条 TA 闭环，方向不同、用途不同。

### 8.1 egress 方向：把阈值变化通知下游

1. switch 收到下游反馈并决定修改 `threshold`；
2. 把 egress 侧 `state` 置 1；
3. 下一次该 VC 有数据发出时，在首包上打 `FC_DATA_FLAG_TA`；
4. 发送成功后把 `state` 置 2；
5. 下游后续反馈若带 `INFINIFLOW_FEEDBACK_FLAG_TA`，则把 `state` 清回 0。

这是一条 **“阈值变化 -> TA 数据包 -> TA 反馈确认”** 的闭环。

### 8.2 ingress 方向：把上游来的 TA 回执回去

1. scheduler 收到上游数据包，若 `flags` 带 `FC_DATA_FLAG_TA`；
2. 在 ingress 侧 `pending_feedback_flags` 里记录 `INFINIFLOW_FEEDBACK_FLAG_TA`；
3. 当这个包后续被 forward 成功发送或被异常释放时，`release_ingress_credit` 会把该标志塞进反馈消息；
4. `feedback_gen` 通过原 ingress 口把 TA 反馈发回上游；
5. ingress 侧本地 `state` 被清回 0。

这是一条 **“上游 TA 数据包 -> switch 反馈确认”** 的闭环。

因此，switch 在实现上既是 **下游方向的 TA 发起者**，又是 **上游方向的 TA 回执者**。

## 9. 线程与 lcore 分配（`main.c`）

InfiniFlow 没有额外线程类型，沿用 switch 的通用 worker 划分：

- **每个 ingress**：
  - 一个 `scheduler_loop`，负责收数据入 egress VC 队列；
  - 一个 `feedback_gen_loop`，负责把反馈发回上游。
- **每个 egress**：
  - 一个 `forward_loop`，负责按 VC 轮询、做信用判断并发包。
- **全局一个**：
  - `feedback_handler_loop`，负责在所有 egress 口接收下游反馈并更新状态。

因此 worker 数量至少要满足：

```text
2 * nb_ingress_ports + nb_egress_ports + 1
```

否则初始化会直接失败。

## 10. 配置项（与 InfiniFlow 相关）

| 参数 | 作用 |
|------|------|
| `--fc-mode infiniflow` | 启用 InfiniFlow 逻辑。 |
| `--initial-fccl` | egress 端口级初始信用，在收到第一批下游反馈前生效。 |
| `--qmin` | backlog 低水位，小于它时尝试增大 `threshold`。 |
| `--qmax` | backlog 高水位，大于等于它时尝试减小 `threshold`。 |
| `--initial-threshold` | 每个 `(egress, vc)` 的初始阈值。 |
| `--port-buffer-pkts` | 每个 ingress 口的共享缓存容量。 |
| `--vc-ring-size` | 每个 `(egress, vc)` 队列的 ring 深度。 |
| `--feedback-ring-size` | 每个 ingress 的反馈消息 ring 深度和预分配消息池大小。 |
| `--route-csv` | `flow_id -> egress_port` 映射表。 |

## 11. 小结数据流

```text
上游 Sender
    │
    │ FC 数据 (0x88B5, 可带 TA)
    ▼
ingress scheduler
    │  按 flow_id 路由，消耗 ingress 共享缓存
    ▼
(egress, vc) ring
    │
    │ deq_limit = min(port_fccl - port_fctbs, threshold - (tx_pkts - vc_dr))
    ▼
egress forward
    │  可在首包上打 TA，改写 src MAC
    ▼
下游 Receiver / 下一跳

        ▲                                           │
        │                                           │
        │      InfiniFlow 反馈 (0x88B7)            │
        │   fccl / vc_dr / vc_bklg / flags         │
        │                                           ▼
feedback_gen ◄── feedback_queue ◄── release_ingress_credit ◄── feedback_handler
    │                                                   ▲
    └──────── 通过原 ingress 口发回上游 ──────────────────┘
```

### 11.1 Mermaid 流程图

```mermaid
flowchart LR
    sender[上游 Sender]
    scheduler[ingress scheduler]
    route[flow_id 路由选择]
    reserve[共享缓存准入\nport occupancy < port_buffer_pkts]
    ring[(egress VC ring)]
    gate[发送门控\nmin(tx_burst,\nport_fccl-port_fctbs,\nthreshold-(tx_pkts-vc_dr))]
    prepare[forward / on_tx_prepare\n首包可带 TA]
    downstream[下游 Receiver / 下一跳]
    release[release_ingress_credit\n释放 ingress 共享缓存与 VC 占用]
    fbq[(feedback queue)]
    fbgen[feedback_gen\n封装 0x88B7]
    upstreamfb[回上游反馈\nfccl / vc_dr / vc_bklg / flags]
    fbh[feedback_handler\negress RX]
    update[on_feedback_rx\n更新 fccl / vc_dr / vc_bklg / threshold / state]

    sender -->|FC 数据 0x88B5\n可能带 FC_DATA_FLAG_TA| scheduler
    scheduler --> route
    route --> reserve
    reserve -->|准入成功| ring
    reserve -.->|共享缓存满| drop1[丢包]

    ring --> gate
    gate -->|允许发送| prepare
    gate -.->|credit_pool=0\n或 vc_credit=0| wait1[本 tick 不发送]
    prepare --> downstream

    prepare -->|发送成功| release
    prepare -.->|未发出且回塞成功| ring
    prepare -.->|未发出且回塞失败| release

    release --> fbq
    fbq --> fbgen
    fbgen -->|原 ingress 口发回| upstreamfb
    upstreamfb --> sender

    downstream -->|InfiniFlow 反馈 0x88B7| fbh
    fbh --> update
    update --> gate

    ta1[TA 闭环 1\nthreshold 变化 -> 下次首包带 TA\n-> 下游反馈带 TA 确认] -.-> prepare
    ta1 -.-> update

    ta2[TA 闭环 2\n收到上游 TA -> pending_feedback_flags\n-> 回上游反馈带 TA] -.-> scheduler
    ta2 -.-> release
    ta2 -.-> fbgen
```

## 12. 总结

本仓库中 switch 侧 InfiniFlow 的核心思想可以概括为三层约束与两条闭环：

1. **ingress 侧** 用 `port_buffer_pkts` + `ingress_port.occupancy` 做共享缓存准入；
2. **egress 侧** 用 `fccl - fctbs` 形成端口级共享信用池；
3. **VC 侧** 用 `threshold - (tx_pkts - vc_dr)` 形成动态的每 VC 发送约束；
4. **反馈闭环** 用 `fccl/vc_dr/vc_bklg` 把 switch 当前接纳能力和 backlog 观测回传给上游；
5. **TA 闭环** 用 `FC_DATA_FLAG_TA` / `INFINIFLOW_FEEDBACK_FLAG_TA` 串起阈值变化的通知与确认。

因此，switch 在 InfiniFlow 模式下不是简单的“按固定信用转发”，而是通过 **共享 buffer、端口共享 headroom、VC 动态 threshold，以及 TA 协调**，在上下游之间维持一个分层的流控闭环。


## 讨论

1. switch 的 egress VC不加准入控制，数据包从ingress到达之后经过路由转发直接放到egress ring queue中。