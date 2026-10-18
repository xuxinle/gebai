// 侧栏视图族：资源管理器/搜索/变更(scm)/运行/扩展/大纲 六个视图的组装。
// 拆分自 main.cpp，零逻辑改动——只是 out-of-line 化。

#include <algorithm>
#include <cstdio>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "page.hpp"
#include "st/core/string.hpp"
#include "st/text/diff.hpp"

namespace gbcode_ui {


  /// 工作台各视图的标题（侧栏顶部一行：标题 + 该视图的动作按钮）。
  /// 资源管理器行高（紧凑：贴近桌面文件管理器；组件默认值面向可点清单场景）。
  static constexpr float kExplorerRowHeight{24.0f};


  // —— 视图 6：比较侧栏（已删除，2026-10-13）——
  //
  // 旧版在此处放「设 A/设 B」端点按钮钮（任意两端比较的侧栏入口）；用户要求
  // 删除比较侧栏后整套下线（活动栏入口也一并移除，见 build_activity_bar）。
  // 单文件差异（open_diff）仍从变更面板/提交详情/文件历史可达。


  void CodeEditorPage::build_sidebar_head(Composer& c, std::string_view title) {
    (void)row(c, {.gap = 4.0f, .height = 30.0f, .id = "sidebar-head"}, [&] {
      (void)text(c, [title] { return std::string(title); },
                 {.grow = true, .weight = FontWeight::Medium, .id = "sidebar-title"});
      build_sidebar_actions(c);
    });
  }


  /// 各视图的动作按钮（按当前视图给不同的一组）。
  void CodeEditorPage::build_sidebar_actions(Composer& c) {
    const auto icon_button = [&](const char* id, const char* icon, const char* tip,
                                 std::function<void()> on_click) {
      (void)custom<Button>(c, [this, id, icon, tip, on_click = std::move(on_click)](Button& b) {
        b.set_id(id);
        b.set_icon(icon);
        b.set_variant(Button::Variant::Ghost);
        b.set_size(Button::Size::Small);
        // 图标钮必须带提示：图标不自解释，而 `tip` 本来就在这里（以前被 `(void)tip`扔掉）。
        b.set_hint(tip);
        b.on_click = on_click;
      }, {.width = 24.0f, .height = 24.0f, .key = id});
    };
    switch (activity_.value()) {
      case 0:
        icon_button("explorer-refresh", "refresh", "刷新", [this] { refresh_tree(); });
        icon_button("explorer-new-file", "plus", "新建文件", [this] { explorer_create(false); });
        icon_button("explorer-new-folder", "folder", "新建文件夹",
                    [this] { explorer_create(true); });
        icon_button("explorer-save", "download", "保存当前", [this] { save(); });
        break;
      case 1:
        icon_button("search-go", "search", "重新搜索", [this] { run_search(last_query_); });
        icon_button("search-clear", "close", "清空结果", [this] {
          search_hits_.set({});
          search_hit_paths_.clear();
          last_query_.clear();
          // 摘要必须跟着清：它写的是“N 处命中”，不清就与空列表自相矛盾
          //（清空了还显示 8 处命中）。
          search_summary_.set("");
        });
        break;
      case 2:
        icon_button("scm-refresh", "refresh", "刷新状态", [this] { refresh_git(); });
        break;
      case 3:
        icon_button("run-build", "play", "构建", [this] { run_task("build"); });
        icon_button("run-test", "check", "测试", [this] { run_task("test"); });
        break;
      case 5:
        icon_button("outline-refresh", "refresh", "刷新大纲", [this] {
          refresh_document_symbols();
        });
        icon_button("nav-clear", "close", "收起导航结果", [this] {
          // 清数据还是只收起：**只收起**（数据保留——用户可能马上还要看下一处，
          // 清了就要重新 F12）。收起后再有多处命中会重新展开（on_navigation_results
          // 会再次 set true）。
          nav_results_open_.set(false);
        });
        break;
      default:
        icon_button("ext-refresh", "refresh", "重新枚举", [this] { refresh_languages(); });
        break;
    }
  }


