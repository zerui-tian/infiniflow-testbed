首先我们需要完成一个初步的sender。
数据包大小为8000B，sender通过读取csv文件中的配置项来定时发送指定packet数目的flow。
csv文件的header分别是：fid(flow id，每个流唯一标识，int证书）, vc(vitual channel,流使用的虚拟频道id，int证书），len（流数据包个数），stime（流量开始时间，单位是秒，支持浮点数），dest（目标id，int整数）。
为了合理高效处理这些流量，我们需要一个activity manager核心来管理这些流量，包括流量的启动和销毁。
当一条流量被启动时，我们需要一个scheduler核心遍历所有正在活动的flow为它们创建对应数据包，并推入到特定的VC的ring buffer中。每个VC会有一个ring buffer，每个ring buffer的大小固定。如果ring buffer满了，则跳过该flow，遍历下一个flow。当发送数目达到flow对应的数据包个数len时，标记该flow死亡，不在遍历该flow。另外activity manager负责清理。
还需要一个forward核心，遍历所有VC，从vc中取出数据包放入到网卡发送队列中进行发送。
这里需要注意vc的ring buffer是一个单生产者单消费者模型，需要注意并发冲突。
