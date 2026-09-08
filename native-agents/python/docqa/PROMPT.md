# docqa —— 本地文档问答（RAG 检索层）

本地知识库检索问答子代理：为文档目录建 BM25 索引，检索返回命中片段（词元高亮）——RAG 检索阶段即插即用。

## 工具

- `docqa_index(dir, incremental=true)`：为目录递归建索引（md/txt/log/csv，单文件上限 2MB；跳过 venv/node_modules/.git 等噪声目录）。中文二元分词 + 英文词元。索引持久化（按源目录哈希分文件），增量模式 mtime/size 未变的文档复用旧词条。
- `docqa_query(query, dir?, top_k=5)`：BM25 检索（k1=1.5 b=0.75），返回命中文档与含【高亮】词元的上下文片段；缺省 dir 用最近更新的索引。
- `docqa_status()`：全部索引状态（源目录/文档数/建立时间）。

另含 Python 语言框架基础工具（run/pip/status）——常驻进程执行 Python 代码与依赖管理。

## 典型用法

1. 先索引：`docqa_index {"dir": "D:/docs/knowledge"}` → 得到文档数与词表规模
2. 检索问答：`docqa_query {"query": "边车协议超时怎么处理", "top_k": 3}` → 命中片段供回答引用
3. 文档变更后再 index（增量只重分词变化文件），query 复用持久化索引

## 设计要点

- 纯标准库（json/hashlib/math/re/os），无外部依赖——`python_pip` 可按需补装分词/向量库升级为语义检索
- 索引落 `{agent_dir}/index-{hash}.json`，进程重启零重建
- 常驻边车：索引在内存，重复 query 零加载开销
