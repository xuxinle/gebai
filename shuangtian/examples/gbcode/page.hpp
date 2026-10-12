/// gbcode 页面类声明：整个 IDE 是一个声明式 `Component`（拆分自原 main.cpp）。
///
/// 布局五层（自上而下）：标题栏（菜单并栏）/ 活动栏 + 侧栏 + 编辑区 / 底部面板 / 状态栏。
/// 实现按函数族分文件：page_build.cpp（骨架）、page_sidebar.cpp（侧栏视图）、
/// page_bottom.cpp（底部面板）、page_dialogs.cpp（浮层）、page_actions.cpp（动作）。
///
/// 控制通道兼容钩子（`tools/*.py` 与子代理依赖，**不可改名**）：
/// `#btn-theme`、`#editor`、`#status`、`#sidebar-split`、`#sidebar`、`#activity-search`、
/// `#titlebar`、`#terminal-input` / `#terminal-output`、`#command-palette`。

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "git_service.hpp"
#include "lsp_bridge.hpp"
#include "page_data.hpp"
#include "session_store.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/code_editor.hpp"
#include "st/ui/components/completion_popup.hpp"
#include "st/ui/components/command_palette.hpp"
#include "st/ui/components/diff_view.hpp"
#include "st/ui/components/feedback.hpp"
#include "st/ui/components/file_dialog.hpp"
#include "st/ui/components/input.hpp"
#include "st/ui/components/list.hpp"
#include "st/ui/components/menu.hpp"
#include "st/ui/components/merge_view.hpp"
#include "st/ui/components/overlay.hpp"
#include "st/ui/components/scroll.hpp"
#include "st/ui/components/split_view.hpp"
#include "st/ui/components/tabs.hpp"
#include "st/ui/components/terminal.hpp"
#include "st/ui/components/title_bar.hpp"
#include "st/ui/components/tree.hpp"
#include "st/ui/components/window_frame.hpp"
#include "st/ui/dsl.hpp"
#include "st/ui/icon.hpp"

namespace gbcode_ui {

using namespace st::ui;
using namespace st::ui::dsl;

struct CodeEditorPage : Component {
 public:
  CodeEditorPage(std::vector<Sample> files, std::string workspace, std::string tool_root,
                 float editor_font_scale = kEditorFontScale,
                 float editor_line_spacing = CodeEditor::kDefaultLineSpacing)
      : editor_font_scale_(editor_font_scale), editor_line_spacing_(editor_line_spacing),
        tool_root_(std::move(tool_root)), files_(std::move(files)),
        workspace_(std::move(workspace)) {}

  CodeEditorPage(std::vector<Sample> files, std::string workspace,
                 float editor_font_scale = kEditorFontScale,
                 float editor_line_spacing = CodeEditor::kDefaultLineSpacing)
      : editor_font_scale_(editor_font_scale), editor_line_spacing_(editor_line_spacing),
        files_(std::move(files)), workspace_(std::move(workspace)) {}

  /// 真值源读取口（`run_app` 注入为 `UiRoot::theme().mode()`）。
  ///
  /// **主题不在页面里存影子状态**：真值源是 `UiRoot::theme()`（由 App 持有）。
  /// 页面自存一个 `dark_` 会与它**静默分岔**——按钮文案与状态栏都跟着影子走，
  /// 而画面一点没变（实测故障：按了「暗色」、状态栏还报「视觉令牌已切换」）。
  /// 本页只负责「按当前真值算出下一档」，像个按钮该做的那样。
  std::function<ThemeMode()> theme_mode{};
  /// 主题切换出口（`run_app` 注入为 `Application::set_theme_mode`）。
  std::function<void(ThemeMode)> theme_setter{};
  /// 当前是否暗色——**从真值源读**，不缓存。
  [[nodiscard]] auto theme_is_dark() const -> bool {
    return theme_mode && theme_mode() == ThemeMode::Dark;
  }
  /// 外层窗框（由 `run_app` 装配后回填）：标题栏归它所有，本页只负责推标题。
  WindowFrame* frame_{nullptr};
  /// 菜单的**声明式宿主**（由 `run_app` 把本页 mount 进标题栏的 `leading` 槽）。
  ///
  /// 为什么不直接在 `build()` 里声明菜单：标题栏是**外壳**（`WindowFrame` 自持，
  /// 在声明树之外），而菜单要长在标题栏一行里——两者不在同一棵树上。
  /// `mount_into` 正好表达「声明式的一小块挂到既有元素的槽位」，于是整个 IDE
  /// 仍是一个 `Component`（只是它现在占两处树位），菜单与标题栏合并成一行。
  dsl::DeclarativeHost* menu_host_{nullptr};
  /// 菜单头部的**所有权**（`mount_into` 返回；`MenuHeader` 定义在本类之后，
  /// 此处只能前向声明）。
  std::unique_ptr<dsl::DeclarativeHost> menu_host_owned_{};

  // —— 状态 ——
  State<std::vector<OpenBuffer>> buffers_{std::vector<OpenBuffer>{}};
  State<std::size_t> active_{0};
  State<bool> sidebar_visible_{true};
  State<std::size_t> activity_{0};        ///< 活动栏选中项（0=资源管理器 … 4=扩展）
  State<bool> bottom_visible_{true};      ///< 终端面板开合（收起 = 隐藏下半，子树与状态保留）
  State<std::string> status_{"就绪"};
  State<std::string> cursor_text_{"Ln 1, Col 1"};
  State<std::string> selection_text_{""};
  // 终端：会话/滚回/作业全在**框架组件** `st::ui::Terminal` 里，
  // 应用侧只留一个指针与"忙闲"镜像（后者用于面板按钮的实时性）。
  // （面板收起已改隐藏方案：`Terminal` 常驻子树，指针不再有悬垂窗口。）
  Terminal* terminal_ptr{nullptr};
  bool terminal_busy_{false};
  State<std::size_t> menu_open_{kNoMenu};
  State<bool> palette_open_{false};
  State<std::string> palette_query_{""};
  State<bool> palette_shows_files_{false};   ///< Ctrl+P（文件表）vs Ctrl+Shift+P（命令表）
  State<bool> find_open_{false};
  State<std::string> find_counter_{"0/0"};
  /// 转到行浮层（Ctrl+G / 选择菜单）。
  State<bool> goto_line_open_{false};
  State<std::string> goto_line_error_{""};
  /// 查找条选项（大小写 / 全词 / 正则）。
  State<bool> find_case_{false};
  State<bool> find_word_{false};
  State<bool> find_regex_{false};
  State<std::vector<std::string>> dir_expanded_{std::vector<std::string>{}};
  State<std::vector<std::string>> search_hits_{std::vector<std::string>{}};
  State<std::string> search_summary_{""};
  /// 当前编辑器的高亮语言（状态栏/扩展面板与编辑器共用一处真值源）。
  State<std::string> language_{"cpp"};
  /// 输出面板按来源分栏（`St build` / `St test` / `终端`…）。

  static constexpr std::size_t kNoMenu{static_cast<std::size_t>(-1)};

  // —— 非状态引用（逃生舱的强类型句柄；每次 build 重新取得）——
  CodeEditor* editor{nullptr};
  /// 编辑器是否已套用初始配置（字号档位/缩进宽）。只做一次：
  /// 这些值可以随后由属性面改（设置面板/控制通道），每帧重写就把它抹了。
  bool editor_configured_{false};
  MenuBar* menu_bar_ptr{nullptr};

  // —— 行尾 blame 注解（B5）——
  //
  // 数据面（`git_service::read_blame`）早已就绪，缺的是界面。做法：
  // ① **开关**（`blame_on_`）；② **按视口取数**（只问看得见那几十行，不整文件）；
  // ③ 结果做**行号 → 注解文本**的缓存，拼进 `LineDecoration::end_text`。
  //
  // 为何按视口取而不是一次性拉整文件：`git blame` 是子进程，一个万行文件的整次 blame
  // 要几百毫秒——而用户每滚动一次都想要新的行。取视口那几十行的增量成本是可接受的。
  bool blame_on_{false};

  // —— 单文件历史弹窗（B6）——
  //
  // 取数结果放进 **`State`**（而不是普通成员 + 手标脏）：State 变化会自动触发重组，
  // 而普通成员改了重组不会发生（弹窗会永远显示“无历史”）。
  State<bool> history_open_{false};
  State<std::vector<gbcode::LogEntry>> history_entries_{};
  /// 弹窗标题用的文件路径（只在打开时定一次，不进重组依赖）。
  std::string history_path_{};
  bool history_wanted_{false};              ///< 「下一帧取历史」待办
  /// `--open` 的待办路径（帧首泵消费；空 = 无）。
  std::string pending_open_path_{};
  /// 已经拿到 blame 的文件（与 `blame_path_` 不同时空 = 需要重新取）。
  std::string blame_path_{};
  /// 行号（1 起）→ 注解文本（`张三 2024-01-01`）。取数按视口分块，缺的行按需补。
  std::map<std::size_t, std::string> blame_lines_{};
  /// 已经**问过**的行区间（避免同一段反复起子进程；记 1 起闭区间）。
  std::vector<std::pair<std::size_t, std::size_t>> blame_fetched_{};
  CommandPalette* palette_ptr{nullptr};
  Input* find_replace_input{nullptr};   ///< 替换文本不进状态（不参与重组）
  Input* find_needle_input{nullptr};    ///< 查找输入框（打开后把焦点交给它）
  bool focus_find_pending_{false};      ///< 「查找条开→下一帧聚焦输入框」的待办标记
  Input* goto_line_input{nullptr};      ///< 转到行的行号输入框
  bool goto_line_pending_focus_{false}; ///< 同上（转到行浮层）

  // —— 侧栏各视图的非状态数据（不进重组依赖：这些是“算一次用一帧”的快照）——
  std::vector<std::string> search_hit_paths_{};   ///< 与 `search_hits_` 同序的绝对路径
  /// 搜索面板行 → 命中下标（分组行占位 `-1`；与 `nav_row_to_result_` 同一套路）。
  std::vector<std::size_t> search_row_to_hit_{};
  std::vector<std::size_t> search_hit_lines_{};   ///< 同序的 1 起行号
  std::vector<std::size_t> search_hit_columns_{}; ///< 同序的列号（字节偏移）
  std::string last_query_{};                      ///< 上次搜索词（重跑用）
  std::string search_glob_{};                     ///< 文件过滤 glob
  State<bool> search_case_{false};
  State<bool> search_word_{false};
  State<bool> search_regex_{false};
  /// Git 状态快照（`refresh_git` 填充；渲染期读的是它而不是每次重查）。
  ///
  /// 版本管理重构（2026-10-09）：从"平铺列表"升级为**分组快照**——冲突/已暂存/
  /// 未暂存/未跟踪四组（XY 矩阵语义见 git_service.hpp 文件头），既暂存又改的
  /// 文件在两组各出现一次（IDEA 同款）。
  gbcode::RepoSnapshot git_{};
  bool git_probed_{false};
  /// “变更视图请求读一次状态”的待办（渲染期置、帧首泵消费——渲染期不能起子进程）。
  bool git_probe_wanted_{false};
  /// 已“访问过”的侧栏视图下标（首次激活后纳入保活集合；见 `build_sidebar`）。
  ///
  /// 为何要它：光“全部常驻”会让**启动首帧**一次性构建五棵子树（首帧最贵），
  /// 而用户真正要的是“**看过的**视图切回来还在”。两者合起来由这套集合表达。
  /// 必须 `State`：它变了要触发重组（否则新访问的视图下一帧也不会被声明）。
  State<std::vector<std::size_t>> sidebar_visited_{{0}};   // 0 = 资源管理器（初始视图）

