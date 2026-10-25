#include "st/ui/hot_reload.hpp"

#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "st/core/fs.hpp"
#include "st/core/hash.hpp"
#include "st/core/log.hpp"
#include "st/core/time.hpp"
#include "st/ext/json.hpp"
#include "st/ui/element.hpp"
#include "st/ui/script_bridge.hpp"
#include "st/ui/script_host.hpp"
#include "st/ui/ui_root.hpp"

namespace st::ui {
namespace {

/// 一份**已读入内存**的资源：内容 + 指纹。
///
/// 为什么指纹用**内容**而不是 mtime：本仓库已经吃过一次「mtime 判不掉改动」的账
/// （`CONVENTIONS.md` §7.4：`git checkout`/`copy2` 恢复文件后 mtime 可能比产物还旧，
/// 于是"改了没生效"或"跑的是旧二进制"）。改文件即生效这条路里，判错方向的代价是
/// **保存了却看不到变化**——那是这条通路唯一会让人放弃它的症状。文件是几 KB 的文本，
/// 250ms 一次的 FNV-1a 全部成本可忽略，换来的是"保存就一定生效"的确定性。
struct Watched {
  std::string path{};
  std::uint64_t fingerprint{0};
  bool present{false};

  /// 重新取基线（读不到：`present=false`——**不报错**，因为"文件暂时不存在"
  /// 在编辑器原子保存（写临时文件 → rename）的过程里是常态，报错只是噪音）。
  void rebase() {
    auto text = st::fs::read_text(path);
    present = text.has_value();
    fingerprint = present ? st::hash::fnv1a64(*text) : 0;
  }

  /// 内容变了没（**读文件比对哈希**，不是看 mtime；两边都不存在时不算变）。
  [[nodiscard]] auto dirty() const -> bool {
    auto text = st::fs::read_text(path);
    if (!text.has_value()) return false;
    return !present || st::hash::fnv1a64(*text) != fingerprint;
  }
};

/// XML 建树结果的报告 → 一句话（**成功也用**：认不出的属性是"界面少了点东西"的
/// 最常见原因，而静默跳过会把排查从"看日志"变成"读源码"）。
[[nodiscard]] auto describe(const XmlBuildReport& report) -> std::string {
  std::string out = std::format("{} 个元素", report.elements);
  if (!report.unknown_tags.empty()) {
    out += "；认不出的标签: ";
    for (std::size_t index = 0; index < report.unknown_tags.size(); ++index) {
      if (index != 0) out += ", ";
      out += report.unknown_tags[index];
    }
  }
  if (!report.unknown_attributes.empty()) {
    out += std::format("；未生效的属性 {} 处: ", report.unknown_attributes.size());
    // 只列前 6 条：完整清单在 stderr（人要看的是"有没有问题"，不是逐条读）
    for (std::size_t index = 0; index < report.unknown_attributes.size() && index < 6; ++index) {
      if (index != 0) out += ", ";
      out += report.unknown_attributes[index];
    }
    if (report.unknown_attributes.size() > 6) out += " …";
  }
  if (!report.parse_warning.empty()) out += "；解析提示: " + report.parse_warning;
  return out;
}

}  // namespace

struct HotReload::Impl {
  UiRoot* root{nullptr};
  ScriptHost* script{nullptr};
  ScriptBridge* bridge{nullptr};
  Options options{};
  Factory factory{};

  /// 上一次**已生效**的 XML 根（未装载为 nullptr）。
  Element* current{nullptr};
  Watched xml{};
  Watched script_file{};
  std::vector<Watched> pages{};
  bool visible{true};
  std::int64_t last_poll_ms{0};
  std::string last_summary{};
  std::uint64_t ui_refreshes{0};
  std::uint64_t script_refreshes{0};
  /// 上一次刷新失败的原因（空 = 没失败过）。成功刷新会清掉它。
  std::string last_error{};

  [[nodiscard]] auto xml_path() const -> std::string {
    return st::fs::join(options.ui_dir, options.xml_name);
  }
  [[nodiscard]] auto script_path() const -> std::string {
    return st::fs::join(options.ui_dir, options.script_name);
  }

