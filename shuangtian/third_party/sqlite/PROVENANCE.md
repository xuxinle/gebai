# SQLite（内联源码）— 来源与改动台账

## 这是什么

SQLite 的 **amalgamation** 分发形态：整个数据库引擎合并成 3 个文件。
上游就是**鼓励**这样用的（[How To Compile SQLite](https://sqlite.org/howtocompile.html)）：
把 `sqlite3.c` + `sqlite3.h` 编进自己的程序，得到"无需安装、无 ABI 版本问题"的嵌入式数据库。

| 文件 | 字节 | 说明 |
|---|---:|---|
| `sqlite3.c` | 9281384 | 引擎实现（单翻译单元，`#include` 进来即可编） |
| `sqlite3.h` | 661946 | 公共 C API（`sqlite3_open_v2` / `sqlite3_prepare_v3` / `sqlite3_step` …） |
| `sqlite3ext.h` | 38149 | **可加载扩展**的开发头。本框架不加载原生扩展（见下），此文件保留只为"上游三件套完整"与将来的取数，**不参与构建** |
| `st_sqlite3_config.h` | 本仓库 | **霜天的编译期开关**（唯一改动点，见下） |

## 版本与来源

```c
#define SQLITE_VERSION        "3.50.2"
#define SQLITE_VERSION_NUMBER 3050002
#define SQLITE_SOURCE_ID      "2025-06-28 14:00:48 2af157d77fb1304a74176eaee7fbc7c7e932d946bf25325e9c26c91db19e3079"
```

- **上游发布位**：`https://sqlite.org/2025/sqlite-amalgamation-3500200.zip`（`3500200` = 3.50.2，
  末位恒为 0；与 rust-lang 镜像的 `libsqlite3-sys/upgrade.sh` 所取同一发布包一致）
- **本次取数路径**：`libsqlite3-sys 0.35.0`（crates.io，crate 的 bundled 副本；
  本地 crates.io 镜像 rsproxy.cn）内的 `sqlite3/{sqlite3.c,sqlite3.h,sqlite3ext.h}`。
  该副本即由上述官方 zip **按文件原样导出**（`unzip -p`），未做任何修改。
- **为何走这条路**：本机网络直连 sqlite.org 超时（30s 无响应），而 SQLite 官方不提供镜像证书
  （见 https://sqlite.org/mirrors.html ：镜像仅供只读访问，不得作为权威分发）；改由 crates.io 拉取
  同一发布包导出的副本，**并用版本宏与 SOURCE_ID 交叉确认**——3.50.2 与 rust 侧
  `upgrade.sh` 记录的 `sqlite-amalgamation-3500200` 完全吻合。
- **许可证**：SQLite 是 **public domain**（作者已声明放弃著作权）。仓库内附 `SQLITE_PUBLIC_DOMAIN.txt`
  （官方 `https://sqlite.org/copyright.html` 的声明原文），满足"分发时保留声明"的通行做法。

## 改动

**上游 3 个文件一字未改**（`sha256sum -c` 可校验，见 `../CHECKSUMS.sha256`）。

霜天的编译期裁剪全部集中在 `st_sqlite3_config.h`：由 `st.pkg` 的 `c_flags`
以 `-include st_sqlite3_config.h` **前置**到 `sqlite3.c` 的编译单元里。
逐项开关与取舍理由写在该文件头部（含"刻意不打开"的清单）。
这样做的理由：上游可追溯性——"我们动了什么"是一个文件、一份 diff，而不是散在 9MB 里。

## 裁剪结果（相对上游默认）

打开：FTS5、R-tree、dbstat、STAT4、列来源信息、数学函数、API 参数防御；外键默认开、
双引号不当字符串、serialized 线程模式。
裁掉：已废弃 API、**原生扩展加载**、shared-cache。
（JSON 函数自 3.38 起默认编入，无需开关。）

## 与本仓库的集成点

| 位置 | 内容 |
|---|---|
| `st.pkg` | `third_party_sources` 收 `third_party/sqlite/*.c`；`include_dirs` 加 `third_party/sqlite`；`c_flags` 前置 `-include st_sqlite3_config.h` |
| `include/st/ext/database.hpp` / `src/ext/database.cpp` | C++20 侧封装（`st::ext::Database` / `Statement` / `Value`）——**公共头不外泄任何 sqlite3 符号** |
| `tests/ext_database_test.cpp` | 建库/参数化增删改查/事务/BLOB/错误路径的用例 |
| `third_party/CHECKSUMS.sha256` | 逐文件 SHA-256 |

## 为什么走 `st.pkg` 的第三方通道而不是"我们自己写一个数据库"

`third_party_sources` 通道的语义（`CONVENTIONS.md` §3.8）正好匹配 amalgamation 形态的依赖：
**原样引入、放宽告警（`-w`）、不进 PCH、不做 sanitizer 插桩**。
SQLite 是 C 代码（9MB 单文件），我们的 `-Werror` 严格 C++ 集对它没有意义，
而"上游抛异常/新警告就改上游"会直接摧毁可追溯性——这正是该通道存在的理由。
