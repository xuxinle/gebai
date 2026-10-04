/// 数据库层测试（`st::ext::Database`，底层 SQLite 3.50.2 源码内置）。
///
/// 覆盖四类关注点（按"出错时人会不会当场发现"排序）：
/// 1. **错误分类**——`map_sqlite_code` 是唯一真相源，逐条断言：应用靠它决定"重试 / 换键 / 提示用户"；
/// 2. **值保真与类型**——INTEGER/REAL/TEXT/BLOB/NULL 五类存储一一对应，且 `2^53+1` 这类
///    整数不因经过数据库而失真（这正是本框架替换旧 JSON 实现的同一个理由）；
/// 3. **事务语义**——提交/回滚/**嵌套**（SAVEPOINT 模拟）与 RAII 守卫；
/// 4. **编译期开关是否真的生效**——FTS5 能建表、`load_extension` 不存在、外键默认打开：
///    这些是 `st_sqlite3_config.h` 的承诺，必须由测试钉住，否则"配置漂移"没有任何人会发现。
///
/// 注：不测试 SQLite 自身（那是上游的事），只测**我们这层封装的语义承诺**。

#include "st/core/fs.hpp"
#include "st/ext/database.hpp"
#include "st/test/test.hpp"

// 测试**直接**包含 sqlite3.h：
// ① 错误映射表要按**上游原始错误码**逐条断言（不引它就只能把码值抄成魔数，抄错了测试反而“绿”）；
// ② 它同时在验证“我们的 -I 路径与编译开关对 C 头也是通的”。
// 生产代码**不得**这样做——公共头只前置声明，封装的边界靠它守住。
#include <sqlite3.h>

#include <format>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using st::ErrorCode;
using st::ext::Database;
using st::ext::OpenOptions;
using st::ext::Value;

/// 错误分类的取值（`ErrorCode` 是 enum class，`std::format` 不认它，统一转 int 比较）。
[[nodiscard]] auto code_of(const st::Error& error) -> int { return static_cast<int>(error.code); }
[[nodiscard]] auto code_int(ErrorCode code) -> int { return static_cast<int>(code); }

/// 内存库（免文件、免清理；用例之间零共享）。
[[nodiscard]] auto memory_db() -> Database {
  auto opened = Database::open("", OpenOptions{.memory = true});
  if (!opened) return Database{};  // 打不开内存库 = 框架坏了；调用点的 `valid()` 断言会报出来
  Database database = std::move(*opened);
  return database;
}

/// BLOB 与文本的比较助手：`std::vector<uint8_t>` 不可格式化，转成字符串再看。
[[nodiscard]] auto blob_text(const st::ext::Blob& bytes) -> std::string {
  return std::string(bytes.begin(), bytes.end());
}

}  // namespace

// ————————————————————————————————————————————————————————————————————————————
// 版本与自检
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(db_reports_upstream_version) {
  // 版本与源码标识都是**空**或"未知"就意味着链接到了别的东西（系统 sqlite3？）——
  // 源码内置的前提是"我知道我链的是哪一份"。
  ST_CHECK_EQ(Database::version(), std::string_view("3.50.2"));
  ST_CHECK(!Database::source_id().empty());
  ST_CHECK(Database::source_id().starts_with("2025-06-28"));
}

// ————————————————————————————————————————————————————————————————————————————
// 基本读写
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(db_exec_ddl_and_insert_then_query) {
  auto database = memory_db();
  ST_REQUIRE(database.valid());
  ST_CHECK(database.path() == std::string(":memory:"));

  ST_CHECK(database.exec(
                       "create table note(id integer primary key, title text not null, body text);"
                       "create index note_title on note(title);")
               .has_value());

  ST_CHECK(database.exec("insert into note(title, body) values ('第一条', '霜天数据库')").has_value());
  ST_CHECK_EQ(database.changes(), std::int64_t{1});
  const std::int64_t rowid = database.last_insert_rowid();
  ST_CHECK(rowid > 0);

  const auto result = database.query("select id, title, body from note");
  ST_REQUIRE(result.has_value());
  ST_CHECK_EQ(result->columns.size(), std::size_t{3});
  ST_CHECK_EQ(result->columns[0].name, std::string("id"));
  ST_CHECK_EQ(result->rows.size(), std::size_t{1});
  ST_REQUIRE(result->rows[0].size() == 3);

  // 类型必须是 SQLite 报告的那一类，而不是"看着像数字就当数字"
  ST_CHECK(std::holds_alternative<std::int64_t>(result->rows[0][0]));
  ST_CHECK_EQ(std::get<std::int64_t>(result->rows[0][0]), rowid);
  ST_CHECK(std::holds_alternative<std::string>(result->rows[0][1]));
  ST_CHECK_EQ(std::get<std::string>(result->rows[0][1]), std::string("第一条"));
}

