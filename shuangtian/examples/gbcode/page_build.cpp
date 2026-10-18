// build 骨架族：页面形态的主干（build/workbench/main/editor_area/状态栏/活动栏）
// 与每帧泵（终端/语言服务）。拆分自 main.cpp，零逻辑改动——只是 out-of-line 化。

#include <algorithm>
#include <cstdio>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "page.hpp"
#include "st/core/string.hpp"
#include "st/core/time.hpp"
#include "st/core/platform.hpp"
#include "st/core/log.hpp"
#include "st/text/diff.hpp"
#include "st/ui/components/menu.hpp"
#include "st/ui/svg.hpp"

namespace gbcode_ui {


  // ══════════════════════════════════════════════════════════════════════════
  // build()：只描述形态
  // ══════════════════════════════════════════════════════════════════════════


  /// 标题栏**文案**：窗框（外壳）自持标题栏，本页只把“现在该显示什么”推过去
  /// （文件名 + 脏点，与 VSCode 把当前文件名写进标题栏同构）。
  ///
  /// 为什么保留这个“推”而不把标题也搬进 `MenuHeader`：标题属于**窗框**的状态
  /// （它决定窗口叫什么），菜单只是恰好与它同行；两者合并不等于所有权合并。
  void CodeEditorPage::push_title() {
    if (frame_ == nullptr || frame_->title_bar() == nullptr) return;
    frame_->title_bar()->set_title(window_title());
  }


  /// 每帧推进的**构建前动作**（由入口在 `tick` 之前调）。
  ///
  /// 两件事：① 把工作线程攒下的终端输出搬进 `State`（本帧重组才能排进布局）；
  /// ② 泵终端。
  ///
  /// 收起面板已改走 `SplitView::set_second_hidden`（终端常驻子树、只隐藏），
  /// `terminal_ptr` 不再有悬垂窗口——旧的"关最后一个会话 → 收面板"延迟路径
  ///（`bottom_close_pending_` + 拆树时清指针）随拆树方案一起删除。
  void CodeEditorPage::update_terminal() { pump_terminal(); pump_language_service(); }


  /// 每帧泵语言服务：搬消息、把诊断推给编辑器与问题列表。
  ///
  /// 为什么在 `tick()` **之前**泵（与终端同一个理由）：诊断要在本帧的布局/绘制里
  /// 生效，放在 `tick` 之后就永远慢一帧（用户看到"改完错字，波浪线还在"）。
  void CodeEditorPage::pump_language_service() {
    // ⚠ **返回值只表示"诊断更新了"，不是"有没有事要做"**。
    //
    // 旧写法 `if (!lsp_.pump()) return;` 把两件事混为一谈：补全响应/其它请求响应
    // 回来时 `pump` 返回 false，于是整个函数提前返回——**补全永远弹不出来**
    //（实测：请求 id 正常发出、clangd 也回了 100 项候选，弹层就是不出现）。
    // 正确写法：泵**总是执行**（它内部搬消息、派发回调），返回值只决定
    // 要不要刷新编辑器诊断。
    pump_hover_word();
    flush_terminal_commands();
    // 终端里的 git 命令完成后的状态刷新（延迟落地：命令完成时机不可知）。
    //
    // ⚠ **位置必须在 `flush_terminal_commands()` 之后**：写操作只往队列里塞命令
    // 并置 `git_refresh_pending_`，真正的投递在 `flush_terminal_commands()` 里。
    // 刷新块排在它前面的话，会在“命令还没送出去”的那一帧就把挂起标志消费掉
    // ——下一个条件（终端安静）此时天然成立，于是 `refresh_git()` 读到的是
    // **命令执行之前**的快照，而标志已被清空、再也不会有第二次。
    // 实测症状：点暂存后自动刷新“发生了但什么都没变”，而手点「刷新状态」立刻变。
    //
    // ⚠ 判据不能用 `!terminal_ptr->any_busy()`：PTY 模式下 `busy` 的含义是
    // “shell 活着”（常量真）——用它当门控会让这条刷新**永远不会发生**。
    //
    // 改用“终端安静了多久”，但**必须配一个上限**：真终端会持续有输出
    //（提示符重绘、PowerShell 周期性的空行），单靠“安静”可能永远等不到。
    // 上限到点就刷（多读一次 git 无副作用；只是可能读到命令尚未生效的中间态，
    // 而下一次刷新会把它纠正——比“永远不刷”好得多）。
    constexpr std::int64_t kTerminalQuietMs = 300;
    constexpr std::int64_t kGitRefreshDeadlineMs = 1500;
    if (git_refresh_pending_) {
      const std::int64_t now_ms = st::time::now_ms();
      if (git_refresh_since_ms_ == 0) {
        git_refresh_since_ms_ = now_ms;
        // 入队那一帧先脏一次：界面上“点完没任何反应”是最差的反馈。
        git_revision_.set(git_revision_.value() + 1);
      }
      const std::int64_t elapsed = now_ms - git_refresh_since_ms_;
      // ⚠ 必须先等一个“结算期”：命令刚发出的那一瞬间终端还没有任何回显，
      // `idle_ms` 天然就是一个大值——直接用“安静”作判据会在命令**执行之前**
      // 就刷一次并把挂起标志消耗掉（实测：自动刷新“发生了但什么都没变”）。
      const bool quiet =
          elapsed >= kTerminalQuietMs &&
          (terminal_ptr == nullptr || terminal_ptr->idle_ms(now_ms) >= kTerminalQuietMs);
      const bool deadline = elapsed >= kGitRefreshDeadlineMs;
      if (quiet || deadline) {
        git_refresh_pending_ = false;
        git_refresh_since_ms_ = 0;
        refresh_git();
        // 日志/分支/标签…与工作区快照是两批数据（`ensure_git_log` 管后者），
        // 提交/切分支/暂存都会同时改变两边——一起标脏，否则会出现
        // “变更面板对了、日志还停在上一次提交”。
        git_revision_.set(git_revision_.value() + 1);
      }
    }
    // 变更视图“首次要读一次 git 状态”的意图（渲染期只置标记，读在这里落地）。
    // 为何需要：视图改为**常驻声明 + 隐藏保活**后，构建函数每帧都会跑到；
    // 而“首帧读一次”不能在渲染期直调（见 `build_scm_view` 的注释）。
    if (git_probe_wanted_) {
      git_probe_wanted_ = false;
      if (!workspace_.empty() && !git_probed_) refresh_git();
    }
    ensure_git_log();
    // `--open`：首帧之后打开（见 `set_pending_open_path` 的说明）。
    flush_pending_open();
    ensure_diff();
    // 三方合并：同上（`git show :1:/:2:/:3:` 是子进程）。
    ensure_merge();
    flush_merge_write();
    // 行尾 blame：帧首泵（起子进程不能在渲染期）。
    ensure_blame();
    // 单文件历史：同上（`git log --follow` 也是子进程）。
    ensure_file_history();
    (void)lsp_.pump();   // 诊断到达与否只影响 push_diagnostics_to_editor 内部的指纹
    // 待落地的跳转（跨文件）：编辑器实例在新文件装载后才就位，所以在这里补做。
    apply_pending_navigation_jump();
    // 问题列表刷新：**无条件走到这里**（内部指纹早退）。
    //
    // 旧代码 `if (!diagnostics_changed) return;` 挡住了两个真时机：
    // ① 「打开新文件后的第一帧」——装载发生在 build() 末尾，本轮泵早就过了，
    //    下一帧又因 `pump()` 无新诊断而早退，于是**内置轻量检查永远不跑**
    //    （实测：打开含 3 处问题的 README，计数恒在 0，点一下计数按钮才变 3）；
    // ② 「切到无 LSP 诊断的文件」——旧计数残留（实测：从 3 掉到 1 而非 0）。
    // 指纹把「该不该刷」变成数据比较，三个时机（装载后/编辑回调/本泵）
    // 全部收敛到这里，代价是每个几条字符串哈希。
    refresh_problems();
    push_diagnostics_to_editor();
  }


