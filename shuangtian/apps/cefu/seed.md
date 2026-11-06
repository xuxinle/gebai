# 值班知识（示例种子）

> 这份文件是**首启动的种子**：库为空时自动导入成词条草稿（待确认状态）。
> 正式用法是从你自己的 runbook 导入（`kb.importText`），或让模型用 `kb.put` 写。

## TSDB 写入延迟升高

写入 p99 延迟明显上涨，磁盘 IO 可能打满。

- 先查批量写放大倍数（阈值 5x）
- 再查 compaction 进度是否落后
- 确认磁盘 IO 是否打满
- 查副本同步状态

- 命令：curl /metrics | grep tsdb_write_amplification
- 改写放大阈值 5x

## 训练任务长期 pending

任务提交后长时间处于 pending 状态，未进入运行。

- 先查该用户队列排队数与配额
- 再查可用节点资源
- 确认调度器是否正常

- 命令：kubectl get queue -o wide
- 负责人：张三（算法组，配额问题他拍板）

## 任务被抢占

运行中的任务突然停止，日志无异常退出码。

- 查调度事件确认被谁抢占
- 确认是否能提升优先级

- 命令：kubectl get events --field-selector reason=Preempted

## GPU 利用率低

GPU util 长期低于 50%，但任务仍在运行。

- 先对比 CPU 与 GPU 利用率
- 若 CPU 打满而 GPU 空闲，看 dataloader worker 数
- 看通信耗时占比

- 命令：nvidia-smi -l 1

## 配额问题找谁

需要调整配额或审批额度。

- 联系算法组负责人张三（ou_abc）
- 配额问题他拍板

## CI 失败怎么判断真假

CI 流水线报红。

- 先重跑一次
- 连续两次失败才判定为真挂
- 单次失败多为 flaky test

## 用户说「还是不行」怎么办

用户反馈问题仍在，但描述模糊。

- 回看该群前 5 条消息确认上下文
- 不要直接猜
