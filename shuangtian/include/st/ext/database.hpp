#pragma once

/// 数据库：**基于 SQLite**（`third_party/sqlite/`，3.50.2，public domain）——源码内置的嵌入式数据库。
///
/// 定位：给应用与独立工程一份**零安装、无 ABI 版本问题、跨平台同版本**的结构化存储。
/// 上游推荐的正是这种用法（amalgamation 源码与自己的一起编），本框架的第三方通道
/// （`st.pkg` 的 `third_party_sources`）与它天然吻合——详见 `docs/sqlite_integration.md`。
///
/// ## 为什么公共头里没有 `sqlite3.h`
///
/// `sqlite3.h` 有 660 KB、600+ 个 C 符号。若它出现在本头里，**每一个**包含 `database.hpp`
/// 的翻译单元都要解析它（编译期成本），而且 `sqlite3_*` 会随处可用——"用 C API 绕过封装"
/// 于是变成随手可做的事，封装层的错误码映射与线程承诺就形同虚设。
/// 因此这里只**前置声明** `sqlite3` / `sqlite3_stmt`，把指针包在 `std::unique_ptr`（带自定义
/// 删除器）/`std::shared_ptr` 里——句柄的生命周期由 RAII 保证，C 类型永远不必出现在类型签名上。
///
/// ## 错误模型
///
/// 一切失败经 `Result`/`Status` 返回（`CONVENTIONS.md` §3.2），**不抛异常**。
/// SQLite 本身是错误码模型，映射规则集中在一处（`map_sqlite_code`）并被测试逐条断言——
/// 因为"这条错误属于哪一类"直接决定应用能否写出正确的重试逻辑（`Busy` 该重试、
/// `Exists` 该换键、`Permission` 该提示用户）。
///
/// ## 线程模型
///
/// 库以 `SQLITE_THREADSAFE=1`（serialized）编译：**一个连接可被多个线程调用**，库内自带互斥。
/// 但本封装的建议口径更严：**一个 `Database` 归一个线程用**——需要并发就读写分离、
/// 每线程各持一个连接（配 WAL 模式）；理由是"谁在什么时候用了这个连接"这件事，
/// 想清楚比"理论上可以共享"值钱。`no_mutex` 选项用于**显式声明**单线程使用以省掉互斥开销。
///
/// `Statement` 不可复制（只可移动）：复制句柄会让两条执行流共用一个游标，是最隐蔽的一类缺陷。
///
/// ## 安全姿态
///
/// - **不加载原生扩展**：编译期 `SQLITE_OMIT_LOAD_EXTENSION`，`sqlite3_load_extension()` 与
///   `load_extension()` SQL 函数都不存在——与脚本层同一姿态：可执行代码入口必须显式设计；
/// - `SQLITE_DQS=0`：SQL 里的双引号只当标识符，写错引号**当场报错**而不是静默变字符串；
/// - 外键默认打开（`SQLITE_DEFAULT_FOREIGN_KEYS=1`）、API 入口带参数防御（`SQLITE_ENABLE_API_ARMOR`）。
///
/// 逐项编译开关在 `third_party/sqlite/st_sqlite3_config.h`（**上游文件一字未改**）。

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "st/core/error.hpp"
#include "st/core/fs.hpp"

/// 前置声明：公共头不引 `sqlite3.h`（理由见上）。
struct sqlite3;
struct sqlite3_stmt;

