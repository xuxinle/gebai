#include "st/ext/database.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstring>
#include <format>
#include <memory>
#include <numeric>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "st/core/string.hpp"
#include "sqlite3.h"

namespace st::ext {

/// 连接内部状态：C 连接指针 + 事务深度 + 诊断快照。
///
/// **本文件是框架里唯一把 sqlite3 类型写进类型的翻译单元**——`Database`/`Statement` 只持有
/// 指向它的 `shared_ptr`。公共头（`include/st/ext/database.hpp`）因此不必包含 660 KB 的
/// `sqlite3.h`，上层也拿不到任何 `sqlite3_*` 符号（"绕过封装"从"随手可做"变成"需要刻意"）。
///
/// ## 生命周期与引用计数
///
/// 原生连接由**最后一个引用**关闭（`Database` 与它派出的 `Statement` 一起计数）：
/// 语句比连接对象活得久时，连接是"僵尸"状态但依然可用——这正是 `sqlite3_close_v2` 的正式语义
/// （delayed close：等所有语句 finalize 后才真正释放）。于是
/// "语句悬在一个已析构的 Database 上"不是未定义行为，而是被精确定义的资源释放顺序。
///
/// ## 事务深度
///
/// SQLite 没有真正的嵌套事务。这里用 SAVEPOINT 模拟（见 `Database::begin`）：
/// `depth` 记录当前嵌套层数，0 = 不在事务里。
struct detail::Connection {
  sqlite3* handle{nullptr};
  std::string path{};        ///< 打开时的路径（内存库为 ":memory:"）
  int transaction_depth{0};  ///< 事务嵌套深度（0 = 无事务）
  int last_code{SQLITE_OK};
  std::string last_message{};

  ~Connection() noexcept {
    if (handle != nullptr) {
      // `_v2`：有活跃语句时转为僵尸、延后释放——**正是引用计数这里需要的语义**。
      sqlite3_close_v2(handle);
      handle = nullptr;
    }
  }

  Connection(const Connection&) = delete;
  auto operator=(const Connection&) -> Connection& = delete;
  Connection() = default;

  /// 记录一次失败（分类取自 SQLite 自己的扩展错误码与消息）。
  void record_error(int code) {
    last_code = code;
    const char* text = handle != nullptr ? sqlite3_errmsg(handle) : sqlite3_errstr(code);
    last_message = text != nullptr ? std::string(text) : std::string(sqlite3_errstr(code));
  }

  void clear_error() {
    last_code = SQLITE_OK;
    last_message.clear();
  }

  [[nodiscard]] auto failure() const -> Err {
    return unexpected(map_sqlite_code(last_code), error_text());
  }