  /// 侧栏内容（按活动栏选中项切换）。
  ///
  /// 五个视图各自独立成函数：视图多起来之后「一熊 if/else 里塞五段界面」会让每加一个
  /// 视图都要重读一整堆无关代码。
  void CodeEditorPage::build_sidebar(Composer& c, const std::vector<OpenBuffer>& buffers, std::size_t active) {
    (void)buffers;
    (void)active;
    // 各视图**全部常驻声明**，非当前的用 `set_visible(false)` 隐藏。
    //
    // 为何不按当前视图只声明一个（旧的 `switch` 写法）：切走时那棵子树会被
    // **销毁**——滚动位置、输入框里的字、`Tree` 的展开态、`ScrollView` 的偏移
    // 全部丢掉，再切回来是“刚打开的样子”。用户在这里丢过具体的东西：在资源
    // 管理器里翻到某个目录、在搜索框里打了一半的关键词、在变更列表里滚到某一行——
    // 切一下视图就没了。
    //
    // 代价：已访问的视图每帧多构建几棵子树。实测这层是纯声明开销（不碰 IO——
    // 目录扫描有 `dir_cache_`、git 数据在泵里读，layout 只占 0.2~1.1ms），
    // 比起“丢失用户位置”的代价可以接受。若某个视图将来变重（如带大数据集的
    // 列表），再给它单独做虚拟化。
    //
    // ⚠ 隐藏用 `Panel::set_visible(false)`（不是 `height = 0`）：框架在
    // 绘制/命中/布局三处都统一跳过不可见元素（`Element::paint` 开头 `!visible_` 直接返回、
    // `element.cpp:264` 命中跳过），所以隐藏视图既不占位也不抢输入、还**不参与绘制**
    //——这与底部终端面板“只藏不拆”是同一招。
    column(c, {.gap = 6.0f, .padding = 10.0f, .grow = true, .id = "sidebar-body"}, [&] {
      struct View {
        std::size_t index;
        const char* key;
        const char* id;
        void (CodeEditorPage::*build)(Composer&);
      };
      // ⚠ `static constexpr` 不能与成员函数指针类型一起推：`constexpr` 要求
      // 字面类型，而成员指针不是——写成 `static const`（初始化仍是编译期常量）。
      static const View kViews[] = {
          {0, "sidebar-explorer", "sidebar-explorer", &CodeEditorPage::build_explorer},
          {1, "sidebar-search", "sidebar-search", &CodeEditorPage::build_search_view},
          {2, "sidebar-scm", "sidebar-scm", &CodeEditorPage::build_scm_view},
          {3, "sidebar-run", "sidebar-run", &CodeEditorPage::build_run_view}};
      const std::size_t current = activity_.value();
      // **首次访问才声明，之后保活**：
      // ① 没访问过的视图不参与构建——否则启动首帧要一次性把五棵子树全建出来
      //    （首帧最贵：文本光栅化、图标解析都落在这一帧），启动会明显卡一下；
      // ② 访问过的视图一直留在树上（只隐藏），切换才能“回来还是原样”。
      // 两条合起来才是用户要的语义：**看过的都要能恢复**，没看过的本来无状态可恢复。
      const auto visited = sidebar_visited_.value();
      for (const View& view : kViews) {
        const bool declared =
            view.index == current ||
            std::find(visited.begin(), visited.end(), view.index) != visited.end();
        if (!declared) continue;
        // `custom_container<Panel>` 的子声明回调**不带参数**（与 `custom<T>` 同），
        // 所以构建函数在子声明里用当前 `Composer` 调用。
        const auto build = view.build;
        (void)custom_container<Panel>(
            c,
            [this, build, &c] { (this->*build)(c); },
            [current, index = view.index, id = view.id](Panel& panel) {
              panel.set_id(id);
              panel.set_visible(current == index);
              panel.style().gap = 6.0f;
            },
            {.grow = true, .key = view.key});
      }
      // 大纲与扩展两个视图仍按当前声明（它们的内容本就按一次查询/一次扫描即
      // 生成，没有“用户位置”可丢）；与上面那几个分开写，是为了不让“保活”这条
      // 规则被无差别地扩到没必要的地方。
      if (current == 5) {
        (void)custom_container<Panel>(
            c, [this, &c] { build_outline_view(c); },
            [](Panel& panel) { panel.set_id("sidebar-outline"); },
            {.grow = true, .key = "sidebar-outline"});
      } else if (current != 0 && current != 1 && current != 2 && current != 3) {
        (void)custom_container<Panel>(
            c, [this, &c] { build_extensions_view(c); },
            [](Panel& panel) { panel.set_id("sidebar-extensions"); },
            {.grow = true, .key = "sidebar-extensions"});
      }
    });
  }


  // —— 视图 0：资源管理器（真工作区树）——
  //
  // 2026-10-18：无工作区时的“内置样例列表”分支已删除。正式形态下没有工作区
  // 就**如实显示空状态**（并指路去打开文件夹），而不是拿假文件把界面填满。
  void CodeEditorPage::build_explorer(Composer& c) {
    build_sidebar_head(c, "资源管理器");
    if (workspace_.empty()) {
      (void)column(c, {.gap = 6.0f, .padding_x = 12.0f, .padding_y = 10.0f, .id = "explorer-empty"}, [&] {
        (void)text(c, [] { return std::string("未打开文件夹"); },
                   {.weight = FontWeight::Medium, .id = "explorer-empty-title"});
        (void)text(c, [] { return std::string("文件 → 打开文件夹（Ctrl+K Ctrl+O）"); },
                   {.color = Tone::Muted, .id = "explorer-empty-hint"});
      });
      return;
    }
    (void)custom_container<ScrollView>(
        c,
        [&] {
          // 资源管理器用**紧凑行高**（桌面文件管理器的密度量级）——默认行高
          // 面向"可点清单"场景（触屏/大屏），放在侧栏里条目间距显得过松。
          Element& node = tree(c, workspace_nodes(),
                               [this](const std::string& key, bool expanded) {
                                 on_tree_toggle(key, expanded);
                               },
                               [this](const std::string& key) {
                                 if (key.rfind("dir:", 0) != 0) open_path(key);
                               },
                               {.id = "workspace-tree"});
          if (auto* widget = dynamic_cast<Tree*>(&node); widget != nullptr) {
            widget->set_row_height(kExplorerRowHeight);
            // 右键 → 文件操作菜单（新建/重命名/删除）。
            //
            // 为何不走 `set_event_handler`：`Tree::on_event` 对左键单击返回 true（已处理），
            // 而右键是我们**接在 MouseDown 上的专用语义**——把它放在 `on_event` 里
            // 与“选中/展开”并列，才是同一把尺子（与 `CodeEditor::on_context_menu` 同构）。
            widget->on_context_menu = [this, &node](std::string_view key) {
              context_key_.set(std::string(key));
              // 锚点用**行在屏幕上的位置**：右键不改选中，菜单就要贴着那一行出现。
              // ⚠ 这里只能拿“窗口”级坐标——树控件的行矩形要经它的 `row_rect` 换算，
              // 而那个接口不对外。退而用“鼠标位置”不准（回调不带坐标），
              // 所以锚点取树控件自身的左上角向右下偏移一点：菜单稳定出现在树旁边，
              // 不会因为滚动位置不同而飘到屏幕外。
              explorer_menu_anchor_ = st::math::Point{node.bounds().x + 24.0f,
                                                      node.bounds().y + 24.0f};
              explorer_menu_open_.set(true);
            };
          }
        },
        [](ScrollView& scroll) { scroll.set_id("explorer-scroll"); },
        {.grow = true, .id = "explorer-host"});
    if (workspace_nodes().empty()) {
      (void)text(c, [this] { return std::string("目录为空或不可读：") + workspace_; });
    }
  }


