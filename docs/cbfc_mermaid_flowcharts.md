# CBFC 各模块 Mermaid 详细流程图

本文将 CBFC（Credit-Based Flow Control）在 `sender / switch / receiver` 三侧的实现，拆成可直接渲染的 Mermaid 图，重点覆盖：

- CBFC 流控信息（`fccl / fctbs / occupancy / capacity`）如何更新
- 数据包与反馈包如何在模块间传递
- 各关键分支（credit 不足、队列满、发送失败）如何处理

---

## 1) 模块级总览（数据与反馈双通道）

```mermaid
flowchart LR
    subgraph SND["Sender 模块"]
        SCHED["scheduler\n构造 FC 数据包(0x88B5)\n按 vc_id 入 sender VC ring"]
        FWD_S["forward\n按 credit=fccl-fctbs 出队并 TX\n成功后 fctbs+=n_tx"]
        FRX["feedback_rx\n收 0x88B6\n更新 sender.vc_fc_state.fccl"]
        SCHED --> FWD_S
    end

    subgraph SW["Switch 模块"]
        SCH_SW["scheduler(ingress)\n收 0x88B5\noccupancy<capacity 才入 VC ring"]
        FWD_SW["forward\ncredit=max(fccl-fctbs,0)\nTX 成功后 fctbs++\n计算 feedback_fccl=fctbs+remaining"]
        FGEN["feedback_gen\n将(vc_id,fccl)封装 0x88B6\n从 ingress TX 回上游"]
        FH["feedback_handler(egress RX)\n收下游 0x88B6\n写入 switch.vc_fc_state.fccl"]
        SCH_SW --> FWD_SW
        FWD_SW --> FGEN
        FH --> FWD_SW
    end

    subgraph RCV["Receiver 模块"]
        RX["main RX\n收 0x88B5\nreceived++\nfccl=buffer_cap+received"]
        QFB["feedback queue/pool\n预分配对象入队"]
        FTX["feedback_tx\n封装 0x88B6\nTX 回上游(目的MAC=原包源MAC)"]
        RX --> QFB --> FTX
    end

    FWD_S -- "FC 数据包 0x88B5" --> SCH_SW
    FWD_SW -- "FC 数据包 0x88B5" --> RX
    FTX -- "CBFC反馈 0x88B6" --> FH
    FGEN -- "CBFC反馈 0x88B6" --> FRX
```

---

## 2) Sender 详细流程（生产、门控、反馈更新）

```mermaid
flowchart TD
    A["producer loop / scheduler_run_tick"] --> B{"flow有效 && vc_id合法?"}
    B -- "否" --> B0["丢弃/跳过"]
    B -- "是" --> C["分配mbuf并封装FC头\n(flow_id, vc_id, ether=0x88B5)"]
    C --> D{"sender VC ring入队成功?"}
    D -- "否(ring满)" --> D0["释放mbuf并计drop"]
    D -- "是" --> E["等待forward消费"]

    E --> F["forward按VC轮询"]
    F --> G["读取 fccl, fctbs\ncredit=max(fccl-fctbs,0)"]
    G --> H{"credit > 0 ?"}
    H -- "否" --> H0["本轮该VC不出队"]
    H -- "是" --> I["最多出队 min(tx_burst, credit)"]
    I --> J["rte_eth_tx_burst 发送"]
    J --> K["成功n_tx: fctbs += n_tx"]
    J --> L["失败部分: 释放未发送mbuf并计drop"]

    M["feedback_rx loop"] --> N{"ether_type == 0x88B6 && 长度合法?"}
    N -- "否" --> N0["忽略并释放"]
    N -- "是" --> O["解析(vc_id, fccl)"]
    O --> P{"vc_id < nb_vc ?"}
    P -- "否" --> P0["忽略并释放"]
    P -- "是" --> Q["原子写 sender.vc_fc_states[vc_id].fccl"]
    Q --> R["后续forward使用新fccl解锁发送窗口"]
```

---

## 3) Switch 详细流程（入向缓存、出向信用、双向反馈）

```mermaid
flowchart TD
    A["ingress scheduler\nswitch_scheduler_run_tick"] --> B["RX ingress口收包"]
    B --> C{"ether_type == 0x88B5 ?"}
    C -- "否" --> C0["非FC数据包: 丢弃/忽略"]
    C -- "是" --> D["解析 vc_id"]
    D --> E{"vc_id合法?"}
    E -- "否" --> E0["丢包"]
    E -- "是" --> F{"occupancy < capacity ?"}
    F -- "否" --> F0["容量门控触发: 丢包"]
    F -- "是" --> G["occupancy++ (reserve slot)"]
    G --> H{"入 vc_queues[vc_id].ring 成功?"}
    H -- "否(ring满)" --> H0["回滚 occupancy-- 并丢包"]
    H -- "是" --> I["等待 forward 消费"]

    I --> J["forward loop 按VC轮询"]
    J --> K["credit=max(fccl-fctbs,0)"]
    K --> L{"credit > 0 ?"}
    L -- "否" --> L0["本tick该VC不出队"]
    L -- "是" --> M["出队 <= min(tx_burst, credit)"]
    M --> N["occupancy -= 实际出队数"]
    N --> O["flow_map查找目标端口\n仅目标=egress的包进入发送候选"]
    O --> P["改写源MAC为egress MAC\n保留原始源MAC用于回送反馈"]
    P --> Q["egress TX burst"]

    Q --> R{"每个候选包发送成功?"}
    R -- "否" --> R0["释放mbuf并计tx_drop"]
    R -- "是" --> S["on_tx_success:\nfctbs++\nremaining=max(capacity-occupancy,0)\nfeedback_fccl=fctbs+remaining"]
    S --> T["按 ingress_idx 入 feedback_queue\n消息=(vc_id, feedback_fccl, dst=原始源MAC)"]

    U["feedback_gen loop(每ingress一条)"] --> V["出队 feedback_msg"]
    V --> W["封装0x88B6: (vc_id,fccl)\n源MAC=ingress口MAC 目的MAC=msg.dst"]
    W --> X["从同一 ingress TX 回上游(sender方向)"]

    Y["feedback_handler loop(egress RX)"] --> Z{"收到0x88B6且长度合法?"}
    Z -- "否" --> Z0["忽略并释放"]
    Z -- "是" --> AA["解析(vc_id, fccl)"]
    AA --> AB{"vc_id合法?"}
    AB -- "否" --> AB0["忽略并释放"]
    AB -- "是" --> AC["cbfc_on_feedback_rx:\n原子写 switch.vc_fc_state[vc_id].fccl"]
    AC --> AD["forward后续轮次使用新fccl"]
```