  [[nodiscard]] auto error_text() const -> std::string {
    const std::string_view message =
        last_message.empty() ? std::string_view(sqlite3_errstr(last_code)) : std::string_view(last_message);
    if (message.empty()) return std::string(st::to_string(map_sqlite_code(last_code)));
    return std::format("{}{}", message, last_code == SQLITE_OK ? "" : std::format(" (sqlite {})", last_code));
  }
};

namespace {

/// 把 C 侧返回值转成 `Status`（成功即 `ok()`，失败记录诊断并映射分类）。
///
/// 单点集中"记录 + 映射"是刻意的：散落各处写 `unexpected(map_sqlite_code(rc), sqlite3_errmsg(db))`
/// 会漏记诊断（`last_error()` 于是返回上一次的错误，误导排查），也会漏掉某些调用点的映射。
[[nodiscard]] auto check(const std::shared_ptr<detail::Connection>& connection, int code) -> Status {
  if (code == SQLITE_OK || code == SQLITE_ROW || code == SQLITE_DONE) return ok();
  if (connection) connection->record_error(code);
  return connection ? connection->failure()
                    : unexpected(map_sqlite_code(code), std::string(sqlite3_errstr(code)));
}

/// UTF-8 路径 → 文件名探测：SQLite 的 `open_v2` 自己处理 UTF-8；
/// 这里只区分"必需参数"与"目标不存在"。
[[nodiscard]] auto is_uri(std::string_view path) noexcept -> bool {
  return path.starts_with("file:");
}

/// 整数列的**原样**取出。
///
/// `sqlite3_column_int64` 会按 C 语义把 TEXT/REAL 也一并转换，而我们要的是
/// "这一列**存的就是**整数"。先取文本再解析，得到的就是它自己的字节；解析失败才退回
/// `column_int64`——这一步让 `Value` 的类型与 SQLite 报告的**存储类别**严格一致，
/// 不引入静默转换（"数值在往返中被改成另一种类型"是数据层最难查的一类问题）。
[[nodiscard]] auto snapshot_int64(sqlite3_stmt* handle, int index) -> std::int64_t {
  const unsigned char* raw = sqlite3_column_text(handle, index);
  const int size = sqlite3_column_bytes(handle, index);
  if (raw != nullptr && size > 0) {
    // `void*` 中转（而非 `reinterpret_cast`）：任意对象指针 → `void*` 是隐式转换，
    // `void*` → `char*` 是 `static_cast` 的明文允许项——两者都是语言里的正规转换，
    // 不碰 `reinterpret_cast`（`CONVENTIONS.md` §2 的 R6 禁用它）。
    const void* generic = raw;
    const auto* first = static_cast<const char*>(generic);
    const auto* last = first + size;
    std::int64_t parsed = 0;
    if (const auto outcome = std::from_chars(first, last, parsed);
        outcome.ec == std::errc{} && outcome.ptr == last) {
      return parsed;
    }
  }
  return sqlite3_column_int64(handle, index);
}

/// 绑定文本/BLOB：**自己拷一份**交给 SQLite 托管。
///
/// 为什么不用上游的 `SQLITE_TRANSIENT`：它是 C 风格转换的宏
/// （`#define SQLITE_TRANSIENT ((sqlite3_destructor_type)-1)`），在本框架的
/// `-Wold-style-cast` 严格集下不可用。改用"拷一份 + 把所有权交给 SQLite"
/// （析构器取本库自己的分配器 `sqlite3_free`，配套无错），效果完全一致：
/// 绑定返回后调用方的字符串随即可释放，不存在悬垂。
///
/// **空值 vs 空串的坑**（实测踩到）：`sqlite3_bind_blob/text` 的**数据指针为 NULL 时
/// 整个调用退化成 `sqlite3_bind_null`**（上游文档明说）。于是
/// "空字符串 / 空 BLOB" 会静默变 NULL——应用写 `value_text("")` 却存进 NULL，
/// 读回来是 `nullptr` 而不是空串。所以这里哪怕长度为 0 也**分配一个字节**：
/// 指针非 NULL + 长度 0 ⇒ 真正的空串/空 BLOB（与 SQLite 的类型五分法一致）。
///
/// 所有权约定（看着过上游 `bindText()` 确认过）：无论绑定成功还是失败，
/// 只要 destructor 不是 `SQLITE_STATIC`/`SQLITE_TRANSIENT`，SQLite **都会**调用它——
/// 因此这里不需要（也不能）在失败路径上自己释放。
[[nodiscard]] auto bind_bytes(sqlite3_stmt* handle, int index, const void* data, int size,
                              bool as_text) -> int {
  const auto capacity = static_cast<sqlite3_uint64>(size > 0 ? size : 1);
  void* copy = sqlite3_malloc64(capacity);
  if (copy == nullptr) return SQLITE_NOMEM;
  if (size > 0 && data != nullptr) std::memcpy(copy, data, static_cast<std::size_t>(size));
  if (as_text) {
    return sqlite3_bind_text(handle, index, static_cast<const char*>(copy), size, sqlite3_free);
  }
  return sqlite3_bind_blob(handle, index, copy, size, sqlite3_free);
}

/// 标识符按 SQL 规范用双引号包起来（内部的双引号翻倍）。
///
/// 为什么必须有它：savepoint 名字来自调用方（甚至可能来自配置），而
/// **savepoint 的语法位置不接受参数绑定**——只能由我们自己转义。
/// 直接拼字符串是教科书级的注入口（`a"; DROP TABLE t; --` 就能得手）。
[[nodiscard]] auto quote_identifier(std::string_view name) -> std::string {
  std::string out;
  out.reserve(name.size() + 2);
  out.push_back('"');
  for (const char glyph : name) {
    if (glyph == '"') out.push_back('"');
    out.push_back(glyph);
  }
  out.push_back('"');
  return out;
}

/// 嵌套事务内部用的 savepoint 名（按深度编号，调用方看不见）。
[[nodiscard]] auto nest_savepoint_name(int depth) -> std::string {
  return std::format("st_nest_{}", depth);
}

[[nodiscard]] auto savepoint_sql(int depth) -> std::string {
  return std::format("SAVEPOINT {}", nest_savepoint_name(depth));
}

[[nodiscard]] auto release_savepoint_sql(int depth) -> std::string {
  return std::format("RELEASE {}", nest_savepoint_name(depth));
}

[[nodiscard]] auto rollback_savepoint_sql(int depth) -> std::string {
  return std::format("ROLLBACK TO {}", nest_savepoint_name(depth));
}

}  // namespace

// ————————————————————————————————————————————————————————————————————————————
// 值构造
// ————————————————————————————————————————————————————————————————————————————

auto value_null() -> Value { return Value{nullptr}; }
auto value_integer(std::int64_t number) -> Value { return Value{number}; }
auto value_real(double number) -> Value { return Value{number}; }
auto value_text(std::string_view text) -> Value { return Value{std::string(text)}; }
auto value_blob(std::span<const std::uint8_t> bytes) -> Value {
  return Value{Blob(bytes.begin(), bytes.end())};
}

// ————————————————————————————————————————————————————————————————————————————
// 错误码映射（唯一真相源）
// ————————————————————————————————————————————————————————————————————————————

auto map_sqlite_code(int code) -> ErrorCode {
  // ① **扩展码专有**的细分先处理（它们的低位是主码，落到下面会被主码吸走）：
  //    唯一/主键冲突是"目标已存在"，而 CHECK/NOT NULL/外键违反是"输入非法"——
  //    两者对调用方的含义不同（一个该换键、一个该改数据）。
  switch (code) {
    case SQLITE_CONSTRAINT_UNIQUE:
    case SQLITE_CONSTRAINT_PRIMARYKEY:
    case SQLITE_CONSTRAINT_ROWID:
      return ErrorCode::Exists;
    case SQLITE_IOERR_ACCESS:
      return ErrorCode::Permission;
    default:
      break;
  }
  // ② 其余按**主码**（低 8 位）分类：扩展位只携带更细的原因，不改变错误的种类。
  switch (code & 0xFF) {
    case SQLITE_CONSTRAINT:
      return ErrorCode::Invalid;
    case SQLITE_ABORT:
    case SQLITE_INTERRUPT:
      return ErrorCode::Cancelled;
    case SQLITE_READONLY:
    case SQLITE_PERM:
    case SQLITE_AUTH:
      return ErrorCode::Permission;
    case SQLITE_BUSY:
    case SQLITE_LOCKED:
    case SQLITE_PROTOCOL:
      // 锁竞争类：调用方的正确反应是**退避重试**（连接已配 `busy_timeout`，
      // 走到这里说明连超时都等完了——那就是真忙）。
      return ErrorCode::Busy;
    case SQLITE_CORRUPT:
    case SQLITE_NOTADB:
    case SQLITE_FORMAT:
      // 数据文件不是我们以为的东西：**不是**"SQL 写错了"，而是"库坏了/不是库"。
      return ErrorCode::Parse;
    case SQLITE_FULL:
    case SQLITE_TOOBIG:
      return ErrorCode::Overflow;
    case SQLITE_CANTOPEN:
    case SQLITE_EMPTY:
    case SQLITE_NOTFOUND:
      return ErrorCode::NotFound;
    case SQLITE_NOLFS:
      // 平台不支持该特性（大文件等）——属于"这个环境干不了这件事"。
      return ErrorCode::Unsupported;
    case SQLITE_RANGE:
      // 参数序号越界；本封装的 `bind` 已前置检查，走到这里意味着内部两处判断分叉了。
      return ErrorCode::Invalid;
    case SQLITE_MISUSE:
    case SQLITE_INTERNAL:
    case SQLITE_SCHEMA:
      // 我们自己的调用方式违反了 API 契约——这是**框架缺陷**，不是用户错误。
      // 单独分类的价值：它在测试里应当被当成红灯，而在应用日志里一眼可辨。
      return ErrorCode::Internal;
    case SQLITE_NOMEM:
    case SQLITE_ERROR:
    case SQLITE_IOERR:
    default:
      return ErrorCode::Io;
  }
}

// ————————————————————————————————————————————————————————————————————————————
// Statement
// ————————————————————————————————————————————————————————————————————————————

Statement::Statement(std::shared_ptr<detail::Connection> connection, sqlite3_stmt* handle)
    : connection_(std::move(connection)), handle_(handle) {}

Statement::~Statement() {
  if (handle_ != nullptr) sqlite3_finalize(handle_);
}

Statement::Statement(Statement&& other) noexcept
    : connection_(std::move(other.connection_)), handle_(other.handle_) {
  other.handle_ = nullptr;
}

auto Statement::operator=(Statement&& other) noexcept -> Statement& {
  if (this != &other) {
    if (handle_ != nullptr) sqlite3_finalize(handle_);
    connection_ = std::move(other.connection_);
    handle_ = other.handle_;
    other.handle_ = nullptr;
  }
  return *this;
}

auto Statement::valid() const noexcept -> bool { return handle_ != nullptr; }

auto Statement::sql() const -> std::string {
  if (handle_ == nullptr) return {};
  const char* text = sqlite3_sql(handle_);
  return text != nullptr ? std::string(text) : std::string{};
}

auto Statement::parameter_count() const -> int {
  return handle_ != nullptr ? sqlite3_bind_parameter_count(handle_) : 0;
}

auto Statement::bind(int index, const Value& value) -> Status {
  if (handle_ == nullptr) return unexpected(ErrorCode::Invalid, "语句无效（默认构造或已移动）");
  if (index < 1 || index > parameter_count()) {
    return unexpected(ErrorCode::Invalid,
                      std::format("参数序号 {} 越界（本语句 {} 个参数，序号从 1 起）", index,
                                  parameter_count()));
  }
  int code = SQLITE_OK;
  std::visit(
      [&](const auto& item) {
        using Kind = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Kind, std::nullptr_t>) {
          code = sqlite3_bind_null(handle_, index);
        } else if constexpr (std::is_same_v<Kind, std::int64_t>) {
          code = sqlite3_bind_int64(handle_, index, item);
        } else if constexpr (std::is_same_v<Kind, double>) {
          code = sqlite3_bind_double(handle_, index, item);
        } else if constexpr (std::is_same_v<Kind, std::string>) {
          code = bind_bytes(handle_, index, item.data(), static_cast<int>(item.size()), true);
        } else {
          code = bind_bytes(handle_, index, item.empty() ? nullptr : item.data(),
                            static_cast<int>(item.size()), false);
        }
      },
      value);
  return check(connection_, code);
}

