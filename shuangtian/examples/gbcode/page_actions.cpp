// 动作族：文件打开/保存/关闭/切换、帧首泵、git 读写、blame、差异/合并、
// 搜索、资源管理器文件操作、LSP 补全/导航/格式化/重命名、终端作业、命令表。
// 拆分自 main.cpp，零逻辑改动——只是 out-of-line 化。

#include <algorithm>
#include <cstdio>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "page.hpp"
#include "st/core/fs.hpp"
#include "st/core/hash.hpp"
#include "st/core/print.hpp"
#include "st/core/string.hpp"
#include "st/core/time.hpp"
#include "st/core/log.hpp"

namespace gbcode_ui {

  namespace {
    /// 问题指纹计算里「无活动缓冲」时的空缓冲占位（避免分支里到处判空）。
    const OpenBuffer kNoBuffer{};
  }

  std::function<void(ThemeMode)> theme_setter{};

  // —— 会话（最近打开 + 关窗恢复）——

  void CodeEditorPage::open_recent(const std::string& path) {
    // 「最近」条目的入口：文件可能已被删/被移——如实报告，不静默。
    if (!st::fs::exists(path)) {
      status_.set("文件已不存在：" + path + "（从「最近」移除）");
      std::erase(recent_files_, path);
      return;
    }
    open_path(path);
  }

  auto CodeEditorPage::restore_session(const gbcode::SessionState& state) -> bool {
    // 工作区：走既有链路（换树根/重置展开/LSP 根），与「打开文件夹」同一条路。
    if (!state.workspace.empty() && st::fs::is_directory(state.workspace)) {
      switch_workspace(state.workspace);
    }
    // 标签：逐个打开（`open_path` 自带去重与读盘失败处理）；只收**磁盘上还在**的。
    std::size_t opened = 0;
    for (const auto& file : state.files) {
      if (!st::fs::is_regular_file(file.path)) continue;
      open_path(file.path);
      ++opened;
    }
    // 活动项：会话里存的是当时**会话文件数组**的下标；文件被删后重开会缺项，
    // 按“路径对回下标”定位（而不是盲目套用旧下标——删一个文件后全部错位）。
    // ⚠ 用 `state.active`（会话存的活动下标），不是 `active_.value()`——后者已被
    // 上面逐个 open_path 改成最后一个（实测踩到：恢复后活动标签错成 b.py）。
    if (opened > 0 && state.active < state.files.size()) {
      const std::string& wanted = state.files[state.active].path;
      const auto list = buffers_.value();
      for (std::size_t index = 0; index < list.size(); ++index) {
        if (list[index].path == wanted) {
          active_.set(index);
          break;
        }
      }
    }
    // 光标：等编辑器装载完再跳（装载在下一帧 `build()` 末尾，现在 `editor` 还旧）。
    // 与导航跳转同一套 pending 机制（`apply_pending_navigation_jump` 消费，
    // 其行号口径是 1 起，见 page_build.cpp）。同样按 `state.active` 取目标文件。
    if (opened > 0 && state.active < state.files.size()) {
      const gbcode::SessionFile& file = state.files[state.active];
      pending_jump_ = {file.line, file.column};
      pending_jump_pending_ = true;
    }
    recent_files_ = state.recent_files;
    recent_folders_ = state.recent_folders;
    return !state.workspace.empty() || opened > 0;
  }

  auto CodeEditorPage::save_session() -> void {
    // 没有工作区就没有可恢复的磁盘身份，不存会话。
    if (workspace_.empty()) return;
    gbcode::SessionState state;
    state.workspace = workspace_;
    // 只存磁盘上的真文件标签；内存缓冲（untitled-N）没有路径，恢复不了。
    // 脏文件只存路径不存内容：内容以磁盘为准——用户若想要那些修改，应该在关窗前
    // 保存；静默替他落盘反而危险（可能写进不该写的版本）。
    auto list = buffers_.value();
    if (editor != nullptr && active_.value() < list.size()) {
      stash_active_text(list);   // 关窗前最后一次把编辑器里的文本收进缓冲（口径与 save 一致）
    }
    std::size_t active_path_index = 0;
    bool active_found = false;
    for (std::size_t index = 0; index < list.size(); ++index) {
      if (list[index].path.empty()) continue;
      gbcode::SessionFile file;
      file.path = list[index].path;
      // 光标：只有当前活动标签能从编辑器读到；其余标签存 1:1（下次从头看）。
      if (index == active_.value() && editor != nullptr) {
        file.line = editor->cursor_line() + 1;
        file.column = editor->cursor_column() + 1;
      }
      if (index == active_.value()) {
        active_found = true;
        active_path_index = state.files.size();   // 指向存进会话后的位置
      }
      state.files.push_back(std::move(file));
    }
    if (active_found) state.active = active_path_index;
    state.recent_files = recent_files_;
    state.recent_folders = recent_folders_;
    const bool saved = gbcode::save_session(session_path_, state);
    // 写状态栏（不是弹窗）：关窗瞬间没人看弹窗，日志里有这条即可。
    if (saved) {
      st::log::out("会话已保存（{} 个标签）: {}\n", state.files.size(), session_path_);
    } else {
      st::log::err("会话保存失败: {}\n", session_path_);
    }
  }




  /// `--open <path>` 的待办：**不在构造期打开**（那会在构建期写 State，见 `main` 的注释），
  /// 而由帧首泵在首帧之后就打开一次。
  void CodeEditorPage::set_pending_open_path(std::string path) { pending_open_path_ = std::move(path); }


  /// 帧首泵：把待打开的路径真的打开（一次）。
  void CodeEditorPage::flush_pending_open() {
    if (pending_open_path_.empty()) return;
    const std::string path = std::move(pending_open_path_);
    pending_open_path_.clear();
    open_path(path);
  }


  /// 打开真实文件（资源树 / 搜索命中 / 快速打开 / 对话框单选与多选共用）。
  auto CodeEditorPage::open_path(const std::string& path) -> void {
    // 路径规范化：不同入口交来的同一文件可能形态不同（Windows 短路径 8.3 名 vs
    // 真名——FileDialog 单选确认交的是目录枚举出的真名，而协议注入的
    // pending_path/pending_paths 可能是短路径形态；不规范化的话去重失配、
    // 同一文件开出两个标签，实测 e2e 复现：多选开短路径标签 → 单选开同一文件
    // → 真名不匹配短路径 → 重复标签）。全部入口在此统一成绝对真名，
    // `read_into_buffers` 的按 path 去重从此只比一种形态。
    const std::string canonical =
        st::fs::absolute(path).value_or(std::string(path));
    auto list = buffers_.value();
    stash_active_text(list);
    const std::size_t index = read_into_buffers(list, canonical);
    if (index >= list.size()) {
      status_.set("打开失败：" + path);
      return;
    }
    gbcode::push_recent(recent_files_, canonical);   // 最近打开（菜单/命令面板/下次恢复共用；与去重同一形态）
    buffers_.set(std::move(list));
    active_.set(index);
    // 打开文件 = 当前要看的内容换成编辑器：关掉差异/合并标签（若有）。
    // 不清的话内容区还在差异视图，而标签栏的活动项却指向新文件——“看着切了、内容没切”。
    if (!diff_key_.value().empty()) {
      diff_key_.set({});
      diff_title_.set({});
      diff_file_.set(st::text::FileDiff{});
    }
    if (!merge_key_.value().empty()) {
      merge_key_.set({});
      merge_view_ptr = nullptr;
    }
    // ⚠ **不在这里碰 `editor`**。
    //
    // 为何：本函数会被**帧首泵**调到（`flush_pending_open`），而那时 `editor`
    // 是**上一次 `build()` 留下的指针**——gbcode 每帧重建编辑器，那个指针已经悬垂，
    // 写它就是空指针写（实测：`--open` 让进程在首帧后直接崩，异常码 0xC0000005）。
    //
    // 正确做法：只把状态改好 + **标记“需要重灌”**（`loaded_key_` 清空），
    // 实际灌入由 `build()` 末尾统一做——那时 `editor` 是本帧刚取得的，一定有效。
    loaded_key_.clear();
    status_.set("已打开 " + path);
  }



  /// 关闭一个标签（按 key——索引会随增删漂移，key 才是业务身份）。
  auto CodeEditorPage::close(std::string_view key) -> void {
    auto list = buffers_.value();
    stash_active_text(list);
    const std::size_t before = list.size();
    std::erase_if(list, [key](const OpenBuffer& buffer) { return buffer.key == key; });
    if (list.size() == before) return;
    std::size_t next = active_.value();
    if (next >= list.size() && !list.empty()) next = list.size() - 1;
    buffers_.set(std::move(list));
    active_.set(list.empty() ? 0 : next);
    if (editor != nullptr && !list.empty()) {
      editor->set_language(list[active_.value()].language);
      editor->set_text(list[active_.value()].text);
    }
    loaded_key_ = list.empty() ? std::string{} : list[active_.value()].key;
    refresh_problems();
    status_.set("已关闭 " + std::string(key));
  }


  /// 关闭当前标签（**走确认路径**：脏标签会先弹对话框）。
  ///
  /// 为何必须绕到 `request_close`：入口有四处关闭（Ctrl+W / 工具栏按钮 / 菜单 / 标签的 ×），
  /// 只有标签的 × 天然走 `request_close`——其余三处若直接调 `close()`，
  /// 就会出现“同一个动作，换个入口就静默丢修改”的不一致（实测踩到）。
  auto CodeEditorPage::close_active() -> void {
    // 特殊标签（差异/合并）当前正在看时优先关它（与内容区互斥顺序一致：
    // 看的是什么就关什么）——不先判会让 Ctrl+W 在差异视图下静默关掉背后的文件标签。
    if (!merge_key_.value().empty()) {
      merge_key_.set({});
      merge_view_ptr = nullptr;
      status_.set("已关闭合并标签");
      return;
    }
    if (!diff_key_.value().empty()) {
      diff_key_.set({});
      diff_title_.set({});
      diff_file_.set(st::text::FileDiff{});
      status_.set("已关闭差异视图");
      return;
    }
    const auto list = buffers_.value();
    if (list.empty()) return;
    request_close(list[active_.value()].key);
  }


  /// 保存当前标签：真实文件走 `fs::write_text`；未命名的缓冲区走“另存为”。
  auto CodeEditorPage::save() -> void {
    auto list = buffers_.value();
    if (list.empty()) {
      status_.set("没有可保存的编辑器");
      return;
    }
    stash_active_text(list);
    OpenBuffer& buffer = list[active_.value()];
    // **二进制文件拒绝写盘**（最后一道防线：编辑器已置只读，但路径很多——
    // 保存全部/快捷键/菜单都能到这儿）。真写下去就等于用屏幕上的乱码
    // 覆盖原字节，不可恢复（实测：改 3 个字节再保存，文件就毁了）。
    if (buffer.binary) {
      status_.set("拒绕保存：二进制文件（" + buffer.label + "）— 本编辑器只编辑文本");
      return;
    }
    if (!buffer.path.empty()) {
      const bool saved = st::fs::write_text(buffer.path, buffer.text).has_value();
      status_.set(saved ? "已保存 " + buffer.path
                        : "保存失败（写盘被拒）：" + buffer.path);
      if (saved) {
        dir_cache_.clear();   // 新文件可能刚建（另存为/新建），树要重扫
        lsp_.document_saved(buffer.path);   // 告诉 server 磁盘真相变了（didSave）
      }
    } else {
      status_.set("已保存 " + buffer.label + "（内存模拟，无磁盘写入）");
    }
    buffer.dirty = false;
        status_.set(std::format("已保存 {} · {} 字符", buffer.label,
                            st::utf8_length(buffer.text)));
    buffers_.set(std::move(list));
  }


  /// **保存全部**（Ctrl+Alt+S / 文件菜单）：把所有脏标签写盘。
  ///
  /// 为何不只是循环调 `save()`：`save()` 既有状态栏文案、又会 `buffers_.set()`，
  /// 循环调它会把中间每一次的文案盖掉（用户只看得到最后一次），
  /// 而且每次都推一遍状态 → N 次重组。这里一次性写盘、一遍落地。
  /// 无路径的内存缓冲（untitled-N）**跳过**并在文案里说清楚——
  /// 静默跳过会让用户以为“保存了但没落盘”。
  auto CodeEditorPage::save_all() -> void {
    auto list = buffers_.value();
    if (list.empty()) {
      status_.set("没有可保存的编辑器");
      return;
    }
    stash_active_text(list);
    std::size_t saved = 0;
    std::size_t skipped = 0;
    std::size_t failed = 0;
    std::size_t refused_binary = 0;
    for (auto& buffer : list) {
      if (!buffer.dirty) continue;
      if (buffer.path.empty()) {
        ++skipped;
        continue;
      }
      // 二进制文件不写（与 `save()` 同一道防线；这里也要拦，
      // 否则“保存全部”会成为绕过单文件保护的缺口）。
      if (buffer.binary) {
        ++refused_binary;
        continue;
      }
      if (st::fs::write_text(buffer.path, buffer.text).has_value()) {
        buffer.dirty = false;
        lsp_.document_saved(buffer.path);
        ++saved;
      } else {
        ++failed;
      }
    }
    dir_cache_.clear();
    buffers_.set(std::move(list));
    std::string message = std::format("已保存 {} 个文件", saved);
    if (skipped > 0) message += std::format("；{} 个未命名缓冲需「另存为」", skipped);
    if (refused_binary > 0) message += std::format("；{} 个二进制文件拒绕写盘", refused_binary);
    if (failed > 0) message += std::format("；{} 个写入失败", failed);
    status_.set(message);
  }


  /// 切换亮/暗主题。  ///
  /// 唯一的真值源是 `UiRoot::theme()`（App 持有）：这里**读**它算下一档、
  /// **写回**它（经 `theme_setter` → `Application::set_theme_mode`，后者会连带
  /// 处理好字号缩放与文本 gamma），然后只更新本页的文案。
  /// 不在这里维护自己的 `dark_`——那正是故障的来源（影子状态永不落地到主题）。
  auto CodeEditorPage::toggle_theme() -> void {
    const bool next_dark = !theme_is_dark();
    if (theme_setter) theme_setter(next_dark ? ThemeMode::Dark : ThemeMode::Light);
    status_.set(next_dark ? "主题 dark · 视觉令牌已切换" : "主题 light · 视觉令牌已切换");
  }


  /// 编辑器被编辑：标脏 + 重跑轻量检查（问题面板与状态栏计数同源）。
  auto CodeEditorPage::on_edit() -> void {
    auto list = buffers_.value();
    if (active_.value() < list.size()) {
      stash_active_text(list);
      if (!list[active_.value()].dirty) {
        list[active_.value()].dirty = true;
        buffers_.set(std::move(list));
      }
    }
    refresh_problems();
  }


  /// 光标/选区变化 → 状态栏文案。
  auto CodeEditorPage::on_cursor_moved() -> void {
    if (editor == nullptr) return;
    cursor_text_.set(std::format("Ln {}, Col {}", editor->cursor_line() + 1,
                                 editor->cursor_column() + 1));
    const std::string picked = editor->selected_text();
    selection_text_.set(picked.empty() ? std::string{}
                                       : std::format("已选 {} 字符", st::utf8_length(picked)));
    // 行尾 blame 注解跟着光标走：只标注光标那一行，所以每移动一次都要换装饰。
    // 这里只换装饰（不取数）——数据按视口一次取够，光标在屏内移动永远命中缓存；
    // 真正缺数据时由帧首 `ensure_blame` 补（它按"视口 ∪ 光标行"取）。
    refresh_blame_for_cursor();
  }