  /// 落地"上一帧记下的跨文件跳转"（定义/引用结果）。
  ///
  /// 为什么要等一帧：`jump_to_location` 里 `open_path` 只是改了状态，
  /// 编辑器实例要到**下一次重组**才拿到新文本（`set_text` 在那里发生）——
  /// 立刻 `goto_line` 会跳在旧文件上（实测：跳定义时行号对了但内容还是上一个文件）。
  void CodeEditorPage::apply_pending_navigation_jump() {
    if (!pending_jump_pending_ || editor == nullptr) return;
    // 编辑器装的还是旧文件时不落地（等它换过来）。
    if (loaded_key_ != current_buffer_path()) return;
    pending_jump_pending_ = false;
    const std::size_t line = pending_jump_.first;
    const std::size_t column = pending_jump_.second;
    editor->goto_line(line);
    if (column > 0) {
      // 跳到列：`goto_line` 落在行首，再按偏移把光标移到标识符起点。
      const std::size_t start = editor->cursor_index() + (column - 1);
      editor->set_selection(start, start);
    }
    editor->scroll_to_line(line);
  }


  /// 把 LSP 诊断推给编辑器（只在**内容变了**时写，见 `lsp_diagnostics_stamp_`）。
  void CodeEditorPage::push_diagnostics_to_editor() {
    if (editor == nullptr) return;
    const auto& problems = lsp_.problems();
    // 指纹：条数 + 首条位置/消息长度（便宜且够用——诊断集合变了基本都会变它）。
    std::size_t stamp = problems.size() * 131U;
    if (!problems.empty()) {
      stamp += problems.front().line * 7U + problems.front().column + problems.front().message.size();
    }
    if (stamp == lsp_diagnostics_stamp_) return;
    lsp_diagnostics_stamp_ = stamp;

    std::vector<CodeEditor::DiagnosticMark> marks;
    marks.reserve(problems.size());
    const std::string text = editor->text();
    for (const auto& problem : problems) {
      CodeEditor::DiagnosticMark mark{};
      // LSP 给的是 行/列（1 起）——转成**组件口径的字节偏移**。
      // 用组件的行索引：第 `line-1` 行的起始字节 + 列偏移（按 UTF-16 码元数）。
      mark.begin = CodeEditorPage::offset_for_line_column(text, problem.line - 1, problem.column - 1);
      // 范围结束未知（我们只存了起点）：标到"该行末尾"——视觉上就是这一行的这段。
      mark.end = CodeEditorPage::line_end_offset(text, problem.line - 1);
      if (mark.end < mark.begin) mark.end = mark.begin;
      mark.severity = problem.warning ? CodeEditor::DiagnosticSeverity::Warning
                                      : CodeEditor::DiagnosticSeverity::Error;
      mark.message = problem.message;
      mark.source = problem.source;
      marks.push_back(std::move(mark));
    }
    editor->set_diagnostics(std::move(marks));
  }


  /// 第 `line`（0 基）行第 `column`（0 基，UTF-16 码元）列在全文里的字节偏移。
  [[nodiscard]] auto CodeEditorPage::offset_for_line_column(const std::string& text, std::size_t line,
                                                   std::size_t column) -> std::size_t {
    std::size_t offset = 0;
    for (std::size_t current = 0; current < line; ++current) {
      const std::size_t newline = text.find('\n', offset);
      if (newline == std::string::npos) return text.size();
      offset = newline + 1;
    }
    const auto position = st::lsp::Position{.line = line, .character = column};
    return st::lsp::position_to_offset(text, position);
  }


  /// 第 `line`（0 基）行的结束偏移（不含换行符）。
  [[nodiscard]] auto CodeEditorPage::line_end_offset(const std::string& text, std::size_t line)
      -> std::size_t {
    std::size_t offset = 0;
    for (std::size_t current = 0; current < line; ++current) {
      const std::size_t newline = text.find('\n', offset);
      if (newline == std::string::npos) return text.size();
      offset = newline + 1;
    }
    const std::size_t newline = text.find('\n', offset);
    return newline == std::string::npos ? text.size() : newline;
  }


  void CodeEditorPage::build(Composer& c) {
    const auto& buffers = buffers_.value();
    const std::size_t active = active_.value();
    const bool has_editor = !buffers.empty();

    // 编辑器实例可能在本次重组中新建/换绑：先置空，由 `build_editor_area` 重新取得
    editor = nullptr;
    diff_view_ptr = nullptr;   // 同上：差异视图实例每帧重建
    // **浮层里的输入框**：它们在上一次重组里被销毁，本帧还没重建。
    //
    // 不置空就会踩到已释放的 Input——实测（无头 E2E，转到行浮层）：
    // “聚焦输入框”的待办曾排在 `build_goto_line` **之前**，拿着上一帧的
    // `goto_line_input` 调 `host()->set_keyboard_focus(...)`，无头下随机
    // SIGSEGV（0xC0000005），带窗口时只表现为“焦点偶尔不对”。
    find_needle_input = nullptr;
    goto_line_input = nullptr;
    fs_input = nullptr;
    // `grow = true` 不可省：本页住在窗框的**内容槽**（列容器）里，不 grow 就只按内容的
    // 自然高度占位——实测 800px 窗口里页面只有 525px 高，底部剩 195px 空白，
    // 编辑器被压到 295px。留白与「代码区太矮」是同一个根因。
    column(c, {.gap = 0.0f, .grow = true, .id = "editor-page"}, [&] {
      build_workbench(c, buffers, active, has_editor);
      build_status_bar(c);
      build_find_bar(c);
    });
    push_title();   // 标题栏（外壳）的文案仍由本页推——见 `push_title` 的注释
    // 命令面板：**条件声明**（关掉 = 本帧不声明 → 框架 sweep 摘除）
    if (palette_open_.value()) build_palette(c);
    // 编辑器右键菜单 / 关闭确认对话框 / 快捷键一览：同一套条件声明。
    build_editor_context(c);
    build_explorer_context(c);
    build_close_confirm(c);
    build_shortcuts_card(c);
    build_open_dialog(c);
    build_goto_line(c);
    build_fs_action(c);
    build_completion_popup(c);
    build_rename_dialog(c);
    build_git_confirm(c);
    build_file_history(c);
    build_branch_new_dialog(c);
    build_tag_new_dialog(c);
    // 浮层内的“聚焦输入框”待办：**必须排在对应的构建之后**——
    // 排在前面拿到的是上一帧已销毁的指针（本函数开头已把两个指针置空，
    // 所以即使顺序被人改回去，也只是“这一次没聚焦”，不会再踩已释放对象）。
    apply_pending_focus();
    apply_pending_goto_focus();
    apply_pending_fs_focus();
    // 文本灌入：只在本帧的编辑器实例与「已装载的标签」不一致时写
    // （`set_text` 会清撤销栈并把光标归零；每次重组都写会让打字被重置——实测踩到）
    if (editor != nullptr && active < buffers.size() && buffers[active].key != loaded_key_) {
      editor->set_language(buffers[active].language);
      editor->set_text(buffers[active].text);
      // 二进制文件按只读打开（与 `OpenBuffer::binary` 同一真值源）。
      // 不设的话它能被编辑、Ctrl+S 把屏幕上的乱码写回原文件（实测）。
      // 非二进制一律回 false：否则从二进制标签切到普通文件会“继承”只读态。
      editor->set_read_only(buffers[active].binary);
      loaded_key_ = buffers[active].key;
      // 切到新文件：把语言服务指向它（懒启动 server）——只对**磁盘上的真文件**做，
      // 内存缓冲（untitled-N）没有 URI 可同步。
      sync_language_document(buffers[active]);
      // 新文件装载完毕：立即刷一次问题列表（指纹早退，成本可忽略）。
      // 只依赖帧首泵的话要等到下一帧，且那帧可能因「无新诊断」走不到刷新——
      // 这就是「打开文件后计数恒为 0」的第二个成因（另一个在 pump_language_service）。
      refresh_problems();
    }
    // 当前文件内容变了就同步给 server（`sync_document` 内部按"文本是否变化"早退，
    // 所以每帧调用是安全的——这是**增量同步的唯一入口**）。
    if (active < buffers.size()) {
      const auto& buffer = buffers[active];
      if (!buffer.path.empty() && lsp_.active_path() == buffer.path) {
        lsp_.sync_document(buffer.path, buffer.text);
      }
    }
  }