  /// 清掉脚本侧的**逻辑**：事件绑定 + 定时器 + 脚本状态。
  ///
  /// 为什么必须清：JS 在同一引擎里 eval，`(function(){ … })()` 每装载一次就在新作用域里
  /// 重建闭包——旧闭包仍然被 `handlers`/`timers` 表**强引用**（那是 append-only 的表），
  /// 它们不会自己消失。不清的后果有两个，都是实测级：① 一次点击跑两遍旧处理器
  /// （表现是"点了没反应/值又弹回去"）；② 每个定时器被装出第二份，重载几次后
  /// CPU 与内存一起涨。
  ///
  /// 清的是 `script_api.js` 的两张表（宿主只经公开入口 `clear_bindings` 动自己的表）。
  auto reset_script_state() -> Status {
    auto outcome = script->eval_prelude(
        "__reset_script_state()", "<hot-reload>");
    if (!outcome.has_value()) {
      return st::unexpected(outcome.error().code,
                            std::format("脚本运行时重置失败：{}", outcome.error().message));
    }
    return st::ok();
  }

  /// 全部脚本页的路径（主脚本在前，额外页按序）。
  [[nodiscard]] auto script_paths() const -> std::vector<std::string> {
    std::vector<std::string> paths{};
    if (!options.ui_dir.empty()) paths.push_back(script_path());
    for (const auto& page : options.pages) {
      paths.push_back(st::fs::join(options.ui_dir, page));
    }
    return paths;
  }
};

HotReload::HotReload(UiRoot& root, ScriptHost& script, ScriptBridge& bridge, Options options)
    : impl_(std::make_unique<Impl>()) {
  impl_->root = &root;
  impl_->script = &script;
  impl_->bridge = &bridge;
  impl_->options = std::move(options);
  impl_->visible = impl_->options.start_visible;
  impl_->xml.path = impl_->xml_path();
  impl_->script_file.path = impl_->script_path();
  for (const auto& page : impl_->options.pages) {
    Watched watched;
    watched.path = st::fs::join(impl_->options.ui_dir, page);
    impl_->pages.push_back(std::move(watched));
  }
}

HotReload::~HotReload() = default;

void HotReload::set_factory(Factory factory) { impl_->factory = std::move(factory); }

auto HotReload::refresh_ui() -> Status {
  if (impl_->options.ui_dir.empty()) {
    return st::unexpected(st::ErrorCode::Unsupported,
                          "未配置资源目录（HotReload::Options::ui_dir）");
  }
  if (impl_->root->content() == nullptr) {
    // 没有内容就没有"挂到哪"的落点：壳层必须先 `set_content`（窗框/标题栏）再建热重载。
    return st::unexpected(st::ErrorCode::Invalid,
                          "根内容为空：请先 app.set_content(壳层)，再 refresh_ui()");
  }

  // —— ① 建树（失败即返回，界面上什么都还没动）——
  XmlBuildReport report;
  std::unique_ptr<Element> tree{};
  if (impl_->factory) {
    tree = impl_->factory();
    if (tree == nullptr) {
      return st::unexpected(st::ErrorCode::Parse, "建树钩子返回了空树");
    }
  } else {
    XmlBuildOptions build_options;
    build_options.strict = impl_->options.strict_xml;
    tree = st::ui::build_from_xml_file(impl_->xml_path(), &report, build_options);
    if (tree == nullptr) {
      impl_->last_error = report.parse_warning.empty() ? "建树失败（未给出原因）"
                                                       : report.parse_warning;
      return st::unexpected(st::ErrorCode::Parse,
                            std::format("{} 建树失败：{}", impl_->xml_path(), impl_->last_error));
    }
    // 认不出的**标签**按“建树失败”处理（界面保持旧版）——这与 `build_from_xml` 自己的
    // 容错取向不同，差别在“换树”这个场景：
    //
    // · 认不出的**属性**只丢一个属性值（元素还在、界面大致完整）→ 告警但照旧换上去；
    // · 认不出的**标签**是**整棵子树没有句柄**→ 容错建出来的树看起来只剩半个页面。
    //   而写文件的人正在看窗口，“保存后界面瘦了”比“保存后报错、界面没变”难查得多。
    //
    // 于是：属性告警、标签拒绝——两种错，两种处置，各自与后果对应。
    if (!report.unknown_tags.empty() && !impl_->options.strict_xml) {
      std::string names{};
      for (const auto& tag : report.unknown_tags) {
        if (!names.empty()) names += ", ";
        names += std::format("<{}>", tag);
      }
      impl_->last_error = std::format("认不出的标签: {}", names);
      return st::unexpected(st::ErrorCode::Parse,
                            std::format("{} 建树失败：{}", impl_->xml_path(), impl_->last_error));
    }
  }

  // —— ② 换根（原子段：从这里到结束都没有失败路径）——
  Element* previous = impl_->current;
  Element* parent = previous != nullptr ? previous->parent() : nullptr;
  if (parent == nullptr) {
    // 首次装载：默认挂到根内容下（与 `xmlapp` 的手工写法同位置）。
    parent = impl_->root->content();
  }
  if (previous != nullptr) {
    std::unique_ptr<Element> retired = parent->remove_child(previous);
    impl_->current = nullptr;
    // `retired` 在这里析构：旧树整棵销毁。旧树上的脚本绑定由
    // `ScriptHost::prune_dead_bindings` 在下次事件/注册时回收（见头文件的"降级如实"）。
  }
  tree->set_visible(impl_->visible);
  impl_->current = parent->add_child(std::move(tree));
  if (impl_->current->host() == nullptr) {
    // 挂入时 `add_child` 已 `adopt_host()`；走到这里只可能是父链没接上宿主
    // （壳层不是从 `set_content` 挂进来的）——如实记一次，不静默。
    impl_->last_error = "新树没有拿到宿主契约（壳层可能未经 UiRoot::set_content 挂载）";
  }
  // 换根后整棵树都换了：布局与损坏区全部作废（`set_content` 同样这么干）。
  impl_->root->mark_dirty_all();

  // —— ③ 报告（成功也报：未生效的属性就是"界面少了点东西"的线索）——
  const std::string summary = describe(report);
  impl_->last_summary = std::format("界面已重建：{}（{}）", impl_->xml_path(), summary);
  impl_->last_error.clear();
  ++impl_->ui_refreshes;
  ST_LOG_INFO("{}", impl_->last_summary);
  if (!report.unknown_attributes.empty() || !report.unknown_tags.empty() ||
      !report.parse_warning.empty()) {
    ST_LOG_WARN("[hot] {}：{}", impl_->xml_path(), summary);
  }
  // 基线更新：重建之后**立刻**取一次内容，避免把"这次重建用到的内容"当成下一次的改动。
  impl_->xml.rebase();
  return st::ok();
}

auto HotReload::refresh_script() -> Status {
  const auto paths = impl_->script_paths();
  if (paths.empty()) {
    return st::unexpected(st::ErrorCode::Unsupported,
                          "未配置脚本（HotReload::Options::ui_dir 为空）");
  }
  // 宿主侧无法知道"上一轮谁调了 refresh_ui"，但**重放本身**不需要重 expose：
  // 桥挂的宿主函数（`app.*`）在整轮重载中一直存在（那是引擎里的全局函数，不受
  // 页面脚本重装载影响）。所以"换树后重放"走的就是下面这条同一条路径。

  // —— ① 先把全部页读进内存（原子性的一半：读失败时一行旧逻辑都不动）——
  std::vector<std::string> sources{};
  for (const auto& path : paths) {
    auto text = st::fs::read_text(path);
    if (!text.has_value()) {
      impl_->last_error = std::format("读不到脚本 {}：{}", path, text.error().message);
      return st::unexpected(text.error().code, impl_->last_error);
    }
    sources.push_back(std::move(*text));
  }

  // —— ② 语法检查全部页（原子性的另一半：任一页写错，旧逻辑原封不动）——
  for (std::size_t index = 0; index < paths.size(); ++index) {
    auto checked = impl_->script->check_syntax(sources[index]);
    if (!checked.has_value()) {
      impl_->last_error = std::format("{}：{}", paths[index], checked.error().message);
      return st::unexpected(st::ErrorCode::Parse, impl_->last_error);
    }
  }

  // —— ③ 换逻辑（从这里到结束都没有失败路径）——
  // 绑定：先清宿主侧表（`ScriptHost::bindings`），再清 JS 侧表（handlers/timers/state）。
  // 前者用公开入口；后者必须在**旧闭包还在**时清（脚本重载后全局函数会换成新的）。
  impl_->script->clear_bindings();
  if (auto status = impl_->reset_script_state(); !status) {
    impl_->last_error = status.error().message;
    return status;
  }
  for (std::size_t index = 0; index < paths.size(); ++index) {
    if (auto status = impl_->bridge->run(sources[index], paths[index]); !status) {
      // 语法已检查过，走到这里是**运行时**错误（顶层代码抛异常）：界面保持新树，
      // 逻辑报错带文件名——不假装成功，也不回滚（回滚会把上一版逻辑装回来，
      // 而用户刚刚明确保存了这一版）。
      impl_->last_error = status.error().message;
      return st::unexpected(status.error().code, impl_->last_error);
    }
  }
  impl_->last_summary = paths.size() == 1
                            ? std::format("逻辑已重装：{}", paths.front())
                            : std::format("逻辑已重装：{}（{} 页）", paths.front(), paths.size());
  impl_->last_error.clear();
  ++impl_->script_refreshes;
  ST_LOG_INFO("{}", impl_->last_summary);
  // 基线更新
  impl_->script_file.rebase();
  for (std::size_t index = 0; index < impl_->pages.size() && index + 1 < paths.size(); ++index) {
    impl_->pages[index].path = paths[index + 1];
    impl_->pages[index].rebase();
  }
  return st::ok();
}

auto HotReload::refresh_all() -> Status {
  if (auto status = refresh_ui(); !status) return status;
  return refresh_script();
}

auto HotReload::poll() -> std::size_t {
  if (!impl_->options.watch) return 0;
  if (impl_->options.ui_dir.empty()) return 0;
  const std::int64_t now = st::time::now_ms();
  if (impl_->last_poll_ms != 0 &&
      static_cast<std::uint64_t>(now - impl_->last_poll_ms) < impl_->options.poll_ms) {
    return 0;
  }
  impl_->last_poll_ms = now;

  bool xml_dirty = impl_->xml.dirty();
  bool script_dirty = impl_->script_file.dirty();
  for (auto& page : impl_->pages) {
    script_dirty = page.dirty() || script_dirty;
  }
  std::size_t refreshed = 0;
  // 顺序：**先界面后逻辑**。新逻辑往往引用新 XML 里的 id，反过来会先报一堆
  // "找不到元素"。界面失败时不动逻辑（逻辑挂在刚建出来的树上才有意义）。
  if (xml_dirty) {
    if (auto status = refresh_ui(); status) {
      ++refreshed;
      // 界面重建会销毁旧树：旧树上的绑定已经死了，脚本必须重放一遍才重新绑上。
      // **只在重建成功时**重放：换树失败时什么都没变，此时重放只会把"还挂在旧树上"
      // 的绑定先清掉——一次写错的 XML 会顺手把可用的界面弄成不响应（与“原子替换”相反）。
      if (!script_dirty) script_dirty = true;
    } else {
      ST_LOG_WARN("[hot] 界面重载失败（旧界面保持，改回来会自动生效）：{}", status.error().message);
      // 基线照更新：同一份写错的内容不该每 250ms 再报一次。
      impl_->xml.rebase();
    }
  }
  if (script_dirty) {
    if (auto status = refresh_script(); status) {
      ++refreshed;
    } else {
      ST_LOG_WARN("[hot] 逻辑重载失败（旧逻辑保持运行）：{}", status.error().message);
      impl_->script_file.rebase();
      for (auto& page : impl_->pages) page.rebase();
    }
  }
  return refreshed;
}

void HotReload::note_external_refresh() noexcept {
  impl_->xml.rebase();
  impl_->script_file.rebase();
  for (auto& page : impl_->pages) page.rebase();
  impl_->last_poll_ms = st::time::now_ms();
}

void HotReload::set_ui_visible(bool visible) {
  impl_->visible = visible;
  if (impl_->current != nullptr) {
    impl_->current->set_visible(visible);
    impl_->root->mark_dirty_all();
  }
}

auto HotReload::ui_visible() -> bool { return impl_->visible && impl_->current != nullptr &&
                                             impl_->current->visible(); }

auto HotReload::status() -> st::Json {
  st::Json result = st::Json::object();
  result["ui_dir"] = impl_->options.ui_dir;
  result["xml"] = impl_->xml.path;
  result["script"] = impl_->script_path();
  st::Json pages = st::Json::array();
  for (const auto& page : impl_->pages) pages.push_back(page.path);
  result["pages"] = std::move(pages);
  result["watch"] = impl_->options.watch;
  result["ui_loaded"] = impl_->current != nullptr;
  result["ui_visible"] = ui_visible();
  result["ui_refreshes"] = impl_->ui_refreshes;
  result["script_refreshes"] = impl_->script_refreshes;
  result["bindings"] = impl_->script->bindings().size();
  result["last_summary"] = impl_->last_summary;
  result["last_error"] = impl_->last_error;
  return result;
}

auto HotReload::last_summary() const -> const std::string& { return impl_->last_summary; }

auto HotReload::current_ui() const noexcept -> Element* { return impl_->current; }

}  // namespace st::ui
