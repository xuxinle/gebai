/// `st::ui::HotReload` 的契约测试：**改文件即生效**。
///
/// 这一层的价值全在四个行为上，测试就钉这四个（每条都对应一次真实的失效方式）：
/// ① **替换**：改 XML 换的是整棵树（新元素进来、旧元素真的没了）；
/// ② **原子**：XML/JS 写错时界面对应的那一半**保持原状**（不是“先清后失败”）；
/// ③ **清场**：JS 重装前把旧绑定/定时器/状态清干净（不清就是“一次点击跑两遍处理器”）；
/// ④ **不重编**：整个过程只有文件 IO —— 所以它在**没有构建**的情况下也能这么测
///    （本用例不编任何东西，跑完也就几十毫秒）。
///
/// ⚠ 反例的价值：这几条如果只在 e2e 里验，改动一处“顺手清空再装”不会有人发现；
/// 而真出问题时的现象是“保存后界面瘦了一半”或“点一下加两条”——都是**看起来像业务 bug** 的错。

#include "st/ui/hot_reload.hpp"

#include <filesystem>
#include <chrono>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "st/core/fs.hpp"
#include "st/core/json.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/script_bridge.hpp"
#include "st/ui/script_host.hpp"
#include "st/ui/ui_root.hpp"
#include "st/test/test.hpp"

namespace {

/// 一个临时资源目录：`main.xml` + `app.js` 由用例现写（**不碰仓库里的示例文件**）。
struct Workspace {
  std::filesystem::path dir{};

  explicit Workspace(const char* name) {
    dir = std::filesystem::temp_directory_path() / std::string("st_hot_reload_") / name;
    std::error_code code{};
    std::filesystem::remove_all(dir, code);
    std::filesystem::create_directories(dir, code);
  }
  ~Workspace() {
    std::error_code code{};
    std::filesystem::remove_all(dir, code);
  }
  Workspace(const Workspace&) = delete;
  auto operator=(const Workspace&) -> Workspace& = delete;

  void write(const char* file, const std::string& text) const {
    // 与编辑器一致：**先写临时文件再替换**（不是截断重写）。热重载判"变了"靠内容指纹，
    // 但那不该依赖写入手法——这条写法顺手把"同一秒内连改两次"也覆盖了。
    const auto path = dir / file;
    const auto temp = dir / (std::string(file) + ".tmp");
    {
      std::ofstream out(temp, std::ios::binary | std::ios::trunc);
      out << text;
    }
    std::filesystem::rename(temp, path);
  }
  [[nodiscard]] auto path(const char* file) const -> std::string {
    return (dir / file).string();
  }
};

auto find_id(st::ui::UiRoot& root, std::string_view id) -> st::ui::Element* {
  return root.find(id);   // `UiRoot::find` 就是全树按 id 找（与协议/脚本同一条路）
}

/// 元素自报的 `text` 属性（Element 基类没有 `text()`：那是各组件自己的属性面）。
auto text_of(st::ui::Element& element) -> std::string {
  return element.get_property("text").value_or("");
}

/// 走真实派发链点一下某个元素（与用户点击同一条路径：命中 → 分发 → 脚本绑定）。
///
/// 为何不 `$('#go').click()`：那是脚本侧动作（与 `invoke click` 同源），
/// 而"绑定叠没叠"只在**事件观察者**这条路径上才看得见——一次派发跑两遍处理器就是它。
void click_at(st::ui::UiRoot& root, st::ui::Element& target) {
  // 先布局：命中测试靠 `bounds()`，未排布时矩形全零（点不到任何元素）。
  root.layout(true);
  const st::math::Rect bounds = target.bounds();
  st::ui::Event event;
  event.kind = st::ui::EventKind::Click;
  event.position =
      st::math::Point{bounds.x + bounds.width * 0.5f, bounds.y + bounds.height * 0.5f};
  event.button = 1;
  event.click_count = 1;
  (void)root.dispatch(event);
}

constexpr const char* kXml = R"(<Column id="root" gap="8" grow="true">
  <Text id="title">原始标题</Text>
  <Button id="go" label="运行"/>
</Column>
)";