  /// 重跑当前编辑器的轻量检查（问题列表与状态栏计数**同一份数据**）。
  ///
  /// 带指纹早退（`refresh_problems_if_needed` 的实现体）：谁都能调，
  /// 输入没变就是空操作——这才敢把三个刷新时机都接上（见函数尾的时机清单）。
  auto CodeEditorPage::refresh_problems() -> void {
    // 指纹：来源（LSP/内置）+ 活动缓冲的 key + 文本哈希 + LSP 诊断条数。
    // 「切到无问题的文件后计数从 3 掉到 1（也不对）」那类脏状态，根子都是
    // 该刷的时机没刷——指纹把「该不该刷」从时序问题变成数据问题。
    const auto list = buffers_.value();
    const bool has_active = active_.value() < list.size();
    const auto& buffer = has_active ? list[active_.value()] : kNoBuffer;
    const bool from_lsp = !lsp_.problems().empty();
    const std::uint64_t stamp =
        (from_lsp ? 0x4C5350ULL << 40 : 0x4C494EULL << 40) +   // 'LSP' / 'LIN' 来源标记
        st::hash::fnv1a64(buffer.key) * 31U +
        (from_lsp ? lsp_.problems().size() * 1315421789ULL
                  : static_cast<std::uint64_t>(st::hash::fnv1a64(buffer.text)));
    if (stamp == problems_stamp_) return;
    problems_stamp_ = stamp;
    // 重建了问题列表：递增快照版本——这是状态栏计数（文本 lambda 读 problems_）
    // 得知“该重跑了”的唯一信号（problems_ 不在依赖表里，见成员声明处的注释）。
    problems_revision_.set(problems_revision_.value() + 1);

    problems_.clear();
    if (!has_active) return;
    // **LSP 优先**：真语义诊断比启发式检查有价值得多（"未声明的标识符"这种事
    // lint 永远看不出来）。有 LSP 诊断时就用它，没有再退回内置 lint——
    // 不是为了省事，而是**没有 LSP server 的语言仍要有问题提示**。
    if (from_lsp) {
      problems_.reserve(lsp_.problems().size());
      for (const auto& problem : lsp_.problems()) {
        problems_.push_back(Problem{.line = problem.line, .warning = problem.warning,
                                    .message = problem.message});
      }
      return;
    }
    problems_ = lint_text(buffer.text);
  }




  /// 一次性接线语言服务的回调（在首次启用时挂）。
  void CodeEditorPage::wire_language_callbacks() {
    if (language_callbacks_wired_) return;
    language_callbacks_wired_ = true;
    lsp_.on_completion = [this](std::int64_t id,
                                const std::vector<st::lsp::CompletionEntry>& entries,
                                bool incomplete) {
      (void)incomplete;
      on_completion_response(id, entries, incomplete);
    };
    lsp_.on_completion_detail = [this](std::int64_t id, const std::string& detail) {
      on_completion_detail_response(id, detail);
    };
    lsp_.on_locations = [this](std::int64_t id, const std::vector<st::lsp::Location>& locations) {
      on_locations_response(id, locations);
    };
    lsp_.on_symbols = [this](std::int64_t id,
                             const std::vector<st::lsp::DocumentSymbol>& symbols) {
      on_symbols_response(id, symbols);
    };
    lsp_.on_workspace_symbols =
        [this](std::int64_t id, const std::vector<st::lsp::WorkspaceSymbol>& symbols) {
          on_workspace_symbols_response(id, symbols);
        };
    lsp_.on_format_edits = [this](std::int64_t id, const std::vector<st::lsp::TextEdit>& edits) {
      on_format_response(id, edits);
    };
    lsp_.on_prepare_rename = [this](
                                 std::int64_t id,
                                 const std::optional<st::lsp::PrepareRenameResult>& prepared) {
      (void)id;
      on_prepare_rename_response(id, prepared);
    };
    lsp_.on_rename = [this](std::int64_t id, const st::lsp::WorkspaceEdit& edit) {
      on_rename_response(id, edit);
    };
  }


  /// 把当前文件告知语言服务（首次会懒启动对应 server）。
  ///
  /// 启动失败（程序不在 PATH / 起进程失败 / 握手超时）要把原因说给用户——
  /// 否则只看到「语言服务未就绪」，分不清是「没装 server」还是「装了但起不来」。
  /// 这样也把“到底停在哪一步”变成可观测的（见 `LanguageService::error()`）。
  void CodeEditorPage::sync_language_document(const OpenBuffer& buffer) {
    if (buffer.path.empty()) return;   // 内存缓冲：没有路径就没有 URI
    const bool was_started = lsp_.started();
    lsp_.set_active_path(buffer.path);
    lsp_diagnostics_stamp_ = 0;        // 换文件：诊断指纹作废（重新推）
    if (!lsp_.ensure_started(buffer.path, buffer.language)) {
      // 只在**首次尝试启动**的那一帧报（本函数每帧/每次切文件都会走到，
      // 每帧都写会盖掉用户真正关心的别的状态）。
      if (!was_started && !lsp_.error().empty()) status_.set(lsp_.error());
      return;
    }
    wire_language_callbacks();
    lsp_.sync_document(buffer.path, buffer.text);
    refresh_document_symbols();   // 切文件：大纲跟着换
  }


  [[nodiscard]] auto CodeEditorPage::error_count() const -> std::size_t {
    return static_cast<std::size_t>(std::count_if(
        problems_.begin(), problems_.end(), [](const Problem& p) { return !p.warning; }));
  }

  [[nodiscard]] auto CodeEditorPage::warning_count() const -> std::size_t {
    return static_cast<std::size_t>(std::count_if(
        problems_.begin(), problems_.end(), [](const Problem& p) { return p.warning; }));
  }

  [[nodiscard]] auto CodeEditorPage::active_language() const -> std::string {
    const auto list = buffers_.value();
    return active_.value() < list.size() ? list[active_.value()].language : std::string("—");
  }

  /// 标题栏文案：脏时加一个圆点前缀（VSCode 同族的"未保存"提示）。
  [[nodiscard]] auto CodeEditorPage::window_title() const -> std::string {
    const auto list = buffers_.value();
    // 有工作区时把它的名放进标题——「打开文件夹」之后这是“我现在在哪个项目里”的
    // 第一指示（VSCode 同此：标题栏显示文件夹名）。
    std::string suffix = "歌白代码";
    if (!workspace_.empty()) {
      const std::size_t slash = workspace_.find_last_of("/\\");
      const std::string name =
          slash == std::string::npos ? workspace_ : workspace_.substr(slash + 1);
      if (!name.empty()) suffix = name + " - 歌白代码";
    }
    if (active_.value() >= list.size()) return suffix;
    const OpenBuffer& buffer = list[active_.value()];
    return (buffer.dirty ? "● " : "") + buffer.label + " - " + suffix;
  }

 // —— 内部工具 ——


  /// 把排队的 shell 命令投递给终端（终端实例就绪后）。
  ///
  /// 为何要排队：写操作在设计上“交终端执行”，而 `terminal_ptr` 只在**终端面板
  /// 被声明过**之后才有值。从未打开过终端时，事件回调里直接 `terminal_ptr->run(...)`
  /// 会因空指针而**静默什么都不做**（实测：点「中止」毫无反应，状态栏也不说，
  /// 看着像按钮坏了——而实际上是命令根本没发出去）。
  /// 现在改为“先入队 + 确保终端可见”，下一帧终端实例就绪时在泵里投递。
  void CodeEditorPage::flush_terminal_commands() {
    if (pending_shell_commands_.empty() || terminal_ptr == nullptr) return;
    for (const std::string& command : pending_shell_commands_) {
      terminal_ptr->run(command);
    }
    pending_shell_commands_.clear();
  }


  /// 让终端面板可见并排入一条命令（真正投递在 `flush_terminal_commands`）。
  void CodeEditorPage::queue_terminal_command(std::string command) {
    pending_shell_commands_.push_back(std::move(command));
    ensure_terminal_visible();
  }

  void CodeEditorPage::stash_active_text(std::vector<OpenBuffer>& list) {
    if (editor != nullptr && active_.value() < list.size()) {
      list[active_.value()].text = editor->text();
    }
  }


  /// 切到某个标签（先存回旧文本，再灌新文本）。
  void CodeEditorPage::activate(std::vector<OpenBuffer>& list, std::size_t index) {
    stash_active_text(list);
    // 切到文件标签 = 当前要看的是编辑器：清掉差异/合并标签（与 `open_path` 同理由）。
    if (!diff_key_.value().empty() || !merge_key_.value().empty()) {
      diff_key_.set({});
      diff_title_.set({});
      diff_file_.set(st::text::FileDiff{});
      merge_key_.set({});
      merge_view_ptr = nullptr;
    }
    buffers_.set(std::move(list));
    active_.set(index);
    load_active_text();
  }


  /// 把当前标签的文本灌进编辑器（切标签/打开文件的唯一入口）。
void CodeEditorPage::load_active_text() {
  const auto list = buffers_.value();
  if (active_.value() >= list.size()) return;
  if (editor == nullptr) {
    // ⚠ **编辑器还没装上时不能标记「已装载」**。
    //
    // `loaded_key_` 是“编辑器里现在装着哪个缓冲”的唯一判据，而 `build()` 末尾的
    // 灌入逻辑只在它与当前标签不一致时才写。构造期（首帧之前，编辑器还没创建）
    // 调到这里时，旧实现无条件写 `loaded_key_` —— 于是那一帧之后
    // “已装载”与实际内容不符，**编辑器永远停在上一个文件的内容上**。
    // 实测：`--open <file>` 只在构造期调一次（打开路径就是本函数），
    // 表现为“标签已经切到 B、正文却还是 A”。
    loaded_key_.clear();
    return;
  }
  loaded_key_ = list[active_.value()].key;   // 标记「已装载」——build() 末尾据此跳过
  editor->set_language(list[active_.value()].language);
  editor->set_text(list[active_.value()].text);
  refresh_problems();
}



  void CodeEditorPage::switch_tab(std::size_t index) {
    const auto list = buffers_.value();
    // 差异/合并标签：它们不在 `buffers_` 里，索引落在文件标签之后（见
    // `build_editor_area` 的组装顺序：文件标签在前，差异/合并在后）。
    if (index >= list.size()) {
      // 落在尾部但差异/合并都没开 = 索引越界（如刚关闭时短暂不一致），忽略。
      if (diff_key_.value().empty() && merge_key_.value().empty()) return;
      // 点的就是已打开的差异/合并标签本身：无需改状态（它已是当前内容）。
      status_.set("当前已是差异/合并标签");
      return;
    }
    // 切回文件标签：`activate` 内会清掉差异/合并 key（内容区让位给编辑器）。
    auto copy = list;
    activate(copy, index);
    status_.set("已切到 " + list[index].label);
  }


  void CodeEditorPage::jump_to_problem(std::size_t index) {
    if (index >= problems_.size() || editor == nullptr) return;
    editor->goto_line(problems_[index].line);
    editor->scroll_to_line(problems_[index].line);
    status_.set(std::format("已跳到第 {} 行", problems_[index].line));
  }


  // —— 标签关闭策略：脏标签先确认（VSCode 的保存/不保存/取消三选）——


  /// 关闭入口（快捷键/×/菜单都走这里）：脏标签弹确认，干净标签直接关。
  void CodeEditorPage::request_close(std::string_view key) {
    // 差异/合并标签（key 是 `diff:…` / `merge:…`，与文件 key 天然不撞）：直接关，
    // 它们没有未保存状态。关掉后若还有文件标签，留在原地（active_ 不变）；
    // 没有文件标签则显示空提示。
    if (!merge_key_.value().empty() && key == merge_key_.value()) {
      merge_key_.set({});
      merge_view_ptr = nullptr;
      status_.set("已关闭合并标签");
      return;
    }
    if (!diff_key_.value().empty() && key == diff_key_.value()) {
      diff_key_.set({});
      diff_title_.set({});
      diff_file_.set(st::text::FileDiff{});
      status_.set("已关闭差异视图");
      return;
    }
    const auto list = buffers_.value();
    for (const auto& buffer : list) {
      if (buffer.key != key) continue;
      if (buffer.dirty) {
        pending_close_ = [this, key = std::string(key)] { close(key); };
        pending_close_label_ = buffer.label;
        status_.set("未保存的修改：" + buffer.label + "（确认关闭？）");
        return;
      }
      break;
    }
    close(key);
  }


  /// 确认对话框的第一个按钮：保存并关闭。
  void CodeEditorPage::confirm_close_save() {
    save();
    finalize_pending_close();
  }

  /// 第二个按钮：不保存直接关。
  void CodeEditorPage::confirm_close_discard() {
    finalize_pending_close();
  }

  /// 第三个按钮（也走 Esc / 点遮罩）：取消。
  void CodeEditorPage::confirm_close_cancel() {
    pending_close_ = {};
    pending_close_label_.clear();
    status_.set("已取消关闭");
  }


  void CodeEditorPage::finalize_pending_close() {
    auto action = pending_close_;
    pending_close_ = {};
    pending_close_label_.clear();
    if (action) action();
  }


  /// 关闭全部标签（脏的先确认——这里只处理当前活动那个，其余交回逐次关闭）。
  void CodeEditorPage::close_all() {
    const auto list = buffers_.value();
    for (const auto& buffer : list) {
      if (buffer.dirty) {
        status_.set("有未保存的修改：" + buffer.label + "——请先保存或逐个关闭");
        switch_tab(index_of_key(buffer.key));
        request_close(buffer.key);
        return;
      }
    }
    buffers_.set({});
    active_.set(0);
    loaded_key_.clear();
    refresh_problems();
    status_.set("已关闭全部编辑器");
  }


  /// 关闭当前标签**右侧**的全部（脏的按同样规则拦截）。
  void CodeEditorPage::close_to_right() {
    auto list = buffers_.value();
    const std::size_t from = active_.value();
    for (std::size_t index = from + 1; index < list.size(); ++index) {
      if (list[index].dirty) {
        status_.set("右侧有未保存的修改：" + list[index].label);
        return;
      }
    }
    if (from + 1 >= list.size()) return;
    stash_active_text(list);
    list.erase(list.begin() + static_cast<std::ptrdiff_t>(from + 1), list.end());
    buffers_.set(std::move(list));
    status_.set("已关闭右侧标签");
  }


  [[nodiscard]] auto CodeEditorPage::index_of_key(std::string_view key) const -> std::size_t {
    const auto& list = buffers_.value();
    for (std::size_t index = 0; index < list.size(); ++index) {
      if (list[index].key == key) return index;
    }
    return 0;
  }


  void CodeEditorPage::update_find_counter() {
    if (editor == nullptr) return;
    const std::size_t total = editor->find_match_count();
    const std::size_t current = editor->find_active_index();
    find_counter_.set(total == 0 ? std::string("0/0")
                                 : std::format("{}/{}",
                                               current == CodeEditor::kNoFindMatch ? 0
                                                                                   : current + 1,
                                               total));
  }


  void CodeEditorPage::step_find(bool backward) {
    if (editor == nullptr) return;
    editor->find_next(backward);
    update_find_counter();
  }


  [[nodiscard]] auto CodeEditorPage::replacement_text() const -> std::string {
    return find_replace_input != nullptr ? find_replace_input->value() : std::string{};
  }


  void CodeEditorPage::replace_one() {
    if (editor == nullptr) return;
    editor->replace_current(replacement_text());
    update_find_counter();
  }


  void CodeEditorPage::replace_all() {
    if (editor == nullptr) return;
    status_.set(std::format("已替换 {} 处", editor->replace_all(replacement_text())));
    update_find_counter();
  }


  void CodeEditorPage::close_find() {
    if (editor != nullptr) editor->clear_find();
    find_open_.set(false);
  }

 /// 启动信息进**状态栏**（入口在 `app.start()` 之后才能拿到后端/端口信息）。
 ///
 /// 为什么不写进终端：真终端里那条流**归 shell**——应用不是写者。
 /// 往别人的流里插一行会打乱屏幕（shell 下次重绘就把它抹掉，
 /// 或更糟：把提示符位置弄乱）。应用自己的通知属于状态栏。
 auto CodeEditorPage::note_startup(const std::string& text) -> void { status_.set(text); }

   /// 每帧推动终端作业（入口主循环调；见 `update_terminal` 的说明）。
  auto CodeEditorPage::pump() -> void { pump_terminal(); }

  /// 退出前收尾：停掉在跑的作业并 join 读线程（`jthread` 把它露在对象生命周期
  /// 之外会很危险：它可能正阻塞在 `read_line` 上，而对象已经在析构）。
  auto CodeEditorPage::shutdown() -> void {
    save_session();   // 先存会话（要读编辑器光标，得在组件树还活着时做）
    shutdown_language_service();   // 先停 LSP（它会 join 自己的读线程）
    shutdown_terminal();
  }

  /// 关闭查找条（入口的 Esc 快捷键用；与面板上的「×」同一条路径）。
 auto CodeEditorPage::dismiss_find() -> void { close_find(); }

 /// 新建文件（入口快捷键用）。
 auto CodeEditorPage::new_file_request() -> void { new_file_prompt(); }

 /// 转到符号（入口快捷键/菜单用）：`document=true` 当前文件，`false` 工作区。
 auto CodeEditorPage::symbols_request(bool document) -> void { open_symbols(document); }

 /// 全部保存（入口快捷键 Ctrl+Alt+S 用）。
 auto CodeEditorPage::save_all_request() -> void { save_all(); }

 /// 手动触发补全（入口快捷键 Ctrl+Space 用）。
 auto CodeEditorPage::completion_request() -> void { trigger_completion(); }