auto Statement::bind(std::string_view name, const Value& value) -> Status {
  if (handle_ == nullptr) return unexpected(ErrorCode::Invalid, "语句无效（默认构造或已移动）");
  const std::string key(name);
  int index = sqlite3_bind_parameter_index(handle_, key.c_str());
  // 无前缀名字：依次试 `:` / `@` / `$`——SQLite 的索引查询是**精确匹配**（含前缀字符），
  // 因此"前缀可省"这件事必须由我们这里补上（而不是碰运气）。
  if (index == 0 && !name.empty() && name.front() != ':' && name.front() != '@' &&
      name.front() != '$') {
    for (const char prefix : {':', '@', '$'}) {
      const std::string candidate = std::string(1, prefix).append(key);
      index = sqlite3_bind_parameter_index(handle_, candidate.c_str());
      if (index != 0) break;
    }
  }
  if (index == 0) {
    return unexpected(ErrorCode::NotFound,
                      std::format("语句里没有参数 {}（具名参数需与 SQL 中写法一致）", key));
  }
  return bind(index, value);
}

auto Statement::bind_all(const std::vector<Value>& values) -> Status {
  if (handle_ == nullptr) return unexpected(ErrorCode::Invalid, "语句无效（默认构造或已移动）");
  if (static_cast<int>(values.size()) != parameter_count()) {
    return unexpected(ErrorCode::Invalid,
                      std::format("参数个数不符：语句要 {} 个，给了 {}", parameter_count(),
                                  values.size()));
  }
  for (std::size_t position = 0; position < values.size(); ++position) {
    if (auto status = bind(static_cast<int>(position) + 1, values[position]); !status) {
      return status;
    }
  }
  return ok();
}

