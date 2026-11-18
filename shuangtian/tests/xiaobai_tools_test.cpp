/// 小白（xiaobai）的工具层测试：`sh` / `read` / `write` / `edit` 的参数解析与文件语义。
///
/// 为什么这些值得单测：工具是**模型唯一能改变世界的通道**，而它的失败模式
/// 与普通函数不同——错得静默（`edit` 没找到原文却写回一份未改动的文件，
/// 模型会以为改成功了，这是最贵的失败）。所以断言集中在：
///   · `edit` 必须**命中才改**，命不中要如实报错、文件保持原样；
///   · 回传文本要**有上限**（一条 `ls -R` 能撑爆上下文）；
///   · 路径解析按工具根目录走（模型给的都是相对路径）。
#include <string>

#include "st/core/fs.hpp"
#include "st/core/json.hpp"
#include "st/test/test.hpp"

namespace {

/// 与 `main.cpp` 里的实现保持同一语义（纯函数，复制过来钉住行为）。
constexpr std::size_t kTestOutputLimit = 16 * 1024;

[[nodiscard]] auto truncate_output(std::string text) -> std::string {
  if (text.size() <= kTestOutputLimit) return text;
  const std::size_t kept = text.size() - kTestOutputLimit;
  text.resize(kTestOutputLimit);
  text += "\n…（输出过大，已截断 " + std::to_string(kept) + " 字符）";
  return text;
}

[[nodiscard]] auto resolve_path(const std::string& root, const std::string& path) -> std::string {
  if (path.empty()) return root;
  if (path.find(":\\") != std::string::npos || path.find(":/") != std::string::npos ||
      (!path.empty() && (path.front() == '/' || path.front() == '\\'))) {
    return path;
  }
  return st::fs::join(root, path);
}

/// `edit` 的核心语义：命中才替换（与 main.cpp 的 `tool_edit_text` 同源逻辑）。
[[nodiscard]] auto edit_in_memory(std::string& content, const std::string& old_text,
                                  const std::string& new_text) -> std::pair<bool, std::size_t> {
  if (old_text.empty()) return {false, 0};
  std::size_t count = 0;
  std::size_t at = content.find(old_text);
  if (at == std::string::npos) return {false, 0};
  while (at != std::string::npos) {
    content.replace(at, old_text.size(), new_text);
    ++count;
    at = content.find(old_text, at + new_text.size());
  }
  return {true, count};
}

}  // namespace

ST_TEST(xiaobai_tool_schema_json_shape_is_object_not_array) {
  // 回归（2026-11-18 实测崩溃）：工具的 `parameters` 必须是 **对象**。
  //
  // 当时的写法是 `schema({{"path", {...}}}, {...})` —— nlohmann 会把嵌套花括号
  // 解成**数组**，于是 `parameters` 变成 `[{...}]`。后果不是“模型不认”那么简单：
  // 本地读 `delta.tool_calls[i].index` 时抛 `type must be number, but is object`，
  // 直接 `terminate` 打死整个应用。
  //
  // 这条测试只钉**构造方式**（不用花括号初始化 Json）的产物形状。
  const auto field = [](const std::string& type) {
    st::Json spec = st::Json::object();
    spec["type"] = type;
    return spec;
  };
  st::Json properties = st::Json::object();
  properties["path"] = field("string");
  st::Json parameters = st::Json::object();
  parameters["type"] = "object";
  parameters["properties"] = properties;
  st::Json required = st::Json::array();
  required.push_back("path");
  parameters["required"] = required;

  ST_CHECK(parameters.is_object());
  ST_CHECK(!parameters.is_array());
  const st::Json* props = st::json_find(parameters, "properties");
  ST_REQUIRE(props != nullptr);
  ST_CHECK(props->is_object());
  const st::Json* path = st::json_find(*props, "path");
  ST_REQUIRE(path != nullptr);
  ST_CHECK(path->is_object());
  ST_CHECK_EQ(st::json_as_string(*st::json_find(*path, "type")), std::string("string"));

  // 对照：花括号写法确实会变成数组（这就是当时的错）。
  const st::Json wrong = st::Json{{{"path", field("string")}}};
  ST_CHECK(wrong.is_array());
}

ST_TEST(xiaobai_tool_edit_requires_match) {
  // 命不中 ⇒ 不报成功、**内容一字不改**。
  // 反面（曾经的写法）是“没找到就当改完了”，那会让模型基于假前提继续。
  std::string content = "alpha\nbeta\n";
  const auto [ok, count] = edit_in_memory(content, "不存在的原文", "x");
  ST_CHECK(!ok);
  ST_CHECK_EQ(count, std::size_t{0});
  ST_CHECK_EQ(content, std::string("alpha\nbeta\n"));
}

ST_TEST(xiaobai_tool_edit_replaces_all_occurrences) {
  std::string content = "foo bar foo baz foo";
  const auto [ok, count] = edit_in_memory(content, "foo", "Q");
  ST_CHECK(ok);
  ST_CHECK_EQ(count, std::size_t{3});
  ST_CHECK_EQ(content, std::string("Q bar Q baz Q"));
  // 空 old_string 必须拒绝（否则 find("") 恒命中，会变成无限循环写入）。
  std::string other = "abc";
  const auto [ok2, count2] = edit_in_memory(other, "", "x");
  ST_CHECK(!ok2);
  ST_CHECK_EQ(count2, std::size_t{0});
  ST_CHECK_EQ(other, std::string("abc"));
}

ST_TEST(xiaobai_tool_output_is_truncated) {
  // 小输出原样返回。
  const std::string small = "hello";
  ST_CHECK_EQ(truncate_output(small), small);
  // 超限：截到上限并附一句说明（这句话本身是给模型的信息）。
  const std::string big(kTestOutputLimit + 500, 'x');
  const std::string cut = truncate_output(big);
  ST_CHECK(cut.size() > kTestOutputLimit);
  ST_CHECK(cut.size() < big.size());
  ST_CHECK(cut.find("已截断") != std::string::npos);
}

ST_TEST(xiaobai_tool_paths_resolve_against_root) {
  const std::string root = st::fs::join(".", "workspace");
  // 相对路径 → 拼到根下。
  ST_CHECK_EQ(resolve_path(root, "a.txt"), st::fs::join(root, "a.txt"));
  // 绝对路径原样保留（两种平台形态）。
  ST_CHECK_EQ(resolve_path(root, "/etc/hosts"), std::string("/etc/hosts"));
  ST_CHECK_EQ(resolve_path(root, "C:\\temp\\a.txt"), std::string("C:\\temp\\a.txt"));
  // 空路径 → 根自己（`read` 无参时读目录会如实失败，而不是读到别处）。
  ST_CHECK_EQ(resolve_path(root, ""), root);
}