  // —— 视图 1：搜索（工作区文本 + 按文件分组）——
  void CodeEditorPage::build_search_view(Composer& c) {
    build_sidebar_head(c, "搜索");
    (void)custom<Input>(c, [this](Input& field) {
      field.set_id("search-input");
      field.set_placeholder("搜索（回车执行）");
      field.set_icon_prefix("search");
      field.on_submit = [this](std::string_view query) { apply_search(std::string(query)); };
    }, {.key = "search-input"});
    // 选项行：glob 过滤 + 大小写 / 全词 / 正则（与 VSCode 的查找条同族）
    (void)custom<Input>(c, [this](Input& field) {
      field.set_id("search-glob");
      field.set_placeholder("文件过滤（如 src/*.cpp、**.cpp、docs/**）");
      // 提示文案就是语义文档（`st::fs::match_glob`）：`*` **不跨 `/`**，
      // 所以 `*.cpp` 只匹配顶层 .cpp（写 `**.cpp` 才是任意深度）。旧文案举的
      // 例子 `*.cpp` 恰好是语义陷阱——用户照着写会得到 0 命中且无从得知原因。
      field.on_submit = [this](std::string_view glob) {
        search_glob_ = std::string(glob);
        apply_search(last_query_);
      };
    }, {.key = "search-glob"});
    (void)row(c, {.gap = 4.0f}, [&] {
      toggle_chip(c, "search-case", "Aa", search_case_.value(), [this] {
        search_case_.set(!search_case_.value());
        apply_search(last_query_);
      });
      toggle_chip(c, "search-word", "ab", search_word_.value(), [this] {
        search_word_.set(!search_word_.value());
        apply_search(last_query_);
      });
      toggle_chip(c, "search-regex", ".*", search_regex_.value(), [this] {
        search_regex_.set(!search_regex_.value());
        apply_search(last_query_);
      });
      (void)spacer(c);
      (void)text(c, [this] { return search_summary_.value(); }, {.id = "search-summary"});
    });
    // 替换输入行：搜索非空时才出现（“先搜再替”是安全的默认——
    // 先看到命中范围再决定改什么，比先输入两条再回车稳得多）。
    if (!last_query_.empty()) {
      (void)custom<Input>(c, [this](Input& field) {
        field.set_id("search-replace-input");
        field.set_placeholder("替换为…（回车 = 全部替换）");
        field.on_submit = [this](std::string_view value) { replace_in_workspace(value); };
      }, {.key = "search-replace-input"});
    }
    if (search_hits_.value().empty()) {
      (void)text(c, [this] {
        return search_summary_.value().empty()
                   ? std::string("输入关键词后回车，搜索整个工作区")
                   : std::string("没有匹配");
      }, {.id = "search-empty"});
      return;
    }
    // 搜索结果：按**文件分组**（与「导航结果」视图同一形态）。
    //
    // ⚠ 原有注释写着“已按文件分组”而实现是平铺——注释与实现不符比缺功能更坏：
    // 后来者会以为已经有了。分组行的判据与实际与导航结果视图**共用一套写法**，
    // `search_row_to_hit_` 记「面板行 → 命中下标」（分组行占位 -1，不可点）。
    std::vector<ListItemData> items;
    search_row_to_hit_.clear();
    std::string last_file;
    for (std::size_t index = 0; index < search_hits_.value().size(); ++index) {
      // 路径取自 `search_hit_paths_`（与 `search_hits_` 同序的平行数组——
      // 这是本页既有的数据形态，分组需要它，不另建一份）。
      const std::string& path = index < search_hit_paths_.size() ? search_hit_paths_[index]
                                                                 : std::string{};
      if (path != last_file) {
        last_file = path;
        const std::size_t slash = path.find_last_of("/\\");
        const std::string file_name =
            slash == std::string::npos ? path : path.substr(slash + 1);
        items.push_back(ListItemData{.key = std::format("hitfile:{}", index),
                                     .label = file_name});
        search_row_to_hit_.push_back(static_cast<std::size_t>(-1));
      }
      items.push_back(ListItemData{.key = std::format("hit:{}", index),
                                   .label = search_hits_.value()[index]});
      search_row_to_hit_.push_back(index);
    }
    (void)custom_container<ScrollView>(
        c,
        [&] {
        (void)list(c, items, [this](std::size_t index) {
            // 点击行 → 反查它属于哪条命中（分组行不可点——由映射里的哨兵值区分）。
            if (index < search_row_to_hit_.size() &&
                search_row_to_hit_[index] != static_cast<std::size_t>(-1)) {
              activate_search_hit(search_row_to_hit_[index]);
            }
          }, {.id = "search-results"});
        },
        [](ScrollView& scroll) { scroll.set_id("search-scroll"); },
        {.grow = true, .id = "search-host"});
  }


