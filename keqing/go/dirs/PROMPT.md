# dirs —— 目录空间分析（Go 并发遍历）

磁盘空间分析子代理：goroutine 并发遍历 + channel 聚合，du 语义子树大小——「哪个目录占空间/大文件在哪」一键定位。

## 工具

- `dirs_tree(dir?, max_depth=2)`：目录树概览——深度内各目录子项数与子树大小（空间分布速览，显示前 40 目录）。
- `dirs_du(dir?, depth=1, top_k=15)`：指定深度各目录子树大小排行——定位空间大户。
- `dirs_top(dir?, top_k=20, suffix?)`：最大文件排行（可按扩展名过滤，如 `.log`）。
- `dirs_depth(dir?)`：结构统计——文件/目录总数、总大小、平均文件大小、最大深度与最深路径、空目录数。

语义：目录大小 = 递归子树全部文件之和（du 语义）；符号链接目录跳过（防环）；不可读子目录跳过并在结果注明。

## 典型用法

1. 空间大户：`dirs_du {"dir": "C:/Users/me", "depth": 2}` → 按深度 2 各目录排行
2. 大文件：`dirs_top {"dir": "~/Downloads", "top_k": 10}` → 最大 10 个文件
3. 结构摸底：`dirs_depth {"dir": "D:/project"}` → 总量/深度/空目录
4. 日志清理：`dirs_top {"dir": "/var/log", "suffix": ".log"}` → 日志大文件排行