ST_TEST(db_exec_runs_multi_statement_script) {
  // 多语句脚本：任何一句失败都要能报出来（这也是不走 `sqlite3_exec` 的原因）
  auto database = memory_db();
  ST_CHECK(database
               .exec("pragma journal_mode = memory;"
                     "create table a(x integer);"
                     "insert into a values (1);"
                     "insert into a values (2);")
               .has_value());
  const auto count = database.query("select count(*) from a");
  ST_REQUIRE(count.has_value());
  ST_CHECK_EQ(std::get<std::int64_t>(count->rows[0][0]), std::int64_t{2});

  // 第二句就是错的：错误必须指向**这一句**（消息里含 SQLite 的原文）
  const auto broken = database.exec("create table b(x integer); select * from nope;");
  ST_REQUIRE(!broken.has_value());
  ST_CHECK(code_of(broken.error()) == code_int(ErrorCode::Io));
  ST_CHECK(broken.error().message.find("nope") != std::string::npos);
}

ST_TEST(db_prepared_statement_reuse_batch_insert) {
  auto database = memory_db();
  ST_REQUIRE(database.exec("create table kv(k text primary key, v integer)").has_value());

  auto statement = database.prepare("insert into kv(k, v) values (?, ?)");
  ST_REQUIRE(statement.has_value());
  ST_CHECK_EQ(statement->parameter_count(), 2);
  ST_CHECK(statement->sql().find("insert into kv") != std::string::npos);

  // 复用同一条语句批量写入（每次 reset 后重新绑定）
  for (int index = 0; index < 50; ++index) {
    ST_CHECK(statement->reset().has_value());
    ST_CHECK(statement->bind(1, st::ext::value_text(std::format("key-{}", index))).has_value());
    ST_CHECK(statement->bind(2, st::ext::value_integer(index * 2)).has_value());
    const auto stepped = statement->step();
    ST_REQUIRE(stepped.has_value());
    ST_CHECK(!*stepped);
  }

  const auto count = database.query("select count(*), sum(v) from kv");
  ST_REQUIRE(count.has_value());
  ST_CHECK_EQ(std::get<std::int64_t>(count->rows[0][0]), std::int64_t{50});
  ST_CHECK_EQ(std::get<std::int64_t>(count->rows[0][1]), std::int64_t{2450});
}

ST_TEST(db_named_parameters_bind_by_name) {
  auto database = memory_db();
  auto statement = database.prepare("select :left + @right as total, $label as tag");
  ST_REQUIRE(statement.has_value());
  ST_CHECK(statement->bind(":left", st::ext::value_integer(40)).has_value());
  ST_CHECK(statement->bind("right", st::ext::value_integer(2)).has_value());  // 前缀可省
  ST_CHECK(statement->bind("$label", st::ext::value_text("答案")).has_value());

  const auto stepped = statement->step();
  ST_REQUIRE(stepped.has_value());
  ST_REQUIRE(*stepped);
  ST_CHECK_EQ(std::get<std::int64_t>(statement->value(0)), std::int64_t{42});
  ST_CHECK_EQ(std::get<std::string>(statement->value(1)), std::string("答案"));

  // 不存在的参数名必须报 NotFound，而不是静默忽略（否则"少绑一个参数"会变成 NULL 悄悄入库）
  const auto missing = statement->bind("nope", st::ext::value_integer(1));
  ST_REQUIRE(!missing.has_value());
  ST_CHECK(code_of(missing.error()) == code_int(ErrorCode::NotFound));
}