namespace st::ext {

/// 连接内部状态（定义在 `src/ext/database.cpp`）。
///
/// `Database` 与 `Statement` **共享**它（`shared_ptr`）：语句比连接对象活得久时，
/// 底层连接由最后释放的那一方关闭（`sqlite3_close_v2` 的延后关闭语义）。
/// 这样"语句悬在一个已析构的 Database 上"不是未定义行为，而是"连接还活着，用到最后一个引用消失"。
namespace detail {
struct Connection;
}  // namespace detail

/// 列值：SQLite 的 5 种存储类别（NULL / INTEGER / REAL / TEXT / BLOB）一一对应。
///
/// 用 `variant` 而不是"按需调 `column_int()` / `column_text()`"的散接口：**类型是值的一部分**。
/// 取值即穷举，调用方不可能"忘了判 NULL 就直接当字符串拼进去"。
using Blob = fs::Bytes;
using Value = std::variant<std::nullptr_t, std::int64_t, double, std::string, Blob>;

/// 便捷构造（避免调用点写 `Value{std::in_place_index<1>, ...}` 这种位置编码）。
[[nodiscard]] auto value_null() -> Value;
[[nodiscard]] auto value_integer(std::int64_t number) -> Value;
[[nodiscard]] auto value_real(double number) -> Value;
[[nodiscard]] auto value_text(std::string_view text) -> Value;
[[nodiscard]] auto value_blob(std::span<const std::uint8_t> bytes) -> Value;

/// 打开选项（具名开关，而不是 `SQLITE_OPEN_*` 位掩码：随手 `|=` 出个非法组合是常见事故）。
struct OpenOptions {
  /// 只读打开：写操作直接失败（`Permission`），不会"看着是只读、其实改了文件"。
  bool read_only{false};
  /// 允许创建（`read_only` 时忽略）。
  bool create{true};
  /// 内存库（`:memory:`）：进程退出即消失，测试与临时计算用；此时 `path` 被忽略。
  bool memory{false};
  /// 显式声明"本连接只在一个线程用"（`SQLITE_OPEN_NOMUTEX`）：省掉每次调用的互斥开销。
  /// 用它就要**真的**保证单线程——否则是数据竞争，且没有任何检查会提醒你。
  bool no_mutex{false};
  /// 忙等超时：库被别的连接写锁住时，先等这么久再报 `Busy`（而不是立刻失败）。
  /// 默认 5 s——多进程/多连接场景下"立刻失败"几乎全是假警报。
  std::chrono::milliseconds busy_timeout{5000};
  /// 自定义 VFS 名（空 = 默认）。高级用法：内存 VFS、加密 VFS 等由宿主注册后在此指定。
  std::string vfs{};
};

/// 列的元信息（编译期开关 `SQLITE_ENABLE_COLUMN_METADATA` 提供来源信息）。
struct Column {
  std::string name{};           ///< 结果列名（`SELECT a AS x` 取 `x`）
  std::string declared_type{};  ///< 声明类型（`TEXT`/`INTEGER`…）；表达式列可能为空
  std::string table_name{};     ///< 来源表名（表达式列/无来源时为空）
  std::string origin_name{};    ///< 来源列名（同上）
};

/// 一行：按列序的 `Value`。
using Row = std::vector<Value>;

/// 查询结果：列元信息 + 全部行。
///
/// 为什么一次性物化而不是"游标式逐行拉"：SQL 结果集在本框架的用法里是**给界面/序列化用**的，
/// 需要知道总行数、需要多次遍历；而"边 step 边渲染"会让调用方承担游标生命周期。
/// 真要流式处理超大结果集，用 `Statement` 逐行 `step()`（那条路一直是开着的）。
struct QueryResult {
  std::vector<Column> columns{};
  std::vector<Row> rows{};
  /// 是否因上限而截断（`query()` 的 `max_rows` 参数）。
  bool truncated{false};
};

/// 预编译语句（只可移动）。
///
/// 复用一条语句（`reset` + 重新 `bind`）比每次 `prepare` 快得多——SQL 解析与查询规划
/// 是数据库里最贵的一段。批量写入请**循环用同一条 Statement**。
class Statement {
 public:
  Statement() = default;
  ~Statement();
  Statement(Statement&&) noexcept;
  auto operator=(Statement&&) noexcept -> Statement&;
  Statement(const Statement&) = delete;
  auto operator=(const Statement&) -> Statement& = delete;

  /// 是否持有有效句柄（默认构造或已移动走的为否）。
  [[nodiscard]] auto valid() const noexcept -> bool;

  /// 该语句的 SQL 文本（语句为准备时的原文）。
  [[nodiscard]] auto sql() const -> std::string;

  /// 参数个数（`?` 与具名参数合计；具名参数在 SQL 里出现多次只算一个）。
  ///
  /// 参数序号是 SQLite 的 1 起下标，而下面 `value`/`read_row()` 的列下标是 0 起
  /// （与 SQLite 的 C API 一致）。两套下标在这里**刻意保留**而不是统一改写：
  /// 调用方看到的数字必须与 SQL 和 SQLite 文档对得上，"框架自作主张减一"才是灾难。
  [[nodiscard]] auto parameter_count() const -> int;