auto Statement::step() -> Result<bool> {
  if (handle_ == nullptr) return unexpected(ErrorCode::Invalid, "语句无效（默认构造或已移动）");
  const int code = sqlite3_step(handle_);
  if (code == SQLITE_ROW) {
    if (connection_) connection_->clear_error();
    return true;
  }
  if (code == SQLITE_DONE) {
    if (connection_) connection_->clear_error();
    return false;
  }
  if (connection_) connection_->record_error(code);
  return connection_ ? Result<bool>(connection_->failure()) : Result<bool>(unexpected(map_sqlite_code(code), sqlite3_errstr(code)));
}

auto Statement::reset() -> Status {
  if (handle_ == nullptr) return unexpected(ErrorCode::Invalid, "语句无效（默认构造或已移动）");
  // `sqlite3_reset` 会把上一次执行的错误作为返回值带回来（"上一次失败了"这件事不因复位消失），
  // 但那不是一次新的失败：这里只在**复位本身**失败时上报（API misuse）。
  const int code = sqlite3_reset(handle_);
  if (code == SQLITE_OK) return ok();
  return check(connection_, code);
}

auto Statement::clear_bindings() -> Status {
  if (handle_ == nullptr) return unexpected(ErrorCode::Invalid, "语句无效（默认构造或已移动）");
  return check(connection_, sqlite3_clear_bindings(handle_));
}