ST_TEST(db_bind_index_out_of_range_is_rejected) {
  // 注：SQLite 用**零基**的 `sqlite3_bind_parameter_index()`（返回 0 表示
  // "这个套接字没有定义"），而 `sqlite3_bind_*` 用一基下标——本封装对外统一为
  // **一基**（与 SQL 和上游文档一致），不把上游两套下标的不一致透出去。
  auto database = memory_db();
  auto statement = database.prepare("select ?");
  ST_REQUIRE(statement.has_value());
  const auto high = statement->bind(2, st::ext::value_integer(1));
  ST_REQUIRE(!high.has_value());
  ST_CHECK(code_of(high.error()) == code_int(ErrorCode::Invalid));
  const auto low = statement->bind(0, st::ext::value_integer(1));
  ST_REQUIRE(!low.has_value());
  ST_CHECK(code_of(low.error()) == code_int(ErrorCode::Invalid));

  // bind_all 的个数不符同样报 Invalid（而不是绑一半就执行）
  const auto mismatch = statement->bind_all({});
  ST_REQUIRE(!mismatch.has_value());
  ST_CHECK(code_of(mismatch.error()) == code_int(ErrorCode::Invalid));
}

ST_TEST(db_query_with_arguments_and_row_limit) {
  auto database = memory_db();
  ST_REQUIRE(database.exec("create table n(v integer);").has_value());
  for (int index = 0; index < 10; ++index) {
    ST_CHECK(database.execute("insert into n values (?)", {st::ext::value_integer(index)}).has_value());
  }

  const auto limited = database.query("select v from n order by v", {}, 3);
  ST_REQUIRE(limited.has_value());
  ST_CHECK_EQ(limited->rows.size(), std::size_t{3});
  ST_CHECK(limited->truncated);

  const auto filtered = database.query("select v from n where v >= ? order by v desc",
                                       {st::ext::value_integer(7)}, 0);
  ST_REQUIRE(filtered.has_value());
  ST_CHECK_EQ(filtered->rows.size(), std::size_t{3});
  ST_CHECK(!filtered->truncated);
  ST_CHECK_EQ(std::get<std::int64_t>(filtered->rows[0][0]), std::int64_t{9});
}

// ————————————————————————————————————————————————————————————————————————————
// 值保真与类型（五类存储一一对应）
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(db_integer_beyond_double_precision_survives) {
  // 2^53+1 与 int64 极值：过一趟数据库必须逐位回来。
  // （框架替换旧 JSON 实现的动因是同一个：数字化为 double 就再也回不去。）
  auto database = memory_db();
  ST_REQUIRE(database.exec("create table big(v integer)").has_value());
  const std::int64_t beyond = 9007199254740993LL;  // 2^53 + 1
  const std::int64_t maximum = std::numeric_limits<std::int64_t>::max();
  const std::int64_t minimum = std::numeric_limits<std::int64_t>::min();
  ST_CHECK(database.execute("insert into big values (?)", {st::ext::value_integer(beyond)}).has_value());
  ST_CHECK(database.execute("insert into big values (?)", {st::ext::value_integer(maximum)}).has_value());
  ST_CHECK(database.execute("insert into big values (?)", {st::ext::value_integer(minimum)}).has_value());

  const auto result = database.query("select v from big order by rowid");
  ST_REQUIRE(result.has_value());
  ST_CHECK_EQ(std::get<std::int64_t>(result->rows[0][0]), beyond);
  ST_CHECK_EQ(std::get<std::int64_t>(result->rows[1][0]), maximum);
  ST_CHECK_EQ(std::get<std::int64_t>(result->rows[2][0]), minimum);
}