  /// **在工作区里全部替换**（搜索视图的「全部替换」）。
  ///
  /// 为何要在多文件层做而不是循环调编辑器：搜索命中的是**磁盘上的文件**
  ///（跟当前打开的标签无关，甚至没打开过）。走 `st::fs` 读写，
  /// 改完再刷新搜索与已打开的缓冲。
  ///
  /// `st::replace_all_plain` 不做正则——与搜索视图自己的“迷你正则”**能力对齐**：
/// 正则开启时**如实拒绝**（宁可不做，不可做错）。
///
/// ⚠ **只在“真工作区搜索”的结果上可用**：搜索结果对磁盘文件存绝对路径，
/// 而“内置样例回退”（无 `--workspace`）存的是样例名——后者走不了替换，
/// 所以先判每个路径是不是绝对路径，**如实拒掉**而不是“目标 5；无命中 5”。
/// （那串数字看着像“没搜到”，实际上一个都没读。）
void CodeEditorPage::replace_in_workspace(std::string_view replacement) {
    const std::string needle = last_query_;
    if (needle.empty()) {
      status_.set("先搜索再替换");
      return;
    }
    if (search_regex_.value()) {
      status_.set("工作区替换不支持正则（请关掉 .* 开关）——不拿不准的规则去改磁盘");
      return;
    }
    if (workspace_.empty()) {
      status_.set("需要打开一个文件夹才能做工作区替换");
      return;
    }
    // 去重：同一个文件可能在结果里出现多行。
    // 同时排除非绝对路径（内置样例名）——见函数头说明。
    std::vector<std::string> files;
    bool skipped_non_file = false;
    for (const auto& path : search_hit_paths_) {
      if (path.empty()) continue;
      if (!st::fs::is_absolute(path)) {
        skipped_non_file = true;
        continue;
      }
      if (std::find(files.begin(), files.end(), path) == files.end()) files.push_back(path);
    }
    if (files.empty()) {
      status_.set(skipped_non_file ? "当前结果来自内置样例（非磁盘文件），无法替换"
                                   : "没有可替换的文件");
      return;
    }
    // 诊断：把首个目标路径报出来。 “0 处 / 0 个文件”有两种截然不同的成因——
    // 命中已不存在（正常）与**路径口径不对、根本没读到文件**（缺陷）。
    // 不把这两者分开，用户与开发者都只能看到同一个“0”。
    status_.set(std::format("替换中：{} 个文件，首项 {}", files.size(), files.front()));
    std::size_t touched = 0;
    std::size_t replaced = 0;
    std::size_t unreadable = 0;
    std::size_t unchanged = 0;
    std::string probe_note{};
    for (const std::string& path : files) {
      const auto text = st::fs::read_text(path);
      if (!text.has_value()) {
        ++unreadable;
        continue;
      }
      if (touched == 0 && unchanged == 0 && unreadable == 0) {
        // 首件诊断：把“读到了什么”与“找不找得到”一并报出。
        //（“读到空文件”与“命中为零”是完全不同的两件事，不分开就没法定位。）
        probe_note = std::format("首件 {} 字节 {}；含词 {}", text->size(), path,
                                  text->find(needle) != std::string::npos ? "是" : "否");
      }
      // 大小写不敏感时**不改写原串的大小写**：只按同一口径找命中再逐处替掉。
      std::string updated;
      std::size_t count = 0;
      if (search_case_.value()) {
        updated = st::replace_all(*text, needle, replacement);
        // 不计次数：只关心“动过没有”（状态栏报的是文件数，不是精确命中数）——
        // 真要精确计数得再扫一遍，而这儿多扫一遍的价值几乎为零。
        count = updated == *text ? 0U : 1U;
      } else {
        // 不区分大小写：在**小写副本**上找位置，改的是原文——
        // 这样“不改写原串大小写”与“位置一致”同时成立。
        //
        // `updated` 必须先有内容：初版写成 `std::string lowered = updated;`
        // 而那一刻 `updated` 还是**空串**（它要到下面才被赋值），
        // 于是循环一次也不进、`count` 永远是 0——症状是“目标 5、无命中 5”，
        // 而文件里明明含那串（诊断行把这一点直接打了出来）。
        updated = *text;
        std::string lowered = to_lower(*text);
        const std::string lowered_needle = to_lower(needle);
        std::size_t at = lowered_needle.empty() ? std::string::npos
                                                : lowered.find(lowered_needle);
        while (at != std::string::npos) {
          updated.replace(at, needle.size(), replacement);
          ++count;
          lowered.replace(at, needle.size(), replacement);
          at = lowered.find(lowered_needle, at + replacement.size());
        }
      }
      if (count == 0) {
        ++unchanged;
        continue;
      }
      if (!st::fs::write_text(path, updated).has_value()) {
        status_.set(std::format("写入失败：{}", path));
        return;
      }
      ++touched;
      replaced += count;    }
    // 已打开的标签跟着重读（磁盘真相变了；不重读会“替换了但编辑器里还是旧的”）。
    auto list = buffers_.value();
    for (auto& buffer : list) {
      if (buffer.path.empty()) continue;
      if (std::find(files.begin(), files.end(), buffer.path) == files.end()) continue;
      if (const auto text = st::fs::read_text(buffer.path); text.has_value()) {
        buffer.text = *text;
        buffer.dirty = false;
      }
    }
    buffers_.set(std::move(list));
    load_active_text();
    dir_cache_.clear();
    apply_search(needle);   // 重跑搜索：命中数应回落
    // 状态栏把**次因**一并报出：“0 处”有两种截然不同的成因——
    // 命中已不存在（正常）与路径读不到（缺陷）。不分开的后果是排障要重跑一次搜索
    // 才能看出区别（实测：首版只报总数，查“0”花了好几轮）。
    status_.set(std::format("工作区替换：{} 处 / {} 文件（目标 {}；读失败 {}；无命中 {}；{}）",
                            replaced, touched, files.size(), unreadable, unchanged, probe_note));
  }