  void CodeEditorPage::on_menu(const std::string& menu, const std::string& item) {
    menu_open_.set(kNoMenu);   // 选完即关
    // 「最近打开」条目：id 直接携带路径（recent-file:<path> / recent-folder:<path>）
    // ——菜单是动态构建的，下标会变，路径才是稳定身份。
    if (item.rfind("recent-file:", 0) == 0) {
      open_recent(item.substr(12));
      return;
    }
    if (item.rfind("recent-folder:", 0) == 0) {
      switch_workspace(item.substr(14));
      return;
    }
    if (item == "toggle-terminal") {
      // 两个菜单（查看/运行）都指向它：开合终端面板
      bottom_visible_.set(!bottom_visible_.value());
      return;
    }
    if (menu == "file" && item == "save") {
      save();
    } else if (menu == "file" && item == "close-tab") {
      close_active();
    } else if (menu == "file" && item == "close-all") {
      close_all();
    } else if (menu == "file" && item == "save-as") {
      save_as();
    } else if (menu == "file" && item == "save-all") {
      save_all();
    } else if (menu == "file" && (item == "new" || item == "open")) {
      if (item == "new") new_file_prompt();
      else open_file_dialog();
      } else if (menu == "file" && item == "open-folder") {
        open_folder_dialog();
    } else if (menu == "view" && item == "command-palette") {
      open_palette(false);
    } else if (menu == "view" && item == "toggle-sidebar") {
      sidebar_visible_.set(!sidebar_visible_.value());
    } else if (menu == "view" && item == "find") {
      open_find();
    } else if (menu == "view" && item == "toggle-theme") {
      toggle_theme();
    } else if (menu == "run" && (item == "run-task" || item == "run-test" || item == "run-lint")) {
      run_task(item == "run-task" ? "build" : (item == "run-test" ? "test" : "lint"));
    } else if (menu == "help" && item == "about") {
      status_.set("歌白代码 0.1.0 · 霜天框架示例 · 声明式组装");
    } else if (menu == "help" && item == "shortcuts") {
      shortcuts_open_.set(true);
    } else {
      apply_editor_menu(menu, item);
    }
  }


  /// 编辑/选择菜单：直接作用于当前编辑器（一次性命令，不涉及界面形态）。
  void CodeEditorPage::apply_editor_menu(const std::string& menu, const std::string& item) {
    (void)menu;
    if (editor == nullptr) return;
    if (item == "undo") editor->undo();
    else if (item == "redo") editor->redo();
    else if (item == "comment") editor->toggle_comment();
    else if (item == "select-all") editor->select_all();
    else if (item == "copy") {
      // 真实复制：写编辑器剪贴板（无头环境没有系统剪贴板，见组件侧说明）
      const std::string picked = editor->selected_text();
      status_.set(picked.empty() ? "没有选中内容"
                                 : std::format("已复制 {} 字符", st::utf8_length(picked)));
      (void)editor->invoke_action("copy", {});
    } else if (item == "paste") {
      (void)editor->invoke_action("paste", {});
    } else if (item == "cut") {
      (void)editor->invoke_action("cut", {});
    } else if (item == "goto-line") {
      open_goto_line();
    } else if (item == "goto-symbol") {
      open_symbols(true);
    } else if (item == "goto-workspace-symbol") {
      open_symbols(false);
    }
  }


  // —— 3. 主体三栏 ——
  void CodeEditorPage::build_main(Composer& c, const std::vector<OpenBuffer>& buffers, std::size_t active,
                  bool has_editor) {
    row(c, {.grow = true, .id = "main-row"}, [&] {
      // ⚠ **活动栏不在这里**：它在 `build_workbench` 那一层（与底部面板并列、
      // 满高）。本函数只管“侧栏与编辑区”这一对横向分栏。
      //
      // **侧栏不常驻**（活动栏常驻，两者是独立的东西）：活动栏是“能去哪儿”的入口
      //（切视图的唯一途径），侧栏是“当前视图的内容”。曾经把两者绑在一个
      // `sidebar_visible_` 上，于是关掉侧栏连入口一起没了（实测：Ctrl+B 之后
      // 活动栏消失、无从点回去）。
      if (!sidebar_visible_.value()) {
        build_editor_area(c, buffers, active, has_editor);
        return;
      }
      // 侧栏与编辑区之间用 SplitView（宽度可拖）——**两个子面板就是它的两个子位**，
      // 声明式里用 `custom_container<SplitView>` 让两次子声明分别落到 first/second。
      (void)custom_container<SplitView>(
          c,
          [&] {
            (void)custom_container<Panel>(c, [&] { build_sidebar(c, buffers, active); },
                                          [](Panel& panel) {
                                            panel.set_id("sidebar");
                                            // 侧栏是“内容容器”：`Surface::Base`。
                                            // 为何必须声明（2026-10-18 配色重整）：不声明 = 透明，
                                            // 底色会透到页面 `bg` 上，与里面的 `Tree`（自己填 `surface`）
                                            // 形成“外暗内亮”的颠倒；而且与编辑器完全同色（对比度 1.000）。
                                            panel.set_surface(Element::Surface::Base);
                                          },
                                          {.key = "sidebar"});
            (void)custom_container<Panel>(
                c, [&] { build_editor_area(c, buffers, active, has_editor); },
                [](Panel& panel) {
                  panel.set_id("editor-column");
                  // 编辑区是“**内容阅读面**”：`Surface::Content`。
                  // 为何不用 `Sunken`：两个主题的“围白方向”相反——
                  //   暗色：内容比外壳**暗**（往里沉）；
                  //   亮色：内容**归白**、外壳压浅灰（VSCode Light / IDEA Light 同构）。
                  // 一个档位表达语义，具体取哪层 token 由主题决定（见 `Surface::Content`）。
                  panel.set_surface(Element::Surface::Content);
                },
                {.grow = true, .key = "editor"});
          },
          [this](SplitView& split) {
            split.set_id("sidebar-split");
            split.set_min_ratio(0.12f);
            // 比例存档在 `sidebar_ratio_`（非响应式）：拖动回调写它，重组时读它。
            // ⚠ 不能每帧写死默认值：声明式每帧都会跳这个 configure，写死等于
            // 把用户拖到的位置抹回默认（实测：拖到 0.5，一点任何按钮就弹回默认）。
            split.set_ratio(sidebar_ratio_, false);
            // 归位目标 = 默认比例（0.2 = 1/5 宽）：双击/单击分割线回到 1/5，
            // 而不是跳到一半——「恢复默认布局」的语义该回到默认值。
            split.set_default_ratio(kSidebarRatio);
            split.on_change = [this](float ratio) { sidebar_ratio_ = ratio; };
          },
          {.grow = true, .id = "sidebar-split"});
    });
  }