  /// 已折叠的分组（跨刷新保留——刷新一下就把分组合回去等于没记住用户的操作）。
  ///
  /// ⚠ 必须 `State`：普通 vector 改了不触发重组，组头点下去**没反应**
  ///（这是 LSP 阶段 4 的 `completion_open_` 同一个坑——bool 版实测过）。
  State<std::vector<std::string>> git_collapsed_groups_{{}};
  /// 多步操作状态（merge/rebase 中时给“继续/跳过/中止”入口）。
  gbcode::OperationState git_operation_{};
  /// 标签 / 远程 / 暂存（stash）/ 引用清单（底部各视图的数据面）。
  std::vector<gbcode::TagInfo> git_tags_{};
  std::vector<gbcode::RemoteInfo> git_remotes_{};
  std::vector<gbcode::StashEntry> git_stashes_{};
  std::vector<gbcode::RefInfo> git_refs_{};
  std::string git_upstream_{};
  /// 写操作审计尾部（最近 20 条；渲染期只消费）。
  std::vector<std::string> git_audit_{};
  /// 新建分支 / 建标签的小输入浮层。
  State<bool> branch_new_open_{false};
  State<std::string> branch_new_name_{};
  State<bool> tag_new_open_{false};
  State<std::string> tag_new_name_{};
  State<bool> tag_new_annotated_{false};
  /// 底部面板当前标签：0=终端 1=Git。
  /// Git 内的子视图：分支 / 日志 / 标签 / 暂存 / 远程。
  State<std::size_t> git_tab_{1};
  /// 日志过滤（提交信息 / 作者 / 路径）与“只显示当前分支”。
  State<std::string> log_filter_text_{};
  State<std::string> log_filter_author_{};
  State<std::string> log_filter_path_{};
  State<bool> log_filter_current_branch_{false};
  State<gbcode::ChangeGroup> git_selected_group_{gbcode::ChangeGroup::Staged};
  /// 危险操作确认浮层（放弃更改/删除未跟踪/重置/强推）。
  State<bool> git_confirm_open_{false};
  std::string git_confirm_title_{};
  std::string git_confirm_body_{};
  std::function<void()> pending_git_confirm_{};
  /// 提交框文本。
  State<std::string> commit_message_{};
  /// 最近提交信息（去重，最多 20 条）——IDEA 的“历史消息下拉”。
  std::vector<std::string> commit_message_history_{};
  /// 修正上一笔提交（`--amend`）。
  State<bool> commit_amend_{false};
  /// 覆盖作者（`Name <email>`；空 = 用仓库配置）。
  State<std::string> commit_author_{};
  /// 提交面板“更多选项”展开态。
  State<bool> commit_options_open_{false};
  /// 提交框输入框指针（按钮提交时读——浮层每帧重建，指针只在构建帧有效）。
  Input* commit_input_ptr{nullptr};
  /// 终端里的 git 命令跑完后要刷新状态（pump 里落地——命令完成时机不可知）。
  bool git_refresh_pending_{false};
  // —— Git 工具窗（阶段 B）——
  /// 排队的 shell 命令（终端实例就绪后投递——见 `flush_terminal_commands`）。
  std::vector<std::string> pending_shell_commands_{};
  /// 底部单槽当前显示的面板（0=终端 1=Git）。
  State<std::size_t> bottom_tab_{0};
  /// 日志/分支已读（帧首泵读；git 操作后置 false 重读）。
  bool git_log_loaded_{false};
  /// 需要读日志（Git 标签可见或状态栏入口点过；泵里消费）。
  bool git_log_wanted_{false};
  std::vector<gbcode::LogEntry> git_log_{};
  std::vector<gbcode::BranchInfo> git_branches_{};
  /// 选中的提交（详情摘要与高亮用）。
  std::string git_selected_hash_{};
  State<std::string> git_commit_summary_{};
  /// 选中提交的**元信息**（详情栏显示作者与时间——日志行窄，放不下）。
  std::string git_selected_author_{};
  std::string git_selected_date_{};
  /// 选中提交的文件清单（`path, +N, −N`）——渲染期只消费（读在 `show_commit` 里）。
  std::vector<std::tuple<std::string, std::size_t, std::size_t>> git_commit_files_{};
  // —— 任意两端比较（参照歌白工作台的「比较视图」）——
  /// 比较视图是否开启（开启时编辑区换成比较面板）。
  State<bool> compare_open_{false};
  /// 两端端点文本（保留字 `WORKTREE`/`INDEX` 或任意 rev）。
  State<std::string> compare_from_{};
  State<std::string> compare_to_{"WORKTREE"};
  /// 三点对比（按共同祖先起算——分支对比的正确默认语义）。
  State<bool> compare_merge_base_{false};
  /// 结果（泵里读；渲染期只消费）。
  gbcode::CompareResult compare_result_{};
  bool compare_loaded_{false};
  bool compare_wanted_{false};
  /// 端点选择浮层：正在选哪一端（0 = A / 1 = B；2 = 未开）。
  State<std::size_t> compare_pick_{2};
  /// 当前打开的**并列差异**（空 key = 不是差异标签）。
  State<std::string> diff_key_{};
  State<std::string> diff_title_{};
  State<st::text::FileDiff> diff_file_{};
  /// 需求打开差异（帧首泵读文本——渲染期起子进程会崩，与 `ensure_git_log` 同理）。
  bool diff_wanted_{false};
  std::string diff_pending_title_{};
  gbcode::CompareOptions diff_pending_options_{};
  std::string diff_pending_path_{};
  /// 差异视图实例（当前帧有效）——`hunk` 导航靠它。
  DiffView* diff_view_ptr{nullptr};
  /// 逐行暂存模式（差异视图上方多一条工具条）。
  ///
  /// 为何是模式而不是"永远显示勾选列"：勾选列会占掉左侧栏宽度（本来就窄），
  /// 而"挑着提交几行"是阶段性动作（做完就回到"看差异"）。
  State<bool> diff_staging_{false};

  // —— 三方合并视图（冲突处置）——
  ///
  /// 它是与差异视图**并列的第三种“编辑区内容”**（见下面 `build` 里的互斥顺序），
  /// 而不是一个浮层：合并要对照着看三栏、还可能来回滚很久，浮层会让人没法一边看
  /// 冲突一边看别的。
  State<std::string> merge_key_{};    ///< 空 = 不是合并标签
  State<std::string> merge_path_{};   ///< 被处理的冲突文件（相对仓库根）
  /// 划分结果（普通成员而不是 `State<>`：声明式 diff 要求 `operator==`，
  /// 而 `MergePlan` 没有；它也不参与界面 diff——视图从三份文本自己算）。
  st::text::MergePlan merge_plan_{};
  State<std::string> merge_base_text_{};
  State<std::string> merge_ours_text_{};
  State<std::string> merge_theirs_text_{};
  /// 需求打开合并视图（帧首泵取三仝内容——渲染期起子进程会崩，与差异同理）。
  bool merge_wanted_{false};
  std::string merge_pending_path_{};
  /// 合并视图实例（当前帧有效）——F7 冲突导航靠它。
  MergeView* merge_view_ptr{nullptr};
  /// 三仝内容是否已装载（避免每帧重算划分）。
  bool merge_loaded_{false};
  /// 「选择变了、等帧首落盘」的标记。
  ///
  /// 为何不在选择回调里直接写：那会在事件派发**过程中**同步写文件（可能失败/变慢），
  /// 而此刻 UI 正处在“半完成的事件处理”里。改为置标记、帧首泵统一落盘——
  /// 那里也是唯一改文件的位置（与 `ensure_*` 同一节奏）。
  bool merge_write_pending_{false};
  /// 状态栏问题计数“轮跳”到第几处（反复点同一个计数就依次往下跳）。
  std::size_t problem_cycle_{0};
  /// 已注册的语言清单（扩展视图用；懒加载一次）。
  std::vector<std::string> languages_{};
  /// 打开对话框的状态跟踪：`dialog_seen_` 记“上一帧对话框已在树上”——
  /// 它从无到有（本帧才被声明）时才初始化目录，之后用户导航不再被覆盖。
  /// 不能用“进程级一次”的标记：对话框关掉再开是**同一个元素复用**
  /// （声明式同 key 复用），那个标记不会重置，第二次打开目录就是空的（实测踩到）。
  bool dialog_seen_{false};
  /// 文件对话框起始目录（上次确认时所在；空 = 从工作区/当前目录起）。
  std::string dialog_start_dir_{};
  /// 关闭脏标签的待确认动作（非空 = 弹了确认对话框）。
  std::function<void()> pending_close_{};
  /// 待确认关闭的标签名（对话框正文用）。
  std::string pending_close_label_{};
  /// 编辑器右键菜单开关 + 锚点（锚点跟随鼠标最后位置）。
  /// 资源管理器文件操作（右键菜单）的待执行动作。
  ///
  /// 为何用枚举而不是几个 bool：新建文件/新建文件夹/重命名/删除**互斥**，
  /// 用四个 bool 会出现“同时为真”的非法状态（而浮层只能显示一种）。
  enum class FsAction : std::uint8_t { None, NewFile, NewFolder, Rename, Delete };
  State<FsAction> fs_action_kind_{FsAction::None};
  State<bool> fs_action_open_{false};
  /// 浮层里显示的目标路径（删除/重命名时用户要看的）。
  State<std::string> fs_action_target_{};
  /// 浮层里那个名字输入框（每次重组重新赋值；**不得跟帧**引用它）。
  Input* fs_input{nullptr};
  /// 浮层刚打开时要聚焦名字输入框（与 `goto_line_pending_focus_` 同一套路）。
  bool fs_focus_pending_{false};
  /// 资源管理器右键菜单：开关 + 锚点 + 目标行 key（**右键不改选中**，
  /// 所以目标必须单独记——否则“右键一个文件、菜单作用在另一个上”）。
  State<bool> explorer_menu_open_{false};
  st::math::Point explorer_menu_anchor_{};
  State<std::string> context_key_{};

  /// 「转到符号」面板的数据源：当前文件大纲 vs 工作区符号。
  enum class SymbolKind : std::uint8_t { Document, Workspace };
  State<SymbolKind> symbols_kind_{SymbolKind::Document};
  State<bool> palette_shows_symbols_{false};

  State<bool> context_open_{false};
  st::math::Point context_anchor_{};
  /// 快捷键一览浮层开关。
  State<bool> shortcuts_open_{false};
  /// 打开/另存对话框的待执行动作（选完路径后调）。
  std::function<void(const std::string&)> pending_file_action_{};

  // —— 动作：只写状态（同步由框架做）——


  /// 打开一份内置样例（已在标签里 = 激活；不在 = 新标签）。
   auto open_sample(const Sample& sample) -> void;


  /// `--open <path>` 的待办：**不在构造期打开**（那会在构建期写 State，见 `main` 的注释），
/// 而由帧首泵在首帧之后就打开一次。
void set_pending_open_path(std::string path);


// —— 会话（最近打开 + 关窗恢复）——

/// 会话文件的路径（构造时由 `default_session_path()` 定；e2e 用 ST_HOME 重定向）。
/// 抽成成员而不是每次现算：路径要稳定（保存与读取必须是同一个文件），
/// 且测试里可能想换位置。
std::string session_path_{};
/// 最近打开的文件（MRU，去重上限 10；`open_path` 成功时头插）。
std::vector<std::string> recent_files_{};
/// 最近打开的文件夹（换工作区时头插；恢复入口与菜单共用）。
std::vector<std::string> recent_folders_{};

/// 会话恢复（启动时调一次，见 `main` 的说明）：工作区、标签、活动项、光标。
/// **不在构造期调**——与 `set_pending_open_path` 同一条纪律（构建期写 State 会
/// 触发递归重组）；这里把待恢复清单记下来，由帧首泵落地。
/// 返回 true = 恢复了点什么（用于状态栏播报）。
auto restore_session(const gbcode::SessionState& state) -> bool;

/// 保存当前会话到 `session_path_`（退出前调；状态栏报告结果，不弹窗）。
auto save_session() -> void;

/// 打开会话文件里记录的某个文件（菜单/命令面板的「最近」入口）。
auto open_recent(const std::string& path) -> void;


