/// 智能体文档树与组件注册表的一致性测试。
///
/// ## 为什么这件事必须做成测试
///
/// 文档树（`docs/agent-docs/`）是**智能体查组件能力的唯一入口**：属性面、动作面、源码位置
/// 全在 `components/<族>/<组件>.md` 里，由 `tools/agent_docs_gen.py` 从组件头/实现生成。
///
/// 它不是"顺手更新的文档"，而是**能力清单**：漏一页 = 那个组件对模型不存在（模型只能靠
/// `find` 盲猜属性名）；页面过期 = 模型按错的属性面写代码，而错得不像自己的错。
/// 纯人工维护必然失效——本仓已有先例（`docs/README.md` 曾长期只收 26/49 份，
/// 见 `docs_map_test.cpp`）。所以判据在这里落成可执行测试：
///
/// | 判据 | 抓到什么 |
/// |---|---|
/// | 注册表每个名字都有页 | 新增组件忘了重跑生成器 |
/// | 每一页都对应注册表或登记过的"不进注册表"名单 | 删组件/改名后留下的陈旧页 |
/// | 页面结构与生成器契约一致（front matter + 四个小节） | 生成器改了模板而旧页没重生成 |
/// | 入口页存在且分层可发现 | 地图被清空/层级改名 |
///
/// 与 `docs_map_test.cpp` 的分工：那个管 `docs/*.md` 一级目录的收录，这里管**组件树**。
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/fs.hpp"
#include "st/core/log.hpp"
#include "st/test/test.hpp"

namespace {

constexpr std::string_view kComponentsDir = "include/st/ui/components";
constexpr std::string_view kTreeDir = "docs/agent-docs";
constexpr std::string_view kPagesDir = "docs/agent-docs/components";

/// 不进声明式注册表、但确实有文档页的组件（三维族等，理由写在各头末尾）。
const std::vector<std::string_view> kUnregistered{"ThreeView", "SceneView", "LabelOverlay", "GraphView"};

/// 从组件头里抽注册名：`ST_UI_COMPONENT(_WITH)?(::st::ui::X, "Name", ...)`。
///
/// 为什么手写扫描而不用正则：这里只需要"第二个参数的字面量"，手写解析在跨行写法
/// （不少组件头把两参数折行）下同样成立，且不引入 `<regex>` 的编译代价。
std::vector<std::string> registered_names(std::string_view text) {
  std::vector<std::string> names;
  std::string_view rest = text;
  while (true) {
    const auto at = rest.find("ST_UI_COMPONENT");
    if (at == std::string_view::npos) break;
    rest = rest.substr(at);
    const auto open = rest.find('(');
    if (open == std::string_view::npos) break;
    const auto comma = rest.find(',', open);
    if (comma == std::string_view::npos) break;
    const auto quote = rest.find('"', comma);
    const auto newline = rest.find('\n', comma);
    // 引号必须落在同一行的括号实参里（防止把宏后面的代码当参数）
    if (quote == std::string_view::npos || (newline != std::string_view::npos && quote > newline + 200)) break;
    const auto close = rest.find('"', quote + 1);
    if (close == std::string_view::npos) break;
    names.emplace_back(rest.substr(quote + 1, close - quote - 1));
    rest = rest.substr(close + 1);
  }
  return names;
}

/// 文档页文件名（不含 `.md`）→ 相对路径。只看 `components/<族>/<组件>.md` 这一层。
std::vector<std::string> component_pages() {
  std::vector<std::string> pages;
  const auto entries = st::fs::list_dir(kPagesDir);
  if (!entries.has_value()) return pages;
  for (const auto& family : *entries) {
    if (!family.is_dir) continue;
    const auto family_dir = std::string(kPagesDir) + "/" + family.name;
    const auto leafs = st::fs::list_dir(family_dir);
    if (!leafs.has_value()) continue;
    for (const auto& leaf : *leafs) {
      if (leaf.is_dir) continue;
      if (leaf.name.size() <= 3U) continue;
      if (leaf.name.compare(leaf.name.size() - 3U, 3U, ".md") != 0) continue;
      pages.push_back(leaf.name.substr(0, leaf.name.size() - 3U));
    }
  }
  std::sort(pages.begin(), pages.end());
  return pages;
}

}  // namespace

ST_TEST(agent_docs_cover_every_registered_component) {
  // ## 注册表里的每个组件都必须有一页
  const auto entries = st::fs::list_dir(kComponentsDir);
  if (!entries.has_value()) {
    st::log::err("[docs] 列组件头目录失败：{}\n", entries.error().message);
    ST_REQUIRE(entries.has_value());
  }

  std::vector<std::string> registered;
  for (const auto& entry : *entries) {
    if (entry.is_dir) continue;
    if (entry.name.size() <= 4U) continue;
    if (entry.name.compare(entry.name.size() - 4U, 4U, ".hpp") != 0) continue;
    const auto text = st::fs::read_text(std::string(kComponentsDir) + "/" + entry.name);
    if (!text.has_value()) continue;
    for (auto& name : registered_names(*text)) registered.push_back(std::move(name));
  }
  std::sort(registered.begin(), registered.end());
  registered.erase(std::unique(registered.begin(), registered.end()), registered.end());

  const auto pages = component_pages();
  std::vector<std::string> missing;
  for (const auto& name : registered) {
    if (!std::binary_search(pages.begin(), pages.end(), name)) missing.push_back(name);
  }

  st::log::out("[docs] 注册组件 {} 个，组件页 {} 页\n", registered.size(), pages.size());
  for (const auto& name : missing) {
    st::log::err("[docs] 缺组件页：{}（重跑 python tools/agent_docs_gen.py）\n", name);
  }
  ST_CHECK(missing.empty());
  // 防"清空组件头"式假绿：注册表本身必须有一定规模
  ST_CHECK(registered.size() >= 60U);
}