  void CodeEditorPage::build_activity_bar(Composer& c) {
    struct Activity {
      const char* icon;
      const char* id;
      const char* hint;
    };
    // 活动栏的按钮**只**有图标：没有提示就是一个凭记忆图的图标墙（用户报过
    // “按钮要有悬浮提示”）。`hint` 顺带把键位写进去——提示是“发现功能”的地方。
    static constexpr Activity kActivities[] = {{"folder", "explorer", "资源管理器（Ctrl+Shift+E）"},
                                               {"search", "search", "搜索（Ctrl+Shift+F）"},
                                               {"diff", "scm", "变更（Ctrl+Shift+G）"},
                                               {"play", "run", "运行"},
                                               {"package", "extensions", "语言与扩展"},
                                               // 「导航结果」升级为「大纲」：当前文件符号树为主体，
                                               // 多处导航命中附在下方（2026-10-13）。
                                               {"symbol", "outline", "大纲"}};
    const std::size_t current = activity_.value();
    // `surface = Alt`：与标题栏/状态栏**同色**——三者合起来是一圈“外壳”，
    // 色不一致会在拐角处露馅（见 `Element::Surface` 的说明）。
    column(c, {.gap = 2.0f, .padding_y = 4.0f, .width = 44.0f,
               .surface = Element::Surface::Alt, .id = "activity-bar"}, [&] {
      for (std::size_t index = 0; index < std::size(kActivities); ++index) {
        // 图标按钮：`custom<Button>`（Button 的 icon 是一等接口，属性面没有）
        const std::string id = std::string("activity-") + kActivities[index].id;
        // **选中态只取决于“当前是哪个视图”，与侧栏展开与否无关**。
        // 活动栏常驻之后，它就是“当前视图在哪”的唯一指示——收起侧栏时也必须有，
        // 否则用户看不出收起来的是哪一栏（VSCode 同此：收起侧栏后该项仍高亮）。
        const bool on = index == current;
        (void)custom<Button>(c, [&, index, on](Button& b) {
          b.set_id(id);
          b.set_icon(kActivities[index].icon);
          b.set_variant(on ? Button::Variant::Soft : Button::Variant::Ghost);
          b.set_size(Button::Size::Small);
          b.set_hint(kActivities[index].hint);
          b.on_click = [this, index] {
            // 点当前视图 = 收起侧栏（IDEA/VSCode 通行行为）；点别处 = 切视图并展开。
            if (activity_.value() == index && sidebar_visible_.value()) {
              sidebar_visible_.set(false);
              return;
            }
            activity_.set(index);
            sidebar_visible_.set(true);
            mark_sidebar_visited(index);
          };
        }, {.width = 40.0f, .height = 36.0f, .key = kActivities[index].id});
      }
      // —— 下组：终端 ——
      //
      // 用**弹性空隙**把它与上面五个**视图**分开（VSCode / Windows Terminal 的惯例：
      // 下组是“工具/面板”（开关类的），上组是“导航”（互斥切视图的））。
      // 注意空格子必须带 `grow`：裸 `spacer` 在列里只是固定高的小段，
      // 不会把后面的项顶到底部（实测：终端按钮被挤在资源树下面，看不见）。
      (void)column(c, {.grow = true, .id = "activity-spacer"}, [&] {});
      // —— Git（历史：分支 | 日志）——
      //
      // 激活态与终端**同一推法**：面板开着 && 停在这个标签。点击三态：
      // 隐藏中 → 打开并切到 Git；停在别的标签 → 切过来；已激活 → 收起面板
      //（与上组“点当前视图 = 收起”同一交互词形）。
      (void)custom<Button>(c, [this](Button& b) {
        b.set_id("activity-git");
        b.set_icon("git-branch");
        b.set_hint("Git 工具窗（分支 / 日志 / 提交内容）");
        // 激活 = 下半可见 && 当前槽是 Git（与终端按钮同一推法）。
        const bool on = bottom_visible_.value() && bottom_tab_.value() == 1;
        b.set_variant(on ? Button::Variant::Soft : Button::Variant::Ghost);
        b.set_size(Button::Size::Small);
        b.on_click = [this] { toggle_git_panel(); };
      }, {.width = 40.0f, .height = 36.0f, .key = "activity-git"});
      (void)custom<Button>(c, [this](Button& b) {
        b.set_id("activity-terminal");
        b.set_icon("terminal");
        b.set_hint("终端（Ctrl+`）");
        // 激活 = 下半可见 && 当前槽是终端（状态从真实可见性推，不是另存标记）。
        const bool on = bottom_visible_.value() && bottom_tab_.value() == 0;
        b.set_variant(on ? Button::Variant::Soft : Button::Variant::Ghost);
        b.set_size(Button::Size::Small);
        b.on_click = [this] { toggle_terminal_panel(); };
      }, {.width = 40.0f, .height = 36.0f, .key = "terminal"});
    });
  }


  /// 底部单槽的三态切换（两按钮同一词形）：
  /// 隐藏中 → 打开并显示我；显示的是别人 → 切到我；显示的是我 → 收起下半。
  ///（与上组活动栏“点当前视图 = 收起侧栏”同一交互词形。）
  void CodeEditorPage::toggle_bottom_panel(std::size_t mine) {
    if (!bottom_visible_.value()) {
      bottom_tab_.set(mine);
      bottom_visible_.set(true);
    } else if (bottom_tab_.value() != mine) {
      bottom_tab_.set(mine);
    } else {
      bottom_visible_.set(false);
    }
  }


  /// Git 按钮：切到 Git 工具窗（打开顺带要日志）。
  void CodeEditorPage::toggle_git_panel() {
    const bool opening = !bottom_visible_.value() || bottom_tab_.value() != 1;
    toggle_bottom_panel(1);
    if (opening) git_log_wanted_ = true;
  }


  /// 开关底部终端面板（活动栏按钮 / `` Ctrl+` `` 同一条路径）。
  ///
  /// 打开时把键盘焦点交给终端输入行（`Terminal` 自己会在下一次事件里接住），
  /// 这是“点了终端按钮就能直接敲命令”的那一步。
  void CodeEditorPage::toggle_terminal_panel() {
    const bool opening = !bottom_visible_.value() || bottom_tab_.value() != 0;
    toggle_bottom_panel(0);
    if (opening) status_.set("终端：已展开");
  }


  /// “命令要在终端里跑给用户看”的统一入口：确保下半开着且停在终端
  ///（Git 操作、运行任务、diff 进终端等都走这里）。
  void CodeEditorPage::ensure_terminal_visible() {
    if (!bottom_visible_.value() || bottom_tab_.value() != 0) {
      bottom_tab_.set(0);
      bottom_visible_.set(true);
    }
  }


  /// 把某个侧栏视图标记为“已访问”（此后它常驻树上、切回来还是原样）。
  ///
  /// 集中成一个函数（而不是散在各入口）：活动栏、状态栏分支格、命令面板、
  /// “与…比较”按钮……都能切视图；漏一处的表现是“从那个入口切过去、切回来位置就丢了”
  /// ——而这种“只有某条路径会丢”最难被发现。
  void CodeEditorPage::mark_sidebar_visited(std::size_t index) {
    auto visited = sidebar_visited_.value();
    if (std::find(visited.begin(), visited.end(), index) != visited.end()) return;
    visited.push_back(index);
    sidebar_visited_.set(std::move(visited));
  }


  /// 切当前编辑器的高亮语言（也改写标签的语言字段，切回来时保持一致）。
  void CodeEditorPage::set_language(const std::string& name) {
    language_.set(name);
    auto list = buffers_.value();
    if (active_.value() < list.size()) list[active_.value()].language = name;
    buffers_.set(std::move(list));
    if (editor != nullptr) editor->set_language(name);
    status_.set("语言已切为 " + name);
  }


  /// 当前活动缓冲是否处于 Markdown 预览形态（是 .md 且开关开着）。
  bool CodeEditorPage::md_preview_active() const {
    const auto& buffers = buffers_.value();
    const std::size_t index = active_.value();
    if (index >= buffers.size()) return false;
    if (!md_preview_.value()) return false;
    const std::string& path = buffers[index].path;
    return path.ends_with(".md") || path.ends_with(".markdown") || path.ends_with(".mdx");
  }


