# Sender / Receiver CBFC 流控逻辑整理

## 1. 总览

这个项目在 `sender` 和 `receiver` 之间实现了一套按 VC 维度工作的 CBFC（Credit-Based Flow Control，基于信用的流控）机制。

- `sender` 根据当前信用值决定某个 VC 是否还能继续发送
- `receiver` 维护每个 VC 的接收侧状态，并回传携带最新 `fccl` 的反馈报文
- 反馈报文使用独立的以太类型，由 `sender` 异步接收并更新本地流控状态

数据报文和反馈报文的公共头定义在 `src/core/fc_header.h`：

- FC 数据报文类型：`FC_ETHER_TYPE = 0x88B5`
- CBFC 反馈报文类型：`CBFC_FEEDBACK_ETHER_TYPE = 0x88B6`
- 数据头字段：`flow_id`、`vc_id`
- 反馈头字段：`vc_id`、`fccl`

## 2. 核心状态变量

### 2.1 Sender 侧

`src/sender/sender_ctx.h` 里定义了每个 VC 的流控状态：

- `fccl`：sender 当前从反馈报文中学到的最新流控上界
- `fctbs`：sender 在该 VC 上已经记账、已通过 `rte_eth_tx_burst()` 成功提交给网卡 TX 的累计报文数

在 CBFC 模式下，sender 使用下面的公式判断还能不能继续发：

```text
credit = max(fccl - fctbs, 0)
```

它的含义是：

- `credit > 0`：这个 VC 还可以继续发
- `credit == 0`：这个 VC 被阻塞，要等新的反馈把 `fccl` 往前推进

### 2.2 Receiver 侧

`src/receiver/receiver_ctx.h` 里定义了每个 VC 的 CBFC 状态：

- `buffer_cap`：分配给该 VC 的静态缓冲额度
- `received`：该 VC 上已经累计接收的 FC 数据包数量

receiver 每收到一个包后，会按下面的方式计算新的 `fccl`：

```text
fccl = buffer_cap + received
```

这说明这里的 `fccl` 是一个“累计发送上界”，不是“当前剩余空闲缓冲”。

## 3. Sender 侧逻辑

### 3.1 初始化

`src/sender/main.c`

- 解析 `--fc-mode` 和 `--initial-fccl`
- 当 `fc_mode=cbfc` 时，每个 VC 的初始状态为：
  - `fccl = initial_fccl`
  - `fctbs = 0`
- sender 会启动三个工作循环，分别跑在不同 worker lcore 上：
  - producer loop：激活 flow，调用 `scheduler_run_tick()`
  - forward loop：从 VC ring 取包并发到网卡
  - feedback RX loop：接收 CBFC 反馈并更新 `fccl`

因此 sender 在启动后不需要等第一批反馈报文，就能依靠 `initial_fccl` 先发出一个初始窗口。

### 3.2 Producer 调度（无 CBFC 门控）

`src/sender/scheduler.c`

对每个活跃 flow，sender 的处理过程是：

1. 检查 flow 状态是否合法，VC 是否越界
2. 分配 mbuf，构造 FC 数据报文，并入队到对应 VC ring

在 CBFC 模式下，scheduler **不再**根据 `fccl`/`fctbs` 做门控；producer 仍可能向 ring 中堆积尚未被 forward 取走的报文（直至 ring 满导致入队失败）。

### 3.3 Forward 门控与 `fctbs` 更新

`src/sender/forward.c`

forward 按 VC 遍历，从对应 ring 出队并调用 `rte_eth_tx_burst()`。当 `fc_mode == cbfc` 时：

1. 在 `rte_ring_sc_dequeue_burst()` 之前读取该 VC 的 `fccl`、`fctbs`，计算 `credit = max(fccl - fctbs, 0)`
2. 若 `credit == 0`，该 VC 本轮不出队
3. 否则本次最多出队 `min(burst_size, credit)` 个 mbuf，再发往网卡
4. 对 **`rte_eth_tx_burst()` 实际返回的成功发送个数** `n_tx`，执行 `fctbs += n_tx`（未成功发出的 mbuf 被释放且不增加 `fctbs`）