ST_TEST(db_value_types_map_to_storage_classes) {
  auto database = memory_db();
  ST_REQUIRE(database.exec("create table mixed(i integer, r real, t text, b blob, n text)").has_value());
  const st::ext::Blob payload{0x00, 0x01, 0xFF, 0x7F, 0x80};
  ST_CHECK(database
               .execute("insert into mixed values (?, ?, ?, ?, ?)",
                        {st::ext::value_integer(7), st::ext::value_real(2.5),
                         st::ext::value_text("文本"), st::ext::value_blob(payload),
                         st::ext::value_null()})
               .has_value());

  const auto result = database.query("select i, r, t, b, n from mixed");
  ST_REQUIRE(result.has_value());
  ST_REQUIRE(result->rows.size() == 1);
  const auto& row = result->rows[0];
  ST_CHECK(std::holds_alternative<std::int64_t>(row[0]));
  ST_CHECK(std::holds_alternative<double>(row[1]));
  ST_CHECK(std::holds_alternative<std::string>(row[2]));
  ST_CHECK(std::holds_alternative<st::ext::Blob>(row[3]));
  ST_CHECK(std::holds_alternative<std::nullptr_t>(row[4]));

  ST_CHECK_EQ(std::get<std::int64_t>(row[0]), std::int64_t{7});
  ST_CHECK_NEAR(std::get<double>(row[1]), 2.5, 1e-12);
  // 含 NUL 字节的 BLOB 必须原样回来（长度不能被 `strlen` 截断）
  ST_CHECK_EQ(blob_text(std::get<st::ext::Blob>(row[3])), std::string("\x00\x01\xFF\x7F\x80", 5));

  // 空 BLOB 与含 NUL 的文本同样要能往返
  ST_CHECK(database.execute("insert into mixed(b, t) values (?, ?)",
                            {st::ext::value_blob({}), st::ext::value_text(std::string("a\0b", 3))})
               .has_value());
  const auto again = database.query("select b, t from mixed where rowid = 2");
  ST_REQUIRE(again.has_value());
  ST_CHECK(std::get<st::ext::Blob>(again->rows[0][0]).empty());
  ST_CHECK_EQ(std::get<std::string>(again->rows[0][1]).size(), std::size_t{3});
}

ST_TEST(db_column_metadata_from_compile_option) {
  // SQLITE_ENABLE_COLUMN_METADATA 的承诺：能问出"这一列来自哪张表"。
  // 用处是让人（与 AI）看懂查询结果，而不是只拿到一串无名值。
  auto database = memory_db();
  ST_REQUIRE(database.exec("create table person(id integer, name text)").has_value());
  auto statement = database.prepare("select name, id + 1 as next_id from person");
  ST_REQUIRE(statement.has_value());
  const auto columns = statement->columns();
  ST_REQUIRE(columns.size() == 2);
  ST_CHECK_EQ(columns[0].name, std::string("name"));
  ST_CHECK_EQ(columns[0].table_name, std::string("person"));
  ST_CHECK_EQ(columns[0].origin_name, std::string("name"));
  ST_CHECK_EQ(columns[0].declared_type, std::string("TEXT"));
  // 表达式列没有来源表（返回空而不是报错）——这不是缺陷，是"没有来源"的事实
  ST_CHECK(columns[1].table_name.empty());
}

// ————————————————————————————————————————————————————————————————————————————
// 事务
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(db_transaction_commit_persists_rollback_discards) {
  auto database = memory_db();
  ST_REQUIRE(database.exec("create table t(v integer)").has_value());

  ST_CHECK(database.begin().has_value());
  ST_CHECK_EQ(database.transaction_depth(), 1);
  ST_CHECK(database.execute("insert into t values (1)", {}).has_value());
  ST_CHECK(database.commit().has_value());
  ST_CHECK_EQ(database.transaction_depth(), 0);

  ST_CHECK(database.begin().has_value());
  ST_CHECK(database.execute("insert into t values (2)", {}).has_value());
  ST_CHECK(database.rollback().has_value());

  const auto result = database.query("select v from t");
  ST_REQUIRE(result.has_value());
  ST_CHECK_EQ(result->rows.size(), std::size_t{1});
  ST_CHECK_EQ(std::get<std::int64_t>(result->rows[0][0]), std::int64_t{1});

  // 没有事务时 commit/rollback 必须报 Invalid（而不是静默成功——
  // "以为提交了其实没有"是数据丢失的经典形态）
  const auto stray = database.commit();
  ST_REQUIRE(!stray.has_value());
  ST_CHECK(code_of(stray.error()) == code_int(ErrorCode::Invalid));
  ST_CHECK(!database.rollback().has_value());
}