  /// 切换 Markdown 预览/源码。非 md 缓冲上调用是**无操作**（按钮本来就只往 md 上装，
  /// 这条防线保险快捷键路径直接切到非 md 文件后残留开关引发误渲染）。
  void CodeEditorPage::toggle_md_preview() {
    const auto& buffers = buffers_.value();
    const std::size_t index = active_.value();
    if (index >= buffers.size()) return;
    const std::string& path = buffers[index].path;
    const bool is_md = path.ends_with(".md") || path.ends_with(".markdown") ||
                       path.ends_with(".mdx");
    if (!is_md) {
      status_.set("Markdown 预览只对 .md 文件可用");
      return;
    }
    md_preview_.set(!md_preview_.value());
    // ⚠ 关预览时必须清「已装载」标记：预览态下 CodeEditor 不在树上（互斥声明），
    // 切回时 dsl 的位置对齐会**新建**一个空编辑器（旧实例已随分支切换销毁）；
    // 不清的话 `build()` 末尾看到 `key == loaded_key_` 就跳过灌入，编辑器永远空白
    //（实测：预览→源码后编辑区全空，用户报「渲染切回源码显示空白」）。
    // 开预览不受影响（CodeEditor 被销毁，但 MarkdownView 自己按 buffers 同步内容）。
    if (!md_preview_.value()) loaded_key_.clear();
  }


