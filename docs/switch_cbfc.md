# Switch 侧 CBFC 机制说明

本文描述本仓库中 **switch** 在 `FC_MODE_CBFC` 下如何配合数据报文与反馈报文完成基于信用的流控（实现见 `src/switch/`）。

## 1. 角色与报文类型

- **数据面**：识别以太网类型 `FC_ETHER_TYPE`（`0x88B5`）的 FC 数据报文，载荷头中含 `flow_id`、`vc_id`（`fc_data_header_t`，见 `src/core/fc_header.h`）。
- **反馈面**：识别 `CBFC_FEEDBACK_ETHER_TYPE`（`0x88B6`）的 CBFC 反馈报文，载荷为 `vc_id` + `fccl`（`cbfc_feedback_header_t`）。

Switch 在 CBFC 模式下同时承担：**收数据入 VC 队列**、**按信用向 egress 发送**、**把“新 FCCL”经原 ingress 口发回上游**、以及 **在 egress 口接收下游反馈并更新本地 FCCL**。

## 2. 每 VC 状态（`switch_vc_fc_state_t`）

每个虚拟通道 `vc_id` 维护一份状态（`switch_ctx.h`）：

| 字段 | 含义（switch 实现语义） |
|------|-------------------------|
| `fccl` | 当前信用上限，由 **在 egress 口收到的 CBFC 反馈** 更新；初值为配置项 `initial_fccl`。 |
| `fctbs` | 本 switch **已成功向 egress 发出的该 VC 报文计数**（每成功发送 1 包自增 1）。 |
| `occupancy` | 当前该 VC 在 switch 内已入队、尚未被 forward 取走的报文数（ingress 入队时增加，forward 出队时减少）。 |
| `capacity` | 该 VC 在 switch 侧允许缓存的报文上限（`vc_capacity_pkts`），与 `occupancy` 配合做 **入向背压**。 |

## 3. 入向路径：调度器（`scheduler.c`）

每个 ingress 口对应一个调度循环 `switch_scheduler_run_tick`：

1. 从 ingress **收包**。
2. 仅处理 `FC_ETHER_TYPE` 数据报；解析 `vc_id`，越界则丢包。
3. **容量门控**：`try_reserve_vc_slot` 在 `occupancy < capacity` 时把 `occupancy` 加 1，否则丢包（不进入 VC 队列）。
4. 将 mbuf **入对应 VC 的无锁环**（`vc_queues[vc_id].ring`）；若环满则回滚 `occupancy` 并丢包。

注意：**ingress 路径不直接查 `fccl/fctbs`**；CBFC 的“能不能往 egress 发”在 forward 阶段用信用控制。

## 4. 出向路径：转发与信用（`forward.c` + `main.c` 中 `cbfc_*`）

`switch_forward_run_tick` 按 VC 轮询，核心逻辑如下。

### 4.1  dequeue 前：计算本 tick 可用信用

当 `fc_mode == FC_MODE_CBFC` 且已注册 `fc_ops` 时：

- 调用 `calc_credit`（即 `cbfc_calc_credit`）：
  - `credit = max(fccl - fctbs, 0)`
- 若 `credit == 0`，该 VC **本 tick 不从环上取包**。
- 否则本 VC 本次最多出队 `min(tx_burst_size, credit)` 个 mbuf。

含义：**在下游认可的信用范围（`fccl`）内，减去本机已转发计数（`fctbs`），得到还可向 egress 发送的包数**。

### 4.2  出队后：更新占用、改写 MAC、发送

- 从环上 `dequeue_burst` 成功后，`occupancy` 减去实际出队数量。
- 对每包做长度与 `flow_id` 查表（`switch_flow_map_lookup`），仅当目标为配置的 **egress 端口** 才进入发送候选；发送前把以太网 **源 MAC 改为 egress 口 MAC**（`ctx->egress_mac`），并保存 **原始源 MAC** 供反馈时使用。
- `rte_eth_tx_burst` 发往 `egress_port` / `egress_tx_queue_id`。

### 4.3  发送成功：推进 `fctbs` 并排队反馈

对 **每一个实际发送成功** 的 mbuf（`j < n_tx`）：

1. 调用 `on_tx_success`（`cbfc_on_tx_success`）：
   - `fctbs` 自增 1（原子加）。
   - 读取当前 `occupancy`，计算 `remaining = max(capacity - occupancy, 0)`（switch 上该 VC **剩余可再接收**的槽位）。
   - 返回值：`feedback_fccl = 新的 fctbs + remaining`。