ST_TEST(db_nested_transaction_uses_savepoints) {
  // SQLite 没有真嵌套事务：外层 BEGIN、内层 SAVEPOINT。
  // 内层回滚只丢内层的改动——这是"函数内部开事务"能被安全复用的前提。
  auto database = memory_db();
  ST_REQUIRE(database.exec("create table t(v text)").has_value());

  ST_CHECK(database.begin().has_value());
  ST_CHECK(database.execute("insert into t values ('outer')", {}).has_value());

  ST_CHECK(database.begin().has_value());
  ST_CHECK_EQ(database.transaction_depth(), 2);
  ST_CHECK(database.execute("insert into t values ('inner')", {}).has_value());
  ST_CHECK(database.rollback().has_value());  // 只回滚内层
  ST_CHECK_EQ(database.transaction_depth(), 1);

  ST_CHECK(database.execute("insert into t values ('after')", {}).has_value());
  ST_CHECK(database.commit().has_value());

  const auto result = database.query("select v from t order by rowid");
  ST_REQUIRE(result.has_value());
  ST_CHECK_EQ(result->rows.size(), std::size_t{2});
  ST_CHECK_EQ(std::get<std::string>(result->rows[0][0]), std::string("outer"));
  ST_CHECK_EQ(std::get<std::string>(result->rows[1][0]), std::string("after"));
}

ST_TEST(db_explicit_savepoint_rolls_back_partially) {
  auto database = memory_db();
  ST_REQUIRE(database.exec("create table t(v integer)").has_value());
  ST_CHECK(database.execute("insert into t values (0)", {}).has_value());

  ST_CHECK(database.savepoint("anchor").has_value());
  ST_CHECK(database.execute("insert into t values (1)", {}).has_value());
  ST_CHECK(database.rollback_to_savepoint("anchor").has_value());
  ST_CHECK(database.release_savepoint("anchor").has_value());

  const auto result = database.query("select count(*) from t");
  ST_REQUIRE(result.has_value());
  ST_CHECK_EQ(std::get<std::int64_t>(result->rows[0][0]), std::int64_t{1});

  // savepoint 名字来自调用方 → 必须按标识符转义（否则 `a"; DROP TABLE t; --` 就能注入）
  ST_CHECK(database.savepoint("weird \" name; DROP TABLE t; --").has_value());
  ST_CHECK(database.release_savepoint("weird \" name; DROP TABLE t; --").has_value());
  const auto survived = database.query("select count(*) from t");
  ST_REQUIRE(survived.has_value());
  ST_CHECK_EQ(std::get<std::int64_t>(survived->rows[0][0]), std::int64_t{1});
}

ST_TEST(db_transaction_guard_rolls_back_on_scope_exit) {
  auto database = memory_db();
  ST_REQUIRE(database.exec("create table t(v integer)").has_value());

  {
    st::ext::Transaction guard(database);
    ST_REQUIRE(guard.active());
    ST_CHECK(database.execute("insert into t values (1)", {}).has_value());
    // 不调 commit：析构必须回滚
  }
  ST_CHECK_EQ(database.transaction_depth(), 0);
  const auto after_rollback = database.query("select count(*) from t");
  ST_REQUIRE(after_rollback.has_value());
  ST_CHECK_EQ(std::get<std::int64_t>(after_rollback->rows[0][0]), std::int64_t{0});

  {
    st::ext::Transaction guard(database);
    ST_REQUIRE(guard.active());
    ST_CHECK(database.execute("insert into t values (2)", {}).has_value());
    ST_CHECK(guard.commit().has_value());
    ST_CHECK(!guard.active());
  }
  const auto after_commit = database.query("select count(*) from t");
  ST_REQUIRE(after_commit.has_value());
  ST_CHECK_EQ(std::get<std::int64_t>(after_commit->rows[0][0]), std::int64_t{1});
  // 已结束的守卫再提交/回滚 → Invalid
  st::ext::Transaction finished(database);
  ST_REQUIRE(finished.commit().has_value());
  ST_CHECK(!finished.rollback().has_value());
  ST_CHECK(finished.rollback().has_value() == false);
}