  void CodeEditorPage::build_editor_area(Composer& c, const std::vector<OpenBuffer>& buffers, std::size_t active,
                         bool has_editor) {
    column(c, {.gap = 0.0f, .grow = true, .id = "editor-area"}, [&] {
      // 标签列表 = 文件标签 + 差异/合并标签（它们也是**一个标签**，不是顶掉标签栏的独占视图）。
      // key 约定：差异/合并用 `diff_key_`/`merge_key_` 的原值（`diff:…` / `merge:…`），
      // 与文件标签的 key（样例名/绝对路径）天然不撞。
      std::vector<TabData> tabs;
      for (const auto& buffer : buffers) {
        tabs.push_back(TabData{.key = buffer.key, .label = buffer.label,
                               .modified = buffer.dirty, .closable = true});
      }
      const bool diff_open = !diff_key_.value().empty();
      const bool merge_open = !merge_key_.value().empty();
      if (merge_open) {
        // 合并标签排在差异前面（与内容区互斥顺序一致：合并优先级最高）。
        tabs.push_back(TabData{.key = merge_key_.value(),
                               .label = std::format("◨ {}", st::fs::file_name(merge_path_.value())),
                               .modified = false, .closable = true});
      }
      if (diff_open) {
        tabs.push_back(TabData{.key = diff_key_.value(),
                               .label = diff_title_.value().empty()
                                            ? std::string("Δ 差异")
                                            : diff_title_.value(),
                               .modified = false, .closable = true});
      }
      // 活动索引：差异/合并任一打开 → 它就是当前内容（文件标签仍保留、可点回）。
      std::size_t active_index = active;
      if (merge_open) {
        active_index = buffers.size();
      } else if (diff_open) {
        active_index = buffers.size() + (merge_open ? 1 : 0);
      }
      // 标签条。
      //
      // 右侧曾有一个「关闭全部编辑器」按钮（id `tab-close-all`，图标 `trash`）——
      // 用户 2026-10-08 问“这个删除按钮干什么用的，删了行不”，已删。三条理由：
      // ① 图标用 `trash`（垃圾桶）语义是“删除”，与“关闭”不同，容易被当成删文件；
      // ② 每个标签自带 × 关闭，而“全部关闭”在**菜单「文件 → 关闭全部编辑器」**
      //    已有入口（旧按钮是重复入口）；
      // ③ 它占 30px 横向宽度，标签多了之后标签区更紧。
      // `close_all()` 本体保留——菜单那条路径仍走它。
      //
      // ⚠ `tabbar` 这层 row **不能省**：外层 `editor-area` 是 column，`Tabs` 的
      // `grow` 在 column 里是**纵向**撑满——不套一层横向 row 的话标签条会长到
      // 整个编辑区那么高（实测：删掉 row 后 `#editor-tabs` 高 200px）。
      // row 的高度固定 34，`Tabs` 的 `grow` 变成横向撑满（它本来的意图）。
      //
      // —— Markdown 预览切换（2026-10-13）——
      // 当前活动缓冲是 .md 时，标签条右缘多一个「预览」开关（id `tab-md-preview`，
      // 控制通道可 invoke）：开 = 编辑区换 MarkdownView（只读渲染），关 = 源码编辑。
      // 为什么放标签栏而不是编辑器内：它是**文档级**开关（跟当前标签走），
      // 放标签栏能一眼看到“这个文件现在处于哪种形态”；且非 md 文件时自动隐藏。
      const bool is_md = active < buffers.size() &&
                         (buffers[active].path.ends_with(".md") ||
                          buffers[active].path.ends_with(".markdown") ||
                          buffers[active].path.ends_with(".mdx"));
      (void)row(c, {.gap = 0.0f, .height = 34.0f, .surface = Element::Surface::Alt,
                    .id = "tabbar"}, [&] {
        Element& tabs_element = dsl::tabs(
            c, tabs, active_index, [this](std::size_t index) { switch_tab(index); },
            [this](const std::string& key) { request_close(key); },
            {.grow = true, .id = "editor-tabs"});
        // ⚠ 声明式重组**每帧都会跳到这个 configure**，而 `Tabs` 元素按 key 复用
        //（尾部槽是它身上的可变状态）——直接 `add_trailing_button` 会每帧追加一个，
        // 越点越多（实测：切十几次预览后尾部堆了十几个眼睛/× 图标）。
        // 框架已提供目标态对齐接口（begin/sync/finish_trailing_sync，槽内复用不堆叠）。
        // **单一预览按钮**：图标恒为 eye，激活态（预览中）用 tooltip 区分语义；
        // 点击 = 开/关预览（同一个按钮，不按状态换图标——用户要求只有一个预览钮）。
        if (auto* widget = dynamic_cast<Tabs*>(&tabs_element); widget != nullptr) {
          widget->begin_trailing_sync();
          if (is_md) {
            (void)widget->sync_trailing_button(
                "eye",
                md_preview_.value() ? "关闭 Markdown 预览（Ctrl+Shift+V）"
                                    : "预览 Markdown（Ctrl+Shift+V）",
                [this] { toggle_md_preview(); });
          }
          widget->finish_trailing_sync();   // 非 md 时目标态为空 → 尾部清空（同一条路径）
        }
      });
      if (!has_editor && tabs.empty()) {
        column(c, {.padding = 24.0f, .grow = true,
                    // 样式系统示范（styling.hpp）：一行声明 = 抬升卡片 + 大圆角，
                    // 阴影/表面/高光随主题联动（换肤自动跟随，无需在此取色）。
                    .elevation = Elevation::Raised, .radius_mode = RadiusMode::LG,
                    .id = "empty-hint"}, [&] {
          (void)icon(c, "code", 40.0f);
          (void)text(c, [] {
            return std::string("从左侧资源管理器打开一个文件，或 Ctrl+Shift+P 打开命令面板");
          });
        });
        return;
      }
      // —— 差异视图 / 合并视图 / 普通编辑器 ——（互斥地共享编辑区：
      // 它们都是“现在要看的内容”，而分屏会让每一块都变窄）。
      // 标签栏已经能看到全部标签（文件 + 差异 + 合并），点文件标签即可切回编辑器
      //（`switch_tab` 清空 diff/merge key），点差异标签重新打开。
      //
      // 顺序**不能反**：合并/差异优先级最高——它们是在变更面板（或提交详情/文件历史）
      // 里**点开的一件东西**，若它们后面还有独占内容且先占位，点文件后屏幕上
      // 什么都不会变（实测：点清单无反应，而状态栏已经报“差异：…”——看着像“点不动”）。
      //
      // 合并排在差异前面：它是从变更面板/横幅里点开的**处置动作**，
      // 优先级高于“看差异”这个阅读动作。
      if (!merge_key_.value().empty()) {
        (void)custom<MergeView>(c, [this](MergeView& view) {
          view.set_id("merge-view");
          // ⚠ **只在内容真的变了时才 `set_sides`**：它会把选择状态全部重置，
          // 而声明式每帧都重建这个回调——每帧重置的话用户刚点的“取他方”会被
          // 下一秒的帧抹掉（表现：“点了没反应”或“选一下就弹回去”）。
          if (view.ours_text() != merge_ours_text_.value() ||
              view.theirs_text() != merge_theirs_text_.value() ||
              view.base_text() != merge_base_text_.value()) {
            view.set_sides(merge_base_text_.value(), merge_ours_text_.value(),
                           merge_theirs_text_.value());
          }
          merge_view_ptr = &view;   // F7 冲突导航靠它
          view.on_choice_changed = [this] {
            // ⚠ **不在回调里写盘**：它会在事件派发过程中同步写文件，而写盘
            // 可能失败/变慢，而此刻 UI 正处于“半完成”的事件处理里。
            // 只置一个标记，帧首泵里统一落盘（那里也是唯一改文件的位置）。
            merge_write_pending_ = true;
          };
        }, {.grow = true, .id = "merge-view-host"});
        return;
      }
      merge_view_ptr = nullptr;
      md_view_ptr = nullptr;
      if (!diff_key_.value().empty()) {
        // 差异视图外面包一层列：上面是逐行暂存工具条（仅在需要时出现），下面才是视图。
        //
        // 为何工具条放在**外面**而不是让 `DiffView` 自己画头部：那条工具条上的按钮要
        // 触发的是**应用层的写操作**（`run_write` + 审计），而组件不该知道 git 是什么。
        (void)column(c, {.grow = true, .id = "diff-column"}, [&] {
          // 入口：**必须有**一个把 `diff_staging_` 置真的东西。
          // 这里原本只有“退出逐行暂存”（`set(false)`），没有任何开关——
          // 于是整套逐行暂存（数据层 + 工具条 + 单测）在当前应用里**不可达**。
          (void)row(c, {.gap = 6.0f, .height = 30.0f, .id = "diff-staging-toggle-row"}, [&] {
            (void)custom<Button>(c, [this](Button& b) {
              b.set_id("diff-staging-toggle");
              b.set_label(diff_staging_.value() ? "退出逐行暂存" : "逐行暂存…");
              b.set_size(Button::Size::Small);
              b.on_click = [this] { diff_staging_.set(!diff_staging_.value()); };
            }, {.height = 24.0f, .id = "diff-staging-toggle-btn"});
            if (!diff_staging_.value()) {
              (void)text(c, [] { return std::string("挑着提交几行时用"); },
                         {.grow = true, .id = "diff-staging-hint"});
            }
          });
          if (diff_staging_.value()) {
            (void)row(c, {.gap = 6.0f, .height = 30.0f, .id = "diff-staging-bar"}, [&] {
              const std::size_t count =
                  diff_view_ptr == nullptr ? 0U : diff_view_ptr->selected_count();
              (void)text(c, [count] {
                return count == 0U ? std::string("逐行暂存：勾选行后点右侧按钮")
                                   : std::format("已选 {} 行", count);
              }, {.grow = true, .id = "diff-staging-count"});
              (void)custom<Button>(c, [this](Button& b) {
                b.set_id("diff-stage-selected");
                b.set_label("暂存选中行");
                b.set_size(Button::Size::Small);
                // 未选任何行时就**如实禁用**（按了没反应比看得见的禁用更差）。
                // 读 State 而非直读组件：`selected_count()` 是组件内部值，
                // 不用 State 的话勾选变化不会触发重组，按钮永远不解除禁用（实测）。
                b.set_enabled(diff_selected_count_.value() > 0U);
                b.on_click = [this] { stage_selected_diff_lines(); };
              }, {.height = 24.0f, .id = "diff-stage-selected-btn"});
              (void)custom<Button>(c, [this](Button& b) {
                b.set_id("diff-unstage-selected");
                b.set_label("取消暂存选中");
                b.set_size(Button::Size::Small);
                b.set_enabled(diff_selected_count_.value() > 0U);
                b.on_click = [this] { unstage_selected_diff_lines(); };
              }, {.height = 24.0f, .id = "diff-unstage-selected-btn"});
              (void)custom<Button>(c, [this](Button& b) {
                b.set_id("diff-staging-close");
                b.set_label("退出逐行暂存");
                b.set_size(Button::Size::Small);
                b.on_click = [this] { diff_staging_.set(false); };
              }, {.height = 24.0f, .id = "diff-staging-close-btn"});
            });
          }
          (void)custom<DiffView>(c, [this](DiffView& view) {
            view.set_id("diff-view");
            view.set_read_only(true);
            view.set_font_size(0.0f);
            view.set_staging_mode(diff_staging_.value());
            view.set_diff(diff_file_.value());
            // 勾选变化 → **必须触发一次重组**。
            //
            // 为何（实测）：工具条上的「暂存选中行」按 `selected_count()` 算 enabled，
            // 而 `enabled` 是**构建期写入**的属性。勾选发生在组件内部（自绘命中），
            // 不经过声明式的状态，于是没有重组——按钮永远停在初始的 disabled，
            // 用户勾了行也点不动（看上去像“勾选没生效”）。这里用一个应用层计数 State
            // 把“组件内部选择变了”抬成响应式信号。
            view.on_selection_changed = [this] {
              const std::size_t count =
                  diff_view_ptr == nullptr ? 0U : diff_view_ptr->selected_count();
              if (diff_selected_count_.value() != count) diff_selected_count_.set(count);
            };
            diff_view_ptr = &view;   // F7（下一个差异）靠它
          }, {.grow = true, .id = "diff-view-host"});
        });
        return;
      }
      // 编辑器：custom<T> 逃生舱（CodeEditor 的一等接口属性面覆盖不到）。
      //
      // 头部**只保留标签栏**（用户 2026-10-08 要求“头部内容都去掉，只保留 tab 和内容”）——
      // 工具栏（`build_editor_toolbar`）与面包屑（`build_breadcrumb`）不在此装配，
      // 它们各自的函数保留在文件里（应用如需可随时接回）。
      //
      // **只写“持久配置”**（字体/行宽），且每项都自带相等早退——它们在语义上是
      // 宿主配置、不归属性面管。**语言与只读态不在这里写**：它们由“当前标签”决定
      // （见 `build()` 末尾的装载逻辑与 `#editor` 的动作面），每次重组都重设会把
      // 控制通道 `set language=...` / `set read_only=true` 当场抹掉（实测：切一次
      // 底部面板就全回默认值）。
      //
      // —— Markdown 预览（2026-10-13）：当前缓冲是 .md 且 `md_preview_` 开着时，
      // 编辑器位置换成一个只读 MarkdownView。与编辑器互斥声明（不是同帧两棵树）：
      // 预览态下不声明 CodeEditor，`editor` 指针自然为空（所有编辑类动作都有空判，
      // 状态栏光标文本停在最后一次编辑的位置——预览是阅读形态，这是预期行为）。
      if (md_preview_active()) {
        (void)custom<MarkdownView>(c, [this](MarkdownView& view) {
          view.set_id("md-preview");   // 控制通道钩子（可 get/set/selectable）
          // 内容只在真变了时写（set_markdown 会重置滚动；每帧重写会把用户滚到的
          // 位置抹回顶部——与 MergeView set_sides 同一类坑）。
          const auto& buffers_now = buffers_.value();
          const std::size_t index = active_.value();
          if (index < buffers_now.size()) {
            if (view.markdown() != buffers_now[index].text) {
              view.set_markdown(buffers_now[index].text);
            }
          }
          view.style().grow = true;
          view.style().padding = st::math::Insets{12.0f, 16.0f, 12.0f, 16.0f};
          // 链接是一等交互（2026-10-13）：点击把 URL 交给宿主打开。
          // 框架只上报「点了哪个 URL」，打开浏览器是平台能力——
          // 与剪贴板同一取向（组件不该知道平台后端长什么样）。
          view.on_link_click = [this](std::string_view url) {
            st::log::info("[gbcode] 打开链接：{}", url);
            status_.set(std::format("链接：{}", url));
            (void)st::platform::open_external(url);
          };
          view.on_link_hover = [this](std::string_view url) {
            // 悬停时状态栏给出 URL——与浏览器的底部提示同一体验。
            if (!url.empty()) status_.set(std::format("链接：{}", url));
          };
          md_view_ptr = &view;
        }, {.grow = true, .id = "md-preview-host"});
        return;
      }
      (void)custom<CodeEditor>(c, [this](CodeEditor& ed) {
        ed.set_id("editor");   // 控制通道钩子：tools/*.py 依赖
        // 每帧无条件写会把控制通道 / 设置项的 `set font_scale=…` 当场抹掉——
        // 与 `read_only` 同一类缺陷（实测：把档位改成 1.0，下一帧就变回 1.15）。
        if (!editor_configured_) {
          ed.set_font_scale(editor_font_scale_);
          ed.set_line_spacing(editor_line_spacing_);
          ed.set_tab_width(4);
          editor_configured_ = true;
        }
        ed.style().grow = true;
        ed.style().padding = st::math::Insets{8.0f, 4.0f, 8.0f, 4.0f};
        ed.on_change = [this](std::string_view text) {
          on_edit();
          // 补全：每次编辑都请求（server 侧很快），但**带防抖**——
          // 只在弹层已开或刚输入触发字符时才发，避免每敲一个键都往返。
                if (completion_open_.value()) request_completion_now(text);
        };
        ed.on_cursor_change = [this] {
          on_cursor_moved();
          if (completion_open_.value()) update_completion_anchor();
        };
        // 悬停词变化 → 防抖后发 LSP hover（不做防抖的话每次像素移动都发请求，
        // server 没那么快，旧响应未到新请求又出——提示会闪跳）。
        ed.on_hover_word = [this](std::size_t begin, std::size_t end) {
          hover_word_pending_ = {begin, end};
          hover_word_changed_at_ = st::time::now_ms();
        };
        // 右键菜单走组件的一等回调（不是 `set_event_handler`——那个永远轮不到：
        // `CodeEditor::on_event` 对任何按钮的按下都返回 true）。
        ed.on_context_menu = [this](st::math::Point at) { open_context_menu(at); };
        // **`Ctrl+点击` → 跳到定义**（VSCode 的编辑器基本手势）。
        //
        // 两个前置条件缺一不可，且都不在应用层：
        // ① 组件把这次点击**当作另一种语义**接住（不先移光标，见 `on_ctrl_click` 的说明）；
        // ② 后端把修饰键**真的填进事件**（Win32 的鼠标消息不带修饰键，
        //    旧实现下 `event.ctrl` 恒为 false——这功能只会在无头模式下可用的假象）。
        // 先把光标移到被点的位置再去跳：`goto_definition` 是“按**当前光标**处取词”，
        // 而 Ctrl+点击的语义是“跳我点到的那个符号”。
        ed.on_ctrl_click = [this](std::size_t index) {
          if (editor == nullptr) return;
          editor->set_cursor_index(index);
          on_cursor_moved();
          goto_definition();
        };
        editor = &ed;
      }, {.grow = true, .id = "editor-host"});
    });
  }