  // —— 视图 5：大纲（当前文件符号树 + 导航结果）——
  //
  // 主体是**当前文件的符号大纲**（LSP DocumentSymbol，嵌套缩进展示；切文件自动刷新，
  // 点击跳到符号处）。多处导航结果（F12/Shift+F12 的命中清单）附在下方——
  // 有命中才出现，与大纲主客分明（2026-10-13 由「导航结果」升级而来）。
  void CodeEditorPage::build_outline_view(Composer& c) {
    build_sidebar_head(c, "大纲");
    // —— 上：符号大纲 ——
    if (symbols_.empty()) {
      (void)text(c, [] {
        return std::string("当前文件没有符号大纲\n（需要语言服务就绪，且文件在磁盘上）");
      }, {.id = "outline-empty"});
    } else {
      // 符号树拍平成缩进行：行 → 符号的映射（`outline_rows_`）在构建期同步维护，
      // 点击回调里按下标反查（与导航结果的 `nav_row_to_result_` 同一套模式）。
      std::vector<ListItemData> items;
      outline_rows_.clear();
      const auto flatten = [&](const auto& list, auto&& self, int depth) -> void {
        for (const auto& symbol : list) {
          const std::string indent(static_cast<std::size_t>(depth) * 2U, ' ');
          // 行文案：缩进 + 名字 + kind（kind 后置小字，点击看符号名不被淹没）。
          // kind 是协议枚举值（1=File…12=Function…），只对常用的几个给名字，
          // 其余不后缀（数字本身对用户没有信息量）。
          const char* kind_name = nullptr;
          switch (symbol.kind) {
            case 5: kind_name = "class"; break;
            case 6: kind_name = "method"; break;
            case 9: kind_name = "ctor"; break;
            case 10: kind_name = "enum"; break;
            case 11: kind_name = "interface"; break;
            case 12: kind_name = "func"; break;
            case 13: kind_name = "var"; break;
            case 14: kind_name = "const"; break;
            case 23: kind_name = "struct"; break;
            default: break;
          }
          const std::string kind_suffix =
              kind_name == nullptr ? std::string{} : std::format("  [{}]", kind_name);
          items.push_back(ListItemData{.key = std::format("sym:{}:{}", depth, items.size()),
                                       .label = indent + symbol.name + kind_suffix});
          outline_rows_.push_back(OutlineRow{.symbol = &symbol, .depth = depth});
          if constexpr (requires { symbol.children; }) {
            self(symbol.children, self, depth + 1);
          }
        }
      };
      flatten(symbols_, flatten, 0);
      (void)custom_container<ScrollView>(
          c,
          [&] {
            (void)list(c, items, [this](std::size_t index) {
              if (index >= outline_rows_.size()) return;
              const st::lsp::DocumentSymbol* symbol = outline_rows_[index].symbol;
              if (symbol == nullptr) return;
              // 跳到符号（同一条 jump_to_location 路径，历史入栈可 Alt+← 回）。
              jump_to_location(st::lsp::Location{
                  .uri = st::lsp::LspClient::path_to_uri(current_buffer_path()),
                  .range = symbol->range,
                  .selection = std::nullopt});
            },
                     {.id = "outline-symbols", .key = "outline-symbols"});
          },
          [](ScrollView& scroll) { scroll.set_id("outline-scroll"); },
          {.grow = true, .id = "outline-host"});
    }
    // —— 下：导航结果（有命中才出现）——
    if (nav_results_.empty() || !nav_results_open_.value()) return;
    (void)row(c, {.gap = 4.0f, .height = 26.0f, .id = "outline-nav-head"}, [&] {
      (void)text(c, [this] {
        return std::format("导航结果（{} 处）", nav_results_.size());
      }, {.grow = true, .weight = FontWeight::Medium, .id = "outline-nav-title"});
    });
    // 按文件分组：前缀行是文件名，其后是该文件的命中行（与搜索结果同形态）。
    // `nav_row_to_result_`：面板行 → 结果下标的映射（分组行占位 -1）。
    std::vector<ListItemData> items;
    nav_row_to_result_.clear();
    std::string last_file;
    for (std::size_t index = 0; index < nav_results_.size(); ++index) {
      const NavResult& result = nav_results_[index];
      const std::size_t slash = result.path.find_last_of("/\\");
      const std::string file_name =
          slash == std::string::npos ? result.path : result.path.substr(slash + 1);
      if (result.path != last_file) {
        last_file = result.path;
        items.push_back(ListItemData{.key = std::format("navfile:{}", index),
                                     .label = file_name});
        nav_row_to_result_.push_back(static_cast<std::size_t>(-1));
      }
      items.push_back(ListItemData{.key = std::format("nav:{}:{}", index, result.line),
                                   .label = std::format("  {}:{}  {}", result.line,
                                                        result.column, result.preview)});
      nav_row_to_result_.push_back(index);
    }
    (void)custom_container<ScrollView>(
        c,
        [&] {
        (void)list(c, items, [this](std::size_t index) {
            // 点击行 → 反查它属于哪个结果（前缀行不可点——由 key 前缀区分）。
            // `list` 的回调给的是**扁平序号**，与 `nav_results_` 下标差着分组行，
            // 所以构建时同步记录「行 → 结果下标」的映射。
            if (index < nav_row_to_result_.size()) open_nav_result(nav_row_to_result_[index]);
          },
                     {.id = "nav-results"});
        },
        [](ScrollView& scroll) { scroll.set_id("nav-scroll"); },
        {.grow = true, .id = "nav-host"});
  }


  /// 开关胶囊（「Aa / ab / .*」这类选项）：按下态由 `active` 决定。
  ///
  /// 为什么不用 `Checkbox`：查找选项是**密集的一行小开关**（三个挤在 200px 宽里），
  /// 带文字的勾选框会把行撑到放不下——胶囊用固定 26px 宽，观感与 VSCode 一致。
  void CodeEditorPage::toggle_chip(Composer& c, const char* id, const char* label, bool active,
                   std::function<void()> on_click) {
    (void)custom<Button>(c, [id, label, active, on_click = std::move(on_click)](Button& b) {
      b.set_id(id);
      b.set_label(label);
      b.set_variant(active ? Button::Variant::Soft : Button::Variant::Ghost);
      b.set_size(Button::Size::Small);
      b.on_click = on_click;
    }, {.width = 30.0f, .height = 24.0f, .key = id});
  }


  /// 小图标钮（侧栏各视图共用；`on_click` 为空 = 纯展示）。
  void CodeEditorPage::scm_icon_button(Composer& c, const std::string& id, const char* icon_name,
                       const char* tip, std::function<void()> on_click) {
    (void)custom<Button>(c, [id = id, icon_name = std::string(icon_name), tip = std::string(tip),
                             on_click = std::move(on_click)](Button& b) mutable {
      b.set_id(id);
      b.set_icon(icon_name);
      b.set_variant(Button::Variant::Ghost);
      b.set_size(Button::Size::Small);
      // 图标按钮的**提示**：图标本身不自解释，而 `tip` 参数本来就写着目的
      //（以前它被丢弃，于是图标按钮只有视觉没有任何文字线索）。
      b.set_hint(tip);
      if (on_click) b.on_click = [on_click]() { on_click(); };
    }, {.width = 26.0f, .height = 22.0f, .key = id});
  }


