# SQLite 集成规划（源码内置 / amalgamation）

> 状态：**已落地**（2026-10-03）。本文是设计记录——写"为什么这么接、边界划在哪、什么被刻意留在外面"，
> 与 `third_party/sqlite/PROVENANCE.md`（来源与台账）、`third_party/sqlite/st_sqlite3_config.h`（逐项开关）互为一体。

## 0. 一句话

把 **SQLite 3.50.2 的 amalgamation（`sqlite3.c` + `sqlite3.h`）直接编进霜天的静态库**：
应用与独立工程拿到的是一份**随源码交付、零安装、版本自定、可审计**的数据库能力，
对外只暴露 `st::ext::Database` 这层 C++20 门面。

这也是上游推荐的用法——[How To Compile SQLite](https://sqlite.org/howtocompile.html)：

> ... the recommended way to compile SQLite ... is to combine the code in `sqlite3.c`
> and `sqlite3.h` with your own C code and compile everything at once.

## 1. 为什么走这条路（三个方案的真实代价）

| 方案 | 代价 |
|---|---|
| **链接系统 sqlite3**（Linux 发行版 / macOS 自带） | 版本由宿主决定（3.8～3.45 都遇得到）、**能力开关由打包者决定**（FTS5、R-tree 常常没编进去），Windows 上根本不存在"系统 sqlite3"；"在我机器上能跑"变成碰运气 |
| **随仓库分发预编译库**（`libsqlite3.a`） | 静态库格式与 ABI 绑死编译器（MinGW 与 MSVC 不通用），交叉编译要维护两套产物；"用什么编译的"变成不可见的隐藏状态 |
| **amalgamation 内联源码**（本方案） | 构建期多编一个翻译单元（实测 **17.3 s @ -O1**，一次性、可缓存）；换来的是**跨三平台同一份代码、同一组开关、同一个版本**，且交叉编译天然生效（用目标编译器重编即得目标产物） |

第三条与框架**既有的第三方通道**（`st.pkg` 的 `third_party_sources`）语义完全吻合：原样引入、`-w`、
不进 PCH、不做 sanitizer 插桩（`CONVENTIONS.md` §3.8）。因此本次接入**没有给构建系统加任何新机制**。

## 2. 版本与取数

- 版本 **3.50.2**（`SQLITE_SOURCE_ID 2025-06-28 ...`）；
- 上游发布位 `https://sqlite.org/2025/sqlite-amalgamation-3500200.zip`；
- 本机直连 sqlite.org 超时（30 s 无响应），且 SQLite 官方明确不授权镜像作权威分发，
  故从 crates.io（本地镜像 rsproxy.cn）取 `libsqlite3-sys 0.35.0` 的 bundled 副本——
  该副本正是由上述官方 zip 按文件 `unzip -p` 原样导出，并用 **版本宏 + SOURCE_ID 交叉确认**；
- 逐文件 SHA-256 登记在 `third_party/CHECKSUMS.sha256`；完整复现路径见 `PROVENANCE.md`。

许可：SQLite 为 **public domain**，仓库内附 `SQLITE_PUBLIC_DOMAIN.txt`（官方声明誊录）。

## 3. 编译期开关：我们唯一的改动面

**上游 3 个文件一字未改**；霜天的裁剪全部集中在 `third_party/sqlite/st_sqlite3_config.h`，
由 `st.pkg` 以 `-include st_sqlite3_config.h` 前置进 `sqlite3.c` 的编译单元。

这么做的理由只有一条，但足够硬：**「我们动了 SQLite 什么」必须是一份可读的 diff**。
如果就地改上游（在 9 MB 里插 `#define`），可追溯性当场崩塌——升级上游时无从知道哪些改动要重放。

| 开关 | 值 | 一句话理由 |
|---|---|---|
| `SQLITE_THREADSAFE` | 1 | serialized：**对外承诺**"一个连接可跨线程用"，`st::ext::Database` 的线程语义按它写 |
| `SQLITE_DEFAULT_MEMSTATUS` | 0 | 每次分配少一次原子计数（不用内存统计接口） |
| `SQLITE_DQS` | 0 | 双引号只当标识符：SQL 引号写错**当场报错**，不静默变字符串 |
| `SQLITE_DEFAULT_FOREIGN_KEYS` | 1 | 外键默认开：静默忽略外键是现代应用更大的坑 |
| `SQLITE_ENABLE_API_ARMOR` | 1 | 参数错误**返回错误码**而不是段错误（换"错误可诊断"） |
| `SQLITE_ENABLE_COLUMN_METADATA` | 1 | `sqlite3_column_table_name()` 等来源信息（列→表溯源） |
| `SQLITE_ENABLE_DBSTAT_VTAB` | 1 | `dbstat` 虚表：应用要显示"哪个表吃磁盘"不必自己扫文件 |
| `SQLITE_ENABLE_FTS5` | 1 | 全文检索（检索类场景的硬需求） |
| `SQLITE_ENABLE_MATH_FUNCTIONS` | 1 | `pow`/`ln`/三角函数等标量函数 |
| `SQLITE_ENABLE_RTREE` | 1 | R-tree 空间索引（矩形范围查询） |
| `SQLITE_ENABLE_STAT4` | 1 | 更准的查询规划统计 |
| `SQLITE_OMIT_DEPRECATED` | 1 | 剔除已废弃 API（省体积、少误用面） |
| `SQLITE_OMIT_LOAD_EXTENSION` | 1 | **不提供**运行时加载原生 DLL/.so —— 与脚本层同一姿态：可执行代码入口必须显式设计 |
| `SQLITE_OMIT_SHARED_CACHE` | 1 | shared-cache 收益有限而失败模式隐蔽；并发读走 WAL + 多连接 |

刻意**不**打开的（不是遗漏）：`ENABLE_SESSION`/`PREUPDATE_HOOK`（变更集捕获，无此需求）、
`ENABLE_UNLOCK_NOTIFY`（需应用侧回调编排）、`SQLITE_DEBUG`/`ENABLE_MEMORY_MANAGEMENT`（调试档）、
`ENABLE_JSON1`（**3.38 起 JSON 函数默认编入**，再定义只是历史写法）。
`SQLITE_TEMP_STORE` 未固定：上游默认（1 = 文件）在 WAL 下本就快，写死只会绑住未来的调优空间。

**开关是编译期事实**：改一个宏必须重编 `sqlite3.c`（`stpm` 把编译标志纳入增量判据，
见 `src/pkg/build.cpp` 的 `compile_fingerprint`），不会出现"改了开关但产物没变"。

## 4. 构建接入（`st.pkg` 三处）

```json
"include_dirs": ["include", "third_party", "third_party/sqlite"],
"third_party_sources": ["third_party/quickjs/*.c", "third_party/sqlite/*.c"],
"c_flags": ["-std=gnu11", "-include", "st_sqlite3_config.h"],
```

自动获得的三条既有纪律（无需新增机制）：

1. **`-w`**：9 MB 上游 C 码不套本工程的 `-Werror` 严格集（"上游抛警告就改上游"会毁掉可追溯性）；
2. **不进 PCH**：`sqlite3.c` 不包含框架头，强制吃 PCH 会改变其编译语义；
3. **不做 sanitizer 插桩**：san 档下 sqlite3.c 不插 ASan/UBSan。

关于第 3 条的**代价**必须写清楚：san 档因此**看不见 sqlite3.c 内部**的内存错误。
这是刻意的取舍——插桩会让 17 s 的单元变成分钟级、且上游代码（成熟度极高）不是我们的质量责任对象；
代价换来的是"san 档跑得动"。我们自己的封装层（`src/ext/database.cpp`）**照常插桩**，
分配器是全局的，越界/悬垂在我们这侧依然会被抓到。

## 5. C++ 封装面（`st::ext::Database`）

设计取向（逐条都是"为什么不是另一种写法"）：

| 决策 | 理由 |
|---|---|
| 公共头**前置声明** `struct sqlite3;` + `unique_ptr` 自定义删除器 | 不把 660 KB 的 C 头塞进每一个包含者；C 符号不外泄 → 上层永远走 `Result` 面 |
| 一切失败经 `Result`/`Status`，**不抛异常** | 框架契约（`CONVENTIONS.md` §3.2）；SQLite 自己就是错误码模型，映射天然 |
| 列值用 `std::variant<nullptr_t, int64, double, string, Bytes>` | 比"`column_int()`/`column_text()` 按需取"的散接口更难误用：**类型是值的一部分**，取值即穷举 |
| `Statement` 只可移动、不可复制 | 句柄语义；复制会让两条执行流共用一个游标（最隐蔽的一类 bug） |
| 事务：`begin/commit/rollback` + **嵌套计数** | SQLite 无真嵌套事务；嵌套时用 SAVEPOINT 语义（外层提交才真提交） |
| 打开选项用**具名 flags 结构**（read_only / create / memory / …） | 不暴露 `SQLITE_OPEN_*` 位掩码，避免"随手 |= 出个非法组合" |
| 线程语义：**一个 Database 一个线程** | 库是 serialized 的，但把"谁在什么时候用"讲清楚比"理论上能共享"更有价值；需要并发就在每线程各持一个连接 |

API 草案（最终以 `include/st/ext/database.hpp` 为准）：

```cpp
namespace st::ext {

enum class SqlError { ... };                  // 结构化失败类（映射到 st::ErrorCode）
struct Column { ... };                        // 列名/声明类型/来源表名（COLUMN_METADATA）
using Value = std::variant<std::nullptr_t, std::int64_t, double, std::string, fs::Bytes>;

struct OpenOptions {
  bool read_only{false};
  bool create{true};
  bool memory{false};                          // ":memory:"（临时库）
  bool no_mutex{false};                        // 明确"我只在一个线程用"（省掉互斥开销）
  std::string vfs{};                           // 高级：自定义 VFS（空=默认）
};

class Statement {                              // 预编译语句（只可移动）
 public:
  auto bind(int index, const Value&) -> Status;          // 1-based（与 SQLite 一致）
  auto bind(std::string_view name, const Value&) -> Status;
  auto step() -> Result<bool>;                           // true=有行；false=完成
  auto column_count() const -> int;
  auto column(int index) -> Value;                       // 当前行的第 index 列
  auto column_name(int index) const -> std::string_view;
  auto reset() -> Status;                                // 复用语句
 private:
  ...
};

class Database {
 public:
  static auto open(std::string_view path, const OpenOptions& = {}) -> Result<Database>;
  auto close() -> Status;                                // 幂等
  auto exec(std::string_view sql) -> Status;             // 多语句 DDL/脚本
  auto prepare(std::string_view sql) -> Result<Statement>;
  auto query(std::string_view sql, const std::vector<Value>& args = {}) -> Result<Rows>;
  auto last_insert_rowid() const -> std::int64_t;
  auto changes() const -> std::int64_t;
  auto begin() -> Status; auto commit() -> Status; auto rollback() -> Status;
  static auto version() -> std::string_view;             // "3.50.2"（host 侧自检）
};

}  // namespace st::ext
```

错误码映射（`SqlError → st::ErrorCode`）的要点：
`SQLITE_CONSTRAINT` → `Exists`（唯一/主键冲突）或 `Invalid`（CHECK/NOT NULL 违反）、
`SQLITE_READONLY`/`SQLITE_PERM` → `Permission`、`SQLITE_BUSY`/`LOCKED` → `Busy`、
`SQLITE_CORRUPT`/`NOTADB` → `Parse`、`SQLITE_FULL` → `Overflow`（磁盘/配额类）、
`SQLITE_MISUSE` → `Internal`（我们自己的调用错，属缺陷）、其余 → `Io`。
**映射表是唯一真相源**，测试里逐条断言——"错误分类"是应用能写出正确重试逻辑的前提。

## 6. 安全姿态（与框架其它外部能力的口径一致）

- **不加载原生扩展**（编译期剔除，`sqlite3_load_extension` 与 `load_extension()` 都不存在）；
- `DQS=0` + 外键默认开 + `API_ARMOR`：把"常见的静默错误"变成"当场可见的错误"；
- 框架**不替应用决定数据文件放哪**（`Database::open` 收路径，应用自己决定；临时库走 `:memory:`）；
- 明确**不做**：数据库加密（SQLCipher 是另一套源码与另一份许可，需要时另立依赖）、网络化（那是服务端的事）。

## 7. 这次**不做**的三件事（以及为什么）

| 未做 | 理由 |
|---|---|
| 控制通道的 `db.*` 方法（让智能体远程建表/查询） | 数据库是**应用私有状态**，把它整片开给远控通道等于把"任意 SQL 执行"暴露给网络上任何一个能连到控制端口的人。要暴露就先做"白名单 + 只读 + 显式开关"，那是独立一轮设计 |
| 声明式层 `useDatabase`（JS 直接读写库） | 同上：脚本沙箱的现有姿态是**不给系统访问**，给数据库访问要先把"哪个应用、哪张表、什么权限"这道闸门设计出来 |
| ORM / 迁移框架 | 框架不该替应用选 ORM；迁移工具在"表结构一改就崩"之前都是过度设计。真要做，也应该是一个**基于 schema 快照 diff 的独立工具**，而不是数据库封装的附属品 |

## 8. 风险与代价（实测）

| 风险 | 量化 / 处置 |
|---|---|
| 首次全量构建变慢 | `sqlite3.c` 实测 **17.3 s @ `-O1 -g1`**，一次性；其后有对象缓存（改无关文件不重编） |
| 产物体积 | 静态库 +≈1 MB 级（`sqlite3.o` 见构建输出）；最终可执行文件只把它真正用到的部分链进去 |
| san 档盲区 | sqlite3.c 不插桩（见 §4），封装层照常插桩 |
| 上游升级 | 换 3 个文件 + 更新 `CHECKSUMS.sha256`/`PROVENANCE.md`；开关在同一个头里，不涉及重放补丁 |
| 多工具链 | 源码内联天然支持交叉编译（`--toolchain=mingw` 用目标编译器重编），无预编译产物问题 |

## 9. 验收清单

- [x] `st build gallery` 通过（首次含 sqlite3.c 编译）
- [x] `st test` 全绿（含 `tests/ext_database_test.cpp`）
- [x] `st lint` 0 违规（`third_party/` 不在扫描根内，见 `lint_project` 的 5 个根）
- [x] `sha256sum -c third_party/CHECKSUMS.sha256` 通过
- [x] 配置自检：FTS5 检索、R-tree 范围查询、dbstat、数学函数、DQS=0、外键默认、`load_extension` 不存在
- [x] 文档同步：README / CONVENTIONS §3.8 / DESIGN §7.4 / SOURCES.md / CHECKSUMS.sha256
- [x] `python tools/check_docs.py` 通过