  /// 帧首泵：把待打开的路径真的打开（一次）。
   void flush_pending_open();


  /// 打开真实文件（资源树 / 搜索命中 / 快速打开共用）。
   auto open_path(const std::string& path) -> void;


   auto open_stlog() -> void;


  /// 关闭一个标签（按 key——索引会随增删漂移，key 才是业务身份）。
   auto close(std::string_view key) -> void;


  /// 关闭当前标签（**走确认路径**：脏标签会先弹对话框）。
  ///
  /// 为何必须绕到 `request_close`：入口有四处关闭（Ctrl+W / 工具栏按钮 / 菜单 / 标签的 ×），
  /// 只有标签的 × 天然走 `request_close`——其余三处若直接调 `close()`，
  /// 就会出现“同一个动作，换个入口就静默丢修改”的不一致（实测踩到）。
   auto close_active() -> void;


  /// 保存当前标签：真实文件走 `fs::write_text`，内置样例走内存模拟。
   auto save() -> void;


  /// **保存全部**（Ctrl+K S / 文件菜单）：把所有脏标签写盘。
  ///
  /// 为何不只是循环调 `save()`：`save()` 既有状态栏文案、又会 `buffers_.set()`，
  /// 循环调它会把中间每一次的文案盖掉（用户只看得到最后一次），
  /// 而且每次都推一遍状态 → N 次重组。这里一次性写盘、一遍落地。
  /// 无路径的内存缓冲（untitled-N）**跳过**并在文案里说清楚——
  /// 静默跳过会让用户以为“保存了但没落盘”。
   auto save_all() -> void;


  /// 切换亮/暗主题。  ///
  /// 唯一的真值源是 `UiRoot::theme()`（App 持有）：这里**读**它算下一档、
  /// **写回**它（经 `theme_setter` → `Application::set_theme_mode`，后者会连带
  /// 处理好字号缩放与文本 gamma），然后只更新本页的文案。
  /// 不在这里维护自己的 `dark_`——那正是故障的来源（影子状态永不落地到主题）。
   auto toggle_theme() -> void;


  /// 编辑器被编辑：标脏 + 重跑轻量检查（问题面板与状态栏计数同源）。
   auto on_edit() -> void;


  /// 光标/选区变化 → 状态栏文案。
   auto on_cursor_moved() -> void;


  /// 重跑当前编辑器的轻量检查（问题列表与状态栏计数**同一份数据**）。
   auto refresh_problems() -> void;


  /// 一次性接线语言服务的回调（在首次启用时挂）。
   void wire_language_callbacks();


  /// 把当前文件告知语言服务（首次会懒启动对应 server）。
   void sync_language_document(const OpenBuffer& buffer);


   [[nodiscard]] auto error_count() const -> std::size_t;

   [[nodiscard]] auto warning_count() const -> std::size_t;

   [[nodiscard]] auto active_language() const -> std::string;

  /// 标题栏文案：脏时加一个圆点前缀（VSCode 同族的"未保存"提示）。
   [[nodiscard]] auto window_title() const -> std::string;


  /// 打开命令面板（`files_table=true` 走 Ctrl+P 的快速打开文件表）。
   auto open_palette(bool files_table, std::string query = {}) -> void;


   auto open_find() -> void;


  /// 帧首处理“查找条已出现→把焦点交给它”（构建期拿不到输入框实例）。
   void apply_pending_focus();


  /// 标题栏**文案**：窗框（外壳）自持标题栏，本页只把“现在该显示什么”推过去
  /// （文件名 + 脏点，与 VSCode 把当前文件名写进标题栏同构）。
  ///
  /// 为什么保留这个“推”而不把标题也搬进 `MenuHeader`：标题属于**窗框**的状态
  /// （它决定窗口叫什么），菜单只是恰好与它同行；两者合并不等于所有权合并。
   void push_title();


  /// 每帧推进的**构建前动作**（由入口在 `tick` 之前调）。
  ///
  /// 两件事：① 把工作线程攒下的终端输出搬进 `State`（本帧重组才能排进布局）；
  /// ② 泵终端。
  ///
  /// 收起面板已改走 `SplitView::set_second_hidden`（终端常驻子树、只隐藏），
  /// `terminal_ptr` 不再有悬垂窗口——旧的"关最后一个会话 → 收面板"延迟路径
  ///（`bottom_close_pending_` + 拆树时清指针）随拆树方案一起删除。
   void update_terminal();


  /// 每帧泵语言服务：搬消息、把诊断推给编辑器与问题列表。
  ///
  /// 为什么在 `tick()` **之前**泵（与终端同一个理由）：诊断要在本帧的布局/绘制里
  /// 生效，放在 `tick` 之后就永远慢一帧（用户看到"改完错字，波浪线还在"）。
   void pump_language_service();


  /// 落地"上一帧记下的跨文件跳转"（定义/引用结果）。
  ///
  /// 为什么要等一帧：`jump_to_location` 里 `open_path` 只是改了状态，
  /// 编辑器实例要到**下一次重组**才拿到新文本（`set_text` 在那里发生）——
  /// 立刻 `goto_line` 会跳在旧文件上（实测：跳定义时行号对了但内容还是上一个文件）。
   void apply_pending_navigation_jump();


  /// 把 LSP 诊断推给编辑器（只在**内容变了**时写，见 `lsp_diagnostics_stamp_`）。
   void push_diagnostics_to_editor();


  /// 第 `line`（0 基）行第 `column`（0 基，UTF-16 码元）列在全文里的字节偏移。
   [[nodiscard]] static auto offset_for_line_column(const std::string& text, std::size_t line, std::size_t column) -> std::size_t;


  /// 第 `line`（0 基）行的结束偏移（不含换行符）。
   [[nodiscard]] static auto line_end_offset(const std::string& text, std::size_t line) -> std::size_t;


   void build(Composer& c) override;


   void on_menu(const std::string& menu, const std::string& item);


  /// 编辑/选择菜单：直接作用于当前编辑器（一次性命令，不涉及界面形态）。
   void apply_editor_menu(const std::string& menu, const std::string& item);


  // —— 转到行（Ctrl+G / 选择菜单）：一个小浮层 + 输入框，不靠“猜” ——
  //
  // 此前这个菜单项被实现成“跳到第一个问题行”，并与用户按下的 Ctrl+G 毫无关系——
  // 菜单项叫“转到行…”而行为是别的，属于可信度问题（用户试一次就不再信任何菜单项）。
  // 现在它是一条真链路：输入行号 → 回车 → 光标与视图都到那一行。
   void open_goto_line();


  /// 提交行号（“3” 或 “3:8” 行:列）。
  ///
  /// 输入以**纯文本**传进来：浮层里输入框的 Enter 可能来自键盘、也可能来自
  /// 控制通道的 `invoke submit`——两条路都是 `Input::on_submit`，语义相同。
   void submit_goto_line(std::string_view text);


  /// 帧首处理“转到行浮层已出现→把焦点交给输入框”（构建期拿不到输入框实例）。
  /// 与查找条同一机制（见 `apply_pending_focus`）。
   void apply_pending_goto_focus();


  /// 转到行浮层（Ctrl+G / 选择菜单 → “转到行…”。）
  /// 重命名输入浮层（F2 → prepareRename → 这个浮层 → Enter 提交）。
  ///
  /// 与"转到行"同一范式（`Input` + 按钮 + 错误信息住在浮层里）：但**多一个
  /// 预填值**——`prepareRename` 给回来的当前名字，用户改几个字比从零敲快得多。
  /// git 危险操作确认（放弃更改 / 删除未跟踪文件）。
  ///
  /// 与「关闭确认」同一 Dialog 形态，但语义是**不可恢复**——按钮文案明确说后果，
  /// 不用"确定/取消"这种不带信息的词。
  /// 新建分支浮层（输入名字 → `git checkout -b <name>`）。
   void build_branch_new_dialog(Composer& c);


  /// 建标签浮层（名字 + 是否注解）。
   void build_tag_new_dialog(Composer& c);


  /// 端点选择浮层：A/B 两端可以从**分支 / 最近提交**里选（也可直接粘 rev）。
  ///
  /// 为何要有它：比较视图的价值就是“任意两端”，而只给几个常用端点的按钮
  /// 等于把“任意”锁在那几个选项上（歌白工作台的端点选择器就是为此存在）。
   void build_compare_picker(Composer& c);


  /// 选好一个端点（写入 A 或 B、关浮层、触发重读）。
   void pick_endpoint(std::size_t which, const std::string& value);


  /// 危险操作确认浮层（写入前先让用户看一眼**将要执行的命令**）。
   void build_git_confirm(Composer& c);


   void build_rename_dialog(Composer& c);


  /// 文件操作浮层（右键菜单的落点）：新建 / 重命名 用输入框，删除用确认。  ///
  /// 形态与 `build_goto_line` / `build_rename_dialog` **完全一致**（同一条浮层套路）：
  /// `overlay` + `card` + `custom<Input>` + 一个“干”按钮。
  /// 名字的读音只从 `fs_input` 指针取（它在每次重组时被重新赋值）——
  /// 不要跨帧存元素指针（浮层里的元素每帧重建，那是悬垂访问）。
   void build_fs_action(Composer& c);


  /// 新建/重命名浮层里的输入框聚焦待办（与 `apply_pending_goto_focus` 同一套路）。
   void apply_pending_fs_focus();


   void build_goto_line(Composer& c);


  // —— 3. 主体三栏 ——
   void build_main(Composer& c, const std::vector<OpenBuffer>& buffers, std::size_t active, bool has_editor);


   void build_activity_bar(Composer& c);


  /// 底部单槽的三态切换（两按钮同一词形）：
  /// 隐藏中 → 打开并显示我；显示的是别人 → 切到我；显示的是我 → 收起下半。
  ///（与上组活动栏“点当前视图 = 收起侧栏”同一交互词形。）
   void toggle_bottom_panel(std::size_t mine);


  /// Git 按钮：切到 Git 工具窗（打开顺带要日志）。
   void toggle_git_panel();


  /// 开关底部终端面板（活动栏按钮 / `` Ctrl+` `` 同一条路径）。
  ///
  /// 打开时把键盘焦点交给终端输入行（`Terminal` 自己会在下一次事件里接住），
  /// 这是“点了终端按钮就能直接敲命令”的那一步。
   void toggle_terminal_panel();


  /// “命令要在终端里跑给用户看”的统一入口：确保下半开着且停在终端
  ///（Git 操作、运行任务、diff 进终端等都走这里）。
   void ensure_terminal_visible();


  /// 把某个侧栏视图标记为“已访问”（此后它常驻树上、切回来还是原样）。
  ///
  /// 集中成一个函数（而不是散在各入口）：活动栏、状态栏分支格、命令面板、
  /// “与…比较”按钮……都能切视图；漏一处的表现是“从那个入口切过去、切回来位置就丢了”
  /// ——而这种“只有某条路径会丢”最难被发现。
   void mark_sidebar_visited(std::size_t index);


  // —— 视图 6：比较（任意两端 → 文件清单 → 单文件并列差异）——
  //
  // 为何要有它（而不只有左侧「变更」）：变更面板回答“我**现在**改了什么”，
  // 比较回答“**任意两个状态**差什么”——「这个提交之后又改了什么」「两个分支差在哪」
  // 只能靠它（歌白工作台的同名视图）。清单本体在**编辑区**：它是内容，不是导航。
   void build_compare_sidebar(Composer& c);