因此 CBFC 门控与 `fctbs` 记账都发生在“离开 VC ring、提交 TX”的路径上；`forward.c` 在 CBFC 下会同时更新 `total_pkts_tx` 与 `fctbs`。

### 3.4 反馈接收

`src/sender/feedback_rx.c`

feedback RX loop 的主要流程是：

1. 轮询 sender 的 RX 队列
2. 按 `CBFC_FEEDBACK_ETHER_TYPE` 过滤反馈包
3. 解析 `vc_id` 和 `fccl`
4. 检查 `vc_id < nb_vc`
5. 原子写入 `ctx->vc_fc_states[vc_id].fccl`

它带来的效果是：

- sender 侧的 `fccl` 会被反馈报文持续刷新
- sender 始终使用“最新 `fccl` + 本地 `fctbs`”来判断某个 VC 是否还允许继续发送

## 4. Receiver 侧逻辑

### 4.1 初始化

`src/receiver/main.c`

- 解析 `--fc-mode`、`--cbfc-buffer-pkts`、`--feedback-ring-size`
- 如果开启 CBFC，就为每个 VC 分配 `vc_cbfc_states`
- receiver 总缓冲 `cbfc_total_buffer_pkts` 会按 VC 近似均分：

```text
base = cbfc_total_buffer_pkts / nb_vc
rem  = cbfc_total_buffer_pkts % nb_vc
buffer_cap[i] = base + (i < rem ? 1 : 0)
```

也就是说，配置的总包级缓冲会被平均切分给所有 VC，前 `rem` 个 VC 会多拿 1 个单位。

### 4.2 数据包接收路径

`src/receiver/main.c` 里的 `process_one_packet()`

每收到一个 FC 数据包，receiver 会做下面几步：

1. 检查以太类型是否为 `FC_ETHER_TYPE`
2. 解析 `flow_id` 和 `vc_id`
3. 更新该 flow 的统计信息
4. 如果开启 CBFC：
   - 找到该 VC 对应的状态
   - 执行 `received += 1`
   - 计算 `fccl = buffer_cap + received`
   - 生成一条反馈消息并尝试入队

因此，receiver 侧的 `fccl` 会随着收包持续单调递增。

### 4.3 反馈消息队列

`src/receiver/feedback_tx.c`

receiver 并不会在 RX fast path 上直接发反馈包，而是先把反馈请求放入预分配队列。这里有三部分：

- `feedback_ring`：待发送的反馈消息队列
- `feedback_free_ring`：空闲反馈消息对象队列
- `feedback_pool`：预先分配好的反馈消息对象池

`receiver_feedback_try_enqueue()` 的流程是：

1. 从 `feedback_free_ring` 取一个空闲消息对象
2. 填写 `vc_id`、`fccl` 和目的 MAC
3. 把消息推进 `feedback_ring`
4. 如果没有空闲对象或者 ring 已满，就增加 `feedback_enqueue_drop`

这种实现方式避免了在 RX 路径上做动态内存分配，比较符合 DPDK 数据面的约束。

### 4.4 反馈包发送

同样在 `src/receiver/feedback_tx.c` 中，专门的反馈线程会：

1. 从 `feedback_ring` 取出待发送消息
2. 分配 mbuf
3. 构造 CBFC 反馈报文：
   - `ether_type = CBFC_FEEDBACK_ETHER_TYPE`
   - 负载字段携带 `vc_id` 和 `fccl`
4. 通过 `rte_eth_tx_burst()` 发送

反馈报文的目的 MAC 直接取自刚收到的数据包的源 MAC，因此它会自然地回送到 sender 方向。

## 5. 端到端交互时序

整个 CBFC 的端到端过程可以概括为：

1. sender 为每个 VC 初始化 `initial_fccl`
2. sender 调度器计算 `credit = fccl - fctbs`
3. 如果还有 credit，就允许当前包入队，并执行 `fctbs += 1`
4. receiver 收到该数据包后，执行 `received += 1`
5. receiver 计算新的 `fccl = buffer_cap + received`
6. receiver 发送一个携带 `(vc_id, fccl)` 的反馈报文
7. sender 收到反馈后更新本地 `fccl`
8. sender 获得新的发送空间，后续调度继续推进