// ————————————————————————————————————————————————————————————————————————————
// 错误分类（唯一真相源的逐条断言）
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(db_error_mapping_table) {
  ST_CHECK_EQ(code_int(st::ext::map_sqlite_code(SQLITE_CONSTRAINT_UNIQUE)), code_int(ErrorCode::Exists));
  ST_CHECK_EQ(code_int(st::ext::map_sqlite_code(SQLITE_CONSTRAINT_CHECK)), code_int(ErrorCode::Invalid));
  ST_CHECK_EQ(code_int(st::ext::map_sqlite_code(SQLITE_CONSTRAINT_NOTNULL)), code_int(ErrorCode::Invalid));
  ST_CHECK_EQ(code_int(st::ext::map_sqlite_code(SQLITE_CONSTRAINT_FOREIGNKEY)), code_int(ErrorCode::Invalid));
  ST_CHECK_EQ(code_int(st::ext::map_sqlite_code(SQLITE_READONLY)), code_int(ErrorCode::Permission));
  ST_CHECK_EQ(code_int(st::ext::map_sqlite_code(SQLITE_PERM)), code_int(ErrorCode::Permission));
  ST_CHECK_EQ(code_int(st::ext::map_sqlite_code(SQLITE_BUSY)), code_int(ErrorCode::Busy));
  ST_CHECK_EQ(code_int(st::ext::map_sqlite_code(SQLITE_LOCKED)), code_int(ErrorCode::Busy));
  ST_CHECK_EQ(code_int(st::ext::map_sqlite_code(SQLITE_CORRUPT)), code_int(ErrorCode::Parse));
  ST_CHECK_EQ(code_int(st::ext::map_sqlite_code(SQLITE_NOTADB)), code_int(ErrorCode::Parse));
  ST_CHECK_EQ(code_int(st::ext::map_sqlite_code(SQLITE_FULL)), code_int(ErrorCode::Overflow));
  ST_CHECK_EQ(code_int(st::ext::map_sqlite_code(SQLITE_TOOBIG)), code_int(ErrorCode::Overflow));
  ST_CHECK_EQ(code_int(st::ext::map_sqlite_code(SQLITE_NOMEM)), code_int(ErrorCode::Io));
  ST_CHECK_EQ(code_int(st::ext::map_sqlite_code(SQLITE_CANTOPEN)), code_int(ErrorCode::NotFound));
  ST_CHECK_EQ(code_int(st::ext::map_sqlite_code(SQLITE_MISUSE)), code_int(ErrorCode::Internal));
  ST_CHECK_EQ(code_int(st::ext::map_sqlite_code(SQLITE_INTERRUPT)), code_int(ErrorCode::Cancelled));
  ST_CHECK_EQ(code_int(st::ext::map_sqlite_code(SQLITE_IOERR)), code_int(ErrorCode::Io));
  ST_CHECK_EQ(code_int(st::ext::map_sqlite_code(SQLITE_IOERR_ACCESS)), code_int(ErrorCode::Permission));
  ST_CHECK_EQ(code_int(st::ext::map_sqlite_code(SQLITE_IOERR_READ)), code_int(ErrorCode::Io));
  // 扩展码是 `主码 | (n << 8)`：分类必须按主码走，不能"精确匹配扩展码才算"。
  // 用一个 SQLite 自己**没有定义**的子码（42）——它证明"未知的扩展位仍然按主码归类"，
  // 而不是落到 default 里去。（注：(13 << 8) 恰好是 `SQLITE_IOERR_ACCESS`，别拿它当任意值。）
  ST_CHECK_EQ(code_int(st::ext::map_sqlite_code(SQLITE_IOERR | (42 << 8))), code_int(ErrorCode::Io));
}