  // —— 视图 2：变更（面板：分组 + 行内动作 + 底部提交框）——
  //
  // 参照歌白文件工作台的设计（IDEA 的 Commit 工具窗就在左侧）：
  // "改了什么 / 要提交什么"是编码时最频繁看的，放左侧随时可见；
  // 底部留给"分支 | 日志"（那是回顾历史时才看的，节奏不同——见 build_git_view）。
  void CodeEditorPage::build_scm_view(Composer& c) {
    build_sidebar_head(c, "变更");
    // ⚠ **渲染期不能同步跑 git**：声明式重组过程中起子进程（`process::run`）
    // 会在重组栈上 fork + 管道等待，任何一处重入（`status_.set` 触发同步重组）
    // 都会踩坏声明式树——这个崩点在 Git 日志那侧实测过（点状态栏分支就 SIGSEGV）。
    //
    // 旧写法在这里直调 `refresh_git()`，能“看起来没事”只是因为变更视图**只在
    // 它是当前视图时才被声明**（切过去的那一帧跑一次）。改成“视图常驻 + 隐藏保活”
    // 之后，本函数**每帧都会跑到**——必须把读取挪到帧首泵（那里已经在做
    // `git_refresh_pending_` 与 `ensure_git_log`）。
    git_probe_wanted_ = true;
    // 把 Git 快照版本读成**声明式依赖**：`refresh_git()` 写它 → 本视图重组。
    // 不读的话变更面板只在“别的状态恰好也变了”时才更新（见 `git_revision_` 的说明）。
    (void)git_revision_.value();
    if (workspace_.empty()) {
      (void)text(c, [] { return std::string("内置样例模式：没有工作区可查 Git 状态"); });
      return;
    }

    // —— 头部：分支 + ahead/behind + 刷新 ——
    (void)row(c, {.gap = 6.0f}, [&] {
      (void)icon(c, "git-branch", 14.0f);
      (void)text(c, [this] {
        std::string label = git_.branch.empty() ? std::string("（未探测）") : git_.branch;
        if (git_.ahead > 0 || git_.behind > 0) {
          label += std::format("  ↑{} ↓{}", git_.ahead, git_.behind);
        }
        return label;
      }, {.grow = true, .id = "git-branch-label"});
    });
    if (!git_.error.empty()) {
      (void)text(c, [this] { return git_.error; }, {.color = Tone::Danger, .id = "git-error"});
      return;
    }
    if (!git_.is_repo) {
      (void)text(c, [] { return std::string("当前工作区不是 Git 仓库"); },
                 {.id = "git-not-repo"});
      return;
    }

    // —— 变更工具条（IDEA 同款：刷新 / 全部暂存 / 全部撤销 / 展开折叠 / 比较）——
    //
    // ⚠ 工具条按钮 id 一律带 `-toolbar` 后缀：侧栏头还有一个同功能的
    // `scm-refresh`（视图头的主按钮），两处同 id 会让 `find`/按 id 点击
    // 命中不确定的一份（实测：同一 id 三份实例，自动化点到了 0x0 那份，
    // 表现为「点了没反应」）。id 是自动化的地址，**同屏同 id = 地址歧义**。
    (void)row(c, {.gap = 2.0f, .id = "scm-toolbar"}, [&] {
      scm_icon_button(c, "scm-refresh-toolbar", "refresh", "刷新状态", [this] { refresh_git(); });
      scm_icon_button(c, "scm-stage-all", "plus", "全部暂存",
                      [this] { stage_group(gbcode::ChangeGroup::Unstaged, true);
                               stage_group(gbcode::ChangeGroup::Untracked, true); });
      scm_icon_button(c, "scm-unstage-all", "minus", "全部取消暂存",
                      [this] { stage_group(gbcode::ChangeGroup::Staged, false); });
      scm_icon_button(c, "scm-collapse-all", "chevron-right", "折叠全部分组",
                      [this] { git_collapsed_groups_.set({"冲突", "已暂存", "未暂存", "未跟踪"}); });
      scm_icon_button(c, "scm-expand-all", "chevron-down", "展开全部分组",
                      [this] { git_collapsed_groups_.set({}); });
      scm_icon_button(c, "scm-discard-all", "undo", "放弃全部未暂存改动",
                      [this] { ask_discard_all(); });
    });

    // —— 分组列表（滚动容器契约：与资源管理器同一个理由——Button 按标签测量，
    // 长路径会横向溢出到编辑区）——
    (void)custom_container<ScrollView>(
        c,
        [&] {
          // ⚠ 快照版本必须**在本作用域**里读：分组/文件行是在 `ScrollView` 这个
          // 子组件自己的作用域内声明的，只在 `build_scm_view` 顶层读的话脏的
          // 只是外层作用域——子作用域不在它的子树里，于是“重组发生了，
          // 但分组列表没重建”（实测：快照版本 r1→r2、文件数 14→15，
          // 而屏幕上的分组与计数纹丝不动）。**每个真正显示 git 数据的
          // scope 都得自己订阅这个版本号**。
          (void)git_revision_.value();
          if (git_.changes.empty()) {
            (void)text(c, [] { return std::string("工作区干净，没有未提交的变更"); });
            return;
          }
          static constexpr gbcode::ChangeGroup kGroups[] = {
              gbcode::ChangeGroup::Conflicted, gbcode::ChangeGroup::Staged,
              gbcode::ChangeGroup::Unstaged, gbcode::ChangeGroup::Untracked};
          for (const auto group : kGroups) {
            const auto items = git_.changes_in(group);
            if (items.empty()) continue;
            const std::string group_name(gbcode::change_group_name(group));
            const auto collapsed_list = git_collapsed_groups_.value();
            const bool collapsed =
                std::find(collapsed_list.begin(), collapsed_list.end(), group_name) !=
                collapsed_list.end();
            // 组头（可折叠 + 计数 + **整组暂存/取消暂存/丢弃**）。
            //
            // 分组级动作是高频口（“这一组我全要/全不要”）——歌白那边是
            // `全部暂存`/`全部取消暂存`；这里也放在组头，与单文件动作同一位置语言。
            (void)row(c, {.gap = 2.0f, .key = "git-grouprow:" + group_name}, [&] {
              (void)custom<Button>(c, [this, group_name, collapsed, count = items.size()](Button& b) {
                b.set_id("git-group-" + group_name);
                b.set_label(std::format("{} {}（{}）", collapsed ? "▸" : "▾", group_name, count));
                b.set_variant(Button::Variant::Ghost);
                b.set_size(Button::Size::Small);
                b.on_click = [this, group_name] {
                  auto groups = git_collapsed_groups_.value();   // 拷贝改再 set（State 的写法）
                  const auto found = std::find(groups.begin(), groups.end(), group_name);
                  if (found != groups.end()) {
                    groups.erase(found);
                  } else {
                    groups.push_back(group_name);
                  }
                  git_collapsed_groups_.set(std::move(groups));
                };
              }, {.height = 24.0f, .grow = true});
              // 整组动作：按组的语义给（未暂存→暂存、已暂存→取消暂存、未跟踪→暂存、冲突→跳过）。
              if (group == gbcode::ChangeGroup::Unstaged ||
                  group == gbcode::ChangeGroup::Untracked) {
                scm_icon_button(c, "stage-all-" + group_name, "plus", "全部暂存",
                                [this, group] { stage_group(group, true); });
              }
              if (group == gbcode::ChangeGroup::Staged) {
                scm_icon_button(c, "unstage-all-" + group_name, "minus", "全部取消暂存",
                                [this, group] { stage_group(group, false); });
              }
              if (group == gbcode::ChangeGroup::Unstaged) {
                scm_icon_button(c, "discard-all-" + group_name, "undo", "放弃本组全部更改",
                                [this] { ask_discard_all(); });
              }
            });
            if (collapsed) continue;
            // 组内条目：行 = 状态字母 + 路径 + 行内动作钮。
            //
            // 路径显示：IDEA 同款“目录灰、文件名亮”——这里用 `dir/` 前缀 + 文件名两段文本
            //（Button 只有一个 label，所以拼成 `状态 目录/ 名字`，目录部分已截断）。
            for (const auto& change : items) {
              (void)row(c, {.gap = 4.0f, .key = "git:" + change.path}, [&] {
                (void)custom<Button>(c, [this, change](Button& b) {
                  b.set_id("git-file-" + change.path);
                  b.set_align(Button::Align::Start);   // 列表行贴左缘（居中会参差不齐）
b.set_label(std::format("{} {}", change.display_code(), change.display_path()));
                  b.set_variant(Button::Variant::Ghost);
                  b.set_size(Button::Size::Small);
                  b.on_click = [this, change] { open_git_diff(change); };
                }, {.height = 26.0f, .grow = true});
                // 行内动作（悬停意图：VSCode 同款三键）。
                if (change.has_unstaged_side() && !change.is_conflicted()) {
                  scm_icon_button(c, "stage-" + change.path, "plus", "暂存更改",
                                  [this, path = change.path] { git_stage(path, true); });
                }
                if (change.has_staged_side()) {
                  scm_icon_button(c, "unstage-" + change.path, "minus", "取消暂存",
                                  [this, path = change.path] { git_stage(path, false); });
                }
                if (!change.has_staged_side() || change.has_unstaged_side()) {
                  scm_icon_button(c, "discard-" + change.path, "undo", "放弃更改",
                                  [this, change] { git_discard(change); });
                }
              });
            }
          }
        },
        [](ScrollView& scroll) { scroll.set_id("scm-scroll"); },
        {.grow = true, .id = "scm-host"});

    // —— 底部常驻提交面板（IDEA 的 Commit 区）：消息 + 选项 + 提交 ——
    //
    // 为什么常驻（而不是"点提交按钮弹对话框"）：提交流是"看一眼变更 → 写消息 →
    // 提交"，消息输入与变更列表同屏才能边看边写；弹对话框会把列表遮住。
    if (git_.is_repo && !git_.changes.empty()) {
      build_commit_panel(c);
    }
  }