  /// 编辑器工具栏（在标签栏下方）：保存 / 撤销 / 重做 / 注释 / 参考线 / 右键菜单入口。
  ///
  /// 为什么要有它（VSCode 其实没有这条工具栏）：霜天的控制通道与键盘快捷键是两条腿，
  /// 而**鼠标用户没有第三条腿**——“保存/撤销”这些最常用的动作在纯鼠标下无处可点。
  /// 工具栏把它们变成可发现、可点击、且有稳定 id 的元素（自动化也能直接 `invoke`）。
  void CodeEditorPage::build_editor_toolbar(Composer& c, const std::vector<OpenBuffer>& buffers,
                            std::size_t active) {
    (void)row(c, {.gap = 2.0f, .padding_x = 8.0f, .height = 30.0f, .id = "editor-toolbar"}, [&] {
      const auto tool = [&](const char* id, const char* icon, const char* label,
                            std::function<void()> on_click) {
        (void)custom<Button>(c, [id, icon, label, on_click = std::move(on_click)](Button& b) {
          b.set_id(id);
          b.set_icon(icon);
          b.set_label(label);
          b.set_variant(Button::Variant::Ghost);
          b.set_size(Button::Size::Small);
          b.on_click = on_click;
        }, {.height = 26.0f, .key = id});
      };
      tool("tool-save", "download", "保存", [this] { save(); });
      tool("tool-undo", "arrow-left", "撤销", [this] {
        if (editor != nullptr) (void)editor->undo();
      });
      tool("tool-redo", "arrow-right", "重做", [this] {
        if (editor != nullptr) (void)editor->redo();
      });
      tool("tool-comment", "code", "注释", [this] {
        if (editor != nullptr) (void)editor->toggle_comment();
      });
      (void)spacer(c);
      const bool dirty = active < buffers.size() && buffers[active].dirty;
      (void)text(c, [dirty] { return dirty ? std::string("● 未保存") : std::string("已保存"); },
                 {.id = "editor-dirty-hint"});
      tool("tool-find", "search", "查找", [this] { open_find(); });
      tool("tool-close", "close", "关闭", [this] { close_active(); });
    });
  }


  /// 面包屑：当前文件路径按 `/` 拆段（段可点——点目录段切到资源管理器并展开）。
  void CodeEditorPage::build_breadcrumb(Composer& c, const std::vector<OpenBuffer>& buffers, std::size_t active) {
    if (active >= buffers.size()) return;
    const OpenBuffer& buffer = buffers[active];
    // 显示路径：取相对工作区的路径（没有工作区时回退到 buffer.path）。
    std::string shown = buffer.path.empty()
                            ? buffer.key
                            : (workspace_.empty() ? buffer.path
                                                  : st::fs::relative_to(buffer.path, workspace_));
    const auto segments = st::split(shown, '/');
    (void)row(c, {.gap = 2.0f, .padding_x = 10.0f, .height = 26.0f, .id = "breadcrumb"}, [&] {
      for (std::size_t index = 0; index < segments.size(); ++index) {
        if (index > 0) {
          (void)icon(c, "chevron-right", 11.0f, {.id = std::format("crumb-sep-{}", index)});
        }
        const bool last = index + 1 == segments.size();
        const std::string segment(segments[index]);
        (void)custom<Button>(c, [this, segment, last](Button& b) {
          b.set_label(segment);
          b.set_id(std::format("crumb-{}", segment));
          b.set_variant(Button::Variant::Ghost);
          b.set_size(Button::Size::Small);
          b.on_click = [this, segment, last] {
            // 点目录段：切到资源管理器（树里高亮/展开由树自身管理）；点文件名段=无动作。
            activity_.set(0);
            sidebar_visible_.set(true);
            mark_sidebar_visited(0);
            status_.set(last ? "当前文件：" + segment : "已切到资源管理器：" + segment);
          };
        }, {.height = 22.0f, .key = "crumb:" + segment});
      }
    });
  }


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
  void CodeEditorPage::build_workbench(Composer& c, const std::vector<OpenBuffer>& buffers, std::size_t active,
                       bool has_editor) {
    row(c, {.gap = 0.0f, .grow = true, .id = "workbench-row"}, [&] {
      // 活动栏：**常驻**且**满高**（见上）。侧栏依旧不常驻（两者是独立的东西）。
      build_activity_bar(c);
      (void)custom_container<SplitView>(
          c,
          [&] {
            (void)custom_container<Panel>(
                c, [&] { build_main(c, buffers, active, has_editor); },
                [](Panel& panel) { panel.set_id("editor-upper"); }, {.grow = true, .key = "upper"});
            // 终端面板**常驻声明、收起只隐藏**（`set_second_hidden`）：
            // `Terminal` 及其全部会话/回看留在树上——重开即恢复，不再重起 shell。
            // 代价是收起时多一棵隐藏子树，换来的是状态保留（VSCode 同款行为）。
            (void)custom_container<Panel>(
                c, [&] { build_bottom(c); },
                [](Panel& panel) { panel.set_id("editor-lower"); }, {.grow = true, .key = "lower"});
          },
          [this](SplitView& split) {
            split.set_id("bottom-split");
            split.set_orientation(SplitView::Orientation::Vertical);
            split.set_min_ratio(0.05f);
            split.set_ratio(bottom_ratio_, false);
            // 归位目标 = 默认比例（编辑器 2/3 ⇒ 终端 1/3 高）。
            split.set_default_ratio(kTerminalTopRatio);
            // 收起/展开：只翻隐藏位（子元素与比例都保留）。
            split.set_second_hidden(!bottom_visible_.value());
            // 拖拽回调只写一个**非响应式**成员：比例由组件自己持有并重排，
            // 写 State 会每拖动一像素触发一次整页重组（浪费且在拖拽中重建子元素）。
            split.on_change = [this](float ratio) { bottom_ratio_ = ratio; };
          },
          {.grow = true, .id = "bottom-split"});
    });
  }