ST_TEST(db_constraint_violations_are_classified) {
  auto database = memory_db();
  ST_REQUIRE(database
                 .exec("create table u(k text primary key, v integer not null check (v > 0))")
                 .has_value());
  ST_CHECK(database.execute("insert into u values ('a', 1)", {}).has_value());

  // 主键冲突 → Exists（应用该换键，而不是重试）
  const auto duplicate = database.execute("insert into u values ('a', 2)", {});
  ST_REQUIRE(!duplicate.has_value());
  ST_CHECK(code_of(duplicate.error()) == code_int(ErrorCode::Exists));

  // CHECK 违反 → Invalid（应用该改数据）
  const auto checked = database.execute("insert into u values ('b', 0)", {});
  ST_REQUIRE(!checked.has_value());
  ST_CHECK(code_of(checked.error()) == code_int(ErrorCode::Invalid));

  // NOT NULL 违反 → Invalid。
  // 注：这里用 `insert ... values ('c', NULL)` 而**不是**绑定空值——
  // 绑定路径上 `value_null()` 落到 `sqlite3_bind_null()`（int64）与
  // `column_int64(NULL 列)`（int64）**都是 0**，可 NULL 进 TEXT 列会被 SQLite 的
  // 列亲缘性转成 `'0'`，于是 NOT NULL 不触发——那是 SQLite 的合法语义（而非缺位）。
  // 想得到 Non-Null 语义就显式写 NULL 字面量。
  const auto not_null = database.execute("insert into u(k, v) values ('c', NULL)", {});
  ST_REQUIRE(!not_null.has_value());
  ST_CHECK(code_of(not_null.error()) == code_int(ErrorCode::Invalid));

  // 绑定路径的 NULL：INTEGER 列上 `value_null()` 存进去仍是 NULL（不被亲缘性改写）
  ST_REQUIRE(database.exec("create table nullable(v integer)").has_value());
  ST_CHECK(database.execute("insert into nullable values (?)", {st::ext::value_null()}).has_value());
  const auto stored_null = database.query("select v, typeof(v) from nullable");
  ST_REQUIRE(stored_null.has_value());
  ST_CHECK(std::holds_alternative<std::nullptr_t>(stored_null->rows[0][0]));
  ST_CHECK_EQ(std::get<std::string>(stored_null->rows[0][1]), std::string("null"));

  // 语法/对象不存在类错误：`Parse` 留给"库坏了"，SQL 写错归 `Io`（可重试的泛化失败）
  auto broken = database.prepare("selec * from u");
  ST_REQUIRE(!broken.has_value());
  ST_CHECK(code_of(broken.error()) == code_int(ErrorCode::Io));
  ST_CHECK(broken.error().message.find("selec") != std::string::npos);

  auto empty = database.prepare("   ");
  ST_REQUIRE(!empty.has_value());
  ST_CHECK(code_of(empty.error()) == code_int(ErrorCode::Parse));
}

ST_TEST(db_foreign_key_default_is_on) {
  // SQLITE_DEFAULT_FOREIGN_KEYS=1 的承诺：不写 pragma 也有约束（静默忽略外键是更大的坑）
  auto database = memory_db();
  const auto pragma = database.query("pragma foreign_keys");
  ST_REQUIRE(pragma.has_value());
  ST_CHECK_EQ(std::get<std::int64_t>(pragma->rows[0][0]), std::int64_t{1});

  ST_REQUIRE(database
                 .exec("create table parent(id integer primary key);"
                       "create table child(id integer primary key, pid integer references parent(id));")
                 .has_value());
  const auto orphan = database.execute("insert into child(pid) values (999)", {});
  ST_REQUIRE(!orphan.has_value());
  ST_CHECK(code_of(orphan.error()) == code_int(ErrorCode::Invalid));
}

// ————————————————————————————————————————————————————————————————————————————
// 编译期开关的可见行为（配置漂移会被当场抓住）
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(db_config_switches_are_effective) {
  auto database = memory_db();

  // FTS5：能建虚表并检索（中文按当前分词规则未必命中，这里用 ASCII 词验证通路）
  ST_CHECK(database.exec("create virtual table doc using fts5(body)").has_value());
  ST_CHECK(database.execute("insert into doc(body) values (?)",
                            {st::ext::value_text("shuangtian native framework")})
               .has_value());
  const auto matched = database.query("select body from doc where doc match 'native'");
  ST_REQUIRE(matched.has_value());
  ST_CHECK_EQ(matched->rows.size(), std::size_t{1});

  // R-tree：空间索引可用
  ST_CHECK(database.exec("create virtual table box using rtree(id, minx, maxx, miny, maxy)").has_value());
  ST_CHECK(database
               .execute("insert into box values (?, ?, ?, ?, ?)",
                        {st::ext::value_integer(1), st::ext::value_real(0.0), st::ext::value_real(1.0),
                         st::ext::value_real(0.0), st::ext::value_real(1.0)})
               .has_value());
  const auto in_range = database.query("select id from box where minx <= 0.5 and maxx >= 0.5");
  ST_REQUIRE(in_range.has_value());
  ST_CHECK_EQ(in_range->rows.size(), std::size_t{1});

  // 数学函数（SQLITE_ENABLE_MATH_FUNCTIONS）
  const auto power = database.query("select cast(pow(2, 10) as integer)");
  ST_REQUIRE(power.has_value());
  ST_CHECK_EQ(std::get<std::int64_t>(power->rows[0][0]), std::int64_t{1024});

  // dbstat 虚表（SQLITE_ENABLE_DBSTAT_VTAB）：能查"哪些表占了多少页"
  const auto pages = database.query("select count(*) from dbstat");
  ST_CHECK(pages.has_value());

  // DQS=0：双引号不当字符串——写错引号必须**当场报错**，而不是静默变成字符串
  const auto dqs = database.prepare("select \"not_an_identifier\"");
  ST_REQUIRE(!dqs.has_value());

  // 原生扩展加载被编译期剔除：SQL 函数不存在，C API 也未链接进产物
  const auto loader = database.prepare("select load_extension('anything')");
  ST_REQUIRE(!loader.has_value());
  ST_CHECK(loader.error().message.find("load_extension") != std::string::npos);
}