ST_TEST(agent_docs_have_no_orphan_pages) {
  // ## 每一页都必须对应注册表或登记过的"不进注册表"名单
  //
  // 陈旧页的危害与缺页不同：它不是"查不到"，而是**查到的是已经不存在的能力**。
  const auto entries = st::fs::list_dir(kComponentsDir);
  ST_REQUIRE(entries.has_value());
  std::vector<std::string> registered;
  for (const auto& entry : *entries) {
    if (entry.is_dir) continue;
    if (entry.name.size() <= 4U) continue;
    if (entry.name.compare(entry.name.size() - 4U, 4U, ".hpp") != 0) continue;
    const auto text = st::fs::read_text(std::string(kComponentsDir) + "/" + entry.name);
    if (!text.has_value()) continue;
    for (auto& name : registered_names(*text)) registered.push_back(std::move(name));
  }
  std::sort(registered.begin(), registered.end());

  std::vector<std::string> orphans;
  for (const auto& page : component_pages()) {
    const std::string_view name{page};
    const bool known = std::binary_search(registered.begin(), registered.end(), page) ||
                       std::find(kUnregistered.begin(), kUnregistered.end(), name) != kUnregistered.end();
    if (!known) orphans.push_back(page);
  }
  for (const auto& name : orphans) {
    st::log::err("[docs] 孤儿组件页：{}（组件已不在注册表：删页或改回注册）\n", name);
  }
  ST_CHECK(orphans.empty());
}

ST_TEST(agent_docs_pages_follow_generator_contract) {
  // ## 每一页的结构必须与生成器模板一致
  //
  // 判据只查**四个对外承诺的小节**（少了它们，页面对模型就只是散文）：
  // front matter（标题/摘要）+ 头与实现 + 属性面 + 动作面 + 更细。
  // 不查措辞（模板可以演进），只查小节存在——与 `docs_map_test.cpp` 同一口径。
  const auto pages = component_pages();
  ST_REQUIRE(!pages.empty());

  const auto entries = st::fs::list_dir(kPagesDir);
  ST_REQUIRE(entries.has_value());
  std::vector<std::string> bad;
  std::size_t checked = 0;
  for (const auto& family : *entries) {
    if (!family.is_dir) continue;
    const auto family_dir = std::string(kPagesDir) + "/" + family.name;
    for (const auto& page : pages) {
      const auto path = family_dir + "/" + page + ".md";
      const auto text = st::fs::read_text(path);
      if (!text.has_value()) continue;
      ++checked;
      const std::string& body = *text;
      const bool has_front = body.rfind("---", 0) == 0 && body.find("title:") != std::string::npos;
      const bool has_head = body.find("## 头与实现") != std::string::npos;
      const bool has_props = body.find("## 属性面") != std::string::npos;
      const bool has_actions = body.find("## 动作面") != std::string::npos;
      const bool has_next = body.find("## 更细") != std::string::npos;
      if (!(has_front && has_head && has_props && has_actions && has_next)) {
        bad.push_back(path);
      }
    }
  }
  st::log::out("[docs] 结构核对 {} 页\n", checked);
  for (const auto& path : bad) {
    st::log::err("[docs] 页面结构与模板不符：{}（重跑 tools/agent_docs_gen.py）\n", path);
  }
  ST_CHECK(bad.empty());
  ST_CHECK(checked >= 60U);
}

ST_TEST(agent_docs_entry_links_two_layers) {
  // ## 入口页必须同时指到两层（overview 与 components）
  //
  // 分层不是目录好看：模型的第一步永远是"我要做 X，读哪页"。入口若只给一层，
  // 它就得自己逛目录——这正是本树要消灭的动作。
  const auto index = st::fs::read_text(std::string(kTreeDir) + "/index.md");
  if (!index.has_value()) {
    st::log::err("[docs] 读不到 {}/index.md\n", std::string(kTreeDir));
    ST_REQUIRE(index.has_value());
  }
  const std::string& text = *index;
  ST_CHECK(text.find("overview/") != std::string::npos);
  ST_CHECK(text.find("components/") != std::string::npos);
  // 必须给出"按意图找页"的入口与维护分工（生成层 vs 手写层）
  ST_CHECK(text.find("我该读哪一页") != std::string::npos);
  ST_CHECK(text.find("agent_docs_gen.py") != std::string::npos);
}