auto Statement::column_count() const -> int {
  return handle_ != nullptr ? sqlite3_column_count(handle_) : 0;
}

auto Statement::columns() const -> std::vector<Column> {
  std::vector<Column> out;
  if (handle_ == nullptr) return out;
  const int count = column_count();
  out.reserve(static_cast<std::size_t>(count));
  for (int index = 0; index < count; ++index) {
    Column column;
    if (const char* name = sqlite3_column_name(handle_, index); name != nullptr) {
      column.name = name;
    }
    if (const char* type = sqlite3_column_decltype(handle_, index); type != nullptr) {
      column.declared_type = type;
    }
    // COLUMN_METADATA 的接口：表达式列（如 `count(*)`）没有来源，返回空指针——这不是错误。
    if (const char* table = sqlite3_column_table_name(handle_, index); table != nullptr) {
      column.table_name = table;
    }
    if (const char* origin = sqlite3_column_origin_name(handle_, index); origin != nullptr) {
      column.origin_name = origin;
    }
    out.push_back(std::move(column));
  }
  return out;
}

auto Statement::value(int index) const -> Value {
  if (handle_ == nullptr || index < 0 || index >= column_count()) return value_null();
  switch (sqlite3_column_type(handle_, index)) {
    case SQLITE_INTEGER:
      return Value(snapshot_int64(handle_, index));
    case SQLITE_FLOAT:
      return Value(sqlite3_column_double(handle_, index));
    case SQLITE_TEXT: {
      const unsigned char* raw = sqlite3_column_text(handle_, index);
      const int size = sqlite3_column_bytes(handle_, index);
      if (raw == nullptr || size <= 0) return Value(std::string{});
      // 同上：经 `void*` 中转，不用 `reinterpret_cast`。
      const void* generic = raw;
      const auto* text = static_cast<const char*>(generic);
      return Value(std::string(text, static_cast<std::size_t>(size)));
    }
    case SQLITE_BLOB: {
      const void* data = sqlite3_column_blob(handle_, index);
      const int size = sqlite3_column_bytes(handle_, index);
      if (data == nullptr || size <= 0) return Value(Blob{});
      const auto* bytes = static_cast<const std::uint8_t*>(data);
      return Value(Blob(bytes, bytes + size));
    }
    case SQLITE_NULL:
    default:
      return value_null();
  }
}

