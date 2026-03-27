# Sender CSV Format

用于配置 sender 的流量输入，文件必须为逗号分隔（CSV）并包含如下表头：

`fid,vc,len,stime,dest`

字段说明：

- `fid`：flow 唯一标识，`uint32` 整数。
- `vc`：虚拟信道 ID，`uint32` 整数，范围应在 `0..(NB_VC-1)`。
- `len`：该 flow 需要发送的数据包个数，`uint32` 且必须 `> 0`。
- `stime`：flow 激活时间（秒），相对 sender 启动时刻，支持浮点数，必须 `>= 0`。
- `dest`：目的 ID，`uint32` 整数。

注意：

- 表头必须严格匹配：`fid,vc,len,stime,dest`。
- 字段数量必须正好 5 列。
- 空行会被忽略。

可参考示例文件：`examples/flows_example.csv`。
