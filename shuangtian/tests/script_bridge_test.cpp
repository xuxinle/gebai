/// `st::ui::ScriptBridge` 的契约测试：**JS 调 C++** 这条通路。
///
/// 它是"纯 XML + JS 开发应用"里 JS 那一半的地基：脚本写界面逻辑，
/// 真实能力（读文件、跑命令、查库）留在 C++。

#include "st/ui/script_bridge.hpp"

#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <vector>

#include "st/core/print.hpp"
#include "st/core/json.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/script_host.hpp"
#include "st/ui/ui_root.hpp"
#include "st/test/test.hpp"

namespace {

using st::Json;

/// 一个最小的"应用能力"集合（测试里当 C++ 侧的能力提供方）。
struct FakeApp {
  int counter{0};
  std::string last_message{};

  auto install(st::ui::ScriptBridge& bridge) -> void {
    (void)bridge.expose("counter.bump", [this](const std::vector<Json>& args) -> st::Result<Json> {
      counter += args.empty() ? 1 : static_cast<int>(args[0].get<double>());
      return Json(counter);
    });
    (void)bridge.expose("counter.get", [this](const std::vector<Json>&) -> st::Result<Json> {
      return Json(counter);
    });
    (void)bridge.expose("text.remember", [this](const std::vector<Json>& args) -> st::Result<Json> {
      if (args.empty()) {
        return st::unexpected(st::ErrorCode::Invalid, "text.remember 需要参数");
      }
      last_message = st::json_as_string(args[0]);
      return Json(last_message);
    });
    // 失败路径：用来验证"C++ 失败 → JS 异常"而不是静默 undefined。
    (void)bridge.expose("always.fail", [](const std::vector<Json>&) -> st::Result<Json> {
      return st::unexpected(st::ErrorCode::Invalid, "故意失败");
    });
  }
};

auto eval_ok(st::ui::ScriptHost& script, const std::string& code) -> Json {
  auto result = script.eval(code, "<test>");
  if (!result.has_value()) {
    ST_FAIL("eval 失败：" + result.error().message);
    return Json();
  }
  return std::move(*result);
}

}  // namespace

ST_TEST(script_bridge_exposes_cpp_functions_under_namespaces) {
  st::ui::UiRoot root;
  st::ui::ScriptHost script(root);
  ST_REQUIRE(script.valid());
  st::ui::ScriptBridge bridge(script, &root);
  FakeApp app;
  app.install(bridge);

  // JS 侧按命名空间调用：`app.counter.bump()` / `app.text.remember(…)`。
  const Json bumped = eval_ok(script, "app.counter.bump(5); app.counter.bump();");
  st::print("[bridge] bump 返回：{}\n", st::json_dump(bumped));
  ST_CHECK(bumped.is_number());
  if (bumped.is_number()) ST_CHECK_EQ(bumped.get<int>(), 6);
  ST_CHECK_EQ(app.counter, 6);

  const Json remembered = eval_ok(script, R"(app.text.remember("你好，霜天");)");
  ST_CHECK_EQ(remembered.get<std::string>(), std::string("你好，霜天"));
  ST_CHECK_EQ(app.last_message, std::string("你好，霜天"));

  // "能用什么"应当可查（诊断/文档两面）。
  if (bridge.exposed().size() != 2) st::print("[bridge] 失败原因：{}\n", bridge.last_error());
  const auto& names = bridge.exposed();
  ST_CHECK(std::ranges::find(names, std::string("counter.bump")) != names.end());
  ST_CHECK(std::ranges::find(names, std::string("text.remember")) != names.end());
}