/// 逻辑页：调一次宿主函数（证明桥还活着）、把状态写进 `state`（验“清场”）、
/// 并给按钮绑一个点击（验“绑定不叠”）。
///
/// ⚠ 绑定前判 `exists`：这页会同时用在“XML 里没有 `#go`”的树形上——
/// 未命中时 `$()` 返回的是空句柄（`set/click` 存在但 `on` 不存在），直接 `.on`
/// 就会报 `not a function`。而热重载的语义是“新 XML 删了元素、逻辑不许因此碎掉”
/// （真实应用也这么写），用例依此模拟。
constexpr const char* kJs = R"((function () {
  'use strict';
  app.probe.bump();
  state.visited = (state.visited || 0) + 1;
  if ($('#go').exists) $('#go').on('click', function () { app.probe.bump(); });
})();
)";

/// 测试夹具：真壳层 + 真脚本站 + 真桥 + HotReload（都是真件，没有桩——
/// 这一层的行为全在它们的配合上，把任何一个换成桩就把被判的行为抹平了）。
struct Fixture {
  Workspace workspace;
  st::ui::UiRoot root;
  st::ui::ScriptHost script;
  st::ui::Button* shell{nullptr};
  int bumps{0};

  explicit Fixture(const char* name) : workspace(name), script(root) {
    // 壳层：热重载换根的落点是 XML 根的父元素（`WindowFrame::content()` 的等价物）。
    auto shell_element = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
    shell_element->set_id("shell");
    root.set_content(std::move(shell_element));
    root.set_viewport(st::math::Size{400.0f, 300.0f});
  }

  [[nodiscard]] auto bridge() -> st::ui::ScriptBridge& {
    if (!bridge_) {
      bridge_ = std::make_unique<st::ui::ScriptBridge>(script, &root);
      (void)bridge_->expose("probe.bump", [this](const std::vector<st::Json>&) -> st::Result<st::Json> {
        ++bumps;
        return st::Json(bumps);
      });
    }
    return *bridge_;
  }

  [[nodiscard]] auto hot(bool watch = false) -> st::ui::HotReload& {
    if (!hot_) {
      hot_ = std::make_unique<st::ui::HotReload>(root, script, bridge(), make_options(watch));
    }
    return *hot_;
  }

  [[nodiscard]] auto make_options(bool watch) const -> st::ui::HotReload::Options {
    st::ui::HotReload::Options options;
    options.ui_dir = workspace.dir.string();
    options.watch = watch;
    return options;
  }

 private:
  std::unique_ptr<st::ui::ScriptBridge> bridge_{};
  std::unique_ptr<st::ui::HotReload> hot_{};
};

}  // namespace

ST_TEST(hot_reload_rebuilds_ui_and_replays_script) {
  Fixture fixture("rebuild");
  fixture.workspace.write("main.xml", kXml);
  fixture.workspace.write("app.js", kJs);

  // 开局：一次装载把两样都装到位（等价于"启动时读一次"）。
  ST_REQUIRE(fixture.hot().refresh_all().has_value());
  ST_CHECK(find_id(fixture.root, "title") != nullptr);
  ST_CHECK_EQ(fixture.bumps, 1);   // 逻辑真的跑了（bridge 可用）
  st::ui::Element* title = find_id(fixture.root, "title");
  ST_REQUIRE(title != nullptr);
  ST_CHECK_EQ(text_of(*title), std::string("原始标题"));

  // ① 改 XML：**换的是树**——新元素进来、旧元素没了。
  fixture.workspace.write("main.xml", R"(<Column id="root" gap="8" grow="true">
  <Text id="title">新标题</Text>
  <Text id="added">新增</Text>
</Column>
)");
  ST_REQUIRE(fixture.hot().refresh_ui().has_value());
  st::ui::Element* new_title = find_id(fixture.root, "title");
  ST_REQUIRE(new_title != nullptr);
  ST_CHECK_EQ(text_of(*new_title), std::string("新标题"));
  ST_CHECK(new_title != title);   // 是新建的元素，不是原地改属性（指针不同即证据）
  ST_CHECK(find_id(fixture.root, "added") != nullptr);
  ST_CHECK(find_id(fixture.root, "go") == nullptr);   // 旧 XML 里的按钮真的退场了

  // ② 重装逻辑：宿主函数仍可用（清了不该清的东西就报 not a function）。
  ST_REQUIRE(fixture.hot().refresh_script().has_value());
  ST_CHECK_EQ(fixture.bumps, 2);

  // ③ 清场：脚本状态被清（`visited` 从头算，不是 1→2）。
  auto state = fixture.script.state();
  ST_REQUIRE(state.has_value());
  const auto visited = st::json_get_i64(*state, "visited", -1);
  ST_CHECK_EQ(visited, std::int64_t{1});

  // ④ 一次事件只调一次处理器（旧绑定没留在宿主表里）。
  //    先把 go 建回来（上面那版 XML 把它删了），再用**真实派发**点一下。
  //
  //    为何不走 `$('#go').click()`：那是脚本动作（与 `invoke click` 同源），
  //    而"绑定叠没叠"只在**事件观察者**路径上看得见（一次派发跑两遍处理器就是它）。
  fixture.workspace.write("main.xml", R"(<Column id="root" gap="8" grow="true">
  <Text id="title">新标题</Text>
  <Button id="go" label="运行"/>
</Column>
)");
  // ⚠ 必须先刷界面再刷逻辑：**先刷逻辑再换树**的话，新绑定会挂在即将被销毁的旧元素上
  //（`on('#go', …)` 是按选择器注册的，注册那一刻元素必须已经在树上）。
  ST_REQUIRE(fixture.hot().refresh_all().has_value());
  st::ui::Element* go = find_id(fixture.root, "go");
  ST_REQUIRE(go != nullptr);
  // 绑定条数：整轮重载后仍应是 1 条（旧闭包的那条被 `clear_bindings` 清掉了）。
  ST_CHECK_EQ(fixture.script.bindings().size(), std::size_t{1});
  const int bumps_before_click = fixture.bumps;
  click_at(fixture.root, *go);
  ST_CHECK_EQ(fixture.bumps, bumps_before_click + 1);
}