   void build_sidebar_head(Composer& c, std::string_view title);


  /// 各视图的动作按钮（按当前视图给不同的一组）。
   void build_sidebar_actions(Composer& c);


  /// 侧栏内容（按活动栏选中项切换）。
  ///
  /// 五个视图各自独立成函数：视图多起来之后「一熊 if/else 里塞五段界面」会让每加一个
  /// 视图都要重读一整堆无关代码。
   void build_sidebar(Composer& c, const std::vector<OpenBuffer>& buffers, std::size_t active);


  // —— 视图 0：资源管理器（真工作区树 / 内置样例列表）——
   void build_explorer(Composer& c);


  // —— 视图 1：搜索（工作区文本 + 按文件分组）——
   void build_search_view(Composer& c);


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
  void replace_in_workspace(std::string_view replacement);


  // —— 视图 5：导航结果（多处定义/引用/实现的清单）——
  //
  // 与搜索视图同一套「文件分组 + 可点行」形态。单独一个视图而不是弹层：
  // 引用列表的用法是「来回看几处」，弹层每点一处就关；侧栏可以一直开着。
   void build_nav_results_view(Composer& c);


  /// 开关胶囊（「Aa / ab / .*」这类选项）：按下态由 `active` 决定。
  ///
  /// 为什么不用 `Checkbox`：查找选项是**密集的一行小开关**（三个挤在 200px 宽里），
  /// 带文字的勾选框会把行撑到放不下——胶囊用固定 26px 宽，观感与 VSCode 一致。
   void toggle_chip(Composer& c, const char* id, const char* label, bool active, std::function<void()> on_click);


  /// 小图标钮（侧栏各视图共用；`on_click` 为空 = 纯展示）。
   void scm_icon_button(Composer& c, const std::string& id, const char* icon_name, const char* tip, std::function<void()> on_click);


  // —— 视图 2：变更（面板：分组 + 行内动作 + 底部提交框）——
  //
  // 参照歌白文件工作台的设计（IDEA 的 Commit 工具窗就在左侧）：
  // "改了什么 / 要提交什么"是编码时最频繁看的，放左侧随时可见；
  // 底部留给"分支 | 日志"（那是回顾历史时才看的，节奏不同——见 build_git_view）。
   void build_scm_view(Composer& c);


  /// 提交面板（消息 + 最近消息下拉 + amend/作者 + 提交 / 提交并推送）。
   void build_commit_panel(Composer& c);


  /// 打开一个变更文件的 diff（**并列差异**，替代旧版“把 patch 敲进只读标签”）。
  ///
  /// 为何是并列而不是一段 patch 文本：diff 要**对照看**（“原来是什么 / 现在是什么”），
  /// 而并列视图靠双行号把两栏逐行对齐；patch 文本做不到（它是一维流）。
   void open_git_diff(const gbcode::GitChange& change);


  /// 比较视图（参照歌白工作台的「比较」）：**任意两端** → 文件清单 → 单文件并列差异。
  ///
  /// 为何要有它（而不只有左侧「变更」）：变更面板回答“我现在改了什么”，
  /// 比较回答“**任意两个状态**之间差什么”——「这个提交之后我又改了什么」
  /// 「两个分支差在哪」只能靠它。
   void build_compare_view(Composer& c);


  // —— 视图 3：运行与调试（真跑 st 子命令）——
   void build_run_view(Composer& c);


  // —— 视图 4：扩展（运行时注册的语言清单）——
   void build_extensions_view(Composer& c);


  /// 切当前编辑器的高亮语言（也改写标签的语言字段，切回来时保持一致）。
   void set_language(const std::string& name);


   void build_editor_area(Composer& c, const std::vector<OpenBuffer>& buffers, std::size_t active, bool has_editor);


  /// 编辑器工具栏（在标签栏下方）：保存 / 撤销 / 重做 / 注释 / 参考线 / 右键菜单入口。
  ///
  /// 为什么要有它（VSCode 其实没有这条工具栏）：霜天的控制通道与键盘快捷键是两条腿，
  /// 而**鼠标用户没有第三条腿**——“保存/撤销”这些最常用的动作在纯鼠标下无处可点。
  /// 工具栏把它们变成可发现、可点击、且有稳定 id 的元素（自动化也能直接 `invoke`）。
   void build_editor_toolbar(Composer& c, const std::vector<OpenBuffer>& buffers, std::size_t active);


  /// 面包屑：当前文件路径按 `/` 拆段（段可点——点目录段切到资源管理器并展开）。
   void build_breadcrumb(Composer& c, const std::vector<OpenBuffer>& buffers, std::size_t active);


  /// 打开编辑器右键菜单（由 `CodeEditor::on_context_menu` 回调调用）。
   void open_context_menu(st::math::Point at);


  // —— 4. 上半 + 终端：一个**上下可分栏**的两段（拖动改高度，见 build）——
  //
  // 为什么中间要一个 SplitView 而不是两个固定高的兄弟：终端的高度是**用户偏好**
  // （看输出时想高、看代码时想矮），固定值只能靠改代码。分栏手柄还顺带把
  // 「上半 / 下半」的边界画成一条可抓的线（与侧栏同一种交互语言）。
  //
  // 底部面板**只有终端一个视图**（问题/输出已删，见 `Problem` 处的说明）——
  // 于是这里不再需要标签栏，只留一行“终端标题 + 状态 + 动作”。
   void build_bottom(Composer& c);


  /// 终端本体（`build_bottom` 的终端面板内容）。
   void build_terminal_body(Composer& c);


  // —— Git 工具窗（底部标签）：分支 | 日志 | 标签 | 暂存 | 远程 ——
  //
  // 与左侧「变更」的分工（歌白工作台同一分法）：这里看**历史与引用**——
  // 分支/标签/远程（仓库的“坐标”）与日志（泳道 + refs 胶囊）；点提交看它改了什么。
  // 变更则在左侧（当下要提交的东西）。
  /// 底部 Git 工具窗：**三栏**（参照 IDEA 的 Git 工具窗）。
  ///
  /// ```
  /// ┌────────────┬─────────────────────────┬──────────────────┐
  /// │ 引用：       │ 日志（选中的引用范围）      │ 提交内容：         │
  /// │ 分支/标签/   │ 泳道 + 短 hash + refs    │ 元信息 + 文件清单   │
  /// │ 暂存/远程    │                         │ （点开并列差异）    │
  /// └────────────┴─────────────────────────┴──────────────────┘
  /// ```
  ///
  /// 为何是这三栏（而不是平铺五个标签页）：三者是一条**动线**——「选一个引用
  /// （我在看哪个分支）→ 看它的提交（发生了什么）→ 看某笔提交的内容（具体改了什么）」，
  /// 每一步的结果都是下一步的输入。做成互斥标签会把这条动线拆成三次“切回去再切过来”。
  /// 原来的五视图仍在，只是收进**左栏的分组**（它们本来就是“引用的种类”）。
   void build_git_view(Composer& c);


  /// 左栏：引用（分支 / 标签 / 暂存 / 远程 四个分组，一次只显一组）。
  ///
  /// 为何用“分组切换”而不是四个固定块：四个列表长度差异很大（分支可能几十条、
  /// 远程通常一条），固定同时显示会被最短的那个浪费掉全部高度。
   void build_ref_sidebar(Composer& c);


  /// Git 工具窗左栏的四个**引用分组**（分支 / 标签 / 暂存 / 远程）。
   void build_git_tabs(Composer& c);


  /// 多步操作横幅（merge/rebase/cherry-pick/revert 中）：继续 / 跳过 / 中止。
   void build_operation_banner(Composer& c);


  /// 子视图 0：分支（本地/远程、切换、新建、删除）。
   void build_branches_pane(Composer& c);


  /// 子视图 2：标签（新建/删除）。
   void build_tags_pane(Composer& c);


  /// 子视图 3：暂存（stash）——弹出/应用/删除。
   void build_stashes_pane(Composer& c);


  /// 子视图 4：远程（拉取/推送/强制推送（带 lease）/新建）。
   void build_remotes_pane(Composer& c);


  /// 中栏：日志（过滤 + 泳道图 + 范围提示）。
  ///
  /// 与右栏的分工：中栏回答“**发生了什么**”（一笔笔提交），右栏回答
  /// “**这一笔改了什么**”（文件与具体差异）。两者同屏才能“选中即看内容”，
  /// 而互斥标签页会把这条动线打断。
   void build_log_pane(Composer& c);


  /// 一栏的标题行（标题 + 右侧一个可选图标钮）——三栏各一份，职责一眼可辨。
   void build_pane_head(Composer& c, const char* id, std::function<std::string()> title, const char* action_id);


  /// 日志工具栏：三个过滤输入 + “仅当前分支” + 刷新。
   void build_log_filters(Composer& c);


  /// 日志行的展示文本（泳道标记 + 短 hash + 主题 + refs 胶囊）。
   auto log_line_label(const gbcode::LogEntry& entry) const -> std::string;


  /// 过滤后的日志（提交信息/作者/路径三个条件都作用在同一条上）。
   auto filtered_log() const -> std::vector<gbcode::LogEntry>;


  /// 右栏：**选中提交的内容**（元信息 + 汇总 + 文件清单；点文件开并列差异）。
  ///
  /// 为何独立成一栏（而不是塞在日志下方）：看历史是“扫很多笔 → 挑一笔 → 看它改了什么”，
  /// 而“改了什么”是**当前唯一要看的东西**——把它压在日志下面会被迫滚动。
   void build_commit_detail(Composer& c);


  /// 确认一个**不可逆/需确认**的写操作（统一入口：弹确认 → 批准后才执行）。
  ///
  /// 与直接执行的区别：`Danger::None` 的操作不弹（弹多了人会盲目点“确定”，
  /// 那才是真危险）；这里只收 `Confirm`/`Backup` 两级。
   void ask_write(const gbcode::WriteIntent& intent, std::string title, std::string body);


  /// 删分支（`-d` 拒删未合并——想强删得再点一次）。
   void ask_delete_branch(const std::string& name);


  /// 强删分支（`-D`）——丢掉未合并的提交，必须说清楚。
   void ask_force_delete_branch(const std::string& name);


  /// 删标签。
   void ask_delete_tag(const std::string& name);


  /// 删一条 stash。
   void ask_drop_stash(const std::string& index);


  /// 强制推送（**带 lease**）。
  ///
  /// 为何不无脑用 `--force`：无 lease 的强推会直接盖掉别人的提交；
  /// `--force-with-lease` 在远端已经前进时**拒推**——这才是安全网。
   void ask_force_push();


  /// 重置到某个提交（三种模式）。
   void ask_reset(const std::string& ref, gbcode::WriteOp op);


  /// 建分支（浮层提交）。
   void create_branch();


  /// 建标签（浮层提交）。
   void create_tag();


  /// 按 hash 打开一个提交（标签行点开用）。
   void open_commit_by_hash(const std::string& hash, const std::string& label);


  /// 切换到分支（终端代敲——checkout 会改工作区，输出必须让用户看见）。
   void checkout_branch(const gbcode::BranchInfo& branch);


  /// 查看一个提交：与它的**父提交**逐文件并列对比（首个提交则与空树比）。
   void show_commit(const gbcode::LogEntry& entry);


  /// 打开某个提交里一个文件的并列差异（父提交 ↔ 该提交）。
  ///
  /// 根提交没有父：`HEAD^` 会报错，而“与不存在的父对比”在语义上就是“与空树对比”
  /// ——歌白那边实测过“不归一时被当成了无差异”。
   void open_commit_file(const std::string& hash, const std::string& path);