一句话理解就是：

- sender 侧控制的是“累计允许发送到哪里”
- receiver 侧通过收包事件不断把这个允许上界往前推

## 6. 与启动脚本的对应关系

`scripts/cbfc_test1/` 里的测试脚本把主要 CBFC 参数暴露了出来：

- `sender1_server3.sh`
  - `--fc-mode`
  - `--initial-fccl`
- `receiver_server2.sh`
  - `--ports`
  - `--fc-mode`
  - `--cbfc-buffer-pkts`
  - `--feedback-ring-size`

receiver 现在支持在一个进程里绑定多个端口，例如：

```text
./build/receiver ... -- --ports 0,1 --fc-mode cbfc --output output/cbfc_test.csv
```

这里的语义是：

- 每个端口都会创建独立的 `receiver_ctx_t`
- 每个端口都有自己的 mempool、flow 统计表、CBFC 状态和反馈队列
- 所有端口在退出时统一写入同一个 CSV

统一输出的 CSV 格式为：

```text
port_id,flow_id,timestamp_count,pps,bps,timestamps_sec
```

其中新增的 `port_id` 列用于区分不同端口上相同的 `flow_id`。

从当前脚本默认值看：

- sender 默认是 `FC_MODE=none`，只是把 `cbfc` 作为可选配置留在那里
- receiver 默认是 `FC_MODE=cbfc`

所以测试时 CBFC 是否真正生效，取决于 sender 和 receiver 两边实际启动时是否同时启用了 `cbfc`。

## 7. 实现特点与注意点

### 7.1 这是“累计边界”风格，不是“剩余信用”风格

当前这套 CBFC 的状态都是单调累计的：

- sender 侧 `fctbs` 只增不减
- receiver 侧 `received` 只增不减
- 反馈里的 `fccl` 也是累计上界

所以它更接近“累计发送许可边界”模型，而不是传统那种“剩余 credit 实时加减”的模型。

### 7.2 门控位置靠前

发送门控放在调度阶段、而且在 mbuf 分配之前，这对 hot path 比较友好：

- 不会引入额外 malloc/free
- 被阻塞的 VC 不会生成无效报文

### 7.3 反馈路径可能成为压力点

receiver 当前是“每收一个 FC 数据包，就尝试产生一个反馈更新”。如果反馈生产或发送跟不上：

- `feedback_ring` 或 `feedback_free_ring` 可能打满
- `feedback_enqueue_drop` 会增加
- sender 看到的 `fccl` 可能滞后
- `fccl` 滞后会让 sender 比实际需要更久地停在阻塞状态

多端口模式下，这个压力会按端口线性叠加，但由于每个端口使用独立反馈队列和独立 TX 服务，端口之间不会直接争抢同一份反馈内存。

### 7.4 流控粒度是 VC，不是 Flow

这套控制逻辑是按 VC 生效的，不是按 flow 生效的：

- 多个 flow 只要共用同一个 `vc_id`，就共用同一份 CBFC 状态
- 某个 flow 的发送会消耗这个 VC 的公共发送窗口

### 7.5 当前 receiver 的统计热点

receiver 的 CSV 统计仍然保留了两个已知热点：

- flow 表在收包路径上可能触发 `flow_table_rehash()`
- 每个 flow 的时间戳数组在收包路径上可能触发 `realloc()`

当前实现通过“退出时统一写 CSV”避免了把文件 IO 带进数据面，但上述动态扩容仍会在高流量、多端口场景下放大延迟抖动。若后续需要进一步优化，更合适的方向是把时间戳采集改成预分配 ring + 独立聚合线程，而不是在 RX 线程里直接维护可增长数组。

## 8. 小结

从代码实现上看，这里的 CBFC 可以总结为：

- sender 用 `fccl - fctbs` 判断某个 VC 还能不能继续发送
- receiver 用 `buffer_cap + received` 计算新的累计发送上界
- 反馈报文异步回传 `(vc_id, fccl)`
- 整个机制按 VC 工作，并通过预分配 ring/对象池来避免在关键数据路径上引入额外动态分配