2. 根据 mbuf 记录的 **ingress 端口** 得到 `ingress_idx`，将一条 `switch_feedback_msg_t`（`vc_id`、`fccl`、`dst_addr`= 原报文源 MAC）送入 **该 ingress 对应的反馈队列**（`enqueue_feedback_msg`）。
3. 释放数据 mbuf（数据已交给网卡发送）。

未成功发出的候选包会被释放并计为 `tx_drop`。

**FCCL 反馈值的直观解释**：`fctbs` 反映“我已经帮你往下游转发了多少包”；加上 `remaining` 反映“我在本 VC 上还能再帮你缓冲多少包”；两者之和作为给上游的 **累计型信用指示**（具体与 sender 端如何消费配合，见 sender/receiver 文档）。

## 5. 反馈报文发出：`feedback_gen.c`

每个 ingress 对应一个 `switch_feedback_gen_run_tick` 循环：

1. 从该 ingress 的 `feedback_queues[i]` 取出 `switch_feedback_msg_t`。
2. 分配 mbuf，封装以太网头 + `cbfc_feedback_header_t`，`ether_type = 0x88B6`，填入 `vc_id`、`fccl`，目的 MAC 为消息中的 `dst_addr`，源 MAC 为该 **ingress 口的 MAC**。
3. 通过 **同一 ingress 口的 TX 队列**（`ingress_tx_queue_id`）发回上游（典型拓扑下即回到 sender）。

若 mbuf 分配失败或发送不完整，会按现有逻辑回滚或释放，避免泄漏。

## 6. 下游反馈接入：`feedback_handler.c`

在 **`egress_port` 的 RX 队列**上收包（与数据同口反向或同链路返回的反馈）：

1. 过滤 `0x88B6` 且长度合法。
2. 解析 `vc_id`、`fccl`，若 `vc_id` 合法且 `fc_ops->on_feedback_rx` 存在，则调用 `cbfc_on_feedback_rx`：**原子写入该 VC 的 `fccl`**。
3. 释放 mbuf。

这样 switch 的 **可发送信用上限** 与 receiver（或下游）公布的窗口对齐。

## 7. 线程与 lcore 分配（`main.c`）

CBFC 相关循环分布在多个 worker lcore 上：

- **每个 ingress**：一个 `scheduler_loop`（收数据入 VC 环）+ 一个 `feedback_gen_loop`（从反馈队列发包回上游）。
- **全局一个** `forward_loop`（所有 VC 的出向转发与反馈入队）。
- **全局一个** `feedback_handler_loop`（在 egress 口收反馈并更新 `fccl`）。

主核通常跑 EAL；需足够 worker 数量，否则初始化会失败。

## 8. 配置项（与 CBFC 相关）

| 参数 | 作用 |
|------|------|
| `--fc-mode cbfc` / `none` | `cbfc` 启用信用与反馈；`none` 时不调用 `calc_credit` / `on_tx_success` 的流控与反馈逻辑（forward 仍可能按其它逻辑转发）。 |
| `--initial-fccl` | 每个 VC 初始 `fccl`。 |
| `--vc-capacity` | 每个 VC 在 switch 内最大缓存包数（`capacity`），与 `occupancy` 一起做入向限流。 |
| `--vc-ring-size` | VC 队列环容量。 |
| `--feedback-ring-size` | 每个 ingress 上反馈消息环与空闲环深度（预分配 `feedback_pool` 条目数）。 |

## 9. 小结数据流

```text
Sender ──(FC 数据, 0x88B5)──► ingress ──► VC ring ──► forward ──(改写 MAC)──► egress ──► Receiver
                ▲                                      │
                │         CBFC 反馈 (0x88B6)           │
                └──── feedback_gen ◄── 反馈队列 ◄──────┘
                                                        │
Receiver（或下游）──── CBFC 反馈 (0x88B6) ───► egress RX ──► feedback_handler ── 更新 fccl
```

整体上，switch 侧 CBFC 通过 **`fccl`（下游/本地反馈上限）与 `fctbs`（已转发计数）差值** 限制出向速率，通过 **`capacity` 与 `occupancy`** 限制入向缓存，并通过 **per-ingress 反馈队列 + feedback_gen** 将更新后的信用回传给上游。