  // 终端面板的组装归**框架组件** `st::ui::Terminal`——应用侧那份自绘的
  // 滚回 + 输入行 + 滚动跟随已经删掉（见 `build_bottom`）。


    // —— 5. 状态栏（兼容钩子 id 全保留：`status` / `btn-theme` / 计数 / 光标 / 语言）——
  //
  // 每一项尽量做成**可点**的：状态栏是 IDE 里“看一眼”的地方，而看一眼之后往往
  // 想知道更多（分支 → 变更面板、问题数 → 问题面板、语言 → 语言列表……）。
  void CodeEditorPage::build_status_bar(Composer& c) {
    row(c, {.gap = 10.0f, .padding_x = 10.0f, .height = 26.0f,
            .surface = Element::Surface::Alt, .id = "statusbar"}, [&] {
      status_item(c, "status-branch", "git-branch", [this] {
        return git_.branch.empty() ? std::string("main") : git_.branch;
      }, [this] {
        // 点分支 → 切到 Git 工具窗（分支/日志在那里；左侧「变更」看的是改动，
        // 两者分工见 build_git_view 的注释）。**只设状态**——日志读取在帧首泵
        //（`ensure_git_log`）做，不在事件回调里起子进程。
        bottom_tab_.set(1);
        bottom_visible_.set(true);
        git_log_wanted_ = true;
      }, "当前分支 — 点一下打开 Git 工具窗");
      // 问题计数必须读快照版本：error_count()/warning_count() 读的是普通成员
      // problems_（不在依赖表里），不读版本的话文本 lambda 永不重跑——
      // 「诊断到了编辑器、计数恒为 0、点一下才对」就是这么来的（实测）。
      (void)problems_revision_.value();
      status_item(c, "status-errors", "error",
                  [this] { return std::to_string(error_count()); }, [this] { show_problems(false); },
                  "错误数 — 点一下跳到第一处");
      status_item(c, "status-warnings", "warning",
                  [this] { return std::to_string(warning_count()); },
                  [this] { show_problems(true); }, "警告数 — 点一下跳到第一处");
      (void)spacer(c);   // 弹性空隙：右侧信息组贴右缘（同标题栏）
      (void)text(c, [this] { return cursor_text_.value(); }, {.id = "cursor-label"});
      (void)text(c, [this] { return selection_text_.value(); }, {.id = "selection-label"});
      // 缩进 / 编码：点一下就地切换（缩进 2↔4↔8；编码在演示里是只读事实）。
      status_item(c, "status-indent", "", [this] { return std::format("空格: {}", tab_width_); },
                  [this] { cycle_tab_width(); }, "缩进宽度 — 点一下切换（2 / 4 / 8）");
      // 编码：做成可点状态项（与缩进/语言同构）。
  //
  // 为何必须可点（实测）：它是纯 `text` 时点击什么也不发生——而它两边都是
  // **可点格子**（缩进切档、语言开列表），第三个格子“点了没反应”会被当成键坏。
  // 点它的合理语义：告诉用户当前编码是怎么判定的、换不了的原因。
  status_item(c, "status-encoding", "", [] { return std::string("UTF-8"); }, [this] {
    status_.set("编码：UTF-8（本编辑器以 UTF-8 读写；非 UTF-8 文件不做自动转换）");
  }, "文件编码 — 点一下看说明");
      // 语言：点一下切到「扩展」视图（那里列出全部可选语言）
      status_item(c, "language-label", "", [this] { return active_language(); }, [this] {
        activity_.set(4);
        sidebar_visible_.set(true);
        mark_sidebar_visited(4);
      }, "当前语言 — 点一下打开语言列表");
      // blame 行尾注解开关：放在状态栏（它影响“现在看到的正文”，而状态栏正是“当前显示什么”的位置）。
      // 做成可点元素还有一个实际好处：**它能被控制通道 `invoke` 触发**
      //（命令面板那条路要模拟键盘，e2e 里脆而慢）。
      status_item(c, "status-blame", "git-branch",
                  [this] { return blame_on_ ? std::string("blame 开") : std::string("blame"); },
                  [this] { toggle_blame(); },
                  "行尾 blame 注解 — 点一下开/关（显示当前视口行的作者与日期）");
      (void)text(c, [this] { return status_.value(); }, {.id = "status"});
      (void)button(c, theme_is_dark() ? "亮色" : "暗色", [this] { toggle_theme(); },
                   {.id = "btn-theme"});
    });
  }


  /// 一个可点的状态栏项（图标 + 文本，点击即触发动作）。
  ///
  /// `hint` 说明“点一下会发生什么”——状态栏是“看一眼”的地方，那些计数/语言/缩进
  /// 格子点下去之后去哪里并不显然（提示是走这一步前的唯一预告）。
  void CodeEditorPage::status_item(Composer& c, const char* id, const char* icon,
                   std::function<std::string()> content, std::function<void()> on_click,
                   std::string hint) {
    (void)custom<Button>(c, [id, icon, content = std::move(content),
                             on_click = std::move(on_click), hint = std::move(hint)](Button& b) {
      b.set_id(id);
      b.set_icon(icon);
      b.set_label(content());
      b.set_variant(Button::Variant::Ghost);
      b.set_size(Button::Size::Small);
      b.set_hint(hint);
      b.on_click = on_click;
    }, {.height = 22.0f, .key = id});
  }


  /// 点状态栏的问题计数：**在编辑器里定位第一处问题**。
  ///
  /// 为何不再“切到问题面板”：面板已删（见 `Problem` 的说明），而状态栏那个计数
  /// 用户真正想做的事是“带我去看看”——所以直接把光标送到那一行并高亮它。
  /// 反复点同一类计数就依次跳到下一处（比开一个列表更少一步）。
  void CodeEditorPage::show_problems(bool warnings_only) {
    refresh_problems();
    std::vector<std::size_t> targets;
    for (std::size_t index = 0; index < problems_.size(); ++index) {
      if (problems_[index].warning == warnings_only) targets.push_back(index);
    }
    if (targets.empty()) {
      status_.set(warnings_only ? "没有警告" : "没有错误");
      return;
    }
    problem_cycle_ = (problem_cycle_ + 1) % targets.size();
    jump_to_problem(targets[problem_cycle_]);
    status_.set(std::format("{} {}/{}：第 {} 行 {}", warnings_only ? "警告" : "错误",
                            problem_cycle_ + 1, targets.size(), problems_[targets[problem_cycle_]].line,
                            problems_[targets[problem_cycle_]].message));
  }


  /// 缩进宽度循环 2 → 4 → 8（点状态栏的缩进格）。
  void CodeEditorPage::cycle_tab_width() {
    tab_width_ = tab_width_ == 2 ? 4 : (tab_width_ == 4 ? 8 : 2);
    if (editor != nullptr) editor->set_tab_width(tab_width_);
    status_.set(std::format("缩进宽度已设为 {}", tab_width_));
  }

}  // namespace gbcode_ui