ST_TEST(hot_reload_keeps_ui_when_xml_is_broken) {
  Fixture fixture("broken-xml");
  fixture.workspace.write("main.xml", kXml);
  fixture.workspace.write("app.js", kJs);
  ST_REQUIRE(fixture.hot().refresh_all().has_value());
  st::ui::Element* title = find_id(fixture.root, "title");
  ST_REQUIRE(title != nullptr);

  // 写坏标签（`<Buton>`）：**整棵建树被拒**——换上去的话页面只剩"能认出的那部分"，
  // 而写文件的人正在看窗口，看到的是"保存后界面瘦了"，比"保存后报错、界面没变"难查得多。
  fixture.workspace.write("main.xml", R"(<Column id="root"><Buton id="oops"/></Column>)");
  const auto status = fixture.hot().refresh_ui();
  ST_CHECK(!status.has_value());
  if (!status.has_value()) {
    ST_CHECK(status.error().message.find("Buton") != std::string::npos);
  }
  // 界面**原封不动**：还是原来的元素（同一个指针），
  ST_CHECK(find_id(fixture.root, "title") == title);
  ST_CHECK(find_id(fixture.root, "go") != nullptr);
  // 也没冒出半棵新树。
  ST_CHECK(find_id(fixture.root, "oops") == nullptr);

  // 改回来 → 立刻可用（失败不该留下"再也装不上"的状态）。
  fixture.workspace.write("main.xml", kXml);
  ST_CHECK(fixture.hot().refresh_ui().has_value());
}

ST_TEST(hot_reload_keeps_script_when_js_has_syntax_error) {
  Fixture fixture("broken-js");
  fixture.workspace.write("main.xml", kXml);
  fixture.workspace.write("app.js", kJs);
  ST_REQUIRE(fixture.hot().refresh_all().has_value());
  const int bumps_before = fixture.bumps;

  // 语法错：**先检查再清场**，因此旧逻辑原封不动（宿主函数还活着、绑定还在）。
  fixture.workspace.write("app.js", "function broken( { this is not js\n");
  const auto status = fixture.hot().refresh_script();
  ST_CHECK(!status.has_value());
  if (!status.has_value()) {
    ST_CHECK(status.error().message.find("app.js") != std::string::npos);
  }
  // 旧绑定没被清（清场发生在检查之后）：仍旧只有那 1 条 `#go` 绑定。
  ST_CHECK_EQ(fixture.script.bindings().size(), std::size_t{1});
  // 旧逻辑还能跑（再触发一次点击，宿主函数照旧累加）。
  st::ui::Element* go = find_id(fixture.root, "go");
  ST_REQUIRE(go != nullptr);
  click_at(fixture.root, *go);
  ST_CHECK_EQ(fixture.bumps, bumps_before + 1);

  // 运行时错误（语法没问题、顶层代码抛）：同样不该把界面弄没；错误如实返回。
  fixture.workspace.write("app.js", "throw new Error('顶层炸了');\n");
  const auto runtime_status = fixture.hot().refresh_script();
  ST_CHECK(!runtime_status.has_value());

  // 改回来 → 恢复。
  fixture.workspace.write("app.js", kJs);
  ST_CHECK(fixture.hot().refresh_script().has_value());
}