  /// 绑定第 `index` 个参数（**1 起**，与 SQLite 一致）。
  /// 错误：`Invalid`（越界）、`Io`（内存不足等）。
  [[nodiscard]] auto bind(int index, const Value& value) -> Status;

  /// 按名字绑定。
  ///
  /// `name` 可以带前缀（`:name` / `@name` / `$name`）也可以不带——不带时依次尝试三种前缀。
  /// 带前缀的写法更精确（SQLite 允许同一句里同时存在 `:a` 与 `@a`），无前缀仅为了少敲一个字符。
  /// 错误：`NotFound`（无此参数名）、`Invalid`、`Io`。
  [[nodiscard]] auto bind(std::string_view name, const Value& value) -> Status;

  /// 按顺序绑定全部参数（`values[i]` → 第 `i+1` 个）。
  [[nodiscard]] auto bind_all(const std::vector<Value>& values) -> Status;

  /// 推进一行：`true` = 取到一行（可用 `value()` 读），`false` = 执行完成。
  /// 错误：约束冲突、只读、I/O 等（映射后的 `Error`）。
  [[nodiscard]] auto step() -> Result<bool>;

  /// 复位以便再次执行（**不清除**绑定值；要清用 `clear_bindings`）。
  [[nodiscard]] auto reset() -> Status;

  /// 清除全部绑定值（复位为 NULL）。
  [[nodiscard]] auto clear_bindings() -> Status;

  /// 结果列数（DDL/DML 语句为 0）。
  [[nodiscard]] auto column_count() const -> int;

  /// 列元信息（只读语句才有意义）。
  [[nodiscard]] auto columns() const -> std::vector<Column>;

  /// 当前行第 `index` 列的值（**0 起**，与 SQLite 一致）。
  /// 越界返回 NULL 值（不抛异常、不终止进程）——配合 `column_count()` 使用。
  [[nodiscard]] auto value(int index) const -> Value;

  /// 当前行整行（等价于按 `column_count()` 逐个 `value`）。
  [[nodiscard]] auto read_row() const -> Row;

  /// 最近一次失败（成功调用后不保证被清空——它是"诊断快照"，不是状态机）。
  [[nodiscard]] auto last_error() const -> Error;

 private:
  friend class Database;
  Statement(std::shared_ptr<detail::Connection> connection, sqlite3_stmt* handle);

  std::shared_ptr<detail::Connection> connection_{};
  sqlite3_stmt* handle_{nullptr};
};

/// 数据库连接。
class Database {
 public:
  Database() = default;
  ~Database();
  Database(Database&&) noexcept;
  auto operator=(Database&&) noexcept -> Database&;
  Database(const Database&) = delete;
  auto operator=(const Database&) -> Database& = delete;

  /// 打开（或创建）数据库。`path` 为 UTF-8 路径；`options.memory` 时用内存库。
  /// 错误：`Invalid`（路径为空且非内存库）、`Permission`、`NotFound`/`Io`（打不开）、`Busy` 等。
  [[nodiscard]] static auto open(std::string_view path, const OpenOptions& options = {})
      -> Result<Database>;

  /// 是否持有可用连接。
  [[nodiscard]] auto valid() const noexcept -> bool;

  /// 释放本引用。有活跃语句时底层连接**延后**关闭（等最后一条语句释放）——
  /// 这不是妥协，而是 `sqlite3_close_v2` 的正式语义：避免"语句还在跑、连接被关掉"的悬垂。
  auto close() noexcept -> void;

  /// 执行一段 SQL 脚本（可分多条语句，如建表 + 建索引）。无结果行。
  /// 错误：`Parse`（语法）、`Io`、`Busy`、`Exists`/`Invalid`（约束）等。
  [[nodiscard]] auto exec(std::string_view sql) -> Status;

  /// 预编译一条 SQL。错误：`Parse`（语法）、`Io`。
  [[nodiscard]] auto prepare(std::string_view sql) -> Result<Statement>;

  /// 查询并物化结果（无参数版本）。
  [[nodiscard]] auto query(std::string_view sql) -> Result<QueryResult>;
  /// 查询并物化结果。`max_rows` 为 0 = 不限；超限即截断并置 `truncated`。
  [[nodiscard]] auto query(std::string_view sql, const std::vector<Value>& arguments,
                           std::size_t max_rows = 0) -> Result<QueryResult>;

