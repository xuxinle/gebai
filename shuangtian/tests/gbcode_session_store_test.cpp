/// gbcode 会话持久化（session_store）的数据层测试：往返保真 / 失败安全 / MRU 语义。
/// 不起 UI（那是 e2e 的事），这里只验「存了什么、读回来是什么、坏了怎么办」。

#include <cstdio>
#include <string>
#include <vector>

#include "st/core/fs.hpp"
#include "st/test/test.hpp"

// session_store 是 gbcode 的应用层源（不在框架 sources 清单里）——与
// gbcode_lsp_bridge_test 同一策略：直接 include 实现编译进测试。
#include "examples/gbcode/session_store.hpp"
#include "examples/gbcode/session_store.cpp"

namespace {

/// 每个用例独立的临时文件（同目录复用会互相踩；make_temp_dir 一次一目录最稳）。
/// 注意：ST_REQUIRE 在非 void 函数里不能用（宏体是裸 return）——临时目录创建
/// 失败时给出可辨识的占位路径，后续断言自然全红。
[[nodiscard]] auto fresh_session_path(const char* tag) -> std::string {
  const auto dir = st::fs::make_temp_dir(std::string("gbsession-") + tag);
  if (!dir.has_value()) return std::string("<temp-dir-failed>/session.json");
  return st::fs::join(*dir, "session.json");
}

/// vector<string> 的相等断言（ST_CHECK_EQ 不接受 vector——std::format 没有它的
/// formatter；拆成逐项比较，报错消息里带下标）。
auto check_list(const std::vector<std::string>& actual, const std::vector<std::string>& expected,
                const char* what) -> void {
  ST_CHECK_EQ(actual.size(), expected.size());
  for (std::size_t index = 0; index < actual.size() && index < expected.size(); ++index) {
    if (actual[index] != expected[index]) {
      std::printf("  不符（%s[%zu]）：%s != %s\n", what, index, actual[index].c_str(),
                  expected[index].c_str());
      ST_CHECK(false);
    }
  }
}

}  // namespace

ST_TEST(gbcode_session_roundtrip_preserves_all_fields) {
  const std::string path = fresh_session_path("rt");
  gbcode::SessionState saved;
  saved.workspace = "C:/work/proj";
  saved.files = {{"C:/work/proj/a.cpp", 3, 7}, {"C:/work/proj/中文 名.md", 12, 1}};
  saved.active = 1;
  saved.recent_files = {"C:/work/proj/a.cpp", "C:/other/b.py"};
  saved.recent_folders = {"C:/work/proj", "C:/other"};

  ST_CHECK(gbcode::save_session(path, saved));
  const gbcode::SessionState loaded = gbcode::load_session(path);

  ST_CHECK_EQ(loaded.workspace, saved.workspace);
  ST_CHECK_EQ(loaded.files.size(), std::size_t{2});
  ST_CHECK_EQ(loaded.files[0].path, std::string("C:/work/proj/a.cpp"));
  ST_CHECK_EQ(loaded.files[0].line, std::size_t{3});
  ST_CHECK_EQ(loaded.files[0].column, std::size_t{7});
  ST_CHECK_EQ(loaded.files[1].path, std::string("C:/work/proj/中文 名.md"));
  ST_CHECK_EQ(loaded.files[1].line, std::size_t{12});
  ST_CHECK_EQ(loaded.active, std::size_t{1});
  check_list(loaded.recent_files, saved.recent_files, "recent_files");
  check_list(loaded.recent_folders, saved.recent_folders, "recent_folders");
}

ST_TEST(gbcode_session_load_is_fail_safe) {
  // ① 文件不存在 → 空态（首次启动的常态，不是错误）。
  const std::string missing = fresh_session_path("missing");
  gbcode::SessionState state = gbcode::load_session(missing + "/nope.json");
  ST_CHECK(state.workspace.empty());
  ST_CHECK(state.files.empty());
  ST_CHECK(state.recent_files.empty());

  // ② 损坏 JSON → 空态。
  const std::string broken = fresh_session_path("broken");
  (void)st::fs::write_text(broken, "{ this is not json");
  state = gbcode::load_session(broken);
  ST_CHECK(state.workspace.empty());
  ST_CHECK(state.files.empty());

  // ③ 类型不符 / 缺字段 → 默认值（不抛、不喊；json_get_* 的失败安全语义）。
  // 注：`json_as_string` 的宽松语义会把数字/布尔也读成文本（配置友好）——
  // 所以 `workspace: 42` 读回 "42"，这不是缺陷，是框架约定；这里断言如实。
  const std::string partial = fresh_session_path("partial");
  (void)st::fs::write_text(partial,
                           R"({"workspace": 42, "files": "not-array", )"
                           R"("active": -3, "recent_files": [1, "ok", null]})");
  state = gbcode::load_session(partial);
  ST_CHECK_EQ(state.workspace, std::string("42"));  // 数字 → 宽松读回文本
  ST_CHECK(state.files.empty());                    // 非数组 → 空
  ST_CHECK_EQ(state.active, std::size_t{0});        // 负数 → 夹到 0
  // 数组里只收字符串元素，其余跳过。
  ST_CHECK_EQ(state.recent_files.size(), std::size_t{1});
  ST_CHECK_EQ(state.recent_files[0], std::string("ok"));

  // ④ line/column 的夹取（0 或负 → 1 起）。
  const std::string clamped = fresh_session_path("clamp");
  (void)st::fs::write_text(clamped,
                           R"({"files": [{"path": "a", "line": 0, "column": -2}]})");
  state = gbcode::load_session(clamped);
  ST_CHECK_EQ(state.files.size(), std::size_t{1});
  if (!state.files.empty()) {
    ST_CHECK_EQ(state.files[0].line, std::size_t{1});
    ST_CHECK_EQ(state.files[0].column, std::size_t{1});
  }
}

ST_TEST(gbcode_push_recent_is_mru_with_dedup_and_limit) {
  std::vector<std::string> recents;
  gbcode::push_recent(recents, "a");
  gbcode::push_recent(recents, "b");
  gbcode::push_recent(recents, "c");
  // 头插：最新在前。
  check_list(recents, {"c", "b", "a"}, "recents");
  // 重复项提到队首（去重，不重复占位）。
  gbcode::push_recent(recents, "a");
  check_list(recents, {"a", "c", "b"}, "recents");
  // 超上限截断（ oldest 掉出）。
  for (char letter = 'd'; letter < 'z'; ++letter) {
    gbcode::push_recent(recents, std::string(1, letter));
  }
  ST_CHECK_EQ(recents.size(), std::size_t{10});
  ST_CHECK_EQ(recents.front(), std::string("y"));
  // 空路径忽略（内置样例没有磁盘身份）。
  gbcode::push_recent(recents, "");
  ST_CHECK_EQ(recents.size(), std::size_t{10});
}

ST_TEST(gbcode_default_session_path_honors_st_home) {
  // ST_HOME 重定向（e2e 用同一机制把会话文件关进临时目录）。
  const std::string home = fresh_session_path("home");
  (void)st::fs::write_text(st::fs::join(home, "probe"), "");
#ifdef _WIN32
  (void)_putenv_s("ST_HOME", home.c_str());
#else
  (void)setenv("ST_HOME", home.c_str(), 1);
#endif
  const std::string path = gbcode::default_session_path();
  ST_CHECK(path.rfind(home, 0) == 0);
  ST_CHECK(path.find("gbcode") != std::string::npos);
#ifdef _WIN32
  (void)_putenv_s("ST_HOME", "");
#else
  (void)unsetenv("ST_HOME");
#endif
}