ST_TEST(hot_reload_watch_detects_content_change_not_timestamp) {
  Fixture fixture("watch");
  fixture.workspace.write("main.xml", kXml);
  fixture.workspace.write("app.js", kJs);
  st::ui::HotReload& hot = fixture.hot(/*watch=*/true);
  ST_REQUIRE(hot.refresh_all().has_value());

  // 没改动 → 不刷新（`poll` 是"有变化才动"，不是"每次都重来"）。
  ST_CHECK_EQ(hot.poll(), std::size_t{0});

  // 改内容 → 刷新（并顺带重放逻辑，因为界面换了树）。
  fixture.workspace.write("main.xml", R"(<Column id="root"><Text id="title">改过的</Text></Column>)");
  // `poll_ms` 默认 250：把节流窗口让过去（否则同一次 poll 里的节流会把这次改动推后）。
  // 这里直接调 `refresh_all` 太弱——要验的是 `poll()` 自己认得出改动，所以等窗口。
  auto refreshed = std::size_t{0};
  for (int attempt = 0; attempt < 40 && refreshed == 0; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    refreshed = hot.poll();
  }
  ST_CHECK(refreshed > 0);
  st::ui::Element* title = find_id(fixture.root, "title");
  ST_REQUIRE(title != nullptr);
  ST_CHECK_EQ(text_of(*title), std::string("改过的"));

  // 内容相同（只是重写一遍）→ **不算变化**：判据是内容指纹而不是 mtime。
  // 这条是"修改保存后没反应"和"根本没改也重载"的分界；用 mtime 的实现会在这里多刷一次。
  fixture.workspace.write("main.xml", R"(<Column id="root"><Text id="title">改过的</Text></Column>)");
  std::this_thread::sleep_for(std::chrono::milliseconds(320));
  ST_CHECK_EQ(hot.poll(), std::size_t{0});
}

ST_TEST(hot_reload_exposes_status_for_diagnosis) {
  Fixture fixture("status");
  fixture.workspace.write("main.xml", kXml);
  fixture.workspace.write("app.js", kJs);
  ST_REQUIRE(fixture.hot().refresh_all().has_value());

  const st::Json status = fixture.hot().status();
  ST_CHECK_EQ(st::json_get_string(status, "xml"),
              st::fs::join(fixture.workspace.dir.string(), "main.xml"));
  ST_CHECK_EQ(st::json_get_string(status, "script"),
              st::fs::join(fixture.workspace.dir.string(), "app.js"));
  ST_CHECK(st::json_get_bool(status, "ui_loaded", false));
  ST_CHECK_EQ(st::json_get_i64(status, "ui_refreshes", 0), std::int64_t{1});
  ST_CHECK_EQ(st::json_get_i64(status, "script_refreshes", 0), std::int64_t{1});
  // 界面可见性开关：只改 XML 根的 visible，不动壳层（"界面藏了"和"应用没了"要能分开）。
  ST_CHECK(fixture.hot().ui_visible());
  fixture.hot().set_ui_visible(false);
  ST_CHECK(!fixture.hot().ui_visible());
  ST_CHECK(find_id(fixture.root, "title") != nullptr);   // 元素都还在，只是不可见
  fixture.hot().set_ui_visible(true);
  ST_CHECK(fixture.hot().ui_visible());

  // 失败原因可查（成功会清掉它，所以这里制造一次失败再读）。
  fixture.workspace.write("main.xml", "<Column><Nope/></Column>");
  ST_CHECK(!fixture.hot().refresh_ui().has_value());
  ST_CHECK(!st::json_get_string(fixture.hot().status(), "last_error").empty());
}