---

## 4) Receiver 详细流程（累计上界生成与异步反馈发送）

```mermaid
flowchart TD
    A["receiver main RX"] --> B{"ether_type == 0x88B5 ?"}
    B -- "否" --> B0["非FC数据包: 忽略/按其他逻辑处理"]
    B -- "是" --> C["解析 flow_id, vc_id"]
    C --> D["更新flow统计"]
    D --> E{"fc_mode == cbfc ?"}
    E -- "否" --> E0["仅统计，不生成CBFC反馈"]
    E -- "是" --> F["vc_state.received += 1"]
    F --> G["fccl = buffer_cap + received"]
    G --> H["尝试入队反馈消息(vc_id,fccl,dst=原包源MAC)"]

    H --> I{"free_ring有空闲对象?"}
    I -- "否" --> I0["feedback_enqueue_drop++"]
    I -- "是" --> J{"feedback_ring入队成功?"}
    J -- "否(ring满)" --> J0["归还对象并feedback_enqueue_drop++"]
    J -- "是" --> K["等待feedback_tx线程发送"]

    K --> L["feedback_tx loop 出队消息"]
    L --> M{"分配mbuf成功?"}
    M -- "否" --> M0["归还对象并计发送失败"]
    M -- "是" --> N["封装0x88B6(vc_id,fccl)\n目的MAC=消息dst"]
    N --> O["rte_eth_tx_burst 发送反馈"]
    O --> P["发送后归还消息对象到free_ring"]
```

---

## 5) 端到端时序图（含 CBFC 信息闭环）

```mermaid
sequenceDiagram
    participant SD as Sender.forward
    participant SWI as Switch.ingress_scheduler
    participant SWF as Switch.forward
    participant RC as Receiver.RX
    participant RFT as Receiver.feedback_tx
    participant SWH as Switch.feedback_handler
    participant SWG as Switch.feedback_gen
    participant SFR as Sender.feedback_rx

    Note over SD: 按VC计算 credit = fccl - fctbs
    SD->>SWI: FC数据包(0x88B5, flow_id, vc_id)
    SWI->>SWI: occupancy<capacity? 是则入VC ring并occupancy++
    SWI->>SWF: VC数据可转发
    SWF->>SWF: credit=max(fccl-fctbs,0)
    SWF->>RC: FC数据包(0x88B5)
    SWF->>SWF: TX成功后 fctbs++\nfeedback_fccl=fctbs+remaining
    SWF->>SWG: 入队反馈消息(vc_id,feedback_fccl,dst=原始源MAC)
    SWG->>SFR: 反馈(0x88B6, vc_id, feedback_fccl)
    SFR->>SFR: 更新 sender.fccl

    RC->>RC: received++\nfccl=buffer_cap+received
    RC->>RFT: 入队反馈消息(vc_id,fccl,dst=原包源MAC)
    RFT->>SWH: 反馈(0x88B6, vc_id, fccl)
    SWH->>SWH: 更新 switch.fccl
    Note over SWH,SWF: switch 下一轮按新fccl重新计算credit
```

---

## 6) 关键状态量关系图（便于排障）

```mermaid
flowchart LR
    subgraph Sender[Sender VC状态]
      S1["fccl_s: 来自switch反馈_rx"]
      S2["fctbs_s: sender发送成功累计"]
      S3["credit_s=max(fccl_s-fctbs_s,0)"]
      S1 --> S3
      S2 --> S3
    end

    subgraph Switch[Switch VC状态]
      W1["fccl_w: 来自下游反馈_handler"]
      W2["fctbs_w: switch向egress成功发送累计"]
      W3["occupancy_w: ingress入队- forward出队"]
      W4["remaining_w=max(capacity-occupancy_w,0)"]
      W5["credit_w=max(fccl_w-fctbs_w,0)"]
      W6["feedback_fccl=fctbs_w+remaining_w"]
      W1 --> W5
      W2 --> W5
      W3 --> W4
      W2 --> W6
      W4 --> W6
    end

    subgraph Receiver[Receiver VC状态]
      R1["buffer_cap_r: 初始化分配"]
      R2["received_r: 收包累计"]
      R3["fccl_r=buffer_cap_r+received_r"]
      R1 --> R3
      R2 --> R3
    end

    W6 -- "0x88B6 回上游" --> S1
    R3 -- "0x88B6 回switch egress" --> W1
```

以上图可作为 CBFC 联调时的“对照图”：

- 若 sender 长期 `credit_s=0`，优先检查 `Sender.feedback_rx` 是否收到 switch 回传；
- 若 switch 长期 `credit_w=0`，优先检查 `Switch.feedback_handler` 是否收到 receiver/下游反馈；
- 若 receiver `feedback_enqueue_drop` 升高，说明反馈路径拥塞，sender/switch 看到的 `fccl` 可能滞后。