  // 两个共用小工具（终端/搜索/面包屑都用）：直接在这里定义——
  // 只写声明的话，后面若有任何分支未走到它们的定义，编译器会报
  // “used but never defined”（`static` 成员函数在文件作用域里就是这个语义）。
  /// 把用户输入的文件名解析为绝对路径（相对终端当前目录 → 工作区）。
   [[nodiscard]] auto resolve_path(std::string_view raw) const -> std::string;


  /// 文本的行数（空串算 0 行）。
   [[nodiscard]] static auto line_count_of(std::string_view text) -> std::size_t;


  /// 终端的工作目录（相对路径按它解析；未显式设置过就用工作区根）。
   [[nodiscard]] auto terminal_exec_dir() const -> std::string;


  /// 上半（资源管理器/编辑区）与下半（问题/输出/终端）的**分栏宿主**。
  ///
  /// 结构：`[活动栏] [上/下分栏]`——活动栏提到这一层，**与底部面板并列**。
  ///
  /// 为什么把活动栏从 `main-row` 里提出来：它是工作台的**全局导航**，不是
  /// “上半区的一部分”。放在 `main-row` 里时，它只到上分区底（y=40..439），
  /// 底部终端面板一展开，活动栏就在终端左边变成一根矮条——与标题栏/状态栏
  /// 这两根**通栏**在视觉上对不齐（用户要求“占满标题栏到状态栏之间的全部高度”）。
  ///
  /// 与 VSCode 同构：`[activity][sidebar|editor]` 上、`[panel]` 下，两者共享
  /// 同一条水平分栏线；活动栏横跨**整个**下半区左侧。
   void build_workbench(Composer& c, const std::vector<OpenBuffer>& buffers, std::size_t active, bool has_editor);


    // —— 5. 状态栏（兼容钩子 id 全保留：`status` / `btn-theme` / 计数 / 光标 / 语言）——
  //
  // 每一项尽量做成**可点**的：状态栏是 IDE 里“看一眼”的地方，而看一眼之后往往
  // 想知道更多（分支 → 变更面板、问题数 → 问题面板、语言 → 语言列表……）。
   void build_status_bar(Composer& c);


  /// 一个可点的状态栏项（图标 + 文本，点击即触发动作）。
  ///
  /// `hint` 说明“点一下会发生什么”——状态栏是“看一眼”的地方，那些计数/语言/缩进
  /// 格子点下去之后去哪里并不显然（提示是走这一步前的唯一预告）。
   void status_item(Composer& c, const char* id, const char* icon, std::function<std::string()> content, std::function<void()> on_click, std::string hint = {});


  /// 点状态栏的问题计数：**在编辑器里定位第一处问题**。
  ///
  /// 为何不再“切到问题面板”：面板已删（见 `Problem` 的说明），而状态栏那个计数
  /// 用户真正想做的事是“带我去看看”——所以直接把光标送到那一行并高亮它。
  /// 反复点同一类计数就依次跳到下一处（比开一个列表更少一步）。
   void show_problems(bool warnings_only);


  /// 缩进宽度循环 2 → 4 → 8（点状态栏的缩进格）。
   void cycle_tab_width();


  // —— 6. 查找替换条（Ctrl+F / Ctrl+H；可见性由 find_open_ 驱动）——
   void build_find_bar(Composer& c);


  /// 用当前选项重跑查找（选项变化时调）。
  ///
  /// 大小写与全词都是**组件的一等选项**（`CodeEditor::FindOptions`）：直接交给
  /// 查找器、在“命中那一刻”判——而不是把查找词改写成 `\b词\b` 这种
  /// “看着支持、实际给出错结果”的做法（本编辑器不做正则，`\b` 只会被当成
  /// 两个字面字符，一处也匹配不到，而且什么错也不报）。
   void refind();


  /// 把页面的两个查找开关翻译成组件的查找选项（**唯一转换点**）。
   [[nodiscard]] auto find_options() const -> CodeEditor::FindOptions;


  // —— 编辑器右键菜单（`ContextMenu` overlay；锚点 = 最后一次鼠标位置）——
   void build_editor_context(Composer& c);


  /// 右键菜单条目（序号 = `kContextItems` 的下标）。
   void run_context_action(std::size_t index);


   static auto context_items() -> std::vector<MenuItem>;


  /// 关闭脏标签确认对话框（`Dialog` overlay；三个按钮与 VSCode 同序）。
   void build_close_confirm(Composer& c);


  /// 键盘快捷键一览（`Ctrl+K` 或帮助菜单；内容与代码里注册的表同源，不手写第二份）。
   void build_shortcuts_card(Composer& c);


   void open_file_dialog();

   void save_as();

  /// 打开**文件夹**（换工作区）：同一条对话框链路，用 `FileDialog` 的目录模式。
   void open_folder_dialog();


  /// 文件对话框（打开 / 另存为 / 新建共用；`pending_open_mode_` 区分）。
  ///
  /// 为什么用真 `FileDialog` 而不是自绘输入框：目录浏览/上级/双击进入这些行为
  /// 组件已经写好且**在无头下可程序化选路**（`set_pending_path`），自绘会全部重写一遍。
   void build_open_dialog(Composer& c);


  // —— 命令面板（overlay；命令表 / 文件表两种）——
   void build_palette(Composer& c);


   /// 把排队的 shell 命令投递给终端（终端实例就绪后）。
  ///
  /// 为何要排队：写操作在设计上“交终端执行”，而 `terminal_ptr` 只在**终端面板
  /// 被声明过**之后才有值。从未打开过终端时，事件回调里直接 `terminal_ptr->run(...)`
  /// 会因空指针而**静默什么都不做**（实测：点「中止」毫无反应，状态栏也不说，
  /// 看着像按钮坏了——而实际上是命令根本没发出去）。
  /// 现在改为“先入队 + 确保终端可见”，下一帧终端实例就绪时在泵里投递。
   void flush_terminal_commands();


  /// 让终端面板可见并排入一条命令（真正投递在 `flush_terminal_commands`）。
   void queue_terminal_command(std::string command);

   void stash_active_text(std::vector<OpenBuffer>& list);


  /// 切到某个标签（先存回旧文本，再灌新文本）。
   void activate(std::vector<OpenBuffer>& list, std::size_t index);


  /// 把当前标签的文本灌进编辑器（切标签/打开文件的唯一入口）。
  void load_active_text();


   void open_by_name(const std::string& name);


   void switch_tab(std::size_t index);


   void jump_to_problem(std::size_t index);


  /// 关闭入口（快捷键/×/菜单都走这里）：脏标签弹确认，干净标签直接关。
   void request_close(std::string_view key);


  /// 确认对话框的第一个按钮：保存并关闭。
   void confirm_close_save();

  /// 第二个按钮：不保存直接关。
   void confirm_close_discard();

  /// 第三个按钮（也走 Esc / 点遮罩）：取消。
   void confirm_close_cancel();


   void finalize_pending_close();


  /// 关闭全部标签（脏的先确认——这里只处理当前活动那个，其余交回逐次关闭）。
   void close_all();


  /// 关闭当前标签**右侧**的全部（脏的按同样规则拦截）。
   void close_to_right();


   [[nodiscard]] auto index_of_key(std::string_view key) const -> std::size_t;


   void update_find_counter();


   void step_find(bool backward);


   [[nodiscard]] auto replacement_text() const -> std::string;


   void replace_one();


   void replace_all();


   void close_find();

  public:
 /// 启动信息进**状态栏**（入口在 `app.start()` 之后才能拿到后端/端口信息）。
 ///
 /// 为什么不写进终端：真终端里那条流**归 shell**——应用不是写者。
 /// 往别人的流里插一行会打乱屏幕（shell 下次重绘就把它抹掉，
 /// 或更糟：把提示符位置弄乱）。应用自己的通知属于状态栏。
   auto note_startup(const std::string& text) -> void;

   /// 每帧推动终端作业（入口主循环调；见 `update_terminal` 的说明）。
   auto pump() -> void;

  /// 退出前收尾：停掉在跑的作业并 join 读线程（`jthread` 把它露在对象生命周期
/// 之外会很危险：它可能正阻塞在 `read_line` 上，而对象已经在析构）。
/// 同的还有一个收尾职责：保存会话（见 `save_session`）。
auto shutdown() -> void;

  /// 关闭查找条（入口的 Esc 快捷键用；与面板上的「×」同一条路径）。
   auto dismiss_find() -> void;

 /// 新建文件（入口快捷键用）。
   auto new_file_request() -> void;

 /// 转到符号（入口快捷键/菜单用）：`document=true` 当前文件，`false` 工作区。
   auto symbols_request(bool document) -> void;

 /// 全部保存（入口快捷键 Ctrl+Alt+S 用）。
   auto save_all_request() -> void;

 /// 手动触发补全（入口快捷键 Ctrl+Space 用）。
   auto completion_request() -> void;

 /// 跳到定义（入口快捷键 F12 用）。
   auto definition_request() -> void;

 /// 找引用（入口快捷键 Shift+F12 用）。
   auto references_request() -> void;

 /// 跳到声明（入口快捷键 Alt+F12 用）。
   auto declaration_request() -> void;

 /// 跳到实现（入口快捷键 Ctrl+F12 用）。
   auto implementation_request() -> void;

 /// 返回上一处跳转（入口快捷键 Alt+Left 用）。
   auto navigate_back_request() -> void;

 /// 悬停信息（入口快捷键 Ctrl+K 用；鼠标悬停走编辑器回调）。
   auto hover_request() -> void;

 /// 格式化整篇（入口快捷键 Shift+Alt+F 用）。
   auto format_request() -> void;

 /// 重命名符号（入口快捷键 F2 用）。
   auto rename_request() -> void;

 /// 刷新 Git 状态（入口快捷键用）。
   auto refresh_git_request() -> void;

 /// 跳到第 index 处问题（入口快捷键用）。
   auto jump_to_problem_request(std::size_t index) -> void;


  /// 扫描工作区内某层目录（`rel` 为相对根的路径；空 = 根），带缓存。
   [[nodiscard]] auto list_workspace_dir(const std::string& rel) const -> const std::vector<DirEntryLite>&;


  /// 工作区树：根 + 已展开目录的子项（展开状态由 `dir_expanded_` 表达）。
  ///
  /// key 口径统一为**相对工作区根的路径**（`dir:src/util/deep`）——目录行与
  /// 文件行同一套派生规则，任何层级的展开/收起都只与这个相对路径打交道。
  /// 旧实现只在插入 depth==1 时重写一次 key，第三层的目录名拼出错误 key、
  /// 与 `dir_expanded_` 永不匹配——「只能展开两级」的根因（实测：点 src 能展开、
  /// 点 util 毫无反应）。改为递归展开 + 同源派生后任意层级都成立。
  ///
  /// 扫描结果按相对路径进 `dir_cache_` 缓存（详见其注释）——本函数每帧被调，
  /// 不能每次都打真文件系统。
   [[nodiscard]] auto workspace_nodes() -> std::vector<TreeNodeData>;


   void on_tree_toggle(const std::string& key, bool expanded);


  /// 递归搜工作区（无工作区时搜内置样例）；结果写进 `search_hits_`。
  ///
  /// 改自原先的“只找一个词”：新增 glob 过滤与大小写/全词/正则三个开关，
  /// 并把命中**按文件分组**（同一个文件的命中排在一起，前缀行给文件名）——
  /// 平铺的结果在几十条之后就完全不可用了（看不出哪几行属于同一个文件）。
   void run_search(const std::string& query);


  /// 真正执行搜索（UI 里的入口与终端 `find` 共用）。
   void apply_search(const std::string& query);


   [[nodiscard]] static auto to_lower(std::string_view text) -> std::string;

   [[nodiscard]] static auto is_word_char(char c) -> bool;