  /// 提交面板（消息 + 最近消息下拉 + amend/作者 + 提交 / 提交并推送）。
  void CodeEditorPage::build_commit_panel(Composer& c) {
    // ⚠ 输入框必须包在**行**里再 `grow`：直接放在外层**列**里 `grow = true`
    // 会让它把剩余**高度**吃满——实测截图里它被拉成三行高的大框，
    // 而下面那行的计数文本被挤成“已…”。
    (void)row(c, {.gap = 4.0f, .id = "commit-message-row"}, [&] {
      (void)custom<Input>(c, [this](Input& field) {
        field.set_id("commit-message");
        field.set_placeholder(commit_amend_.value() ? "修正上一笔提交的信息（--amend）"
                                                    : "提交信息（Ctrl+Enter 提交）");
        field.style().grow = true;
        commit_input_ptr = &field;
        field.on_change = [this](std::string_view value) {
          commit_message_.set(std::string(value));
        };
        field.on_submit = [this](std::string_view) { git_commit(); };
      }, {.height = 26.0f, .grow = true, .key = "commit-message"});
    });
    (void)row(c, {.gap = 4.0f, .id = "commit-options"}, [&] {
      // 修正上一笔（改写 HEAD）：危险度是 `Confirm`——提交时会走确认。
      (void)custom<Button>(c, [this](Button& b) {
        b.set_id("commit-amend");
        b.set_label("修正");
        b.set_variant(commit_amend_.value() ? Button::Variant::Soft : Button::Variant::Ghost);
        b.set_size(Button::Size::Small);
        b.on_click = [this] { commit_amend_.set(!commit_amend_.value()); };
      }, {.height = 24.0f, .key = "commit-amend"});
      // 最近消息下拉（IDEA 同款）：点一下填回输入框（再编辑后提交）。
      if (!commit_message_history_.empty()) {
        (void)custom<Button>(c, [this](Button& b) {
          b.set_id("commit-history");
          b.set_label("最近");
          b.set_variant(Button::Variant::Ghost);
          b.set_size(Button::Size::Small);
          b.on_click = [this] {
            commit_message_.set(commit_message_history_.front());
            if (commit_input_ptr != nullptr) {
              commit_input_ptr->set_text(commit_message_history_.front());
            }
            status_.set("已填入最近一条提交信息");
          };
        }, {.height = 24.0f, .key = "commit-history"});
      }
      (void)custom<Button>(c, [this](Button& b) {
        b.set_id("commit-go");
        b.set_label(commit_amend_.value() ? "修正提交" : "提交");
        b.set_variant(Button::Variant::Primary);
        b.set_size(Button::Size::Small);
        b.on_click = [this] { git_commit(); };
      }, {.height = 24.0f, .grow = true, .key = "commit-go"});
    });
    (void)row(c, {.gap = 4.0f, .id = "commit-actions-row"}, [&] {
      (void)custom<Button>(c, [this](Button& b) {
        b.set_id("commit-and-push");
        b.set_label("提交并推送");
        b.set_variant(Button::Variant::Secondary);
        b.set_size(Button::Size::Small);
        b.on_click = [this] { commit_and_push(); };
      }, {.height = 22.0f, .key = "commit-and-push"});
      (void)custom<Button>(c, [this](Button& b) {
        b.set_id("commit-more");
        b.set_label(commit_options_open_.value() ? "收起选项" : "更多选项");
        b.set_variant(Button::Variant::Ghost);
        b.set_size(Button::Size::Small);
        b.on_click = [this] { commit_options_open_.set(!commit_options_open_.value()); };
      }, {.height = 22.0f, .key = "commit-more"});
      (void)custom<Button>(c, [this](Button& b) {
        b.set_id("stash-all");
        b.set_label("暂存全部");
        b.set_variant(Button::Variant::Ghost);
        b.set_size(Button::Size::Small);
        b.on_click = [this] {
          (void)run_write({.op = gbcode::WriteOp::StashPush, .message = "st"}, "stash push");
        };
      }, {.height = 22.0f, .key = "stash-all"});
    });
    // 暂存计数单独一行：它与上面的按钮同高时会先被放气（实测：被压成“已…”）。
    const std::size_t staged =
        git_.count_in(gbcode::ChangeGroup::Staged) + git_.count_in(gbcode::ChangeGroup::Conflicted);
    (void)text(c, [staged, total = git_.changes.size()] {
      return std::format("已暂存 {}/{} 项{}", staged, total,
                         staged == 0 ? "（将提交全部改动）" : "");
    }, {.color = Tone::Muted, .id = "commit-staged-count"});
    // 作者覆盖（可选；IDEA 的 Commit 对话框同此）。空 = 用仓库配置。
    if (commit_options_open_.value()) {
      (void)custom<Input>(c, [this](Input& field) {
        field.set_id("commit-author");
        field.set_placeholder("作者覆盖，形如 Name <mail>（留空用仓库配置）");
        field.style().grow = true;
        field.on_change = [this](std::string_view value) {
          commit_author_.set(std::string(value));
        };
      }, {.height = 24.0f, .grow = true, .id = "commit-author"});
    }
  }