ST_TEST(script_bridge_rejects_underscore_and_bad_names) {
  st::ui::UiRoot root;
  st::ui::ScriptHost script(root);
  ST_REQUIRE(script.valid());
  st::ui::ScriptBridge bridge(script, &root);
  // `__` 是框架内建扩展层的命名段：占用它会与框架抢名字，且症状极难定位。
  ST_CHECK(!bridge.expose("__internal", [](const std::vector<Json>&) { return Json(); }));
  ST_CHECK(!bridge.expose("", [](const std::vector<Json>&) { return Json(); }));
  ST_CHECK(!bridge.expose("a..b", [](const std::vector<Json>&) { return Json(); }));
  ST_CHECK(!bridge.expose("a.", [](const std::vector<Json>&) { return Json(); }));
  // 正常名字可以。
  ST_CHECK(bridge.expose("ok.name", [](const std::vector<Json>&) { return Json(1); }));
  // 同名前缀不互相覆盖：`a.b` 与 `a.c` 是不同函数。
  ST_CHECK(bridge.expose("ns.one", [](const std::vector<Json>&) { return Json("one"); }));
  ST_CHECK(bridge.expose("ns.two", [](const std::vector<Json>&) { return Json("two"); }));
  ST_CHECK_EQ(eval_ok(script, "app.ns.one()").get<std::string>(), std::string("one"));
  ST_CHECK_EQ(eval_ok(script, "app.ns.two()").get<std::string>(), std::string("two"));
}

ST_TEST(script_bridge_cpp_failure_surfaces_as_js_exception) {
  st::ui::UiRoot root;
  st::ui::ScriptHost script(root);
  ST_REQUIRE(script.valid());
  st::ui::ScriptBridge bridge(script, &root);
  FakeApp app;
  app.install(bridge);

  // C++ 返回失败 → 脚本能 catch 到（不是静默拿到 undefined）。
  const Json caught =
      eval_ok(script, "let caught = ''; try { app.always.fail(); } catch (e) { caught = 'yes'; } caught");
  ST_CHECK_EQ(caught.get<std::string>(), std::string("yes"));
}

ST_TEST(script_bridge_loads_script_file_and_reports_path_on_error) {
  const std::filesystem::path temp =
      std::filesystem::temp_directory_path() / "st_script_bridge_test";
  std::filesystem::create_directories(temp);
  const std::filesystem::path good = temp / "logic.js";
  {
    std::ofstream out(good, std::ios::binary);
    out << "app.counter.bump(3);\n";
  }

  st::ui::UiRoot root;
  st::ui::ScriptHost script(root);
  ST_REQUIRE(script.valid());
  st::ui::ScriptBridge bridge(script, &root);
  FakeApp app;
  app.install(bridge);

  const auto status = bridge.load(good.string());
  ST_CHECK(status.has_value());
  ST_CHECK_EQ(app.counter, 3);
  ST_REQUIRE(bridge.loaded_files().size() == 1);

  // 不存在的文件：失败原因必须**具体到路径**（"脚本出错"没法排查）。
  const auto missing = bridge.load((temp / "nope.js").string());
  ST_CHECK(!missing.has_value());
  ST_CHECK(missing.error().message.find("nope.js") != std::string::npos);

  // 有语法错误的文件：也要带上文件名。
  const std::filesystem::path broken = temp / "broken.js";
  {
    std::ofstream out(broken, std::ios::binary);
    out << "function ( { \n";
  }
  const auto bad = bridge.load(broken.string());
  ST_CHECK(!bad.has_value());
  ST_CHECK(bad.error().message.find("broken.js") != std::string::npos);

  std::filesystem::remove_all(temp);
}