  /// 小型正则匹配器（支持子集见 `apply_search` 的说明）；返回首个匹配的**起始列**。
  ///
  /// 匹配策略：逐起点尝试 + 回溯（模式长度有限、行长度在几千字节量级），
  /// 搜索场景完全够用——不会像完整引擎那样再引一个依赖。
   [[nodiscard]] static auto match_regex(std::string_view pattern, std::string_view text) -> std::optional<std::size_t>;


  // —— Git：真调 `git` 子进程（只读子命令）——
  //
  // 为什么值得接：SCM 面板若只是写死一个 “main” 分支名与 “0 变更”，它就不是信息
  // 而是装饰。真读 `git status --porcelain=v1 -b` 之后，改一个文件、刷新，面板会跟着变。
   void refresh_git();


  /// 开关切一下：开→下一帧取数；关→清掉装饰（界面立刻回到干净态）。
   void toggle_blame();


  /// 打开“当前文件的历史”弹窗（`read_file_history` = `git log --follow`，跨重命名）。
  ///
  /// 为何入口放在**编辑器右键菜单**：它对标的动作是“这个文件改过什么”，而光标就在
  /// 这个文件里；放在日志视图上会变成“选中那个提交的历史”，语义不同。
   void open_file_history();


  /// 帧首泵：取历史（与 `ensure_diff` 同一理由：渲染期起子进程会崩）。
   void ensure_file_history();


  /// 文件历史弹窗（`Dialog` overlay；条目点一下 → 打开该提交对本文件的差异）。
   void build_file_history(Composer& c);


  /// 打开“该提交对本文件”的差异（`<hash>^` ↔ `<hash>`）。
   void open_history_entry(const gbcode::LogEntry& entry);


  /// 拉取视口行的 blame（帧首泵做：`git blame` 是子进程，渲染期起会崩）。
  ///
  /// **只问没问过的行段**：用户滚动时每帧都会想要新行，而 `git blame` 一次几百毫秒；
  /// 已取过的段记在 `blame_fetched_` 里，避免同一段反复起进程。
   void ensure_blame();


  /// 把已拿到的 blame 拼进编辑器装饰。
  ///
  /// 装饰是**整体替换**语义，所以这里只回答“当前该长什么样”。
  /// 与诊断/差异共存时由调用方决定优先级——blame 只在开关打开时写，
  /// 而那个开关默认关，所以不会把别的装饰意外抹掉。
   void apply_blame_decorations();


  /// Git 工具窗的数据读取（帧首泵做——渲染期起子进程会崩，见 build_git_view 注释）。
   void ensure_git_log();


  /// 比较视图的数据读取（帧首泵；与 `ensure_git_log` 同一理由：渲染期起子进程会崩）。
   void ensure_compare();


  /// 并列差异的数据读取（帧首泵）：读**两侧文本**后用 `text::diff` 在本地算结构。
  ///
  /// 为何不直接吃 `git diff` 的 patch 文本：结构化差异要的是**行号映射与两栏对齐**，
  /// 两者都靠 `DiffLine.old_line/new_line`。统一到“取文本 + 本地比”只剩一条路，
  /// 而且将来做“与 HEAD 比当前未保存内容”时不用再换实现。
   void ensure_diff();


  /// 打开一个**并列差异**标签（`options` 描述两端；`path` 是文件）。
  ///
  /// 与旧版“把 `git diff` 敲进只读标签”的区别：这里是**结构**（两栏对齐、双行号、
  /// 可一个差异一个差异地跳），而旧的只是一块带颜色的文本。
  /// 三方合并的数据读取（帧首泵）：取 `:1:/:2:/:3:` 三仝内容并划分区段。
  ///
  /// 为何在帧首泵而不是渲染期：要起 `git` 子进程（`read_conflict_sides`），
  /// 而声明式重组过程中起子进程会把 UI 卡住（与差异同理）。
   void ensure_merge();


  /// 帧首落盘：合并选择变了就把结果写回文件（见 `merge_write_pending_` 的说明）。
  ///
  /// ⚠ 必须从**当前帧的视图实例**取结果（`merge_view_ptr`）——它带着用户刚做的选择；
  /// 而 `merge_plan_` 只是三份内容。视图不在树上时（刚关掉标签）什么都不做。
   void flush_merge_write();


   void open_diff(std::string title, const gbcode::CompareOptions& options, std::string path);


  /// 打开一个**三方合并**标签（`base/ours/theirs` 三栏 + 逐段取舍）。
  ///
  /// 与差异视图并列、同样住在编辑区（见 `build` 里的互斥顺序）。
   void open_merge(std::string path);


  /// 把冲突文件的合结果写回磁盘（经撤销栈与行尾保持——见 `CodeEditor` 的写盘路径）。
  ///
  /// **必须经 `run_write` 之外的路径**：这不是 git 写操作，是普通的文件保存
  ///（内容写完后由用户自己 `git add`，那一步再走 git 通道——两件事分开，
  /// 出问题时能看出是哪一步错）。
   void apply_merge_result(const std::string& text);


  /// 逐行暂存：把差异视图里勾选的行构造成补丁并提交到暂存区。
  ///
  /// ⚠ 两个容易错的地方，都在这里处理：
  /// ① **补丁从视图现取**（`diff_view_ptr->build_stage_patch()`）——它在选择变化后
  ///    才准，缓存会拿到过期的选择；
  /// ② 补丁为空时**如实报状态**（`build_partial_patch` 在"只选了上下文行"时返回空，
  ///    而那正是"选了但不产生改动"的情形，硬发一条命令会让 git 报无关的错）。
   void stage_selected_diff_lines(bool unstage = false);


   void unstage_selected_diff_lines();


  /// —— 写操作的**唯一入口** ——
  ///
  /// 全部写操作都从这里走，原因有三：
  /// ① 命令由 `git_service::write_args` **构造**（参数数组），不在这里拼字符串
  ///    ——拼错了会“看着对、实际改了别的文件”；
  /// ② 不可逆操作先建**备份分支**（`Danger::Backup`），出事能手动回滚；
  /// ③ 无论成败都记**审计**（`.git/st-audit.jsonl`）——事后唯一的可查证据。
  ///
  /// 执行仍是终端作业通道（输出实时回流、可中止），与旧版一致。
  /// 返回是否真的发出了命令（参数不合法时为 false，并如实报状态）。
   bool run_write(const gbcode::WriteIntent& intent, std::string_view label = {});


  /// 暂存/取消暂存**整个分组**（分组级批量：未暂存/未跟踪 → 暂存；已暂存 → 取消）。
  ///
  /// 为什么按分组而不管目录树：目录树要按“选中路径集”逐个发命令，
  /// 而这一层的真实需求是“这一组我全要/全不要”——把它做成一次命令，
  /// 用户在终端里看到的也是一条完整命令（而不是一串）。
   void stage_group(gbcode::ChangeGroup group, bool stage);


  /// 放弃**全部**未暂存改动（危险：要确认，且会自动建备份分支）。
   void ask_discard_all();


  /// 提交并推送。
  ///
  /// 分成**两条命令**（提交、推送）而不拼成 `git commit && git push`：
  /// 提交失败时不该推送（`&&` 天然保证），但拼成一行后审计里只剩一条
  /// “commit && push”——出问题时看不出是哪一步失败。
   void commit_and_push();


  /// 暂存/取消暂存一个文件（写操作：交终端作业通道——输出实时回流、可中止）。
   void git_stage(const std::string& path, bool stage);


  /// 放弃一个文件的更改（危险操作：先确认——`git restore`/`checkout` 不可恢复）。
   void git_discard(const gbcode::GitChange& change);


  /// 提交（提交框 Enter）。
  ///
  /// 走终端而非同步 `process::run` 的理由与 `run_task` 相同：同步阻塞会把
  /// 界面卡住（大仓库提交能到秒级），而终端通道输出实时回流、可中止。
  ///
  /// **不再 `git add -A` 再提交**（旧版的行为）：那会把用户没勾选的文件一并提交。
  /// 现在“暂存什么就提交什么”——没暂存任何东西时才回退到“全提交”并**在状态栏说清**。
   void git_commit();


  /// 展开一个变更文件：与 HEAD 的差异**跑进终端**（只读 `git diff`）。
  ///
  /// 为何不再开一个“输出面板”：差异是**命令的输出**，而命令的输出去处只有一个。
  /// 有两处就会产生“同一个 `git diff` 一会儿在这里、一会在那里”的困惑。
  // —— 运行任务：真跑 `st`（走终端的作业通道，输出实时回流）——
  //
  // 为何搬到终端后面：以前这里是同步 `st::process::run` + 写 `output_`，
  // 于是跑 `st test` 的几十秒里界面假死、看不到任何进展。现在它只是
  // “把命令交给终端”——输出逐行出现，中止按钮真的能杀进程。
   void run_task(const std::string& id);


  /// 枚举运行时已注册的语言（扩展视图）。
   void refresh_languages();


  /// 重新扫描工作区树（资源管理器的刷新按钮）。
   void refresh_tree();


  /// **换工作区**（「打开文件夹…」的落地动作）。
  ///
  /// 语义（与 VSCode 一致的三条）：
  /// * **已打开的文件标签保留**——换工作区是换“资源树的根”，不是关项目；
  ///   用户常要“在新目录里继续看刚编辑的那个文件”，标签全关掉是丢工作。
  /// * 资源树换成新目录并**重置展开状态**（旧展开路径对新根无意义）。
  /// * 标题栏/状态栏跟着换，让用户确认换成功了。
  ///
  /// 校验：目标必须是**存在的目录**（写到状态栏而不是弹窗——与 `close_all` 的姿态一致，
  /// 不打断手头的事）。
   void switch_workspace(const std::string& path);


  /// 新建文件：有工作区时开文件对话框选路径；无工作区时开一份内存缓冲（`untitled-N`）。
  ///
  /// 为何两档都要：内置样例模式的演示价值就在于“不用开文件对话框也能跑起来”，
  /// 而如果无工作区时只回一句“需要 --workspace”，那“新建文件”这个入口在演示里就是死的。
   void new_file_prompt();


  /// 把树里的 key 换成磁盘绝对路径。
  ///
  /// 两种 key 口径（见 `workspace_nodes`）：
  /// - 目录行 `dir:<相对路径>`（相对于工作区根）；
  /// - 文件行**已经是绝对路径**（历史契约：`open_path(key)` 直接当路径用）。
  ///
  /// 初版把两种都当相对路径拼，于是文件行的目标变成 `工作区/工作区/.../file`
  /// （实测：菜单显示的删除目标路径重复了两遍）。归一化就在这里做**一处**——
  /// 所有需要绝对路径的地方都调它，不再各自拼。
   [[nodiscard]] auto tree_key_to_path(std::string_view key) const -> std::string;


  /// 新建条目的目标目录：目录行取它自己，文件行取它的父目录，空 key 取工作区根。
   [[nodiscard]] auto context_directory() const -> std::string;


  /// 在“右键处的目录”下新建一个条目（文件或目录），命名用浮层输入。
   void explorer_create(bool directory);


  /// 重命名右键那一行（文件或目录）。
   void explorer_rename();


  /// 删除右键那一行（**不可恢复**，先确认）。
   void explorer_delete();


  /// 执行已确认的文件系统动作（浮层提交/确认后调用）。
  ///
  /// `name` 只在新建/重命名时用（删除走确认，不看名字）。
   void submit_fs_action(std::string_view name);


  /// 资源管理器右键菜单（与编辑器的右键菜单**同一形态**）。
  ///
  /// 为何不复用 `context_open_`：两个菜单的锚点与条目完全不同，共享一个开关会让
  /// “在编辑器里右键”与“在树里右键”互相触发对方的菜单。
   void build_explorer_context(Composer& c);


  // —— 搜索命中的跳转（打开文件 + 选中命中）——
  /// 点搜索命中 → 打开文件并跳到那一行 + 选中命中词。
   void activate_search_hit(std::size_t index);