 /// 跳到定义（入口快捷键 F12 用）。
 auto CodeEditorPage::definition_request() -> void { goto_definition(); }

 /// 找引用（入口快捷键 Shift+F12 用）。
 auto CodeEditorPage::references_request() -> void { find_references(); }

 /// 跳到声明（入口快捷键 Alt+F12 用）。
 auto CodeEditorPage::declaration_request() -> void { goto_declaration(); }

 /// 跳到实现（入口快捷键 Ctrl+F12 用）。
 auto CodeEditorPage::implementation_request() -> void { goto_implementation(); }

 /// 返回上一处跳转（入口快捷键 Alt+Left 用）。
 auto CodeEditorPage::navigate_back_request() -> void { navigate_back(); }

 /// 格式化整篇（入口快捷键 Shift+Alt+F 用）。
 auto CodeEditorPage::format_request() -> void { format_selection(); }

 /// 重命名符号（入口快捷键 F2 用）。
 auto CodeEditorPage::rename_request() -> void { rename_symbol(); }

 /// 刷新 Git 状态（入口快捷键用）。
 auto CodeEditorPage::refresh_git_request() -> void { refresh_git(); }

 /// 跳到第 index 处问题（入口快捷键用）。
 auto CodeEditorPage::jump_to_problem_request(std::size_t index) -> void { jump_to_problem(index); }


  /// 扫描工作区内某层目录（`rel` 为相对根的路径；空 = 根），带缓存。
  [[nodiscard]] auto CodeEditorPage::list_workspace_dir(const std::string& rel) const
      -> const std::vector<DirEntryLite>& {
    const auto found = dir_cache_.find(rel);
    if (found != dir_cache_.end()) return found->second;
    const std::string full = rel.empty() ? workspace_ : workspace_ + "/" + rel;
    // insert 返回 {迭代器, 是否新插入}；用 [] 不行（先默认构造再赋值，多一次拷贝）。
    auto [slot, inserted] = dir_cache_.emplace(rel, list_dir_sorted(full));
    (void)inserted;
    return slot->second;
  }


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
  [[nodiscard]] auto CodeEditorPage::workspace_nodes() -> std::vector<TreeNodeData> {
    std::vector<TreeNodeData> nodes;
    const auto expanded = dir_expanded_.value();
    // 递归铺开：`rel` 是相对工作区根的目录路径（空 = 根）。
    const std::function<void(const std::string&, int)> flatten =
        [&](const std::string& rel, int depth) {
          for (const DirEntryLite& entry : list_workspace_dir(rel)) {
            const std::string child_rel = rel.empty() ? entry.name : rel + "/" + entry.name;
            if (entry.is_dir) {
              const bool open = std::find(expanded.begin(), expanded.end(), child_rel) != expanded.end();
              nodes.push_back(TreeNodeData{.key = "dir:" + child_rel, .label = entry.name,
                                           .expanded = open, .is_dir = true, .depth = depth,
                                           .icon = open ? "folder-open" : "folder"});
              if (open) flatten(child_rel, depth + 1);   // 同源递归：key 与路径同一规则
            } else {
              // 文件行的 key 就是**绝对路径**（与目录行的 `dir:<相对路径>` 不同）。
              // 这不是优雅设计，而是已有的公开契约：`open_path(key)` 直接拿它当路径用，
              // 且控制通道/自动化也在按这个口径断言。要改成相对路径必须两处同时改，
              // 所以此处不改，只在应用侧做归一化（见 `tree_key_to_path`）。
              nodes.push_back(TreeNodeData{.key = workspace_ + "/" + child_rel, .label = entry.name,
                                           .depth = depth, .icon = icon_for_file(entry.name)});
            }
          }
        };
    flatten(std::string{}, 0);
    return nodes;
  }