auto Statement::read_row() const -> Row {
  Row row;
  const int count = column_count();
  row.reserve(static_cast<std::size_t>(count));
  for (int index = 0; index < count; ++index) row.push_back(value(index));
  return row;
}

auto Statement::last_error() const -> Error {
  if (connection_) return Error{map_sqlite_code(connection_->last_code), connection_->error_text()};
  return Error{ErrorCode::Invalid, "语句无效（默认构造或已移动）"};
}

// ————————————————————————————————————————————————————————————————————————————
// Database
// ————————————————————————————————————————————————————————————————————————————

Database::~Database() { close(); }

Database::Database(Database&& other) noexcept : connection_(std::move(other.connection_)) {}

auto Database::operator=(Database&& other) noexcept -> Database& {
  if (this != &other) {
    close();
    connection_ = std::move(other.connection_);
  }
  return *this;
}

auto Database::open(std::string_view path, const OpenOptions& options) -> Result<Database> {
  if (!options.memory && path.empty()) {
    return unexpected(ErrorCode::Invalid, "数据库路径为空（内存库请设 options.memory = true）");
  }
  const std::string location = options.memory ? std::string(":memory:") : std::string(path);

  int flags = options.read_only ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE;
  if (options.create && !options.read_only) flags |= SQLITE_OPEN_CREATE;
  flags |= options.no_mutex ? SQLITE_OPEN_NOMUTEX : SQLITE_OPEN_FULLMUTEX;
  // URI 形态（`file:...?mode=...&cache=shared` 之类）由调用方显式给出 `file:` 前缀才启用，
  // 避免普通路径里的 `?`/`#` 被当成查询串（Windows 路径里 `#` 并不罕见）。
  if (is_uri(location)) flags |= SQLITE_OPEN_URI;

  auto state = std::make_shared<detail::Connection>();
  state->path = location;
  const char* vfs = options.vfs.empty() ? nullptr : options.vfs.c_str();
  const int code = sqlite3_open_v2(location.c_str(), &state->handle, flags, vfs);
  if (code != SQLITE_OK) {
    // 打开失败时 `handle` 可能是"已分配但不可用"的连接：按 SQLite 的约定必须 close 才能释放。
    state->record_error(code);
    const std::string message = state->error_text();
    if (state->handle != nullptr) {
      sqlite3_close_v2(state->handle);
      state->handle = nullptr;
    }
    return unexpected(map_sqlite_code(code), message);
  }
  // 扩展错误码：后面的错误诊断能区分"UNIQUE 冲突"与"CHECK 违反"（分类更准）。
  sqlite3_extended_result_codes(state->handle, 1);
  if (options.busy_timeout.count() > 0) {
    const auto milliseconds = static_cast<int>(
        std::min<std::int64_t>(options.busy_timeout.count(), 2'147'483'647LL));
    sqlite3_busy_timeout(state->handle, milliseconds);
  }

  Database database;
  database.connection_ = std::move(state);
  return database;
}

auto Database::valid() const noexcept -> bool {
  return connection_ != nullptr && connection_->handle != nullptr;
}

auto Database::close() noexcept -> void { connection_.reset(); }

auto Database::exec(std::string_view sql) -> Status {
  if (!valid()) return unexpected(ErrorCode::Invalid, "连接不可用");
  // 走 prepare/step 而不是 `sqlite3_exec`：多语句脚本要能把**具体是哪一句**失败带回来。
  // `sqlite3_exec` 的错误消息里只有 SQLite 的原文，定位到语句要靠人肉数分号。
  const char* remaining = sql.data();
  const char* end = sql.data() + sql.size();
  while (remaining != nullptr && remaining < end) {
    sqlite3_stmt* handle = nullptr;
    const char* tail = nullptr;
    const int code = sqlite3_prepare_v2(connection_->handle, remaining,
                                        static_cast<int>(end - remaining), &handle, &tail);
    if (code != SQLITE_OK) {
      connection_->record_error(code);
      return connection_->failure();
    }
    remaining = tail;
    if (handle == nullptr) continue;  // 纯空白/注释片段
    const int step_code = sqlite3_step(handle);
    sqlite3_finalize(handle);
    if (step_code != SQLITE_DONE && step_code != SQLITE_ROW) {
      connection_->record_error(step_code);
      return connection_->failure();
    }
  }
  if (connection_) connection_->clear_error();
  return ok();
}

auto Database::prepare(std::string_view sql) -> Result<Statement> {
  if (!valid()) return unexpected(ErrorCode::Invalid, "连接不可用");
  if (sql.empty()) return unexpected(ErrorCode::Invalid, "SQL 为空");
  sqlite3_stmt* handle = nullptr;
  // `prepare_v3` + `PERSISTENT`：把编译结果留在语句上，重复执行的规划成本降到最低。
  const int code = sqlite3_prepare_v3(connection_->handle, sql.data(), static_cast<int>(sql.size()),
                                      SQLITE_PREPARE_PERSISTENT, &handle, nullptr);
  if (code != SQLITE_OK) {
    connection_->record_error(code);
    const std::string message = connection_->error_text();
    if (handle != nullptr) sqlite3_finalize(handle);
    return unexpected(map_sqlite_code(code), message);
  }
  if (handle == nullptr) {
    return unexpected(ErrorCode::Parse, "SQL 里没有可执行的语句（空语句或纯注释）");
  }
  return Statement(connection_, handle);
}

auto Database::query(std::string_view sql) -> Result<QueryResult> {
  return query(sql, {}, 0);
}

auto Database::query(std::string_view sql, const std::vector<Value>& arguments,
                     std::size_t max_rows) -> Result<QueryResult> {
  auto statement = prepare(sql);
  if (!statement) return forward_error(statement.error());
  if (!arguments.empty()) {
    if (auto status = statement->bind_all(arguments); !status) return st::forward_error(status.error());
  }
  QueryResult result;
  result.columns = statement->columns();
  for (;;) {
    auto row = statement->step();
    if (!row) return forward_error(row.error());
    if (!*row) break;
    result.rows.push_back(statement->read_row());
    if (max_rows != 0 && result.rows.size() >= max_rows) {
      // 只判"达到上限"：再探一行需要多一次 step（对超大结果集不划算）。
      // 语义上"恰好等于上限"与"被截断"不可区分——这是 `truncated` 的既有含义，
      // 调用方要么给一个宽松的上限，要么用 Statement 流式读。
      result.truncated = true;
      break;
    }
  }
  return result;
}

auto Database::execute(std::string_view sql, const std::vector<Value>& arguments)
    -> Result<std::int64_t> {
  auto statement = prepare(sql);
  if (!statement) return forward_error(statement.error());
  if (auto status = statement->bind_all(arguments); !status) return st::forward_error(status.error());
  auto row = statement->step();
  if (!row) return forward_error(row.error());
  return changes();
}

auto Database::begin() -> Status {
  if (!valid()) return unexpected(ErrorCode::Invalid, "连接不可用");
  // 嵌套：外层是真的 BEGIN，内层退化为 SAVEPOINT——否则会报
  // "cannot start a transaction within a transaction"（这类错误在"函数内自己开事务"的
  // 组合里必然出现，让调用方被迫把事务当全局状态管，最终到处都是手工配对）。
  //
  // **命名约定（与 commit/rollback 严格配对）**：深度 `d`（进入前）开的 savepoint 叫
  // `st_nest_d`；因此 commit/rollback 在当前深度 `d` 上操作的是 `st_nest_{d-1}`。
  // 这两处必须来自同一个索引公式——写反不会报错，只会“回滚到了另一层”，
  // 是本文件最容易被后续修改改坏的地方（实测：初版 begin 用 `depth - 1` 而 rollback 用
  // `depth - 1`，于是内层回滚打在了不存在的名字上，错误一路蹿到用例才现形）。
  const std::string sql = connection_->transaction_depth == 0
                              ? std::string("BEGIN")
                              : savepoint_sql(connection_->transaction_depth);
  if (auto status = exec(sql); !status) return status;
  ++connection_->transaction_depth;
  return ok();
}

auto Database::commit() -> Status {
  if (!valid()) return unexpected(ErrorCode::Invalid, "连接不可用");
  if (connection_->transaction_depth <= 0) {
    return unexpected(ErrorCode::Invalid, "当前没有进行中的事务（commit 无处可提交）");
  }
  const std::string sql = connection_->transaction_depth == 1
                              ? std::string("COMMIT")
                              : release_savepoint_sql(connection_->transaction_depth - 1);
  if (auto status = exec(sql); !status) return status;
  --connection_->transaction_depth;
  return ok();
}

auto Database::rollback() -> Status {
  if (!valid()) return unexpected(ErrorCode::Invalid, "连接不可用");
  if (connection_->transaction_depth <= 0) {
    return unexpected(ErrorCode::Invalid, "当前没有进行中的事务（rollback 无处可回滚）");
  }
  const std::string sql = connection_->transaction_depth == 1
                              ? std::string("ROLLBACK")
                              : rollback_savepoint_sql(connection_->transaction_depth - 1);
  if (auto status = exec(sql); !status) return status;
  --connection_->transaction_depth;
  return ok();
}

auto Database::transaction_depth() const noexcept -> int {
  return connection_ ? connection_->transaction_depth : 0;
}

auto Database::savepoint(std::string_view name) -> Status {
  return exec(std::format("SAVEPOINT {}", quote_identifier(name)));
}

auto Database::release_savepoint(std::string_view name) -> Status {
  return exec(std::format("RELEASE {}", quote_identifier(name)));
}

auto Database::rollback_to_savepoint(std::string_view name) -> Status {
  return exec(std::format("ROLLBACK TO {}", quote_identifier(name)));
}

auto Database::last_insert_rowid() const -> std::int64_t {
  return valid() ? sqlite3_last_insert_rowid(connection_->handle) : 0;
}

auto Database::changes() const -> std::int64_t {
  return valid() ? sqlite3_changes64(connection_->handle) : 0;
}

auto Database::total_changes() const -> std::int64_t {
  return valid() ? sqlite3_total_changes64(connection_->handle) : 0;
}

auto Database::last_error() const -> Error {
  if (!connection_) return Error{ErrorCode::Invalid, "连接不可用"};
  return Error{map_sqlite_code(connection_->last_code), connection_->error_text()};
}

auto Database::error_message() const -> std::string {
  if (!valid()) return {};
  const char* text = sqlite3_errmsg(connection_->handle);
  return text != nullptr ? std::string(text) : std::string{};
}

auto Database::path() const -> std::string { return connection_ ? connection_->path : std::string{}; }

auto Database::version() -> std::string_view { return sqlite3_libversion(); }
auto Database::source_id() -> std::string_view { return sqlite3_sourceid(); }

// ————————————————————————————————————————————————————————————————————————————
// Transaction
// ————————————————————————————————————————————————————————————————————————————

Transaction::Transaction(Database& database) : database_(&database) {
  if (auto status = database_->begin(); status) active_ = true;
}

Transaction::~Transaction() {
  // 未提交即回滚：作用域的任何退出路径（提前 return、break、continue）都不会留下半个事务。
  if (active_ && database_ != nullptr) (void)database_->rollback();
}

Transaction::Transaction(Transaction&& other) noexcept
    : database_(other.database_), active_(other.active_) {
  other.database_ = nullptr;
  other.active_ = false;
}

auto Transaction::operator=(Transaction&& other) noexcept -> Transaction& {
  if (this != &other) {
    if (active_ && database_ != nullptr) (void)database_->rollback();
    database_ = other.database_;
    active_ = other.active_;
    other.database_ = nullptr;
    other.active_ = false;
  }
  return *this;
}

auto Transaction::active() const noexcept -> bool { return active_; }

auto Transaction::commit() -> Status {
  if (!active_ || database_ == nullptr) return unexpected(ErrorCode::Invalid, "事务已结束");
  auto status = database_->commit();
  if (status) active_ = false;
  return status;
}

auto Transaction::rollback() -> Status {
  if (!active_ || database_ == nullptr) return unexpected(ErrorCode::Invalid, "事务已结束");
  auto status = database_->rollback();
  if (status) active_ = false;
  return status;
}

}  // namespace st::ext