   void apply_jump(std::size_t line, std::size_t column);


  /// 每帧泵一次终端（应用主循环调；见 `pump()`）。
   void pump_terminal();


  /// 退出前收尾：中止还在跑的会话（否则子进程会变成孤儿）。
   void shutdown_terminal();


  /// 退出前收尾：关掉语言服务器（否则 clangd 会变成孤儿进程，占着索引缓存）。
   void shutdown_language_service();


  /// 手动触发补全（Ctrl+Space）。
   void trigger_completion();


  /// 发补全请求（`text` 用于算"光标前的词"作为兜底替换区间）。
   void request_completion_now(std::string_view text);


  /// 更新弹层锚点（光标矩形；弹层贴它下方）。
   void update_completion_anchor();


  /// 构建补全弹层（overlay；条件声明）。
   void build_completion_popup(Composer& c);


  /// 接受补全：用 `insert` 替换光标前的词（或已输入前缀）。
   void accept_completion(const std::string& insert);


  /// 补全响应处理（挂在 `lsp_.on_completion`）。
   void on_completion_response(std::int64_t request_id, const std::vector<st::lsp::CompletionEntry>& entries, bool);


  /// 补全详情响应。
   void on_completion_detail_response(std::int64_t, const std::string& detail);


  /// 当前活动文件的可读信息（路径 + 光标位置）。无则返回 false。
   auto current_document(std::string* path, st::lsp::Position* position) -> bool;


  /// 是否处于"可跳转"状态（供状态栏/命令面板判断可用性）。
   [[nodiscard]] auto navigation_ready() const -> bool;


  /// F12：跳到定义。
   void goto_definition();


  /// 导航族通用发射器：声明 / 类型定义 / 实现（与 F12 同一套前置校验）。
  ///
  /// 单独一个函数而不是三份拷贝：前置校验与状态栏文案的口径必须一致，
  /// 复制三份迟早会漂（改一处忘两处）。
   void goto_navigation_family(const char* family, const char* label);


  /// 跳到声明（C/C++：.cpp 用 F12 常落头文件声明；这里显式请求声明族）。
   void goto_declaration();

  /// 跳到类型定义（光标在变量上时跳它的类型声明处）。
   void goto_type_definition();

  /// 跳到实现（Ctrl+F12；接口→实现）。
   void goto_implementation();


  /// Shift+F12：找引用。
   void find_references();


  /// 悬停（鼠标停留）：请求当前位置的信息。
   void request_hover_at_cursor();


  /// 文档大纲：把符号列表放进 `symbols_`（供面板/命令面板显示）。
   void refresh_document_symbols();


  /// 工作区符号搜索（命令面板输入时调）。
   void search_workspace_symbols(const std::string& query);


  /// 跳到某个位置（定义/引用结果共用）：必要时先打开文件。
  ///
  /// 跳转前把「当前位置」压入历史栈（Alt+Left 回退用）——只在真发生文件/行
  /// 变化时压栈，重复跳同一处不堆积。
   void jump_to_location(const st::lsp::Location& location);


  /// 跳转历史回退（Alt+Left）：回到上一处跳转起点。
   void navigate_back();


  /// 导航结果面板里点某一条：跳过去（同一套 `jump_to_location`，历史同样入栈）。
   void open_nav_result(std::size_t index);


  /// 当前活动缓冲的路径（无则空）。
   [[nodiscard]] auto current_buffer_path() const -> std::string;


  /// 位置结果 → 状态栏摘要（带计数，让用户知道"找到几处"）。
  ///
  /// 多处结果：除跳到第一处外，还把「位置列表」写进侧栏搜索视图可复用的
  /// 结果面板状态（`nav_results_`），切到活动栏「导航结果」可逐个点。
   void on_locations_response(std::int64_t id, const std::vector<st::lsp::Location>& locations);


  /// 悬停响应：显示到编辑器的信息层（贴光标行下方）。
   void on_hover_response(std::int64_t id, const std::optional<st::lsp::HoverInfo>& info);


  /// 大纲响应。
   void on_symbols_response(std::int64_t, const std::vector<st::lsp::DocumentSymbol>& symbols);


  /// 工作区符号响应：填进 `workspace_symbols_`，供「转到工作区符号」面板用。
  ///
  /// 为何要开面板才能看：LSP 的响应是**异步**的（server 扫完整个工作区才回），
  /// 所以固定是“先开面板（空列表）→ 响应回来 → 重组把结果填进去”。
   void on_workspace_symbols_response(std::int64_t, const std::vector<st::lsp::WorkspaceSymbol>& symbols);


  /// 「转到符号」面板的列表项：定义（当前文件大纲）/ 工作区符号看 `symbols_kind_`。
  ///
  /// 两类符号的形状不同（`DocumentSymbol` 带 range/selection_range、可能嵌套；
  /// `WorkspaceSymbol` 自带 location），但**面板只要“名字 + 去哪”**：
  /// 在这里归一化成同一种条目，面板侧就只有一条代码路径。
   [[nodiscard]] auto symbol_commands() const -> std::vector<CommandPalette::Command>;


  /// 打开「转到符号」面板（公开面：入口在快捷键/菜单，与本页其它入口一致）。
   void open_symbols(bool document);


  /// 跳到第 `index` 个符号（面板列表下标，与 `symbol_commands` 同一顺序）。
   void jump_to_symbol(std::size_t index);


  /// 构造格式化选项（`tabSize`/`insertSpaces` 必须与编辑器显示口径一致——
  /// 否则 server 严格按我们给的参数做出来的缩进，在编辑器里看起来是错的）。
   [[nodiscard]] auto formatting_options() const -> st::lsp::FormattingOptions;


  /// 格式化整篇（Shift+Alt+F，VSCode 同键位）。
   void format_document();


  /// 格式化选中区（有选区时走这条，否则整篇）。
   void format_selection();


  /// 格式化结果落地：应用到**当前缓冲**（格式化只针对一个文件）。
   void on_format_response(std::int64_t id, const std::vector<st::lsp::TextEdit>& edits);


  /// 重命名符号（F2，VSCode 同键位）：先 prepareRename 拿当前名字。
   void rename_symbol();


  /// 预备结果回来了：打开输入浮层（预填当前名字）。
   void on_prepare_rename_response(std::int64_t, const std::optional<st::lsp::PrepareRenameResult>& prepared);


  /// 提交重命名（输入浮层 Enter）。
   void submit_rename();


  /// 重命名结果：**`WorkspaceEdit` 可能跨文件**——逐个文件应用。
   void on_rename_response(std::int64_t, const st::lsp::WorkspaceEdit& edit);


  /// 重命名输入浮层的聚焦（与其它输入浮层同一套路）。
   void apply_pending_rename_focus();


  /// 防抖后的悬停处理（每帧调；停够 400ms 才发请求——移动中不闪跳）。
   void pump_hover_word();


  /// 编辑区按键：补全交互（Enter 接受 / Esc 关闭 / 上下选择）优先于编辑器默认。
  /// 返回 true = 已消费。
   auto handle_completion_key(std::string_view key) -> bool;


  /// 请求中止当前会话（面板按钮）。
   void request_terminal_stop();


  /// 命令面板执行一条命令（id 是稳定的业务身份）。
   void run_command(const std::string& id);


  /// 全部命令表（Ctrl+Shift+P）。
   [[nodiscard]] auto command_table() -> std::vector<CommandPalette::Command>;


  /// 快速打开的文件表（Ctrl+P）：有工作区时列真实文件（递归、限量）。
   [[nodiscard]] auto file_commands() const -> std::vector<CommandPalette::Command>;


  // ————————————————————————————————————————————————————————————————————————
  // `MenuHeader`：长在标题栏 `leading` 槽里的那一小块（菜单栏 + 它的下拉面板）
  // ————————————————————————————————————————————————————————————————————————
  //
  // 挂载点：标题栏的 `leading` 槽（`[leading…][标题][trailing…][控制按钮]`）。
  // 为什么用 `leading`（而不是 `trailing`）：这一版的产品形态是**菜单在左、标题跟在其后**
  // （省下一整行 32px 给内容区，窗口顶部的拖动/双击语义仍归标题栏）。
  // 挂在 `leading` 后 `TitleBar::arrange` 会自动做三件事：标题**让到菜单右侧**、
  // 图标让位给槽（两者都画会叠在一起）、`drag_rect` 不再把这一行算成拖动区。
  //
  // 为什么单独成一个 `Component` 而不是让本页 `build()` 多画一块：标题栏是
  // **外壳**（`WindowFrame` 自持、在声明树之外），本页的 `build()` 只管内容槽。
  // `mount_into` 正好表达「声明式的一小块挂到既有元素的槽位」——于是整个 IDE
  // 仍是一个 `Component`（只是它现在占两处树位），菜单与标题栏合并成一行。
  //
  // 它只借宿主两样东西：`menu_open_`（打开的是哪一个，本页也要读它做快捷键）
  // 与 `on_menu`（选条目）。菜单数据与面板 overlay 都归它自己。

  struct MenuHeader : Component {
    CodeEditorPage* page{nullptr};
    MenuBar* bar{nullptr};