  /// 打开一个变更文件的 diff（**并列差异**，替代旧版“把 patch 敲进只读标签”）。
  ///
  /// 为何是并列而不是一段 patch 文本：diff 要**对照看**（“原来是什么 / 现在是什么”），
  /// 而并列视图靠双行号把两栏逐行对齐；patch 文本做不到（它是一维流）。
  void CodeEditorPage::open_git_diff(const gbcode::GitChange& change) {
    // ⚠ **冲突文件走合并视图**（而不是差异视图）：冲突的真实需求是“决定这一段要谁”，
    // 而不是“看哪里变了”。两个视图的入口同一个文件，靠**状态**分流——
    // 用户不需要记住“冲突要用另一个菜单项”。
    if (change.is_conflicted()) {
      open_merge(change.path);
      return;
    }
    gbcode::CompareOptions options;
    // 只暂存过（未再改工作区）→ 与 HEAD 比已暂存内容；否则看工作区。
    const bool staged_only = change.has_staged_side() && !change.has_unstaged_side();
    if (staged_only) {
      options.from = gbcode::DiffEndpoint::parse("HEAD");
      options.to = gbcode::DiffEndpoint::parse("INDEX");
    } else {
      options.from = gbcode::DiffEndpoint::parse("INDEX");
      options.to = gbcode::DiffEndpoint::parse("WORKTREE");
    }
    open_diff(std::format("Δ {}（{}）", st::fs::file_name(change.path),
                          staged_only ? "已暂存" : "工作区"),
              options, change.path);
  }


  // 比较视图（编辑区清单形态）已随比较侧栏一起删除（2026-10-13）——
  // 旧版在编辑区渲染「A/B 端点栏 + 文件清单」，点文件开差异标签；活动栏入口
  // 移除后不可达。单文件差异仍从变更面板/提交详情/文件历史直达（open_diff）。
  // —— 视图 3：运行与调试（真跑 st 子命令）——
  void CodeEditorPage::build_run_view(Composer& c) {
    build_sidebar_head(c, "运行");
    struct Task {
      const char* id;
      const char* label;
      const char* detail;
    };
    static constexpr Task kTasks[] = {{"build", "构建当前目标", "st build gallery --profile dev"},
                                      {"test", "跑框架单测", "st test"},
                                      {"lint", "禁令扫描", "st lint"}};
    for (const Task& task : kTasks) {
      (void)row(c, {.gap = 6.0f}, [&] {
        (void)custom<Button>(c, [this, task](Button& b) {
          b.set_id(std::string("task-") + task.id);
          b.set_label(task.label);
          b.set_icon("play");
          b.set_variant(Button::Variant::Secondary);
          b.set_size(Button::Size::Small);
          b.on_click = [this, id = std::string(task.id)] { run_task(id); };
        }, {.height = 28.0f, .grow = true, .key = task.id});
      });
      (void)text(c, [task] { return std::string("  ") + task.detail; });
    }
    (void)text(c, [] { return std::string("任务输出落在底部终端（实时回流）"); });
    // “有没有活在干”问 **any_busy**：作业归属发起它的会话，用户此刻可能在别的标签上。
    if (terminal_ptr != nullptr && terminal_ptr->any_busy()) {
      (void)row(c, {.gap = 6.0f}, [&] {
        (void)custom<Spinner>(c, [](Spinner& s) { s.set_id("task-spinner"); },
                              {.width = 18.0f, .height = 18.0f, .key = "spinner"});
        (void)text(c, [this] {
          const std::string name = terminal_ptr != nullptr
                                       ? terminal_ptr->get_property("running_name")
                                             .value_or(std::string{})
                                       : std::string{};
          return name.empty() ? std::string("终端忙") : "终端忙：" + name;
        }, {.id = "task-running"});
      });
    }
  }


  // —— 视图 4：扩展（运行时注册的语言清单）——
  void CodeEditorPage::build_extensions_view(Composer& c) {
    build_sidebar_head(c, "语言与扩展");
    if (languages_.empty()) refresh_languages();
    (void)text(c, [] { return std::string("已注册的语法（点一下切到该语言）"); });
    // 同上：语言清单很长，必须在滚动容器里（否则窄侧栏装不下会溢出）。
    (void)custom_container<ScrollView>(
        c,
        [&] {
          for (const auto& name : languages_) {
            (void)custom<Button>(c, [this, name](Button& b) {
              b.set_label(name);
              b.set_variant(language_.value() == name ? Button::Variant::Soft
                                                     : Button::Variant::Ghost);
              b.set_size(Button::Size::Small);
              b.on_click = [this, name] { set_language(name); };
            }, {.key = "lang:" + name});
          }
        },
        [](ScrollView& scroll) { scroll.set_id("ext-scroll"); },
        {.grow = true, .id = "ext-host"});
  }

}  // namespace gbcode_ui