  /// 执行一条带参数的写语句，返回受影响行数（`sqlite3_changes`）。
  /// 错误同 `exec`；`Overflow`（结果行数超 int64 可表示范围的情形不在此列——此处只映射数据库错误）。
  [[nodiscard]] auto execute(std::string_view sql, const std::vector<Value>& arguments)
      -> Result<std::int64_t>;

  /// 事务：`begin` → 若干语句 → `commit`（失败则 `rollback`）。
  ///
  /// **支持嵌套**：SQLite 没有真正的嵌套事务，这里按 SAVEPOINT 语义实现——
  /// 外层 `begin` 发 `BEGIN`，内层 `begin` 发 `SAVEPOINT st_nest_N`，
  /// 内层 `commit` 变成 `RELEASE`。因此"函数内部开事务、被别的事务包住"能正常工作，
  /// 而不是像裸 `BEGIN` 那样报 "cannot start a transaction within a transaction"。
  [[nodiscard]] auto begin() -> Status;
  [[nodiscard]] auto commit() -> Status;
  [[nodiscard]] auto rollback() -> Status;

  /// 当前事务嵌套深度（0 = 不在事务里）。
  [[nodiscard]] auto transaction_depth() const noexcept -> int;

  /// 显式 SAVEPOINT（需要"部分回滚"时用它，而不是自己拼 SQL 字符串）。
  [[nodiscard]] auto savepoint(std::string_view name) -> Status;
  [[nodiscard]] auto release_savepoint(std::string_view name) -> Status;
  [[nodiscard]] auto rollback_to_savepoint(std::string_view name) -> Status;

  /// 最近一次 INSERT 的 rowid（无则 0）。
  [[nodiscard]] auto last_insert_rowid() const -> std::int64_t;
  /// 最近一条语句改动的行数。
  [[nodiscard]] auto changes() const -> std::int64_t;
  /// 连接累计改动的行数（自连接打开起）。
  [[nodiscard]] auto total_changes() const -> std::int64_t;

  /// 最近一次错误（含映射后的分类与 SQLite 的原始消息）。
  [[nodiscard]] auto last_error() const -> Error;
  /// 连接上的错误消息原文（空连接返回空串）。
  [[nodiscard]] auto error_message() const -> std::string;

  /// 打开时使用的路径（内存库为 `":memory:"`）。
  [[nodiscard]] auto path() const -> std::string;

  /// 库版本（如 `"3.50.2"`）与源码标识（用于把"数据文件是哪个版本写的"记录进日志/审计）。
  [[nodiscard]] static auto version() -> std::string_view;
  [[nodiscard]] static auto source_id() -> std::string_view;

 private:
  std::shared_ptr<detail::Connection> connection_{};
};

/// 事务守卫：构造即 `begin`，析构时**未提交则回滚**（RAII）。
///
/// 作用域退出路径（提前 `return`、`break`、异常外的任何跳出）都不会留下半个事务——
/// 这正是"手工 begin/commit 配对"最容易漏的地方。
class Transaction {
 public:
  explicit Transaction(Database& database);
  ~Transaction();
  Transaction(Transaction&&) noexcept;
  auto operator=(Transaction&&) noexcept -> Transaction&;
  Transaction(const Transaction&) = delete;
  auto operator=(const Transaction&) -> Transaction& = delete;

  /// 构造时的 `begin` 是否成功（失败时守卫不做任何事，析构也不会回滚别人的事务）。
  [[nodiscard]] auto active() const noexcept -> bool;
  /// 提交（成功后再析构不做任何事）。错误：`Invalid`（已结束）、数据库错误。
  [[nodiscard]] auto commit() -> Status;
  /// 回滚（成功后再析构不做任何事）。
  [[nodiscard]] auto rollback() -> Status;

 private:
  Database* database_{nullptr};
  bool active_{false};
};

/// SQLite 错误码 → 框架错误分类（**唯一真相源**，被 `tests/ext_database_test.cpp` 逐条断言）。
///
/// `code` 传**扩展**错误码（`sqlite3_extended_errcode` 的返回值）；函数内部自行取主码。
/// 分类的用途是让应用能写出正确反应：`Busy` 该退避重试、`Exists` 该换键、
/// `Permission` 该提示用户、`Parse` 该视为数据损坏而非"我 SQL 写错了"。
[[nodiscard]] auto map_sqlite_code(int code) -> ErrorCode;

}  // namespace st::ext