      void build(Composer& c) override {
    // 菜单表：静态骨架 + **动态「最近打开」小节**（2026-10-12）。
    //
    // 旧实现是 `static const`——那对纯静态菜单没问题，但「最近打开的文件」
    // 要跟着会话变。改为每次 build 重建并比对指纹：变了才 `set_menus`
    //（它会 mark_layout_dirty 且重置 open_index_，每帧重设会把打开态抹掉）；
    // 不变则什么都不做（菜单栏在标题栏那棵小树上，但也没必要白脏）。
    //
    // 「最近」为什么不用子菜单：`MenuPanel` 的 children 只画箭头、没有交互
    // 实现（见 menu.cpp）——平铺在「文件」尾部与 VSCode 桌面版同形。
    const auto build_menus = [this] {
      std::vector<MenuData> menus = {
          {"file", "文件",
           {{.id = "new", .label = "新建文件"},
            {.id = "open", .label = "打开文件…"},
            {.id = "open-folder", .label = "打开文件夹…（Ctrl+Shift+O）"},
            {.id = "save-as", .label = "另存为…"},
            {.separator = true},
            {.id = "save", .label = "保存（Ctrl+S）"},
            {.id = "save-all", .label = "全部保存（Ctrl+K S）"},
            {.separator = true},
            {.id = "close-tab", .label = "关闭编辑器（Ctrl+W）"},
            {.id = "close-all", .label = "关闭全部编辑器"}}},
          {"edit", "编辑",
           {{.id = "undo", .label = "撤销"},
            {.id = "redo", .label = "重做"},
            {.separator = true},
            {.id = "comment", .label = "切换行注释（Ctrl+/）"},
            {.id = "select-all", .label = "全选"}}},
          {"selection", "选择",
           {{.id = "select-all", .label = "全选（Ctrl+A）"},
            {.id = "goto-line", .label = "转到行…"},
            {.id = "goto-symbol", .label = "转到当前文件符号…"},
            {.id = "goto-workspace-symbol", .label = "转到工作区符号…（Ctrl+T）"},
            {.id = "copy", .label = "复制"},
            {.id = "paste", .label = "粘贴"}}},
          {"view", "查看",
           {{.id = "command-palette", .label = "命令面板…（Ctrl+Shift+P）"},
            {.separator = true},
            {.id = "toggle-sidebar", .label = "切换侧栏可见性（Ctrl+B）"},
            {.id = "find", .label = "查找（Ctrl+F）"},
            {.id = "toggle-terminal", .label = "切换底部面板（Ctrl+J）"},
            {.id = "toggle-theme", .label = "切换亮/暗主题"}}},
          {"run", "运行",
           {{.id = "run-task", .label = "运行任务：构建 gallery"},
            {.id = "run-test", .label = "运行任务：st test"},
            {.id = "run-lint", .label = "运行任务：st lint"},
            {.separator = true},
            {.id = "toggle-terminal", .label = "切换终端（底部面板）"}}},
          {"help", "帮助",
           {{.id = "about", .label = "关于 歌白代码"},
            {.id = "shortcuts", .label = "键盘快捷键…"}}}};
      // 「文件」尾部：最近打开的文件夹 + 文件（各自 MRU 前 5——菜单不是清单页，
      // 全放 10 条会把面板拉得老长；命令面板里才是完整 10 条）。
      // label 用「文件名（父目录名）」：同名文件靠目录名区分，绝对路径太长。
      auto& file_items = menus.front().items;
      const auto leaf_of = [](const std::string& path) {
        const std::size_t slash = path.find_last_of("/\\");
        return slash == std::string::npos ? path : path.substr(slash + 1);
      };
      const auto parent_of = [&leaf_of](const std::string& path) {
        const std::string parent = st::fs::parent(path);
        return parent.empty() ? std::string("…") : leaf_of(parent);
      };
      if (page != nullptr && !page->recent_folders_.empty()) {
        MenuItemData separator;
        separator.separator = true;
        file_items.push_back(separator);
        std::size_t shown = 0;
        for (const auto& folder : page->recent_folders_) {
          if (shown++ >= 5) break;
          MenuItemData entry;
          entry.id = std::format("recent-folder:{}", folder);
          entry.label = "打开文件夹：" + leaf_of(folder);
          file_items.push_back(std::move(entry));
        }
      }
      if (page != nullptr && !page->recent_files_.empty()) {
        MenuItemData separator;
        separator.separator = true;
        file_items.push_back(separator);
        std::size_t shown = 0;
        for (const auto& file : page->recent_files_) {
          if (shown++ >= 5) break;
          MenuItemData entry;
          entry.id = std::format("recent-file:{}", file);
          entry.label = leaf_of(file) + "（" + parent_of(file) + "）";
          file_items.push_back(std::move(entry));
        }
      }
      return menus;
    };
    // 指纹 = 菜单 id 拼接（label 由 id 推导，id 稳定即整体稳定）。
    std::string fingerprint;
    for (const auto& menu : build_menus()) {
      for (const auto& item : menu.items) fingerprint += item.id + "|";
    }
    if (fingerprint != menus_fingerprint_) {
      menus_ = build_menus();
      menus_fingerprint_ = fingerprint;
    }
    bar = dsl::menu_bar(
        c, menus_,
        [this](const std::string& menu, const std::string& item) { page->on_menu(menu, item); },
        [this](std::size_t index) {
          page->menu_open_.set(page->menu_open_.value() == index ? kNoMenu : index);
        },
        {.id = "menubar"});
    // 打开状态 → 下一帧声明面板 overlay（不声明 = 自动消失，框架 sweep）
    const std::size_t open = page->menu_open_.value();
    if (open != kNoMenu && bar != nullptr) dsl::menu_panel_overlay(c, *bar, open);
  }

  /// 上一次推给菜单栏的数据（指纹未变时不重设 `set_menus`，见 build 内说明）。
  std::vector<MenuData> menus_{};
  std::string menus_fingerprint_{};
  };


 public:
  // —— 供入口与主循环使用 ——
  /// 编辑器字号档位（构造时定，`--editor-font-scale` 可覆盖）。
  /// 声明在 `files_` 之前：初始化顺序按**声明序**，否则 -Werror=reorder 直接编译失败。
  float editor_font_scale_{kEditorFontScale};
  /// 编辑器行距倍数（构造时定，`--editor-line-spacing` 可覆盖）。
  float editor_line_spacing_{CodeEditor::kDefaultLineSpacing};
  /// 霜天仓库根（`--tool-root`）：PATH 上没有 `st` 时到它下面 `build/bin/st[.exe]` 找。
  std::string tool_root_{};
  std::vector<Sample> files_{};
  std::string workspace_{};
  std::vector<Problem> problems_{};
  std::pair<std::size_t, std::size_t> pending_jump_{0, 0};
  /// 新建/打开/另存对话框开关（三者共用一个 `FileDialog`）。
  State<bool> new_file_open_{false};
  /// 缩进宽度（状态栏可点切换；配到编辑器上一次）。
  int tab_width_{4};
  /// 0 = 打开，1 = 另存为，2 = 新建。
  /// 3 = **打开文件夹**（换工作区）：同一条对话框链路，只是用 `Mode::Directory`。
  int pending_open_mode_{0};
  /// 内存缓冲的递增序号（`untitled-N`）。
  std::size_t untitled_count_{1};

 private:

  /// 目录扫描缓存：相对路径 → 该层条目（`list_workspace_dir` 的背书）。
  ///
  /// 为什么需要它：`workspace_nodes()` 每帧被调（声明式重组），旧实现每帧对
  /// 每个可见目录打一次 `list_dir` 系统调用——树稍大一点，帧预算就被 IO 吃掉
  ///（实测 60fps 下点开几个目录后 CPU 明显上扬）。目录内容在以下时机失效：
  /// 刷新按钮（`refresh_tree`）、收起目录（重展开时重扫）、换工作区、
  /// 新建/删除/重命名文件。变更不频繁，缓存命中率极高。
  mutable std::map<std::string, std::vector<DirEntryLite>, std::less<>> dir_cache_{};
  std::string loaded_key_{};   ///< 编辑器实例当前装载的是哪个标签（防重复 set_text）
  /// 语言服务（LSP）：懒启动、按扩展名挑 server；无 server 的语言**如实不启用**。
  gbcode::LanguageService lsp_{};
  /// 上次推给编辑器的诊断指纹（避免每帧重设——`set_diagnostics` 会标脏整页）。
  std::size_t lsp_diagnostics_stamp_{0};
  /// 补全弹层是否打开（声明式条件；关掉 = 本帧不声明 → 框架摘除）。
  ///
  /// ⚠ **必须是 `State`**：普通 `bool` 改了不会触发声明式重组，overlay 永远不出现
  ///（实测：请求发出、clangd 回了 100 项候选、`on_completion_response` 也跑了，
  ///  就是弹层不出现——因为没有任何东西标脏整页）。诊断途中我顺手加的
  /// `status_.set(...)`（它本身是 State）**意外地**让它工作了，这才定位到根因。
  State<bool> completion_open_{false};
  /// 已发起的补全请求 id（响应回来时核对；旧响应丢弃——输入变了旧的就没意义）。
  std::int64_t completion_request_{0};
  /// 补全触发时记录"替换区间起点"（未给 textEdit 时用它替换已输入的词）。
  std::size_t completion_word_begin_{0};
  /// 补全候选（保留在应用侧，供弹层声明与 resolve 用）。
  std::vector<st::lsp::CompletionEntry> completion_items_{};
  /// 补全弹层的锚点（光标矩形；打开时算一次）。
  st::math::Rect completion_anchor_{};
  /// 语言服务回调是否已接线（只挂一次——重复挂会让回调互相覆盖）。
  bool language_callbacks_wired_{false};
  // —— 导航（阶段 5）的运行时状态 ——
  /// 悬停请求 id（丢弃旧响应；鼠标移动会连发好几个）。
  std::int64_t hover_request_{0};
  /// 悬停防抖：词区间（0,0 = 无）与变化时间戳。
  std::pair<std::size_t, std::size_t> hover_word_pending_{0, 0};
  std::pair<std::size_t, std::size_t> hover_word_sent_{0, 0};
  std::int64_t hover_word_changed_at_{0};
  /// 悬停请求时的行号（响应没给 range 时的兜底）。
  std::uint32_t hover_word_line_{0};
  /// 本次响应是否可显示（词已变时丢弃）。
  bool hover_dropped_{false};
  /// 最近一次"跳到定义/引用"的多处结果（结果面板用）。
  std::vector<st::lsp::Location> locations_{};

  // —— 导航结果面板 + 跳转历史（本次完善）——
  /// 导航结果的一条（多处定义/引用的可点行）。
  struct NavResult {
    std::string path{};        ///< 磁盘路径
    std::size_t line{1};       ///< 1 起
    std::size_t column{1};     ///< 1 起
    std::string preview{};     ///< 目标行文本预览（120 字截断）
    st::lsp::Location location{};  ///< 原始位置（点击跳转用）
  };
  /// 多处导航结果（定义/引用/实现共用；空 = 面板关）。
  std::vector<NavResult> nav_results_{};
  /// 导航结果面板开合（State：变更要触发重组）。
  State<bool> nav_results_open_{false};
  /// 结果面板行 → `nav_results_` 下标（分组行占位 -1；`build_nav_results_view` 维护）。
  std::vector<std::size_t> nav_row_to_result_{};
  /// 跳转历史的一个点（Alt+← 返回）。
  struct NavPoint {
    std::string path{};
    std::size_t cursor{0};
    std::size_t line{0};     ///< 0 起（与编辑器 cursor_line 同口径）
  };
  /// 跳转历史栈（有界 64；重复跳同一处不压栈）。
  std::vector<NavPoint> nav_history_{};
  /// 当前文件的符号大纲。
  std::vector<st::lsp::DocumentSymbol> symbols_{};
  /// 大纲对应的文件路径（切文件后作废）。
  std::string symbols_path_{};
  /// 工作区符号搜索结果。
  std::vector<st::lsp::WorkspaceSymbol> workspace_symbols_{};
  /// 待落地的跳转（跨文件跳转要等编辑器装载；与既有 `pending_jump_` 同一套路）。
  bool pending_jump_pending_{false};
  // —— 编辑类动作（阶段 7）——
  /// 格式化请求 id（旧响应丢弃）。
  std::int64_t format_request_{0};
  /// 重命名输入浮层是否打开。
  State<bool> rename_open_{false};
  /// 重命名输入框的当前文本。
  State<std::string> rename_text_{};
  /// `prepareRename` 给的当前名字（预填用）。
  std::string rename_prefill_{};
  /// 重命名目标文件路径（提交时用；光标可能已动）。
  std::string rename_path_{};
  /// 预填是否已写入输入框（只写一次，否则每帧覆盖用户输入）。
  bool rename_field_seeded_{false};
  /// 重命名输入浮层的输入框指针（按钮提交时读）。
  Input* rename_input{nullptr};
  /// 重命名浮层的聚焦待办（浮层内的输入框下一帧才建好）。
  bool rename_focus_pending_{false};
  /// 侧栏分栏当前宽度比例（与 `bottom_ratio_` 同一套模式，见其注释）：
  /// 拖动回调只写这个非响应式成员，跨重组记住用户拖到哪了。
  /// 口径：`SplitView::ratio` = 首面板（侧栏）的占比。
  float sidebar_ratio_{0.22f};
  /// 底部面板当前高度比例（上下分栏的 `ratio`）。
  ///
  /// **刻意不是 `State`**：分栏组件自己持有比例并据此重排，拖动时它逐像素回调本值——
  /// 若这是个 State，每拖一像素就会标脏整页重组一次（拖拽中重建子元素，手感与开销都差）。
  /// 这里只做“跨重组记住用户拖到哪了”的存档。
  ///
  /// ⚠ 这是 **`SplitView::ratio` 的口径 = 第一面板（编辑器）的占比**，不是终端的份额。
  /// 实测踩到过反直觉的错误：把“想让终端占多”写成把比值改大（0.72 → 0.60），
  /// 结果编辑器反而吃掉了六成高度。
  ///
  /// 把 0.72 → **0.55**：原先终端只拿到 28%（约 6 行）——真终端 6 行不可用
  ///（`dir`/`git log` 一屏就满，连提示符都要滚）。现在终端约 295px ≈ 14 行。
  float bottom_ratio_{0.55f};

};

}  // namespace gbcode_ui