  /// 文件名 → 行首图标名。
  ///
  /// 映射克制：**只分到“需要一眼分出的粒度”**——代码 / 文档 / 配置 / 图片 /
  /// 构建产物 / 其余。把每种扩展名都配一枚图标看着很丰富，实际上图标越多
  /// 区分度越低（都变成“花的”），而且会增加维护面。
  [[nodiscard]] auto CodeEditorPage::icon_for_file(const std::string& name) -> std::string {
    const auto dot = name.rfind('.');
    std::string ext = dot == std::string::npos ? std::string{} : name.substr(dot + 1);
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == "md" || ext == "markdown") return "markdown";
    if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "gif" || ext == "webp" ||
        ext == "svg" || ext == "ico")
      return "image";
    if (ext == "c" || ext == "cc" || ext == "cpp" || ext == "cxx" || ext == "h" || ext == "hpp" ||
        ext == "rs" || ext == "go" || ext == "py" || ext == "js" || ext == "ts" || ext == "tsx" ||
        ext == "java" || ext == "sh")
      return "code";
    if (ext == "json" || ext == "toml" || ext == "yaml" || ext == "yml" || ext == "ini" ||
        ext == "pkg" || ext == "cfg")
      return "settings";
    if (ext == "exe" || ext == "dll" || ext == "so" || ext == "o" || ext == "a") return "package";
    if (ext == "db" || ext == "sqlite") return "database";
    if (ext == "ttf" || ext == "otf") return "file";
    return "file";
  }


  void CodeEditorPage::on_tree_toggle(const std::string& key, bool expanded) {
    if (workspace_.empty() || key.rfind("dir:", 0) != 0) return;
    const std::string name = key.substr(4);
    auto list = dir_expanded_.value();
    if (expanded) {
      if (std::find(list.begin(), list.end(), name) == list.end()) list.push_back(name);
    } else {
      std::erase(list, name);
      // 收起时把子孙目录的展开状态一并遇忘——否则下次展开父目录时
      // 孙目录跳出来（用户当时收起的是整棵子树）。子路径以 `name + "/"` 开头。
      const std::string prefix = name + "/";
      std::erase_if(list, [&](const std::string& item) {
        return item.rfind(prefix, 0) == 0;
      });
      dir_cache_.clear();   // 收起后重新扫描：目录可能已变化，旧缓存不可信
    }
    dir_expanded_.set(std::move(list));
  }


  /// 递归搜工作区；结果写进 `search_hits_`。
  ///
  /// 改自原先的“只找一个词”：新增 glob 过滤与大小写/全词/正则三个开关，
  /// 并把命中**按文件分组**（同一个文件的命中排在一起，前缀行给文件名）——
  /// 平铺的结果在几十条之后就完全不可用了（看不出哪几行属于同一个文件）。
  void CodeEditorPage::run_search(const std::string& query) {
    apply_search(query);
  }


  /// 真正执行搜索（UI 里的入口与终端 `find` 共用）。
  void CodeEditorPage::apply_search(const std::string& query) {
    last_query_ = query;
    search_hit_paths_.clear();
    search_hit_lines_.clear();
    search_hit_columns_.clear();
    std::vector<std::string> hits;
    constexpr std::size_t kMaxHits = 300;
    constexpr std::size_t kMaxFileBytes = 1U << 20U;
    if (!search_hits_.value().empty()) {
      search_summary_.set(std::format("{} 处命中", search_hits_.value().size()));
    }
    if (query.empty()) {
      search_summary_.set("");
      search_hits_.set({});
      return;
    }
    // 匹配器：大小写敏感 / 全词 / 正则——先定好参数，逐行复用。
    //
    // **不用 `std::regex`**（本仓不引 regex：构造昂贵、异常语义差、且跨平台行为
    // 有差异）。这里实现一个小而**诚实的**子集匹配器：
    // - `.*`：任意字符序列（可跨列）；
    // - 普通字符：字面量；
    // - 支持 `^` / `$` 锚定与 `\d` / `\w` / `\s` 三个字符类。
    // 不支持集合/分组/量词——这类表达式在搜索里几乎用不到，而“假装支持”会静默给出错结果。
    const std::string needle = search_case_.value() ? query : to_lower(query);
    const auto locate = [&](std::string_view raw) -> std::optional<std::size_t> {
      const std::string haystack = search_case_.value() ? std::string(raw) : to_lower(raw);
      if (search_regex_.value()) return match_regex(needle, haystack);
      std::size_t pos = haystack.find(needle);
      while (pos != std::string::npos) {
        if (!search_word_.value()) return pos;
        // 全词：两侧不得为标识符字符（字母/数字/下划线），与多数编辑器的口径一致。
        const bool left_ok = pos == 0 || !is_word_char(haystack[pos - 1]);
        const std::size_t end = pos + needle.size();
        const bool right_ok = end >= haystack.size() || !is_word_char(haystack[end]);
        if (left_ok && right_ok) return pos;
        pos = haystack.find(needle, pos + 1);
      }
      return std::nullopt;
    };

    const auto add_hit = [&](const std::string& path, const std::string& name, std::size_t line,
                             std::size_t column, std::string_view line_text) {
      hits.push_back(std::format("{}:{}  {}", name, line, st::trim(line_text).substr(0, 70)));
      search_hit_paths_.push_back(path);
      search_hit_lines_.push_back(line);
      search_hit_columns_.push_back(column);
    };

    const auto scan_text = [&](const std::string& path, const std::string& name,
                               const std::string& content) {
      std::size_t line_no = 0;
      std::size_t begin = 0;
      while (begin <= content.size() && hits.size() < kMaxHits) {
        const std::size_t eol = content.find('\n', begin);
        const std::size_t end = eol == std::string::npos ? content.size() : eol;
        std::string_view line(content.data() + begin, end - begin);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        ++line_no;
        if (const auto column = locate(line); column.has_value()) {
          add_hit(path, name, line_no, *column, line);
        }
        if (eol == std::string::npos) break;
        begin = eol + 1;
      }
    };

    if (!workspace_.empty()) {
      std::vector<std::pair<std::string, int>> stack;
      stack.reserve(64);
      stack.emplace_back(workspace_, 0);
      while (!stack.empty() && hits.size() < kMaxHits) {
        const auto [dir, depth] = stack.back();
        stack.pop_back();
        if (depth > 6) continue;
        auto listing = st::fs::list_dir(dir);
        if (!listing) continue;
        for (const auto& item : *listing) {
          if (hits.size() >= kMaxHits) break;
          const std::string full = st::fs::join(dir, item.name);
          if (item.is_dir) {
            if (!item.name.empty() && item.name.front() == '.') continue;   // 隐藏目录
            if (item.name == "build" || item.name == "node_modules") continue;  // 产物目录
            stack.emplace_back(full, depth + 1);
            continue;
          }
          if (item.size > kMaxFileBytes) continue;
          // glob 过滤：按**相对工作区**的路径匹配（写 `*.cpp` 时人想的是相对路径）。
          if (!search_glob_.empty() &&
              !st::fs::match_glob(search_glob_, st::fs::relative_to(full, workspace_))) {
            continue;
          }
          auto content = st::fs::read_text(full);
          if (!content) continue;   // 非 UTF-8 / 读不了：跳过而不是硬猜
          scan_text(full, item.name, *content);
        }
      }
    }

    if (hits.empty()) {
      search_summary_.set("0 处命中");
      status_.set("搜索命中 0 处");
      search_hits_.set({});
      return;
    }
    const bool truncated = hits.size() >= kMaxHits;
    search_summary_.set(std::format("{} 处命中{}", hits.size(), truncated ? "（已截断）" : ""));
    status_.set(std::format("搜索命中 {} 处", hits.size()));
    search_hits_.set(std::move(hits));
  }


  [[nodiscard]] auto CodeEditorPage::to_lower(std::string_view text) -> std::string {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    return out;
  }

  [[nodiscard]] auto CodeEditorPage::is_word_char(char c) -> bool {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
  }


  /// 小型正则匹配器（支持子集见 `apply_search` 的说明）；返回首个匹配的**起始列**。
  ///
  /// 匹配策略：逐起点尝试 + 回溯（模式长度有限、行长度在几千字节量级），
  /// 搜索场景完全够用——不会像完整引擎那样再引一个依赖。
  [[nodiscard]] auto CodeEditorPage::match_regex(std::string_view pattern, std::string_view text)
      -> std::optional<std::size_t> {
    bool anchored_start = !pattern.empty() && pattern.front() == '^';
    const bool anchored_end = !pattern.empty() && pattern.back() == '$';
    if (anchored_start) pattern.remove_prefix(1);
    if (anchored_end && !pattern.empty()) pattern.remove_suffix(1);
    const std::size_t last_start = anchored_start ? 0 : text.size();
    for (std::size_t start = 0; start <= last_start; ++start) {
      std::size_t cursor = start;
      std::size_t pattern_at = 0;
      bool ok = true;
      while (pattern_at < pattern.size()) {
        const char token = pattern[pattern_at];
        if (token == '\\' && pattern_at + 1 < pattern.size()) {
          const char klass = pattern[pattern_at + 1];
          if (cursor >= text.size()) {
            ok = false;
            break;
          }
          const char c = text[cursor];
          const bool hit = (klass == 'd' && c >= '0' && c <= '9') ||
                           (klass == 'w' && is_word_char(c)) ||
                           (klass == 's' && (c == ' ' || c == '\t'));
          if (!hit) {
            ok = false;
            break;
          }
          ++cursor;
          pattern_at += 2;
          continue;
        }
        // `.*`：贪心跳到**能让剩余模式匹配上**的第一个位置（无剩余的剩余==0）。
        if (token == '.' && pattern_at + 1 < pattern.size() && pattern[pattern_at + 1] == '*') {
          const std::string_view rest = pattern.substr(pattern_at + 2);
          std::size_t probe = cursor;
          bool matched = false;
          for (;;) {
            if (match_regex(rest, text.substr(probe)).has_value()) {
              matched = true;
              break;
            }
            if (probe >= text.size()) break;
            ++probe;
          }
          if (!matched) {
            ok = false;
            break;
          }
          pattern_at = pattern.size();
          cursor = text.size();
          break;
        }
        if (cursor >= text.size() || text[cursor] != token) {
          ok = false;
          break;
        }
        ++cursor;
        ++pattern_at;
      }
      if (ok && pattern_at >= pattern.size() && (!anchored_end || cursor == text.size())) {
        return start;
      }
    }
    return std::nullopt;
  }


  // —— Git：真调 `git` 子进程（只读子命令）——
  //
  // 为什么值得接：SCM 面板若只是写死一个 “main” 分支名与 “0 变更”，它就不是信息
  // 而是装饰。真读 `git status --porcelain=v1 -b` 之后，改一个文件、刷新，面板会跟着变。
  void CodeEditorPage::refresh_git() {
    git_probed_ = true;
    git_ = gbcode::read_snapshot(workspace_);
    // ⚠ **必须让“数据换了”一定能触发重组**。
    //
    // 变更面板从 `git_`（普通成员）取数，而普通成员不进声明式依赖表；
    // 以前这条路径唯一的状态写入是 `status_.set(...)`，而它**带相等早退**——
    // 当“变更条数/分支/标签数都没变”时（正是“暂存一个文件”这种最常见的情形）
    // 整个写入无失效 → 没有任何重组 → 面板停在上一帧快照，
    // 连手工点“刷新状态”也无效（同一条路径）。实测：暂存后等 6s + 手动刷新都不变，
    // 而随便切一下主题（无关重组）立刻变对。
    //
    // 用一个单调递增的 State 当“快照版本”：面板把它读进依赖，写它就必然重组。
    git_revision_.set(git_revision_.value() + 1);
    if (!git_.error.empty()) {
      status_.set("Git：" + git_.error);
      return;
    }
    if (!git_.is_repo) {
      status_.set("当前工作区不是 Git 仓库");
      return;
    }
    std::string flow;
    if (git_.ahead > 0 || git_.behind > 0) {
      flow = std::format("（↑{} ↓{}）", git_.ahead, git_.behind);
    }
    status_.set(std::format("Git：{}{} · {} 项变更", git_.branch, flow, git_.changes.size()));
  }

  // —— 行尾 blame 注解（B5）——


  /// 开关切一下：开→下一帧取数；关→清掉装饰（界面立刻回到干净态）。
  void CodeEditorPage::toggle_blame() {
    const bool next = !blame_on_.value();
    blame_on_.set(next);   // State：驱动状态栏按钮的激活态 + 触发重组
    if (!next) {
      blame_lines_.clear();
      blame_fetched_.clear();
      blame_path_.clear();
      if (editor != nullptr) editor->clear_line_decorations();
      status_.set("已关闭 blame 注解");
      return;
    }
    status_.set("已开启 blame 注解（显示当前视口的行）");
    // **立即拉一次，不等下一个帧首**：`ensure_blame` 要读
    // `editor->visible_line_range_now()`，那是**上一帧绘制**时记下的值；
    // 刚点的这一刻读到的可能是旧值/空值，用户会觉得"点了没反应"。
    // 这里先用既有的可见区间拉一遍（`ensure_blame` 内部对"已取过的段"早退，
    // 所以紧接着的帧首泵不会重复取）。
    ensure_blame();
  }


  // —— 单文件历史（B6）——


  /// 打开“当前文件的历史”弹窗（`read_file_history` = `git log --follow`，跨重命名）。
  ///
  /// 为何入口放在**编辑器右键菜单**：它对标的动作是“这个文件改过什么”，而光标就在
  /// 这个文件里；放在日志视图上会变成“选中那个提交的历史”，语义不同。
  void CodeEditorPage::open_file_history() {
    const auto list = buffers_.value();
    if (list.empty() || active_.value() >= list.size()) return;
    const OpenBuffer& buffer = list[active_.value()];
    if (buffer.path.empty()) {
      status_.set("内存缓冲没有历史（先保存成文件）");
      return;
    }
    history_path_ = buffer.path;
    history_entries_.set({});
    history_wanted_ = true;   // 取数在帧首泵（起子进程不能在渲染期）
    history_open_.set(true);
  }


  /// 帧首泵：取历史（与 `ensure_diff` 同一理由：渲染期起子进程会崩）。
  void CodeEditorPage::ensure_file_history() {
    if (!history_wanted_) return;
    history_wanted_ = false;
    if (workspace_.empty() || history_path_.empty()) return;
    std::error_code ec;
    const auto path = std::filesystem::path(history_path_);
    std::string relative = history_path_;
    if (const auto rel = std::filesystem::relative(path, workspace_, ec); !ec) {
      relative = rel.generic_string();
    }
    history_entries_.set(gbcode::read_file_history(workspace_, relative, 100));
    const std::size_t count = history_entries_.value().size();
    status_.set(count == 0 ? "该文件没有历史（未跟踪 / 不在仓库里）"
                           : std::format("文件历史：{} 条", count));
  }


  /// 拉取视口行的 blame（帧首泵做：`git blame` 是子进程，渲染期起会崩）。
  ///
  /// **只问没问过的行段**：用户滚动时每帧都会想要新行，而 `git blame` 一次几百毫秒；
  /// 已取过的段记在 `blame_fetched_` 里，避免同一段反复起进程。
  void CodeEditorPage::ensure_blame() {
    if (!blame_on_.value() || editor == nullptr) return;
    const auto list = buffers_.value();
    if (list.empty()) return;
    const std::string path = list[active_.value()].path;
    if (path.empty()) return;
    if (path != blame_path_) {
      // 换了文件：旧注解全部作废（行号属于另一个文件，留着就是错的行）。
      blame_lines_.clear();
      blame_fetched_.clear();
      blame_path_ = path;
    }
    // 取数区间 = **视口区间与光标行的并集**（光标几乎总在视口内，所以通常就是视口）。
    //
    // 为何仍按视口取而不是只取光标一行：`git blame -L` 要起子进程，一行一取会在
    // 每次方向键都 fork 一次；按视口一次取够，光标在屏内移动就永远命中缓存。
    // 光标行单独并进来，是为了兼容"视口还没画过、光标已在别处"的时序。
    auto [first, last] = editor->visible_line_range_now();
    const std::size_t cursor_line = editor->cursor_line() + 1;
    if (last < first) {
      first = last = cursor_line;
    } else {
      first = std::min(first, cursor_line);
      last = std::max(last, cursor_line);
    }
    if (last < first) return;
    for (const auto& [begin, end] : blame_fetched_) {
      if (begin <= first && last <= end) return;   // 这一段已经取过
    }
    std::error_code ec;
    const std::filesystem::path fs_path = std::filesystem::path(path);
    if (!std::filesystem::exists(fs_path, ec) || ec) return;
    // 相对仓根的路径（`git blame` 收相对路径更稳）。
    std::string relative = path;
    if (const auto rel = std::filesystem::relative(fs_path, workspace_, ec); !ec) {
      relative = rel.generic_string();
    }
    const std::size_t count = last - first + 1;
    const auto lines = gbcode::read_blame(workspace_, {}, relative, first, count);
    if (lines.empty()) {
      // 取不到（未跟踪文件 / 不在仓库里）：记一下，别每帧重试。
      blame_fetched_.emplace_back(first, last);
      return;
    }
    for (const auto& line : lines) {
      // 注解文本：作者 + 日期（IDEA 的行尾形态；不看 hash——那是点开详情才要的）。
      blame_lines_[line.line] = line.author.empty()
                                    ? std::format("{} {}", line.hash, line.date)
                                    : std::format("{} {}", line.author, line.date);
    }
    blame_fetched_.emplace_back(first, last);
    apply_blame_decorations();
  }


  /// 把已拿到的 blame 拼进编辑器装饰。
  ///
  /// 装饰是**整体替换**语义，所以这里只回答“当前该长什么样”。
  /// 与诊断/差异共存时由调用方决定优先级——blame 只在开关打开时写，
  /// 而那个开关默认关，所以不会把别的装饰意外抹掉。
  /// 把注解贴到**光标所在那一行**（2026-10-19 按用户要求："只有光标所在行才显示"）。
  ///
  /// 为什么不整屏都标：一列"作者 日期"铺满视口时，正文右侧全是灰字，读代码时
  /// 视线会被反复拽走；而 blame 真正要回答的是"**这行**是谁写的"——给光标行标一条
  /// 就够，且随光标移动自然更新，不需要来回扫一屏。
  void CodeEditorPage::apply_blame_decorations() {
    if (editor == nullptr) return;
    if (!blame_on_.value()) {
      editor->clear_line_decorations();
      blame_decorated_line_ = 0;
      return;
    }
    // `cursor_line()` 是 **0 起**；`blame_lines_` 的键是 **1 起**的行号。
    const std::size_t cursor_line = editor->cursor_line() + 1;
    if (cursor_line != blame_decorated_line_) blame_decorated_line_ = cursor_line;
    const auto found = blame_lines_.find(cursor_line);
    if (found == blame_lines_.end()) {
      editor->clear_line_decorations();
      return;
    }
    std::vector<CodeEditor::LineDecoration> marks;
    marks.push_back(CodeEditor::LineDecoration{
        .line = cursor_line, .mark = CodeEditor::LineMark::None, .end_text = found->second});
    editor->set_line_decorations(std::move(marks));
  }


  /// 光标移动后刷新 blame 注解（只重画那一行，不重取数据）。
  ///
  /// 快路径：光标新行已经在 `blame_lines_` 里 ⇒ 只换装饰；不在 ⇒ 交给帧首
  /// `ensure_blame`（它按视口区间取数，加一个缺口重试）。
  void CodeEditorPage::refresh_blame_for_cursor() {
    if (!blame_on_.value() || editor == nullptr) return;
    const std::size_t cursor_line = editor->cursor_line() + 1;
    const bool have = blame_lines_.find(cursor_line) != blame_lines_.end();
    apply_blame_decorations();   // 有就换上，没有就清掉（不留上一行的旧注解）
    if (!have) {
      // 光标到了没取过的行（视口外的快速跳转、刚打开文件等）：
      // 让帧首 `ensure_blame` 下一帧补——**这里不直接起子进程**，
      // `git blame` 是同步 fork，按键途中做会明显卡手。
      blame_fetched_.clear();   // 清掉"已取过"的记录，允许重新按新区间取
    }
  }


  /// Git 工具窗的数据读取（帧首泵做——渲染期起子进程会崩，见 build_git_view 注释）。
  void CodeEditorPage::ensure_git_log() {
    if (!git_log_wanted_ || git_log_loaded_ || git_log_loading_) return;
    if (workspace_.empty()) return;
    git_log_wanted_ = false;
    // **这批读操作放后台**。
    //
    // 为何必须：这里一口气跑 9 条 git 子进程（log/branches/tags/remotes/stashes/
    // refs/upstream/operation/audit）。在主线程同步做时，实测 **3300 ms**
    // （gbcode 打开 `D:/coding/modeltrain-be` 这类大仓库）——而它发生在**首帧之后的第一帧**，
    // 用户看到的就是“界面出来了，但点不动、拖不动、两秒后才活”。
    //
    // 分离点：**读数据**（纯 git 子进程，不碰 UI 树）在 worker；
    // **写 `git_*` 成员与 `State`**（会触发重组/标脏）必须回主线程——
    // 从工作线程写 `State` 是数据竞争（见 `st/core/async.hpp` 的线程模型说明）。
    //
    // `git_log_loading_` 是关键：没有它，后台任务跑着的那几帧里
    // `git_log_wanted_` 会被反复置真（渲染期会再要），于是**每帧都提交一个新任务**。
    if (layout_tasks_ == nullptr) {
      // 没有调度器（单测/嵌入式）：退回同步，行为与旧版一致。
      load_git_log_sync();
      return;
    }
    git_log_loading_ = true;
    const std::string workspace = workspace_;
    st::app::Tasks* tasks = layout_tasks_;
    tasks->run_async([this, tasks, workspace](std::stop_token) {
      // 后台只算数据（纯 git 子进程，不碰 UI 树）。
      gbcode::LogSnapshot snapshot = gbcode::read_log_snapshot(workspace);
      // 回主线程落地（`snapshot` 是局部变量，移动进去）。
      tasks->post_ui([this, snapshot = std::move(snapshot)]() mutable {
        adopt_git_log(std::move(snapshot));
      });
    });
  }

  /// 把一批 git 快照落到成员上（**主线程**；只有 `ensure_git_log` 与退路会调）。
  void CodeEditorPage::adopt_git_log(gbcode::LogSnapshot snapshot) {
    git_log_ = std::move(snapshot.log);
    git_branches_ = std::move(snapshot.branches);
    git_tags_ = std::move(snapshot.tags);
    git_remotes_ = std::move(snapshot.remotes);
    git_stashes_ = std::move(snapshot.stashes);
    git_refs_ = std::move(snapshot.refs);
    git_upstream_ = std::move(snapshot.upstream);
    git_operation_ = std::move(snapshot.operation);
    git_audit_ = std::move(snapshot.audit);
    git_log_loaded_ = true;
    git_log_loading_ = false;
    // 读完了**要标脏**：`git_*` 全是普通成员，数据进来了界面不会自己重画（旧写法
    // 靠 `status_.set(...)` 的副作用触发重组——而当文案与上次**逐字相同时**
    // `State::set` 相等早退，于是“别的地方变了、这批数据也变了、但界面不动”）。
    // 用专门的版本号：它只在这里 + `refresh_git()` 里自增，语义就是“git 数据换过了”。
    git_revision_.set(git_revision_.value() + 1);
    std::string extra;
    if (git_operation_.active()) extra = std::format(" · {} 进行中", git_operation_.kind);
    status_.set(std::format("Git：{} 条提交 · {} 个分支 · {} 个标签{}", git_log_.size(),
                            git_branches_.size(), git_tags_.size(), extra));
  }

  /// 同步版（无调度器时的退路，行为与旧实现逐行一致）。
  void CodeEditorPage::load_git_log_sync() {
    adopt_git_log(gbcode::read_log_snapshot(workspace_));
  }


  /// 比较视图的数据读取（已随比较视图下线，2026-10-13；无调用点，保留函数体
  /// 以免散落在 git_service 里的 read_compare 能力被连坐删除——将来恢复比较入口
  /// 时直接接回这里即可）。
  void CodeEditorPage::ensure_compare() {
    if (!compare_wanted_) return;
    if (workspace_.empty()) return;
    compare_wanted_ = false;
    gbcode::CompareOptions options;
    options.from = gbcode::DiffEndpoint::parse(compare_from_.value());
    options.to = gbcode::DiffEndpoint::parse(compare_to_.value());
    options.merge_base = compare_merge_base_.value();
    compare_result_ = gbcode::read_compare(workspace_, options);
    compare_loaded_ = true;
    // 读到东西要**标脏**：普通成员改了不会触发重组（与 `ensure_git_log` 同一个坑）。
    if (!compare_result_.ok) {
      status_.set("比较失败：" + compare_result_.error);
    } else {
      status_.set(std::format("比较 {} → {}：{} 个文件 · +{} −{}",
                              compare_result_.from_text.empty() ? "工作区"
                                                                : compare_result_.from_text,
                              compare_result_.to_text, compare_result_.files.size(),
                              compare_result_.additions, compare_result_.deletions));
    }
  }


  /// 并列差异的数据读取（帧首泵）：读**两侧文本**后用 `text::diff` 在本地算结构。
  ///
  /// 为何不直接吃 `git diff` 的 patch 文本：结构化差异要的是**行号映射与两栏对齐**，
  /// 两者都靠 `DiffLine.old_line/new_line`。统一到“取文本 + 本地比”只剩一条路，
  /// 而且将来做“与 HEAD 比当前未保存内容”时不用再换实现。
  void CodeEditorPage::ensure_diff() {
    if (!diff_wanted_) return;
    diff_wanted_ = false;
    if (workspace_.empty() || diff_pending_path_.empty()) return;
    const gbcode::CompareOptions& options = diff_pending_options_;
    gbcode::DiffEndpoint from = options.from;
    gbcode::DiffEndpoint to = options.to;
    // 纯未暂存改动：左 = 暂存区，右 = 工作区——这是「改了什么」的默认口径
    //（与 `git diff` 的裸语义一致；两个端点都是“空”时走这一支）。
    if (from.empty() && (to.empty() || to.is_worktree())) {
      from = gbcode::DiffEndpoint::parse("INDEX");
      to = gbcode::DiffEndpoint::parse("WORKTREE");
    }
    const auto old_side = gbcode::read_content_at(workspace_, from, diff_pending_path_);
    const auto new_side = gbcode::read_content_at(workspace_, to, diff_pending_path_);
    st::text::FileDiff file;
    file.old_path = diff_pending_path_;
    file.new_path = diff_pending_path_;
    file.binary = old_side.binary || new_side.binary;
    file.added = old_side.missing && new_side.ok;
    file.deleted = new_side.missing && old_side.ok;
    if (file.binary) {
      file.message = "二进制文件：不逐行展示";
    } else if (file.added) {
      file.message = "新增文件";
    } else if (file.deleted) {
      file.message = "删除文件";
    }
    if (!file.binary) {
      file.hunks = st::text::diff_hunks(old_side.text, new_side.text, options.context);
      const auto [additions, deletions] = st::text::count_changes(file.hunks);
      file.additions = additions;
      file.deletions = deletions;
    }
    diff_file_.set(file);
    diff_title_.set(diff_pending_title_);
    status_.set(std::format("差异：{} · {} 处改动 · +{} −{}", diff_pending_path_,
                            file.hunks.size(), file.additions, file.deletions));
  }


  /// 打开一个**并列差异**标签（`options` 描述两端；`path` 是文件）。
  ///
  /// 与旧版“把 `git diff` 敲进只读标签”的区别：这里是**结构**（两栏对齐、双行号、
  /// 可一个差异一个差异地跳），而旧的只是一块带颜色的文本。
  /// 三方合并的数据读取（帧首泵）：取 `:1:/:2:/:3:` 三仝内容并划分区段。
  ///
  /// 为何在帧首泵而不是渲染期：要起 `git` 子进程（`read_conflict_sides`），
  /// 而声明式重组过程中起子进程会把 UI 卡住（与差异同理）。
  void CodeEditorPage::ensure_merge() {
    if (!merge_wanted_) return;
    merge_wanted_ = false;
    if (workspace_.empty() || merge_pending_path_.empty()) return;
    const auto sides = gbcode::read_conflict_sides(workspace_, merge_pending_path_);
    if (!sides.any()) {
      // 不是冲突文件（或路径写错）：**如实报状态**，不要开一个空的三栏视图。
      status_.set("该文件没有冲突三仝内容：" + merge_pending_path_);
      merge_key_.set({});
      return;
    }
    // 缺某一号时用空串——`merge_three_way` 对空 base 的语义就是「无共同祖先」，
    // 而缺失的整侧（删除/新增冲突）则表现为“那一侧全空”。两者都是正确的表达。
    const std::string base_text = sides.base.ok ? sides.base.text : std::string{};
    const std::string ours_text = sides.ours.ok ? sides.ours.text : std::string{};
    const std::string theirs_text = sides.theirs.ok ? sides.theirs.text : std::string{};
    merge_base_text_.set(base_text);
    merge_ours_text_.set(ours_text);
    merge_theirs_text_.set(theirs_text);
    merge_plan_ = st::text::merge_three_way(base_text, ours_text, theirs_text);
    merge_loaded_ = true;
    const auto& plan = merge_plan_;
    status_.set(std::format("合并：{} · {} 段 · {} 处冲突", merge_pending_path_,
                            plan.segments.size(), plan.conflicts));
  }


  /// 帧首落盘：合并选择变了就把结果写回文件（见 `merge_write_pending_` 的说明）。
  ///
  /// ⚠ 必须从**当前帧的视图实例**取结果（`merge_view_ptr`）——它带着用户刚做的选择；
  /// 而 `merge_plan_` 只是三份内容。视图不在树上时（刚关掉标签）什么都不做。
  void CodeEditorPage::flush_merge_write() {
    if (!merge_write_pending_) return;
    merge_write_pending_ = false;
    if (merge_view_ptr == nullptr) return;
    apply_merge_result(merge_view_ptr->result());
  }


  void CodeEditorPage::open_diff(std::string title, const gbcode::CompareOptions& options, std::string path) {
    // 特殊标签（差异/合并）互斥：同一时刻最多一个。开差异时关掉合并标签
    //（合并是处置动作，看完差异可从变更面板重新点开）。
    if (!merge_key_.value().empty()) {
      merge_key_.set({});
      merge_view_ptr = nullptr;
    }
    diff_pending_title_ = std::move(title);
    diff_pending_options_ = options;
    diff_pending_path_ = std::move(path);
    diff_key_.set("diff:" + diff_pending_path_ + "|" + diff_pending_title_);
    diff_wanted_ = true;
  }


  /// 打开一个**三方合并**标签（`base/ours/theirs` 三栏 + 逐段取舍）。
  ///
  /// 与差异视图并列、同样住在编辑区（见 `build` 里的互斥顺序）。
  void CodeEditorPage::open_merge(std::string path) {
    // 与 `open_diff` 对称：开合并时关掉差异标签（特殊标签互斥）。
    if (!diff_key_.value().empty()) {
      diff_key_.set({});
      diff_title_.set({});
      diff_file_.set(st::text::FileDiff{});
    }
    merge_pending_path_ = std::move(path);
    merge_path_.set(merge_pending_path_);
    merge_key_.set("merge:" + merge_pending_path_);
    merge_loaded_ = false;
    merge_wanted_ = true;
  }


  /// 把冲突文件的合结果写回磁盘（经撤销栈与行尾保持——见 `CodeEditor` 的写盘路径）。
  ///
  /// **必须经 `run_write` 之外的路径**：这不是 git 写操作，是普通的文件保存
  ///（内容写完后由用户自己 `git add`，那一步再走 git 通道——两件事分开，
  /// 出问题时能看出是哪一步错）。
  void CodeEditorPage::apply_merge_result(const std::string& text) {
    if (merge_pending_path_.empty() || workspace_.empty()) return;
    const std::string full = st::fs::join(workspace_, merge_pending_path_);
    if (auto written = st::fs::write_text(full, text); !written) {
      status_.set("写入合并结果失败：" + merge_pending_path_);
      return;
    }
    status_.set(std::format("已写入合并结果：{}（{} 字符，下一步 git add）", merge_pending_path_,
                            text.size()));
    // 写盘后重新取快照：变更面板要多出「已解决待暂存」那一条。
    git_refresh_pending_ = true;
    merge_loaded_ = false;
  }


  /// 逐行暂存：把差异视图里勾选的行构造成补丁并提交到暂存区。
  ///
  /// ⚠ 两个容易错的地方，都在这里处理：
  /// ① **补丁从视图现取**（`diff_view_ptr->build_stage_patch()`）——它在选择变化后
  ///    才准，缓存会拿到过期的选择；
  /// ② 补丁为空时**如实报状态**（`build_partial_patch` 在"只选了上下文行"时返回空，
  ///    而那正是"选了但不产生改动"的情形，硬发一条命令会让 git 报无关的错）。
  void CodeEditorPage::stage_selected_diff_lines(bool unstage) {
    if (diff_view_ptr == nullptr) {
      status_.set("逐行暂存：差异视图不在（先打开一个文件差异）");
      return;
    }
    const std::string patch = diff_view_ptr->build_stage_patch();
    if (patch.empty()) {
      status_.set("逐行暂存：选中的行不足以形成改动（只选上下文行不产生变化）");
      return;
    }
    gbcode::WriteIntent intent;
    intent.op = unstage ? gbcode::WriteOp::UnstagePatch : gbcode::WriteOp::StagePatch;
    intent.patch = patch;
    if (run_write(intent, unstage ? "unstage-partial" : "stage-partial")) {
      status_.set(std::format("{} {} 行（{}）", unstage ? "已取消暂存" : "已暂存",
                              diff_view_ptr->selected_count(), diff_pending_path_));
      // 暂存后选择**保留**：用户可以继续挑下一批（清空会让人以为"操作没生效"）。
    }
  }


  void CodeEditorPage::unstage_selected_diff_lines() { stage_selected_diff_lines(true); }


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
  bool CodeEditorPage::run_write(const gbcode::WriteIntent& intent, std::string_view label) {
    if (workspace_.empty()) return false;
    const auto args = gbcode::write_args(intent);
    if (!args.has_value()) {
      status_.set(std::format("操作参数不合法（{}）——已拒绝执行",
                              label.empty() ? std::string("缺少必填项") : std::string(label)));
      return false;
    }
    // 部分暂存：补丁要先**落盘**再交给 `git apply`（写操作统一经终端命令通道，
    // 那是一条命令字符串、喂不了 stdin）。落盘失败就**不执行**——
    // 宁可不做，也不要让 git 去读一个不存在的补丁。
    if (intent.op == gbcode::WriteOp::StagePatch ||
        intent.op == gbcode::WriteOp::UnstagePatch) {
      const std::string patch_path = st::fs::join(workspace_, ".git/st-stage.patch");
      if (auto written = st::fs::write_text(patch_path, intent.patch); !written) {
        status_.set("部分暂存失败：补丁写盘被拒（" + patch_path + "）");
        return false;
      }
    }
    const std::string display = gbcode::to_display_line(*args);
    (void)display;
    // 不可逆操作：**先把备份分支建好再动手**。备份失败就不执行
    //（宁可不做，也不要“做了但没退路”——备份的成本是一次 `branch`）。
    if (gbcode::needs_backup(intent)) {
      const std::string backup = gbcode::backup_branch_name(workspace_);
      queue_terminal_command("git branch " + gbcode::shell_quote(backup));
      (void)gbcode::append_audit(workspace_, "backup", {"branch", backup}, "issued");
      status_.set(std::format("已建备份分支 {}（回滚：git reset --hard {}）", backup, backup));
    }
    queue_terminal_command(gbcode::to_shell_line(*args));
    (void)gbcode::append_audit(workspace_, label.empty() ? "write" : std::string(label), *args,
                               "issued");
    git_refresh_pending_ = true;
    // 立即自增快照版本：`refresh_git()` 要到“终端安静”或上限到点才跑，
    // 而界面上“点完没任何反应”是最差的反馈——先让依赖它的 scope 脏一次，
    // 数据到了再脏一次（两次都只是重组，代价可忽）。
    git_revision_.set(git_revision_.value() + 1);
    return true;
  }


  /// 暂存/取消暂存**整个分组**（分组级批量：未暂存/未跟踪 → 暂存；已暂存 → 取消）。
  ///
  /// 为什么按分组而不管目录树：目录树要按“选中路径集”逐个发命令，
  /// 而这一层的真实需求是“这一组我全要/全不要”——把它做成一次命令，
  /// 用户在终端里看到的也是一条完整命令（而不是一串）。
  void CodeEditorPage::stage_group(gbcode::ChangeGroup group, bool stage) {
    for (const auto& change : git_.changes_in(group)) {
      git_stage(change.path, stage);
    }
  }


  /// 放弃**全部**未暂存改动（危险：要确认，且会自动建备份分支）。
  void CodeEditorPage::ask_discard_all() {
    ask_write({.op = gbcode::WriteOp::DiscardAll}, "放弃全部未暂存改动",
              "工作区里所有未暂存修改会回到已暂存/已提交的状态。");
  }


  /// 提交并推送。
  ///
  /// 分成**两条命令**（提交、推送）而不拼成 `git commit && git push`：
  /// 提交失败时不该推送（`&&` 天然保证），但拼成一行后审计里只剩一条
  /// “commit && push”——出问题时看不出是哪一步失败。
  void CodeEditorPage::commit_and_push() {
    const std::string message = commit_message_.value();
    if (st::trim(message).empty()) {
      status_.set("提交信息为空");
      return;
    }
    if (terminal_ptr == nullptr || workspace_.empty()) return;
    gbcode::WriteIntent commit{.op = gbcode::WriteOp::Commit, .message = message};
    commit.amend = commit_amend_.value();
    commit.author = commit_author_.value();
    if (!run_write(commit, "commit")) return;
    const std::string branch = git_upstream_.empty() ? git_.branch : std::string{};
    (void)run_write({.op = gbcode::WriteOp::Push, .name = branch}, "push");
    commit_message_.set("");
    commit_amend_.set(false);
    status_.set("已提交并推送");
  }


  /// 暂存/取消暂存一个文件（写操作：交终端作业通道——输出实时回流、可中止）。
  void CodeEditorPage::git_stage(const std::string& path, bool stage) {
    (void)run_write({.op = stage ? gbcode::WriteOp::Stage : gbcode::WriteOp::Unstage,
                     .path = path},
                    stage ? "stage" : "unstage");
  }


  /// 放弃一个文件的更改（危险操作：先确认——`git restore`/`checkout` 不可恢复）。
  void CodeEditorPage::git_discard(const gbcode::GitChange& change) {
    if (change.is_untracked()) {
      git_confirm_title_ = std::format("删除未跟踪文件 {}", change.path);
      git_confirm_body_ = "文件没有进过版本库，删除后无法恢复。确定删除？";
      pending_git_confirm_ = [this, path = change.path] {
        // 未跟踪文件的删除不走 git（它不在索引里），但同样要审计。
        queue_terminal_command("rm -- " + gbcode::shell_quote(path));
        (void)gbcode::append_audit(workspace_, "rm-untracked", {"rm", path}, "issued");
        git_refresh_pending_ = true;
      };
      git_confirm_open_.set(true);
      return;
    }
    const std::string scope =
        change.has_staged_side() && !change.has_unstaged_side() ? "暂存的修改" : "未暂存的修改";
    git_confirm_title_ = std::format("放弃 {}（{}）", scope, change.path);
    git_confirm_body_ = "改动将不可恢复地丢失（git restore）。确定放弃？";
    pending_git_confirm_ = [this, path = change.path,
                            staged_side = change.has_staged_side()] {
      // 两侧都有时先退暂存再还原工作区（一步做完，省得用户分两次）。
      if (staged_side) (void)run_write({.op = gbcode::WriteOp::Unstage, .path = path});
      (void)run_write({.op = gbcode::WriteOp::DiscardOne, .path = path}, "discard");
    };
    git_confirm_open_.set(true);
  }


  /// 提交（提交框 Enter）。
  ///
  /// 走终端而非同步 `process::run` 的理由与 `run_task` 相同：同步阻塞会把
  /// 界面卡住（大仓库提交能到秒级），而终端通道输出实时回流、可中止。
  ///
  /// **不再 `git add -A` 再提交**（旧版的行为）：那会把用户没勾选的文件一并提交。
  /// 现在“暂存什么就提交什么”——没暂存任何东西时才回退到“全提交”并**在状态栏说清**。
  void CodeEditorPage::git_commit() {
    const std::string message = commit_message_.value();
    if (st::trim(message).empty()) {
      status_.set("提交信息为空");
      return;
    }
    if (terminal_ptr == nullptr || workspace_.empty()) return;
    if (!commit_message_history_.empty() &&
        commit_message_history_.front() != message) {
      commit_message_history_.insert(commit_message_history_.begin(), message);
      if (commit_message_history_.size() > 20) commit_message_history_.pop_back();
    }
    gbcode::WriteIntent intent{.op = gbcode::WriteOp::Commit, .message = message};
    intent.amend = commit_amend_.value();
    intent.author = commit_author_.value();
    const bool amending = commit_amend_.value();
    if (amending) {
      // 修正会**改写 HEAD**（已推送时是历史改写）——走确认口。
      ask_write(intent, "修正上一笔提交（--amend）",
                "会改写当前 HEAD 那笔提交（若已推送，需强推才能同步远端）。");
    } else {
      (void)run_write(intent, "commit");
    }
    commit_message_.set("");
    commit_amend_.set(false);
    status_.set(std::format("已{}：{}", amending ? "发起修正" : "提交",
                            std::string(st::trim(message)).substr(0, 40)));
  }


  /// 展开一个变更文件：与 HEAD 的差异**跑进终端**（只读 `git diff`）。
  ///
  /// 为何不再开一个“输出面板”：差异是**命令的输出**，而命令的输出去处只有一个。
  /// 有两处就会产生“同一个 `git diff` 一会儿在这里、一会在那里”的困惑。
  // —— 运行任务：真跑 `st`（走终端的作业通道，输出实时回流）——
  //
  // 为何搬到终端后面：以前这里是同步 `st::process::run` + 写 `output_`，
  // 于是跑 `st test` 的几十秒里界面假死、看不到任何进展。现在它只是
  // “把命令交给终端”——输出逐行出现，中止按钮真的能杀进程。
  void CodeEditorPage::run_task(const std::string& id) {
    if (id == "build") {
      // 真终端里应用只能**代敲**（自己不是那条流的写者）：把命令当输入送给 shell。
      if (terminal_ptr != nullptr) terminal_ptr->run("st build gallery --profile dev");
    } else if (id == "test") {
      if (terminal_ptr != nullptr) terminal_ptr->run("st test");
    } else {
      if (terminal_ptr != nullptr) terminal_ptr->run("st lint");
    }
  }


  /// 枚举运行时已注册的语言（扩展视图）。
  void CodeEditorPage::refresh_languages() {
    languages_ = CodeEditor::available_languages();
    status_.set(std::format("已注册 {} 种语言", languages_.size()));
  }


  /// 重新扫描工作区树（资源管理器的刷新按钮）。
  void CodeEditorPage::refresh_tree() {
    dir_cache_.clear();   // 显式刷新：目录缓存全部作废（磁盘可能已变化）
    dir_expanded_.set(dir_expanded_.value());   // 触发一次重组，树数据本就每帧重扫
    status_.set(workspace_.empty() ? "未打开工作区" : "已刷新工作区树");
  }


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
  void CodeEditorPage::switch_workspace(const std::string& path) {
    if (path.empty()) return;
    if (!st::fs::exists(path) || !st::fs::is_directory(path)) {
      status_.set("不是目录：" + path);
      return;
    }
    workspace_ = path;
    gbcode::push_recent(recent_folders_, path);   // 最近打开的文件夹（菜单/下次恢复共用）
    dialog_start_dir_.clear();   // 换工作区：文件对话框的"上次目录"跟着重置
    lsp_.set_workspace(path);    // 语言服务的根跟着换（旧索引属于旧项目）
    dir_expanded_.set(std::vector<std::string>{});   // 新根：展开状态重来
    dir_cache_.clear();          // 缢存属于旧根，一并作废
    refresh_tree();
    const std::size_t slash = path.find_last_of("/\\");
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    status_.set("工作区：" + (name.empty() ? path : name));
    push_title();   // 标题栏跟着换（窗口标题是“当前在哪”的第一指示）
  }




  /// 新建文件：有工作区时开文件对话框选路径；无工作区时开一份内存缓冲（`untitled-N`）。
  ///
  /// 为何内存缓冲这一档要保留：它是**真实功能**而不是演示占位——VSCode/IDEA 同样
  /// 允许“先写点东西、保存时再决定放哪里”。无工作区时若只回一句“需要 --workspace”，
  /// 那“新建文件”这个入口就是死的。
  void CodeEditorPage::new_file_prompt() {
    if (workspace_.empty()) {
      auto list = buffers_.value();
      stash_active_text(list);
      const std::size_t index = untitled_count_++;
      OpenBuffer buffer;
      buffer.key = std::format("untitled-{}", index);
      buffer.label = std::format("untitled-{}.txt", index);
      buffer.language = "text";
      buffer.text = "";
      list.push_back(std::move(buffer));
      buffers_.set(std::move(list));
      active_.set(buffers_.value().size() - 1);
      load_active_text();
      status_.set("已新建内存缓冲（保存时可用另存为选路径）");
      return;
    }
    pending_open_mode_ = 2;
    new_file_open_.set(true);
  }


  // —— 资源管理器的文件操作（右键菜单）：新建文件 / 新建文件夹 / 重命名 / 删除 ——
  //
  // 全部走 `st::fs`（UTF-8 路径统一入口），**全部要确认**（删除）或**全部可撤销**
  //（新建/重命名是即时动作，靠状态栏与树刷新反馈）。
  //
  // 右键菜单的目标行由 `Tree::on_context_menu` 给出（**不改选中**）；
  // 空 key = 点在空白处，作用域回退到工作区根 / 当前选中项。


  /// 把树里的 key 换成磁盘绝对路径。
  ///
  /// 两种 key 口径（见 `workspace_nodes`）：
  /// - 目录行 `dir:<相对路径>`（相对于工作区根）；
  /// - 文件行**已经是绝对路径**（历史契约：`open_path(key)` 直接当路径用）。
  ///
  /// 初版把两种都当相对路径拼，于是文件行的目标变成 `工作区/工作区/.../file`
  /// （实测：菜单显示的删除目标路径重复了两遍）。归一化就在这里做**一处**——
  /// 所有需要绝对路径的地方都调它，不再各自拼。
  [[nodiscard]] auto CodeEditorPage::tree_key_to_path(std::string_view key) const -> std::string {
    if (workspace_.empty() || key.empty()) return {};
    std::string rel(key);
    if (rel.rfind("dir:", 0) == 0) rel.erase(0, 4);
    if (st::fs::is_absolute(rel)) return rel;   // 文件行：已是绝对路径
    return st::fs::join(workspace_, rel);
  }


  /// 新建条目的目标目录：目录行取它自己，文件行取它的父目录，空 key 取工作区根。
  [[nodiscard]] auto CodeEditorPage::context_directory() const -> std::string {
    const std::string key = context_key_.value();
    if (workspace_.empty()) return {};
    if (key.empty()) return workspace_;
    const std::string path = tree_key_to_path(key);
    if (key.rfind("dir:", 0) == 0) return path;
    return st::fs::parent(path);
  }


  /// 在“右键处的目录”下新建一个条目（文件或目录），命名用浮层输入。
  void CodeEditorPage::explorer_create(bool directory) {
    const std::string base = context_directory();
    if (base.empty()) {
      status_.set("需要打开一个文件夹才能新建");
      return;
    }
    fs_action_kind_.set(directory ? FsAction::NewFolder : FsAction::NewFile);
    fs_action_target_.set(base);
    fs_action_open_.set(true);
    fs_focus_pending_ = true;
  }


  /// 重命名右键那一行（文件或目录）。
  void CodeEditorPage::explorer_rename() {
    const std::string path = tree_key_to_path(context_key_.value());
    if (path.empty()) {
      status_.set("请右键一个文件或文件夹");
      return;
    }
    fs_action_kind_.set(FsAction::Rename);
    fs_action_target_.set(path);
    fs_action_open_.set(true);
    fs_focus_pending_ = true;
  }


  /// 删除右键那一行（**不可恢复**，先确认）。
  void CodeEditorPage::explorer_delete() {
    const std::string path = tree_key_to_path(context_key_.value());
    if (path.empty()) {
      status_.set("请右键一个文件或文件夹");
      return;
    }
    fs_action_kind_.set(FsAction::Delete);
    fs_action_target_.set(path);
    fs_action_open_.set(true);
  }


  /// 执行已确认的文件系统动作（浮层提交/确认后调用）。
  ///
  /// `name` 只在新建/重命名时用（删除走确认，不看名字）。
  void CodeEditorPage::submit_fs_action(std::string_view name) {
    const FsAction action = fs_action_kind_.value();
    const std::string target = fs_action_target_.value();
    fs_action_open_.set(false);
    fs_action_kind_.set(FsAction::None);
    if (action == FsAction::None) return;
    if (action == FsAction::Delete) {
      const auto removed = st::fs::remove_all(target);
      status_.set(removed.has_value() ? std::format("已删除：{}", target)
                                      : std::format("删除失败：{}", removed.error().message));
      refresh_tree();
      return;
    }
    const std::string wanted(name);
    if (wanted.empty()) {
      status_.set("名字为空，已取消");
      return;
    }
    // 名字必须是**单个名字**：`a/b` / `..` / 绝对路径都是路径而非名字。
    // 不拦的话会造出用户没预期的目录与文件（`a/b` 会隐式建 `a/`）。
    if (wanted.find('/') != std::string::npos || wanted.find('\\') != std::string::npos) {
      status_.set("名字不能包含路径分隔符：" + wanted);
      return;
    }
    if (wanted == "." || wanted == "..") {
      status_.set("非法名字：" + wanted);
      return;
    }
    std::string path;
    if (action == FsAction::Rename) {
      path = st::fs::join(st::fs::parent(target), wanted);
    } else {
      path = st::fs::join(target, wanted);
    }
    // 新建同名文件会**清空**已存在的文件（`write_text(path, "")`）——
    // 这是不可逆的数据丢失，必须当场拒绝而不是默默执行。
    if (action == FsAction::NewFile && st::fs::exists(path)) {
      status_.set("已存在同名文件（新建会清空它，已拒绝）：" + path);
      return;
    }
    if (action == FsAction::NewFolder && st::fs::exists(path)) {
      status_.set("已存在同名目录：" + path);
      return;
    }
    if (action == FsAction::Rename && st::fs::exists(path)) {
      status_.set("目标名字已被占用：" + path);
      return;
    }
    st::Status result = st::ok();
    if (action == FsAction::NewFolder) {
      result = st::fs::create_directories(path);
    } else if (action == FsAction::NewFile) {
      result = st::fs::write_text(path, "");
    } else if (action == FsAction::Rename) {
      result = st::fs::rename(target, path);
    }
    if (!result) {
      status_.set(std::format("操作失败：{}", result.error().message));
      return;
    }
    status_.set(std::format("已{}：{}", action == FsAction::Rename ? "重命名" : "新建", path));
    // 新建后把上级目录展开（否则用户看不到刚建的东西）；重命名只刷新。
    if (action == FsAction::NewFolder || action == FsAction::NewFile) {
      auto expanded = dir_expanded_.value();
      const std::string rel = st::fs::relative_to(target, workspace_);
      if (!rel.empty() && rel != "." &&
          std::find(expanded.begin(), expanded.end(), rel) == expanded.end()) {
        expanded.push_back(rel);
      }
      dir_expanded_.set(std::move(expanded));
    }
    refresh_tree();
    // 新建的是文件就顺手打开（“建完就能写”比“建完还要再点一下”省一步）。
    if (action == FsAction::NewFile) open_path(path);
  }


  // —— 搜索命中的跳转（打开文件 + 选中命中）——
  /// 点搜索命中 → 打开文件并跳到那一行 + 选中命中词。
  void CodeEditorPage::activate_search_hit(std::size_t index) {
    if (index >= search_hit_paths_.size() || search_hit_paths_[index].empty()) return;
    const std::size_t line = index < search_hit_lines_.size() ? search_hit_lines_[index] : 1;
    const std::size_t column = index < search_hit_columns_.size() ? search_hit_columns_[index] : 0;
    const std::string path = search_hit_paths_[index];
    open_path(path);
    pending_jump_ = {line, column};
    if (editor != nullptr) {
      apply_jump(line, column);
      pending_jump_ = {0, 0};
    }
  }


  void CodeEditorPage::apply_jump(std::size_t line, std::size_t column) {
    if (editor == nullptr) return;
    editor->goto_line(line);
    const std::size_t start = editor->cursor_index() + column;
    editor->set_selection(start, start + last_query_.size());
    editor->scroll_to_line(line);
    status_.set(std::format("已跳到第 {} 行", line));
  }


  // ════════════════════════════════════════════════════════════════════════
  // 终端：内置命令 + 白名单外部命令（长命令在工作线程上跑，输出逐块回流）
  // ════════════════════════════════════════════════════════════════════════


  /// 每帧泵一次终端（应用主循环调；见 `pump()`）。
  void CodeEditorPage::pump_terminal() {
    if (terminal_ptr != nullptr) terminal_ptr->pump();
  }


  /// 退出前收尾：中止还在跑的会话（否则子进程会变成孤儿）。
  void CodeEditorPage::shutdown_terminal() {
    if (terminal_ptr != nullptr && terminal_ptr->any_busy()) terminal_ptr->send_stop();
  }


  /// 退出前收尾：关掉语言服务器（否则 clangd 会变成孤儿进程，占着索引缓存）。
  void CodeEditorPage::shutdown_language_service() { lsp_.shutdown(1500); }


  // ————————————————————————————————————————————————————————————————————————
  // 补全（LSP 阶段 4）
  // ————————————————————————————————————————————————————————————————————————


  /// 手动触发补全（Ctrl+Space）。
  void CodeEditorPage::trigger_completion() {
    if (editor == nullptr) return;
    const auto list = buffers_.value();
    const std::size_t active = active_.value();
    if (active >= list.size() || list[active].path.empty()) {
      status_.set("补全需要磁盘上的文件（语言服务按路径索引）");
      return;
    }
    if (!lsp_.active()) {
      // 未就绪：**如实说明**（"没反应"会让用户以为快捷键坏了）。
      status_.set(lsp_.error().empty() ? "语言服务未就绪" : lsp_.error());
      return;
    }
    request_completion_now(editor->text());
  }


  /// 发补全请求（`text` 用于算"光标前的词"作为兜底替换区间）。
  void CodeEditorPage::request_completion_now(std::string_view text) {
    if (editor == nullptr || !lsp_.active()) return;
    const auto list = buffers_.value();
    const std::size_t active = active_.value();
    if (active >= list.size() || list[active].path.empty()) return;
    const std::size_t cursor = editor->cursor_index();
    const auto [begin, end] = st::lsp::word_range_before_cursor(text, cursor);
    completion_word_begin_ = begin;
    (void)end;
    const auto position = lsp_.offset_to_position(list[active].path, cursor);
    if (!position.has_value()) return;
    completion_request_ = lsp_.request_completion(list[active].path,
                                                  static_cast<std::uint32_t>(position->line),
                                                  static_cast<std::uint32_t>(position->character));
  }


  /// 更新弹层锚点（光标矩形；弹层贴它下方）。
  void CodeEditorPage::update_completion_anchor() {
    if (editor == nullptr) return;
    completion_anchor_ = editor->caret_screen_rect();
  }


  /// 接受补全：用 `insert` 替换光标前的词（或已输入前缀）。
  void CodeEditorPage::accept_completion(const std::string& insert) {
    completion_open_.set(false);
    if (editor == nullptr) return;
    const std::size_t cursor = editor->cursor_index();
    // 替换区间 = `[completion_word_begin_, cursor)`：这正是"已输入的词"。
    // 用 `set_selection` + `insert_text` 两步（编辑器的一等接口，走撤销栈）。
    const std::size_t begin = std::min(completion_word_begin_, cursor);
    editor->set_selection(begin, cursor);
    editor->insert_text(insert);
    on_edit();
    status_.set("已补全：" + insert);
  }


  /// 补全响应处理（挂在 `lsp_.on_completion`）。
  void CodeEditorPage::on_completion_response(std::int64_t request_id,
                              const std::vector<st::lsp::CompletionEntry>& entries, bool) {
    // 旧响应丢弃：输入变了之后旧候选已经指错位置（插错内容比不补更坏）。
    if (request_id != completion_request_) return;
    completion_items_ = entries;
    if (entries.empty()) {
      completion_open_.set(false);
      return;
    }
    update_completion_anchor();
    completion_open_.set(true);
  }


  /// 补全详情响应。
  void CodeEditorPage::on_completion_detail_response(std::int64_t, const std::string& detail) {
    if (detail.empty()) return;
    // 详情进弹层（下次构建时读 `completion_items_` 的 documentation）。
    if (completion_open_.value()) {
      // 简化：把详情存进当前选中项（弹层每帧重建，读的是同一份数据）。
      (void)detail;
    }
  }



  // ————————————————————————————————————————————————————————————————————————
  // 导航（LSP 阶段 5）：定义 / 引用 / 悬停 / 大纲
  // ————————————————————————————————————————————————————————————————————————


  /// 当前活动文件的可读信息（路径 + 光标位置）。无则返回 false。
  auto CodeEditorPage::current_document(std::string* path, st::lsp::Position* position) -> bool {
    if (editor == nullptr) return false;
    const auto list = buffers_.value();
    const std::size_t active = active_.value();
    if (active >= list.size() || list[active].path.empty()) return false;
    if (!lsp_.active()) return false;
    const auto found = lsp_.offset_to_position(list[active].path, editor->cursor_index());
    if (!found.has_value()) return false;
    *path = list[active].path;
    *position = *found;
    return true;
  }


  /// 是否处于"可跳转"状态（供状态栏/命令面板判断可用性）。
  [[nodiscard]] auto CodeEditorPage::navigation_ready() const -> bool {
    if (editor == nullptr || !lsp_.active()) return false;
    const auto list = buffers_.value();
    return active_.value() < list.size() && !list[active_.value()].path.empty();
  }


  /// F12：跳到定义。
  void CodeEditorPage::goto_definition() {
    std::string path;
    st::lsp::Position position{};
    if (!current_document(&path, &position)) {
      status_.set(lsp_.active() ? "需要磁盘上的文件才能跳转定义" : "语言服务未就绪");
      return;
    }
    if (lsp_.request_definition(path, static_cast<std::uint32_t>(position.line),
                                static_cast<std::uint32_t>(position.character)) == 0) {
      status_.set("定义请求未发出");
    }
  }


  /// 导航族通用发射器：声明 / 类型定义 / 实现（与 F12 同一套前置校验）。
  ///
  /// 单独一个函数而不是三份拷贝：前置校验与状态栏文案的口径必须一致，
  /// 复制三份迟早会漂（改一处忘两处）。
  void CodeEditorPage::goto_navigation_family(const char* family, const char* label) {
    std::string path;
    st::lsp::Position position{};
    if (!current_document(&path, &position)) {
      status_.set(lsp_.active() ? std::format("需要磁盘上的文件才能{}", label)
                                : "语言服务未就绪");
      return;
    }
    const auto line = static_cast<std::uint32_t>(position.line);
    const auto character = static_cast<std::uint32_t>(position.character);
    std::int64_t sent = 0;
    if (std::string_view(family) == "declaration") sent = lsp_.request_declaration(path, line, character);
    else if (std::string_view(family) == "typeDefinition") sent = lsp_.request_type_definition(path, line, character);
    else if (std::string_view(family) == "implementation") sent = lsp_.request_implementation(path, line, character);
    if (sent == 0) status_.set(std::format("{}请求未发出", label));
  }


  /// 跳到声明（C/C++：.cpp 用 F12 常落头文件声明；这里显式请求声明族）。
  void CodeEditorPage::goto_declaration() { goto_navigation_family("declaration", "跳到声明"); }

  /// 跳到类型定义（光标在变量上时跳它的类型声明处）。
  void CodeEditorPage::goto_type_definition() { goto_navigation_family("typeDefinition", "跳到类型定义"); }

  /// 跳到实现（Ctrl+F12；接口→实现）。
  void CodeEditorPage::goto_implementation() { goto_navigation_family("implementation", "跳到实现"); }


  /// Shift+F12：找引用。
  void CodeEditorPage::find_references() {
    std::string path;
    st::lsp::Position position{};
    if (!current_document(&path, &position)) {
      status_.set(lsp_.active() ? "需要磁盘上的文件才能查引用" : "语言服务未就绪");
      return;
    }
    // `include_declaration = false`：找"谁用了它"。含声明的话第一项永远是定义处，
    // 每次都要跳过它——那是"引用列表"该有的形态吗？VSCode 默认含，但它把定义
    // 单独标注；我们没有那个标注能力，所以不含更干净。
    (void)lsp_.request_references(path, static_cast<std::uint32_t>(position.line),
                                  static_cast<std::uint32_t>(position.character), false);
  }


  /// 页面缩放（查看菜单 / Ctrl+= · Ctrl+- · Ctrl+0）。
  ///
  /// `direction`：+1 放大、-1 缩小、0 重置到 100%。
  /// 实现就是 **DPI 缩放**——见 `device_scale_setter` 的说明：霜天全自绘，
  /// `device_scale` 同时决定缓冲尺寸、文本超采样与逻辑像素的物理大小，
  /// 它本身就是"页面缩放"，另造一个缩放因子只会得到两条并行链路。
  void CodeEditorPage::zoom_page(int direction) {
    if (!device_scale_setter || !device_scale_getter) {
      status_.set("当前环境不支持页面缩放");
      return;
    }
    const float current = device_scale_getter();
    float target = current;
    if (direction == 0) {
      target = 1.0f;
    } else {
      // 在档位表里找到"当前值所在的位置"再前/后移一格：
      // 连续缩放（×1.1 这种）在 DPI 下会让字形亚像素相位每档都不同，
      // 观感在"糊/锐"之间来回跳；档位化后每档都是稳定的。
      const auto& levels = kZoomLevels;
      std::size_t index = 0;
      float best = std::numeric_limits<float>::max();
      for (std::size_t i = 0; i < std::size(levels); ++i) {
        if (const float distance = std::abs(levels[i] - current); distance < best) {
          best = distance;
          index = i;
        }
      }
      if (direction > 0) {
        if (index + 1 < std::size(levels)) target = levels[index + 1];
      } else if (index > 0) {
        target = levels[index - 1];
      }
    }
    if (std::abs(target - current) < 0.001f) {
      status_.set(std::format("页面缩放已是 {:.0f}%", target * 100.0f));
      return;
    }
    if (!device_scale_setter(target)) {
      status_.set("页面缩放失败（当前后端不支持）");
      return;
    }
    status_.set(std::format("页面缩放 {:.0f}%", target * 100.0f));
  }



  /// 文档大纲：把符号列表放进 `symbols_`（供面板/命令面板显示）。
  void CodeEditorPage::refresh_document_symbols() {
    const auto list = buffers_.value();
    if (active_.value() >= list.size() || list[active_.value()].path.empty()) {
      symbols_.set({});
      symbols_path_.clear();
      outline_fetch_pending_ = false;
      return;
    }
    // ⚠ LSP 可能还在握手中（`active()` = started && Ready）。旧写法在这里
    // `symbols_.clear()` 后直接返回——于是「打开文件」那一刻拍的大纲请求被
    // **静默吞掉**，而且再也没有人补发（要等用户手点「刷新大纲」）。
    // 实测：打开 .cpp 后大纲恒空，点一次刷新才出来。
    // 现在改成"挂起 + 每帧补试"：握手一完成就自动拉到。
    outline_fetch_pending_ = true;
    pump_outline_fetch();
  }


  /// LSP 就绪后补拉大纲（每帧泵调用；未就绪时什么也不做）。
  void CodeEditorPage::pump_outline_fetch() {
    if (!outline_fetch_pending_) return;
    const auto list = buffers_.value();
    if (active_.value() >= list.size() || list[active_.value()].path.empty()) {
      outline_fetch_pending_ = false;
      return;
    }
    if (!lsp_.active()) return;   // 还在握手：保持挂起，下一帧再看
    outline_fetch_pending_ = false;
    // ⚠ **必须先让 server 知道这个文档（didOpen），再请求符号**。
    //
    // 这条不是防御性冗余，而是本 bug 的最后一块：`sync_language_document` 里
    // 的 `sync_document` 有 `if (!started) return;`——**首帧**调用时 server 刚被
    // 拉起、还没就绪，于是 didOpen 根本没发出去。等握手完成后再请求 `documentSymbol`，
    // clangd 对"没打开过的文件"如实回**空数组**（协议语义：documentSymbol 只对
    // 已同步文档有效）。于是大纲恒空——而单独用标准 LSP 客户端先 didOpen 再请求，
    // 同一个文件能返回 16 个符号（`tools/lsp_symbols_probe.cpp` 就是这个对照）。
    //
    // 这里补一次 `sync_document`（它按 path 去重，已同步过就是 no-op），
    // 保证"请求符号时文档一定已打开"。
    const auto& buffer = list[active_.value()];
    lsp_.sync_document(buffer.path, buffer.text);
    outline_request_ = lsp_.request_document_symbols(buffer.path);
  }


  /// 工作区符号搜索（命令面板输入时调）。
  void CodeEditorPage::search_workspace_symbols(const std::string& query) {
    if (!lsp_.active()) return;
    (void)lsp_.request_workspace_symbols(query);
  }


  /// 跳到某个位置（定义/引用结果共用）：必要时先打开文件。
  ///
  /// 跳转前把「当前位置」压入历史栈（Alt+Left 回退用）——只在真发生文件/行
  /// 变化时压栈，重复跳同一处不堆积。
  void CodeEditorPage::jump_to_location(const st::lsp::Location& location) {
    const std::string path = st::lsp::LspClient::uri_to_path(location.uri);
    if (path.empty()) return;
    // 优先 `selection`（`LocationLink` 的标识符本身），退回 `range`。
    const st::lsp::Range range =
        location.selection.has_value() ? *location.selection : location.range;
    // 跳转历史：记录「从哪里跳」——光标位置 + 当时文件。
    if (editor != nullptr) {
      const NavPoint from{current_buffer_path(), editor->cursor_index(), editor->cursor_line()};
      // 去重：与栈顶相同才跳过（栈空时必须压——首跳没有历史，但「回到首跳前」
      // 恰恰是用户按 Alt+← 时想要的）。
      const bool same = !nav_history_.empty() && nav_history_.back().path == from.path &&
                        nav_history_.back().cursor == from.cursor;
      if (!same) nav_history_.push_back(from);
      if (nav_history_.size() > 64) nav_history_.erase(nav_history_.begin());   // 有界
    }
    if (path != current_buffer_path()) {
      open_path(path);   // 换文件（`open_path` 会切标签并装载）
    }
    // 跳转要**等编辑器装载完**（`set_text` 在重组时发生）——所以用 pending 队列，
    // 与既有的 `pending_jump_` 同一套路（帧首落地）。
    pending_jump_ = {range.start.line + 1, range.start.character + 1};
    pending_jump_pending_ = true;
  }


  /// 跳转历史回退（Alt+Left）：回到上一处跳转起点。
  void CodeEditorPage::navigate_back() {
    while (!nav_history_.empty()) {
      NavPoint point = nav_history_.back();
      nav_history_.pop_back();
      if (point.path.empty()) continue;
      if (point.path != current_buffer_path()) open_path(point.path);
      pending_jump_ = {point.line + 1, 0};
      pending_jump_pending_ = true;
      status_.set(std::format("已返回：{} 第 {} 行", point.path, point.line + 1));
      return;
    }
    status_.set("没有可返回的跳转");
  }


  /// 导航结果面板里点某一条：跳过去（同一套 `jump_to_location`，历史同样入栈）。
  void CodeEditorPage::open_nav_result(std::size_t index) {
    if (index >= nav_results_.size()) return;
    jump_to_location(nav_results_[index].location);
    status_.set(std::format("第 {}/{} 处", index + 1, nav_results_.size()));
  }


  /// 当前活动缓冲的路径（无则空）。
  [[nodiscard]] auto CodeEditorPage::current_buffer_path() const -> std::string {
    const auto list = buffers_.value();
    return active_.value() < list.size() ? list[active_.value()].path : std::string{};
  }


  /// 位置结果 → 状态栏摘要（带计数，让用户知道"找到几处"）。
  ///
  /// 多处结果：除跳到第一处外，还把「位置列表」写进侧栏搜索视图可复用的
  /// 结果面板状态（`nav_results_`），切到活动栏「大纲」可逐个点。
  void CodeEditorPage::on_locations_response(std::int64_t id, const std::vector<st::lsp::Location>& locations) {
    (void)id;
    const std::string family = lsp_.last_navigation_method();
    if (locations.empty()) {
      status_.set(family == "references" ? "没有找到引用" : "没有找到结果");
      nav_results_.clear();
      return;
    }
    if (locations.size() == 1) {
      jump_to_location(locations.front());
      status_.set("已跳转：" + st::lsp::LspClient::uri_to_path(locations.front().uri));
      nav_results_.clear();
      return;
    }
    // 多处：列进 `nav_results_` 供结果面板用，先跳到第一处。
    locations_ = locations;
    nav_results_.clear();
    nav_results_.reserve(locations.size());
    for (std::size_t index = 0; index < locations.size() && index < 200; ++index) {
      const auto& item = locations[index];
      const std::string path = st::lsp::LspClient::uri_to_path(item.uri);
      const st::lsp::Range range = item.selection.has_value() ? *item.selection : item.range;
      // 预览行文本：拉一次文件内容读目标行（有 IO 但只在结果落地时做一次）。
      std::string preview;
      if (const auto content = st::fs::read_text(path)) {
        std::size_t at = 0;
        std::size_t line = 0;
        while (at < content->size() && line < range.start.line) {
          const std::size_t nl = content->find('\n', at);
          if (nl == std::string::npos) break;
          at = nl + 1;
          ++line;
        }
        const std::size_t nl = content->find('\n', at);
        preview = std::string(st::trim(std::string_view(*content).substr(
            at, (nl == std::string::npos ? content->size() : nl) - at)));
        if (preview.size() > 120) preview = preview.substr(0, 120) + "…";
      }
      nav_results_.push_back(NavResult{.path = path,
                                       .line = range.start.line + 1,
                                       .column = range.start.character + 1,
                                       .preview = std::move(preview),
                                       .location = item});
    }
      // 多处结果自动切到**导航结果**视图（用户可随时切走）。
  // 该视图不纳入保活（内容一次查找即生成，没有"用户位置"可丢）——
  // 这里只切下标，不进 `sidebar_visited_`。
  // 切到**导航结果**视图（不是大纲）：多处命中是搜索结果，与文件结构分栏。
  if (activity_.value() != view_index(SidebarView::Nav)) {
    activity_.set(view_index(SidebarView::Nav));
  }
  jump_to_location(locations.front());
  status_.set(std::format("找到 {} 处{}，已跳到第 1 处（Alt+← 返回；其余在「导航结果」侧栏）",
                         locations.size(), family == "references" ? "引用" : ""));
  }


  /// 大纲响应。
  void CodeEditorPage::on_symbols_response(std::int64_t id, const std::vector<st::lsp::DocumentSymbol>& symbols) {
    (void)id;   // 符号请求是"最后一条为准"的语义：旧响应覆盖新响应也无害（内容同源）
    symbols_.set(symbols);
    const auto list = buffers_.value();
    symbols_path_ = active_.value() < list.size() ? list[active_.value()].path : std::string{};
    // 大纲已加载：`symbols_` 是 `State`，`set` 本身就会触发重组
    //（旧实现把它写成普通成员，异步响应到了也没人重画——大纲恒空）。
  }


  /// 工作区符号响应：填进 `workspace_symbols_`，供「转到工作区符号」面板用。
  ///
  /// 为何要开面板才能看：LSP 的响应是**异步**的（server 扫完整个工作区才回），
  /// 所以固定是“先开面板（空列表）→ 响应回来 → 重组把结果填进去”。
  void CodeEditorPage::on_workspace_symbols_response(std::int64_t,
                                     const std::vector<st::lsp::WorkspaceSymbol>& symbols) {
    workspace_symbols_ = symbols;
    symbols_kind_.set(SymbolKind::Workspace);
  }


  /// 「转到符号」面板的列表项：定义（当前文件大纲）/ 工作区符号看 `symbols_kind_`。
  ///
  /// 两类符号的形状不同（`DocumentSymbol` 带 range/selection_range、可能嵌套；
  /// `WorkspaceSymbol` 自带 location），但**面板只要“名字 + 去哪”**：
  /// 在这里归一化成同一种条目，面板侧就只有一条代码路径。
  [[nodiscard]] auto CodeEditorPage::symbol_commands() const -> std::vector<CommandPalette::Command> {
    std::vector<CommandPalette::Command> table;
    const auto flatten = [&](const auto& list, auto&& self, int depth) -> void {
      for (const auto& symbol : list) {
        const std::string indent(static_cast<std::size_t>(depth) * 2U, ' ');
        table.push_back(CommandPalette::Command{.id = std::format("symbol:{}", table.size()),
                                                .title = indent + symbol.name,
                                                .detail = symbol.detail});
        if constexpr (requires { symbol.children; }) {
          self(symbol.children, self, depth + 1);
        }
      }
    };
    if (symbols_kind_.value() == SymbolKind::Document) flatten(symbols_.value(), flatten, 0);
    else {
      for (const auto& symbol : workspace_symbols_) {
        table.push_back(CommandPalette::Command{
            .id = std::format("symbol:{}", table.size()),
            .title = symbol.name,
            .detail = st::lsp::LspClient::uri_to_path(symbol.uri)});
      }
    }
    return table;
  }


  /// 打开「转到符号」面板（公开面：入口在快捷键/菜单，与本页其它入口一致）。
  void CodeEditorPage::open_symbols(bool document) {
    if (document) {
      if (symbols_.value().empty()) {
        status_.set("当前文件没有可用的符号（语言服务未就绪？）");
      }
      symbols_kind_.set(SymbolKind::Document);
    } else {
      if (!lsp_.active()) {
        status_.set("语言服务未就绪，无法搜工作区符号");
        return;
      }
      workspace_symbols_.clear();
      symbols_kind_.set(SymbolKind::Workspace);
      // 空查询 = 让 server 返回它认为最相关的（大多数 server 需要非空查询才回结果，
      // 所以这里先给一个能匹配全部的通配——具体能搜到什么由 server 决定，**如实呈现**）。
      (void)lsp_.request_workspace_symbols("");
    }
    palette_shows_symbols_.set(true);
    palette_query_.set({});
    palette_open_.set(true);
  }


  /// 跳到第 `index` 个符号（面板列表下标，与 `symbol_commands` 同一顺序）。
  void CodeEditorPage::jump_to_symbol(std::size_t index) {
    if (symbols_kind_.value() == SymbolKind::Document) {
      std::size_t cursor = 0;
      const auto walk = [&](const auto& list, auto&& self) -> bool {
        for (const auto& symbol : list) {
          if (cursor == index) {
            jump_to_location(st::lsp::Location{
                .uri = st::lsp::LspClient::path_to_uri(current_buffer_path()),
                .range = symbol.range,
                .selection = std::nullopt});
            return true;
          }
          ++cursor;
          if constexpr (requires { symbol.children; }) {
            if (self(symbol.children, self)) return true;
          }
        }
        return false;
      };
      (void)walk(symbols_.value(), walk);
      return;
    }
    if (index < workspace_symbols_.size()) {
      const st::lsp::WorkspaceSymbol& symbol = workspace_symbols_[index];
      jump_to_location(st::lsp::Location{
          .uri = symbol.uri, .range = symbol.range, .selection = std::nullopt});
    }
  }



  // ————————————————————————————————————————————————————————————————————————
  // 编辑类动作（LSP 阶段 7）：格式化 / 重命名
  // ————————————————————————————————————————————————————————————————————————


  /// 构造格式化选项（`tabSize`/`insertSpaces` 必须与编辑器显示口径一致——
  /// 否则 server 严格按我们给的参数做出来的缩进，在编辑器里看起来是错的）。
  [[nodiscard]] auto CodeEditorPage::formatting_options() const -> st::lsp::FormattingOptions {
    st::lsp::FormattingOptions options{};
    options.tab_size = static_cast<std::uint32_t>(tab_width_);
    options.insert_spaces = true;   // 我们一律插空格（编辑器显示 Tab 也按 tab_width 展开）
    return options;
  }


  /// 格式化整篇（Shift+Alt+F，VSCode 同键位）。
  void CodeEditorPage::format_document() {
    std::string path;
    st::lsp::Position position{};
    if (!current_document(&path, &position)) {
      status_.set(lsp_.active() ? "需要磁盘上的文件才能格式化" : "语言服务未就绪");
      return;
    }
    format_request_ = lsp_.request_format(path, formatting_options());
    if (format_request_ == 0) status_.set("格式化请求未发出");
  }


  /// 格式化选中区（有选区时走这条，否则整篇）。
  void CodeEditorPage::format_selection() {
    if (editor == nullptr) return;
    std::string path;
    st::lsp::Position position{};
    if (!current_document(&path, &position)) {
      format_document();
      return;
    }
    if (!editor->has_selection()) {
      format_document();
      return;
    }
    const auto [begin, end] = editor->selection();
    const auto start = lsp_.offset_to_position(path, begin);
    const auto finish = lsp_.offset_to_position(path, end);
    if (!start.has_value() || !finish.has_value()) return;
    format_request_ = lsp_.request_range_format(
        path, static_cast<std::uint32_t>(start->line), static_cast<std::uint32_t>(start->character),
        static_cast<std::uint32_t>(finish->line), static_cast<std::uint32_t>(finish->character),
        formatting_options());
  }


  /// 格式化结果落地：应用到**当前缓冲**（格式化只针对一个文件）。
  void CodeEditorPage::on_format_response(std::int64_t id, const std::vector<st::lsp::TextEdit>& edits) {
    if (id != format_request_) return;   // 旧响应丢弃
    if (edits.empty()) {
      status_.set("已是格式化状态");
      return;
    }
    if (editor == nullptr) return;
    std::string text = editor->text();
    std::string error;
    if (!st::lsp::apply_edits_in_place(text, edits, &error)) {
      // 失败**如实说**（重叠/越界都可能——静默丢弃会让用户以为格式化没生效）。
      status_.set("格式化失败：" + error);
      return;
    }
    // 落回编辑器与缓冲区：走 `set_text` 会清撤销栈（格式化是"一步操作"，
    // 用户按 Ctrl+Z 期望整个格式化一起撤销——理想做法是压一条自定义撤销记录，
    // 但编辑器目前只支持文本快照撤销；先如实把光标保留在合理位置）。
    editor->set_text(text);
    auto list = buffers_.value();
    const std::size_t active = active_.value();
    if (active < list.size()) {
      list[active].text = text;
      list[active].dirty = true;   // 未保存（与手工编辑一致）
      buffers_.set(std::move(list));
    }
    refresh_problems();
    status_.set(std::format("已格式化（{} 处修改）", edits.size()));
  }


  /// 重命名符号（F2，VSCode 同键位）：先 prepareRename 拿当前名字。
  void CodeEditorPage::rename_symbol() {
    std::string path;
    st::lsp::Position position{};
    if (!current_document(&path, &position)) {
      status_.set(lsp_.active() ? "需要磁盘上的文件才能重命名" : "语言服务未就绪");
      return;
    }
    rename_path_ = path;
    (void)lsp_.request_prepare_rename(path, static_cast<std::uint32_t>(position.line),
                                      static_cast<std::uint32_t>(position.character));
  }


  /// 预备结果回来了：打开输入浮层（预填当前名字）。
  void CodeEditorPage::on_prepare_rename_response(std::int64_t,
                                  const std::optional<st::lsp::PrepareRenameResult>& prepared) {
    if (!prepared.has_value()) {
      // 区分"明确不可重命名"与"没预填但可以试"——后者 `has_value()` 为真。
      status_.set("此处没有可重命名的符号");
      return;
    }
    // 预填名字：**优先从文本里按 range 取**，`placeholder` 只是兜底。
    //
    // 为什么不能只依赖 `placeholder`：clangd 的 `prepareRename` **只给裸 Range、
    // 不给名字**（实测）——那样输入框会是空的，用户得把当前名字整个敲一遍，
    // 而"重命名"的常见动作是"改几个字"（`compute_area` → `rect_area`）。
    // 按 range 从当前文本切片是通用做法（server 给的 range 本来就指向那个标识符）。
    rename_prefill_.clear();
    if (editor != nullptr) {
      const std::string text = editor->text();
      const std::size_t begin = st::lsp::position_to_offset(text, prepared->range.start);
      const std::size_t end = st::lsp::position_to_offset(text, prepared->range.end);
      if (begin < end && end <= text.size()) rename_prefill_ = text.substr(begin, end - begin);
    }
    if (rename_prefill_.empty()) rename_prefill_ = prepared->placeholder;
    rename_text_.set(rename_prefill_);
    rename_field_seeded_ = false;   // 让浮层重新把预填写进输入框
    rename_open_.set(true);
    status_.set(rename_prefill_.empty() ? "输入新名字" : ("重命名：" + rename_prefill_));
  }


  /// 提交重命名（输入浮层 Enter）。
  void CodeEditorPage::submit_rename() {
    const std::string new_name = rename_text_.value();
    rename_open_.set(false);
    if (new_name.empty()) return;
    if (rename_prefill_ == new_name) {
      status_.set("名字未变");
      return;
    }
    std::string path = rename_path_;
    st::lsp::Position position{};
    if (path.empty() || !current_document(&path, &position)) return;
    (void)lsp_.request_rename(path, static_cast<std::uint32_t>(position.line),
                              static_cast<std::uint32_t>(position.character), new_name);
  }


  /// 重命名结果：**`WorkspaceEdit` 可能跨文件**——逐个文件应用。
  void CodeEditorPage::on_rename_response(std::int64_t, const st::lsp::WorkspaceEdit& edit) {
    if (!edit.skipped_resource_ops.empty()) {
      // 资源操作（建文件/改名/删文件）没实现：如实报告，不假装成功。
      status_.set(std::format("部分操作未支持：{}（共 {} 项）",
                              edit.skipped_resource_ops.front(),
                              edit.skipped_resource_ops.size()));
    }
    if (edit.files.empty()) {
      if (edit.skipped_resource_ops.empty()) status_.set("重命名没有产生修改");
      return;
    }
    // 逐个文件：打开缓冲里改文本，不在缓冲里的**读盘 → 改 → 写回**。
    //
    // 为什么要处理"不在缓冲里"的文件：改一个头文件里的函数名会同时命中
    // 所有调用点（包括没打开过的 .cpp）——只改打开着的会让工程处于半改状态，
    // 下次编译报一堆错，而用户不知道为什么。
    std::size_t touched = 0;
    std::size_t failed = 0;
    std::string last_error;
    auto list = buffers_.value();
    for (const auto& file : edit.files) {
      const std::string path = st::lsp::LspClient::uri_to_path(file.uri);
      if (path.empty() || file.edits.empty()) continue;
      // 在缓冲里？
      bool in_buffer = false;
      for (std::size_t index = 0; index < list.size(); ++index) {
        if (list[index].path != path) continue;
        in_buffer = true;
        std::string text = list[index].text;
        if (!st::lsp::apply_edits_in_place(text, file.edits, &last_error)) {
          ++failed;
          break;
        }
        list[index].text = text;
        list[index].dirty = true;
        ++touched;
        break;
      }
      if (in_buffer) continue;
      // 不在缓冲：读盘 → 应用 → 写回（**直接落盘**——重命名是"改工程"的操作，
      // 未打开的文件没有"未保存"的概念；停在那里只会让工程不一致）。
      const auto content = st::fs::read_text(path);
      if (!content.has_value()) {
        ++failed;
        last_error = "读不到 " + path;
        continue;
      }
      std::string text = *content;
      if (!st::lsp::apply_edits_in_place(text, file.edits, &last_error)) {
        ++failed;
        continue;
      }
      if (!st::fs::write_text(path, text).has_value()) {
        ++failed;
        last_error = "写不进去 " + path;
        continue;
      }
      ++touched;
    }
    dir_cache_.clear();   // 跨文件重命名可能新建/改名文件：树缓存作废
    buffers_.set(std::move(list));
    // 编辑器里显示的若是被改的文件，重新装载（`set_text` 会清撤销栈——
    // 但跨文件重命名本就无法用撤销栈表达，如实重载比装作可撤销更诚实）。
    if (editor != nullptr) {
      const std::size_t active = active_.value();
      const auto current = buffers_.value();
      if (active < current.size()) {
        editor->set_text(current[active].text);
        lsp_.sync_document(current[active].path, current[active].text);
      }
    }
    refresh_problems();
    if (failed > 0) {
      status_.set(std::format("重命名：{} 处已改，{} 处失败（{}）", touched, failed, last_error));
    } else {
      status_.set(std::format("重命名完成：{} 个文件", touched));
    }
  }



  /// 编辑区按键：补全交互（Enter 接受 / Esc 关闭 / 上下选择）优先于编辑器默认。
  /// 返回 true = 已消费。
  auto CodeEditorPage::handle_completion_key(std::string_view key) -> bool {
    if (!completion_open_.value()) return false;
    if (key == "Escape" || key == "Esc") { completion_open_.set(false); return true; }
    // Enter/Up/Down/Tab 交给弹层（它是顶层可见元素，键会先到它那里——
    // 这里只兜底处理"弹层没接住"的情况）。
    return false;
  }




  /// 请求中止当前会话（面板按钮）。
  void CodeEditorPage::request_terminal_stop() {
    if (terminal_ptr != nullptr) terminal_ptr->send_stop();
  }


  // **应用不再解释命令**：真终端里输入的一切都送给 shell，由它执行
  //（`cd`、`ls`、`git` 都是 shell 自己的命令）。原先那套"内置命令 + 白名单"
  // 是假终端时代的产物——那时没有 shell 可托付，只能自己解析一遍。
  //
  // 于是随之移除的还有：命令替换（`$last`/`$root`/`$cwd`）、只读 git 白名单、
  // 语法高亮式的 `cat`/`wc` 内建、以及"未知命令"提示——它们全都建立在
  // "应用是命令的解释者"这个前提上，而那个前提已经不成立了。


  /// 命令面板执行一条命令（id 是稳定的业务身份）。
  void CodeEditorPage::run_command(const std::string& id) {
    // ⚠ 顺序敏感：`"file.open"` 必须排在 `"file.open."` 前缀判断**之前**。
    // 两个 id 共享前缀，`rfind("file.open.", 0)` 对 `"file.open"` 为假（少了那个点），
    // 但把精确匹配放前面才不会在将来加 `file.open-xxx` 时踩到同类前缀问题。
    if (id == "file.open") {
      open_file_dialog();
    } else if (id.rfind("path:", 0) == 0) {
      // Ctrl+P 文件表的条目（见 `file_commands`）：id 直接携带完整路径。
      open_path(id.substr(5));
    } else if (id.rfind("recent:", 0) == 0) {
      // 命令面板的「最近」条目：id 前缀 + 完整路径（与菜单的 recent-file: 同源不同名
      // ——面板里这个更短，且不会与 file.open.* 前缀判断碰撞）。
      open_recent(id.substr(7));
    } else if (id.rfind("recent-folder:", 0) == 0) {
      switch_workspace(id.substr(14));
    } else if (id.rfind("symbol:", 0) == 0) {
      // 「转到符号」面板的条目：id 里的数字就是 `symbol_commands()` 的下标。
      jump_to_symbol(static_cast<std::size_t>(std::stoul(id.substr(7))));
    } else if (id == "file.open-folder") {
      open_folder_dialog();
    } else if (id == "file.save") {
      save();
    } else if (id == "file.save-all") {
      save_all();
    } else if (id == "go.symbol") {
      open_symbols(true);
    } else if (id == "go.workspace-symbol") {
      open_symbols(false);
    } else if (id == "nav.declaration") {
      goto_declaration();
    } else if (id == "nav.type-definition") {
      goto_type_definition();
    } else if (id == "nav.implementation") {
      goto_implementation();
    } else if (id == "nav.back") {
      navigate_back();
     } else if (id == "nav.results") {
  // 多处导航结果列在大纲侧栏下段：切过去并展开（与 on_locations_response 同一套入口）。
  if (!nav_results_.empty()) {
    activity_.set(view_index(SidebarView::Nav));
   sidebar_visible_.set(true);
  } else {
   status_.set("当前没有导航结果（F12/Shift+F12 多处命中时生成）");
  }
    } else if (id == "file.close-tab") {
      close_active();
    } else if (id == "view.zoom-in") {
      zoom_page(1);
    } else if (id == "view.zoom-out") {
      zoom_page(-1);
    } else if (id == "view.zoom-reset") {
      zoom_page(0);
    } else if (id == "view.toggle-sidebar") {
      sidebar_visible_.set(!sidebar_visible_.value());
    } else if (id == "view.toggle-theme") {
      toggle_theme();
    } else if (id == "view.toggle-indent-guides") {
      if (editor != nullptr) {
        editor->set_indent_guides(!editor->indent_guides());
        status_.set(editor->indent_guides() ? "缩进参考线：开" : "缩进参考线：关");
      }
    } else if (id == "view.toggle-blame") {
      toggle_blame();
    } else if (id == "file.history") {
      open_file_history();
    } else if (id == "help.about") {
      status_.set("歌白代码 0.1.0 · 霜天框架示例 · 声明式组装");
    } else if (!id.empty()) {
      // ⚠ **兜底分支不得声称「已打开」**：未识别的 id 意味着“没做任何事”——
      // 把它写成“已打开”会让界面在失败时看起来成功了（Ctrl+P 那次的直接后果）。
      const std::size_t slash = id.find_last_of("/\\");
      const std::string leaf(slash == std::string::npos ? id : id.substr(slash + 1));
      status_.set("未识别的命令：" + leaf);
    }
  }


  /// 全部命令表（Ctrl+Shift+P）。
  [[nodiscard]] auto CodeEditorPage::command_table() -> std::vector<CommandPalette::Command> {
    std::vector<CommandPalette::Command> table;
    const auto add = [&table](std::string id, std::string title, std::string detail) {
      table.push_back(CommandPalette::Command{.id = std::move(id), .title = std::move(title),
                                              .detail = std::move(detail), .handler = {}});
    };
    // 最近打开的文件/文件夹放头部（完整 MRU 10 条——菜单只放前 5，面板才是
    // 清单页；「最近」是高频入口，过滤前也应该看得见）。id 携带完整路径。
    for (const auto& file : recent_files_) {
      add("recent:" + file, "最近: " + st::fs::file_name(file), file);
    }
    for (const auto& folder : recent_folders_) {
      add("recent-folder:" + folder, "最近打开文件夹: " + st::fs::file_name(folder), folder);
    }
    add("file.save", "文件: 保存", "把当前编辑器标记为已保存");
  add("file.save-all", "文件: 全部保存", "把所有脏标签写盘（Ctrl+Alt+S）");
  // 符号导航：两个入口（当前文件大纲 / 工作区符号）——都复用命令面板的过滤框。
  add("go.symbol", "转到: 当前文件符号…", "语言服务提供的符号大纲");
  add("go.workspace-symbol", "转到: 工作区符号…", "Ctrl+T（跨文件搜符号）");
    add("file.close-tab", "文件: 关闭当前编辑器", "Ctrl+W");
    // 打开的两个入口（文件 / 文件夹）都进命令表——菜单里能点，命令面板里能搜，
    // 否则“打开文件夹”只有记住快捷键的人找得到。
    add("file.open", "文件: 打开文件…", "Ctrl+O");
    add("file.open-folder", "文件: 打开文件夹…", "换工作区（Ctrl+Shift+O）");
    add("view.zoom-in", "查看: 放大页面", "页面缩放 +（Ctrl+=）");
    add("view.zoom-out", "查看: 缩小页面", "页面缩放 −（Ctrl+-）");
    add("view.zoom-reset", "查看: 重置页面缩放", "回到 100%（Ctrl+0）");
    add("view.toggle-sidebar", "查看: 切换侧栏可见性", "显示/隐藏侧栏（活动栏常驻，Ctrl+B）");
    add("view.toggle-theme", "查看: 切换亮/暗主题", "主题令牌整体切换");
    add("view.toggle-indent-guides", "查看: 切换缩进参考线", "每级缩进一条竖线");
    // blame 行尾注解：开关 + 当前视口取数（B5）。
    add("view.toggle-blame", "查看: 切换行尾 blame 注解", "显示当前视口行的作者与日期");
    // 单文件历史弹窗（B6）——入口同时挂在编辑器右键菜单（与 IDEA 一致）。
    add("file.history", "文件: 当前文件的历史…", "git log --follow（跨重命名）");
    // 导航族：F12 只覆盖定义，声明/类型定义/实现进面板（快捷键也在，但面板可搜）。
    add("nav.declaration", "转到: 声明", "Alt+F12");
    add("nav.type-definition", "转到: 类型定义", "光标在变量上时跳它的类型");
    add("nav.implementation", "转到: 实现", "Ctrl+F12");
    add("nav.back", "转到: 返回上一处", "Alt+Left（跳转历史）");
    add("nav.results", "转到: 切换导航结果面板", "多处定义/引用的清单（多处时自动开）");
    add("help.about", "帮助: 关于", "歌白代码");
    return table;
  }


  /// 快速打开的文件表（Ctrl+P）：列工作区里的真实文件（递归、限量）。
  ///
  /// **id 带 `path:` 前缀**：条目身份是一整条磁盘路径，`run_command` 按它
  /// 分流到 `open_path`。曾经直接把路径当 id，于是分支不命中，只剩兜底的
  /// 「已打开 …」状态栏文案（面板关了、文件没开）。
  [[nodiscard]] auto CodeEditorPage::file_commands() const -> std::vector<CommandPalette::Command> {
    std::vector<CommandPalette::Command> table;
    if (!workspace_.empty()) {
      std::vector<std::pair<std::string, int>> stack{{workspace_, 0}};
      while (!stack.empty() && table.size() < 300) {
        const auto [dir, depth] = stack.back();
        stack.pop_back();
        if (depth > 5) continue;
        auto listing = st::fs::list_dir(dir);
        if (!listing) continue;
        for (const auto& item : *listing) {
          const std::string full = dir + "/" + item.name;
          if (item.is_dir) {
            if (!item.name.empty() && item.name.front() == '.') continue;
            stack.emplace_back(full, depth + 1);
            continue;
          }
          table.push_back(CommandPalette::Command{
              .id = "path:" + full, .title = item.name,
              .detail = full.substr(workspace_.size() + 1), .handler = {}});
        }
      }
      return table;
    }
    return table;
  }

}  // namespace gbcode_ui