ST_TEST(script_bridge_ui_helpers_wrap_the_same_implementation) {
  // `ui.set` / `ui.invoke` / `ui.query` / `ui.exists` 是
  // `ui::apply_properties` / `ui::invoke_element` 的薄包装——
  // 与协议 `set`/`invoke` 同一份实现，不是第三套语义。
  st::ui::UiRoot root;
  auto content = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
  auto button = std::make_unique<st::ui::Button>("确定", st::ui::Button::Variant::Primary,
                                                 st::ui::Button::Size::Small);
  button->set_id("ok");
  content->add_child(std::move(button));
  root.set_content(std::move(content));
  root.layout(true);

  st::ui::ScriptHost script(root);
  ST_REQUIRE(script.valid());
  st::ui::ScriptBridge bridge(script, &root);

  ST_CHECK_EQ(eval_ok(script, "app.ui.exists('ok')").get<bool>(), true);
  ST_CHECK_EQ(eval_ok(script, "app.ui.exists('nope')").get<bool>(), false);

  // 写属性 → 读回来（与协议 set 同一份实现）。
  const Json changed = eval_ok(script, R"(app.ui.set('ok', {label: '已启动'}).join(',');)");
  ST_CHECK(changed.get<std::string>().find("label") != std::string::npos);

  // 按选择器查元素。
  const Json found = eval_ok(script, "app.ui.query('Button').length;");
  ST_CHECK_EQ(found.get<int>(), 1);
}

ST_TEST(script_bridge_run_inline_source) {
  st::ui::UiRoot root;
  st::ui::ScriptHost script(root);
  ST_REQUIRE(script.valid());
  st::ui::ScriptBridge bridge(script, &root);
  ST_CHECK(bridge.expose("add", [](const std::vector<Json>& args) -> st::Result<Json> {
    int sum = 0;
    for (const auto& item : args) sum += static_cast<int>(item.get<double>());
    return Json(sum);
  }));
  // ⚠ 分两步断言（第一版在这里崩过整个测试进程，值得记下来）：
  //
  // 原来只有一行 `bridge.run("globalThis.total = app.add(1, 2, 3);")` 加一句
  // `eval_ok(script, "total").get<int>()`。当 `app.add` 这一侧有任何问题（名字没对上、
  // 调用抛错）时，JS 里 `total` **根本不会被赋值**，于是：
  //   `eval_ok` 拿到 ReferenceError → 返回 `Json()`（null）→ `.get<int>()` **抛异常**。
  // 而当时测试框架**不接用例里的异常**，异常逃到线程入口 = `std::terminate`
  // ⇒ 整套 1268 条用例在第 800 条处整进程消失（只有一句 terminate，没有 FAIL 行）。
  //
  // 所以：**先断言"赋值这一步成功了"**（`run` 的返回值 + `total` 真的存在），
  // 再断言它的值。中间那一步的失败才是"为什么后面的值为空"的直接答案。
  const auto assigned = bridge.run("globalThis.total = app.add(1, 2, 3);");
  ST_CHECK(assigned.has_value());
  if (!assigned.has_value()) {
    // 把 JS 侧的错误原样带出来（否则只能看到"值不对"，看不到"为什么"）。
    ST_FAIL("app.add(1,2,3) 调用失败：" + assigned.error().message);
    return;
  }
  const Json total = eval_ok(script, "total");
  // `get<int>()` 对 null 会**抛**（不是断言失败）——所以先判类型再取。
  if (!total.is_number()) {
    ST_FAIL(std::format("globalThis.total 不是数字（实际 {}）", st::json_dump(total)));
    return;
  }
  ST_CHECK_EQ(total.get<int>(), 6);
  // `run` 的失败也要说清是哪个文件名。
  const auto bad = bridge.run("not valid js ((", "inline-test.js");
  ST_CHECK(!bad.has_value());
  ST_CHECK(bad.error().message.find("inline-test.js") != std::string::npos);
}

ST_TEST(script_bridge_reverse_direction_cpp_calls_js) {
  // C++ → JS 的反向通道（宿主主动推数据给脚本）。
  st::ui::UiRoot root;
  st::ui::ScriptHost script(root);
  ST_REQUIRE(script.valid());
  st::ui::ScriptBridge bridge(script, &root);
  (void)bridge.run("globalThis.on_message = function (text) { return 'got:' + text; };");
  const auto reply = bridge.call("on_message", {Json("hello")});
  ST_REQUIRE(reply.has_value());
  ST_CHECK_EQ(reply->get<std::string>(), std::string("got:hello"));
  // 不存在的函数：失败而不是崩。
  ST_CHECK(!bridge.call("no_such_fn", {}).has_value());
}