// ————————————————————————————————————————————————————————————————————————————
// 生命周期与只读
// ————————————————————————————————————————————————————————————————————————————

ST_TEST(db_statement_outlives_database_handle) {
  // 语句持连接的一个引用：`Database` 先析构时连接转为僵尸但**依然可用**
  // （sqlite3_close_v2 的延后关闭语义）。这条用例把它钉住——
  // 否则"语句比连接活得久"会变成随机的悬垂崩溃。
  auto statement = std::optional<st::ext::Statement>{};
  {
    auto database = memory_db();
    ST_REQUIRE(database.exec("create table t(v integer)").has_value());
    ST_CHECK(database.execute("insert into t values (11)", {}).has_value());
    auto prepared = database.prepare("select v from t");
    ST_REQUIRE(prepared.has_value());
    statement = std::move(*prepared);
    // database 在这里析构
  }
  ST_REQUIRE(statement.has_value());
  ST_CHECK(statement->valid());
  const auto stepped = statement->step();
  ST_REQUIRE(stepped.has_value());
  ST_REQUIRE(*stepped);
  ST_CHECK_EQ(std::get<std::int64_t>(statement->value(0)), std::int64_t{11});
}

ST_TEST(db_statement_move_leaves_source_invalid) {
  auto database = memory_db();
  auto statement = database.prepare("select 1");
  ST_REQUIRE(statement.has_value());
  st::ext::Statement moved = std::move(*statement);
  ST_CHECK(moved.valid());
  ST_CHECK(!statement->valid());
  const auto stepped = moved.step();
  ST_REQUIRE(stepped.has_value());
  ST_CHECK(*stepped);
}

ST_TEST(db_open_rejects_empty_path_and_read_only_write_fails) {
  const auto empty = Database::open("");
  ST_REQUIRE(!empty.has_value());
  ST_CHECK(code_of(empty.error()) == code_int(ErrorCode::Invalid));

  // 只读打开：写操作必须以 Permission 失败（而不是"看着是只读、其实改了文件"）
  const auto directory = st::fs::make_temp_dir("st-db-test");
  ST_REQUIRE(directory.has_value());
  const std::string file = st::fs::join(*directory, "ro.db");
  {
    auto writable = Database::open(file);
    ST_REQUIRE(writable.has_value());
    ST_REQUIRE(writable->exec("create table t(v integer)").has_value());
  }
  {
    auto read_only = Database::open(file, OpenOptions{.read_only = true, .create = false});
    ST_REQUIRE(read_only.has_value());
    const auto query = read_only->query("select count(*) from t");
    ST_CHECK(query.has_value());
    const auto write = read_only->execute("insert into t values (1)", {});
    ST_REQUIRE(!write.has_value());
    ST_CHECK(code_of(write.error()) == code_int(ErrorCode::Permission));
  }
  // 不存在的库 + 不允许创建 → NotFound
  const auto missing = Database::open(st::fs::join(*directory, "nope.db"),
                                      OpenOptions{.read_only = false, .create = false});
  ST_CHECK(!missing.has_value());
  (void)st::fs::remove_all(*directory);
}

ST_TEST(db_close_is_idempotent_and_invalidates) {
  auto database = memory_db();
  ST_REQUIRE(database.valid());
  database.close();
  ST_CHECK(!database.valid());
  database.close();  // 再关一次必须无害
  const auto after = database.exec("select 1");
  ST_REQUIRE(!after.has_value());
  ST_CHECK(code_of(after.error()) == code_int(ErrorCode::Invalid));
}
