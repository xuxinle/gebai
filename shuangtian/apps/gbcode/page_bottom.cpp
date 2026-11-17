// 底部面板族：终端/Git 工具窗（分支|日志|标签|暂存|远程）与提交详情、
// 不可逆写操作的确认浮层触发（ask_*）。拆分自 main.cpp，零逻辑改动。

#include <algorithm>
#include <cstdio>
#include <format>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "page.hpp"
#include "st/core/string.hpp"
#include "st/ui/components/table.hpp"

namespace gbcode_ui {


  /// 打开编辑器右键菜单（由 `CodeEditor::on_context_menu` 回调调用）。
  void CodeEditorPage::open_context_menu(st::math::Point at) {
    context_anchor_ = at;
    context_open_.set(true);
  }


  // —— 4. 上半 + 终端：一个**上下可分栏**的两段（拖动改高度，见 build）——
  //
  // 为什么中间要一个 SplitView 而不是两个固定高的兄弟：终端的高度是**用户偏好**
  // （看输出时想高、看代码时想矮），固定值只能靠改代码。分栏手柄还顺带把
  // 「上半 / 下半」的边界画成一条可抓的线（与侧栏同一种交互语言）。
  //
  // 底部面板**只有终端一个视图**（问题/输出已删，见 `Problem` 处的说明）——
  // 于是这里不再需要标签栏，只留一行“终端标题 + 状态 + 动作”。
  void CodeEditorPage::build_bottom(Composer& c) {
    // —— 底部单槽切换：终端 ↔ Git（同一停靠位，互斥显示）——
    //
    // 结构约定：**两个面板都直接声明在同一层**，非当前的那个用 `Panel::set_visible(false)`
    // 隐藏。两个要点：
    //
    // ① 不能“当前声明一个、隐藏时用另一层容器包起来”——那会让**结构随状态变**，
    //    框架按位置/结构对账时会把子树销毁重建（`Terminal` 一重建，会话与
    //    `terminal_ptr` 就全丢，实测栈在 `AnsiScreen::feed` 的悬垂指针）。
    // ② 两个面板都要包一层**带 key 的容器**：同级分支声明子元素会按位置回收，
    //    切槽时新分支里没显式设的属性会沿用旧分支的值（上一轮在 Git 五视图上
    //    实测过：按钮带着别的面板的 icon/宽度）。
    (void)custom_container<Panel>(
        c,
        [&] { build_terminal_body(c); },
        [this](Panel& panel) {
          panel.set_id("terminal-panel");
          // 终端也是“内容区”：`Sunken`。与编辑器同档——两者都是“我在这里读/写内容”，
          // 而外壳条带（标签条/状态栏）才是 `Alt`。
          panel.set_surface(Element::Surface::Content);
          panel.set_visible(bottom_tab_.value() == slot(BottomSlot::Terminal));
        },
        {.grow = true, .key = "pane:terminal"});
    (void)custom_container<Panel>(
        c,
        [&] { build_git_view(c); },
        [this](Panel& panel) {
          panel.set_id("git-panel");
          panel.set_surface(Element::Surface::Content);
          panel.set_visible(bottom_tab_.value() == slot(BottomSlot::Git));
        },
        {.grow = true, .key = "pane:git"});
    (void)custom_container<Panel>(
        c,
        [&] { build_problems_view(c); },
        [this](Panel& panel) {
          panel.set_id("problems-panel");
          panel.set_surface(Element::Surface::Content);
          panel.set_visible(bottom_tab_.value() == slot(BottomSlot::Problems));
        },
        {.grow = true, .key = "pane:problems"});
  }


  /// 终端本体（`build_bottom` 的终端面板内容）。
  void CodeEditorPage::build_terminal_body(Composer& c) {
    (void)custom<Terminal>(c, [this](Terminal& view) {
      view.set_id("terminal");
      // 默认目录跟着工作区（`switch_workspace` 会同步它）。
      view.set_working_directory(workspace_);
      // **真终端**：起一条真 shell（伪终端 + ANSI 屏幕 + 字节级输入）。
      // 组件自己去重：已有一条就不重复开。
      //
      // ⚠ **只在面板真的可见时才起**（用户要求“启动默认不打开终端”）：
      // 面板收起是“隐藏子树”而不是拆子树，所以不判断的话首帧就会 spawn 一个 shell——
      // 用户看到的是“我没开终端，但进程里已经挂着一个”（而且它占着工作目录与句柄）。
      // 切到可见时本函数会重跑，那时才真正拉起。
      if (!bottom_visible_.value()) {
        terminal_ptr = &view;
        return;
      }
      view.open_shell();
      // 忙闲变化 → 状态栏（"中止"钮的忙闲同步在组件内自动做）。
      view.on_busy_change = [this](bool busy) {
        terminal_busy_ = busy;
        status_.set(busy ? "终端：运行中" : "终端：已退出");
      };
      view.on_error = [this](const std::string& text) { status_.set("终端：" + text); };
      // shell 自己设的标题（`OSC 0/2`）→ 标签跟着变（初始标签是 shell 名）。
      view.on_title_change = [this](std::size_t index, const std::string& title) {
        if (terminal_ptr != nullptr) terminal_ptr->set_session_title(index, title);
      };
      // 换标签 → 状态栏报一声。
      view.on_session_change = [this](std::size_t) { status_.set("终端：已切换会话"); };
      // 组件内"收起"钮 → 应用侧翻隐藏位（面板收起是布局决策，归这里）。
      view.on_request_collapse = [this] { bottom_visible_.set(false); };
      // 关掉**最后一个**会话 = 收起面板（同一条链路）。
      //
      // 面板已是"常驻隐藏"方案（`set_second_hidden`）：`bottom_visible_.set`
      // 只翻隐藏位、不再拆 `Terminal` 子树，因此**可以在 pump 栈上直接调**。
      view.on_close_last_session = [this](bool last) {
        if (last) bottom_visible_.set(false);
      };
      terminal_ptr = &view;
    }, {.grow = true, .key = "terminal"});
  }


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
  void CodeEditorPage::build_git_view(Composer& c) {
    // 空态也要挂 `#git-view`（自动化判"视图出现"靠它——空态不是"没有视图"，
    // 是"视图里没有内容"，两码事）。
    if (workspace_.empty()) {
      column(c, {.padding = 12.0f, .grow = true, .id = "git-view"}, [&] {
        (void)text(c, [] { return std::string("没有工作区 —— 文件 → 打开文件夹"); });
      });
      return;
    }
    // 惰性读在**帧首泵**做（`ensure_git_log`）——渲染期起子进程会在重组栈上
    // fork + 管道等待，任何一处重入（状态 set 触发同步重组）都会踩坏声明式树
    //（实测：点状态栏分支打开 Git 标签 → SIGSEGV，崩点就在渲染期的同步 git 读）。
    // 渲染期只消费 `git_log_`/`git_branches_`/`git_tags_`/…。
    git_log_wanted_ = true;   // 本帧在渲染 Git 视图：下一帧泵读数据（首帧空属正常）
    column(c, {.gap = 0.0f, .grow = true, .id = "git-view"}, [&] {
      // 多步操作中：顶部横幅给“继续 / 跳过 / 中止”（冲突时没有它就只能去命令行）。
      build_operation_banner(c);
      // 三栏 = 嵌套两个 SplitView（`SplitView` 只有两面板：外层“左栏 | 右半”，
      // 右半再分“中栏 | 右栏”）。每层都带 key：不同层的子声明不能按位置回收。
      (void)custom_container<SplitView>(
          c,
          [&] {
            (void)custom_container<Panel>(
                c, [&] { build_ref_sidebar(c); },
                [](Panel& panel) { panel.set_id("git-refs-panel"); },
                {.key = "git-refs"});
            (void)custom_container<Panel>(
                c,
                [&] {
                  (void)custom_container<SplitView>(
                      c,
                      [&] {
                        (void)custom_container<Panel>(
                            c, [&] { build_log_pane(c); },
                            [](Panel& panel) {
                              panel.set_id("git-log-panel");
                              // 内边距：内容不贴面板边缘（标题/表格都退进来 8px）。
                              // 不给就是“字贴着边框”，密集面板里很难读。
                              panel.style().padding = st::math::Insets{8.0f, 8.0f, 8.0f, 8.0f};
                            },
                            {.grow = true, .key = "git-log"});
                        (void)custom_container<Panel>(
                            c, [&] { build_commit_detail(c); },
                            [](Panel& panel) {
                              panel.set_id("git-detail-panel");
                              panel.style().padding = st::math::Insets{8.0f, 8.0f, 8.0f, 8.0f};
                            },
                            {.grow = true, .key = "git-detail"});
                      },
                      [](SplitView& split) {
                        split.set_id("git-log-split");
                        split.set_min_ratio(0.2f);
                        split.set_ratio(0.62f, false);   // 中栏宽、右栏窄（IDEA 同）
                      },
                      {.grow = true, .key = "git-log-split"});
                },
                [](Panel& panel) { panel.set_id("git-log-column"); },
                {.grow = true, .key = "git-log-column"});
          },
          [](SplitView& split) {
            split.set_id("git-view-split");
            split.set_min_ratio(0.12f);
            split.set_ratio(0.2f, false);   // 左栏（引用）窄，其余给日志与内容
          },
          {.grow = true, .key = "git-view-split"});
    });
  }


  /// 左栏：引用（分支 / 标签 / 暂存 / 远程 四个分组，一次只显一组）。
  ///
  /// 为何用“分组切换”而不是四个固定块：四个列表长度差异很大（分支可能几十条、
  /// 远程通常一条），固定同时显示会被最短的那个浪费掉全部高度。
  void CodeEditorPage::build_ref_sidebar(Composer& c) {
    build_git_tabs(c);
    (void)column(c, {.gap = 4.0f, .padding = 8.0f, .grow = true, .id = "git-refs-body"}, [&] {
      switch (git_tab_.value()) {
        case 1:
          (void)column(c, {.gap = 4.0f, .grow = true, .id = "git-pane-branches",
                           .key = "pane:branches"}, [&] { build_branches_pane(c); });
          break;
        case 2:
          (void)column(c, {.gap = 4.0f, .grow = true, .id = "git-pane-tags",
                           .key = "pane:tags"}, [&] { build_tags_pane(c); });
          break;
        case 4:
          (void)column(c, {.gap = 4.0f, .grow = true, .id = "git-pane-stashes",
                           .key = "pane:stashes"}, [&] { build_stashes_pane(c); });
          break;
        default:
          (void)column(c, {.gap = 4.0f, .grow = true, .id = "git-pane-remotes",
                           .key = "pane:remotes"}, [&] { build_remotes_pane(c); });
          break;
      }
    });
  }


  /// Git 工具窗左栏的四个**引用分组**（分支 / 标签 / 暂存 / 远程）。
  void CodeEditorPage::build_git_tabs(Composer& c) {
    struct TabDef {
      const char* id;
      const char* label;
    };
    // 计数进标签（IDEA 同款）：空的列表与“读失败”在界面上一眼可区分。
    // ⚠ 这里只有**四个引用类**分组：日志现在占**中栏**（三栏布局），不再是一个标签。
    const std::string branches =
        std::format("分支{}", git_branches_.empty() ? "" : std::format(" {}", git_branches_.size()));
    const std::string tags =
        std::format("标签{}", git_tags_.empty() ? "" : std::format(" {}", git_tags_.size()));
    const std::string stashes =
        std::format("暂存{}", git_stashes_.empty() ? "" : std::format(" {}", git_stashes_.size()));
    const std::string remotes =
        std::format("远程{}", git_remotes_.empty() ? "" : std::format(" {}", git_remotes_.size()));
    const std::array<std::string, 4> labels{branches, tags, stashes, remotes};
    // 分组编号：1=分支 2=标签 4=暂存 5=远程。
    //
    // 为何是这几个数（而不是 0/1/2/3）：它们是**历史索引**，`git_tab_` 会被会话
    // 恢复/快捷键写入；且 3 曾是“日志”（现已上提为中栏），留着编号不动就少一次
    // 数据迁移。`build_ref_sidebar` 的 `switch` 与这里必须同源。
    const std::array<std::size_t, 4> numbers{1, 2, 4, 5};
    const std::array<const char*, 4> ids{"git-tab-branches", "git-tab-tags", "git-tab-stashes",
                                         "git-tab-remotes"};
    // 标签间距 **6**：不留缝的圆角胶囊会粘成一条，IDEA 新 UI 也是分开的。
    //
    // ⚠ **必须 wrap，且不能写死行高**：左栏宽度由 SplitView 的 ratio 决定（默认
    // 20%×1130≈226px），而四个标签实测共需≈259px（中文小号字 + 计数 + 6px 间距）。
    // 不换行时最后一个「远程」被排到面板右缘之外——面板会裁掉它，而**命中测试
    // 按未裁的框算**（中心点 x≈271 > 面板右缘 270），点击就穿透到背后的
    // `git-view-split`。实测症状：点「远程」标签没反应（e2e 卡在
    // `#remote-force-push` 永远不出现），而用 `invoke` 直接触发回调正常。
    // 行高交给内容（`.height` 不写）：写死行高的话，换行后第二行会溢出到
    // `git-refs-body` 上面，变成“标签叠在分支列表上”。
    (void)row(c, {.gap = 6.0f, .wrap = true, .id = "git-tabs"}, [&] {
      for (std::size_t index = 0; index < labels.size(); ++index) {
        (void)custom<Button>(c, [this, number = numbers[index], label = labels[index],
                                 id = ids[index]](Button& b) {
          b.set_id(id);
          b.set_label(label);
          b.set_variant(git_tab_.value() == number ? Button::Variant::Soft
                                                    : Button::Variant::Ghost);
          b.set_size(Button::Size::Small);
          b.on_click = [this, number] { git_tab_.set(number); };
        }, {.height = 24.0f, .key = std::format("gittab:{}", numbers[index])});
      }
    });
  }


  /// 多步操作横幅（merge/rebase/cherry-pick/revert 中）：继续 / 跳过 / 中止。
  void CodeEditorPage::build_operation_banner(Composer& c) {
    if (!git_operation_.active()) return;
    (void)row(c, {.gap = 6.0f, .id = "git-operation-banner"}, [&] {
      (void)text(c, [this] {
        const std::string tip =
            git_operation_.conflicts.empty()
                ? std::string{}
                : std::format("，{} 个冲突待解决", git_operation_.conflicts.size());
        return std::format("处置中的{} {}", git_operation_.kind, tip);
      }, {.grow = true, .color = Tone::Warning, .id = "git-operation-label"});
      struct Control {
        const char* id;
        const char* label;
        gbcode::WriteOp op;
      };
      static constexpr Control kControls[] = {{"git-op-continue", "继续",
                                               gbcode::WriteOp::Continue},
                                              {"git-op-skip", "跳过", gbcode::WriteOp::Skip},
                                              {"git-op-abort", "中止", gbcode::WriteOp::Abort}};
      for (const auto& control : kControls) {
        (void)custom<Button>(c, [this, control](Button& b) {
          b.set_id(control.id);
          b.set_label(control.label);
          b.set_variant(control.op == gbcode::WriteOp::Abort ? Button::Variant::Danger
                                                             : Button::Variant::Neutral);
          b.set_size(Button::Size::Small);
          b.on_click = [this, control] {
            // `ref` 位传“操作种类”（`merge`/`rebase`/…）——`write_args` 据此映射子命令。
            (void)run_write({.op = control.op, .ref = git_operation_.kind},
                            std::string("control:") + control.label);
          };
        }, {.height = 24.0f, .key = std::string("gitop:") + control.label});
      }
    });
  }


  /// 子视图 0：分支（本地/远程、切换、新建、删除）。
  /// 分支面板（**IDEA 结构**：搜索框 + HEAD/本地/远程 三组可折叠树）。
  ///
  /// ## 为什么不再是一张平铺列表
  ///
  /// IDEA 的分支树回答的是三个不同问题，混在一张平铺表里全都答不好：
  /// ① **我现在在哪**（HEAD 组，只有一条）；
  /// ② **本地有哪些可切**（本地组）；
  /// ③ **远端有哪些**（远程组，按 remote 再分一层）。
  /// 平铺时 15 个分支 + 若干远程条目挤在一起，`origin/xxx` 与本地 `xxx` 只差前缀，
  /// 扫一眼分不清「这个能切」还是「这是远端镜像」。
  ///
  /// ## 三个可折叠组为什么用 `State` 而不是 Tree 组件
  ///
  /// 组只有三层、条目是纯按钮（点击即切分支），用 `Tree` 要额外建一套 key/label/图标
  /// 映射并绕开它的选中语义；这里用「`State<bool>` + 条件声明」更直白，而且
  /// 折叠状态天然持久（`State` 跨帧存活），与左侧资源管理器的展开态同一手法。
  void CodeEditorPage::build_branches_pane(Composer& c) {
    // —— 搜索框（IDEA 顶部「分支或标签」）——
    //
    // 过滤**本地名与远程短名**（不带 `origin/` 前缀）：用户脑子里想的是 "feature"，
    // 不是 "origin/feature"。
    (void)custom<Input>(c, [this](Input& input) {
      input.set_id("git-branch-filter");
      input.set_placeholder("分支或标签");
      input.set_text(branch_filter_text_.value());
      input.on_change = [this](std::string_view text) { branch_filter_text_.set(std::string(text)); };
    }, {.height = 26.0f, .key = "git-branch-filter"});

    (void)row(c, {.gap = 4.0f, .id = "git-branches-actions"}, [&] {
      scm_icon_button(c, "branch-new", "plus", "新建分支", [this] {
        // 新建分支默认从 HEAD 起（IDEA 的 New Branch 同此）。
        branch_new_name_.set(std::string{});
        branch_new_open_.set(true);
      });
      (void)text(c, [this] { return git_.branch.empty() ? std::string{} : "当前：" + git_.branch; },
                 {.grow = true, .color = Tone::Muted, .id = "git-current-branch"});
    });

    (void)custom_container<ScrollView>(
        c,
        [&] {
          if (git_branches_.empty()) {
            (void)text(c, [] { return std::string("（没有分支，或读取失败）"); });
            return;
          }
          const std::string needle = std::string(st::trim(branch_filter_text_.value()));
          /// 是否命中搜索词（对**短名**匹配，见本函数头注释）。
          const auto matches = [&needle](std::string_view short_name) {
            return needle.empty() || short_name.find(needle) != std::string_view::npos;
          };

          // 三组先分桶（一次遍历；顺序保持 git 给的顺序，不重排——那是用户的仓库顺序）。
          std::vector<const gbcode::BranchInfo*> locals;
          std::vector<const gbcode::BranchInfo*> remotes;
          const gbcode::BranchInfo* head = nullptr;
          for (const auto& branch : git_branches_) {
            const std::string_view short_name = gbcode::short_branch_name(branch);
            if (!matches(short_name)) continue;
            if (branch.is_current) head = &branch;
            if (branch.is_remote) {
              remotes.push_back(&branch);
            } else {
              locals.push_back(&branch);
            }
          }
          const bool nothing = head == nullptr && locals.empty() && remotes.empty();
          if (nothing) {
            (void)text(c, [needle] {
              return needle.empty() ? std::string("（没有分支）")
                                    : std::format("没有匹配「{}」的分支", needle);
            }, {.color = Tone::Muted, .id = "git-branch-empty"});
            return;
          }

          // —— 组的公共构件 ——
          //
          // 一组的形态：`[▾/▸] [图标] 标题 (计数)` + 展开时的条目。
          // 折叠箭头在左（IDEA 同位置），整行可点（点击目标远大于那个小三角）。
          const auto group_header = [&](std::string_view key, std::string_view title,
                                        std::size_t count, State<bool>& open,
                                        std::string_view icon_name) {
            (void)custom<Button>(c, [&open, key, title, count, icon_name](Button& button) {
              button.set_id("git-group-" + std::string(key));
              button.set_align(Button::Align::Start);
              // 箭头 + 图标 + 标题 + 计数：全在一个 label 里（`Button` 只画一行）。
              button.set_label(std::format("{} {}  {} ({})", open.value() ? "▾" : "▸",
                                           icon_name, title, count));
              button.set_variant(Button::Variant::Ghost);
              button.set_size(Button::Size::Small);
              button.on_click = [&open] { open.set(!open.value()); };
            }, {.height = 24.0f, .grow = true, .key = "group:" + std::string(key)});
          };

          // —— ① HEAD（只有一条：当前分支）——
          //
          // IDEA 把它单列一组的原因：它是唯一“已经在用”的东西，点它应当是**查看**
          // 而非切换。这里给 Soft 变体（与其余条目区分）+ 不可删除。
          if (head != nullptr) {
            group_header("head", "HEAD", 1, branch_head_open_, "●");
            if (branch_head_open_.value()) {
              // ⚠ key 与 id 都带 **`head:` 前缀**：同一条分支在下方的“本地”组里
              // 还会再出现一次（职责不同，见本段注释）。两处若用同一 key，
              // 声明式会按 key 把两个位置对齐到同一个实例——两行共用一份状态，
              // 表现为“点 HEAD 里那条，高亮跑到了本地组”这类错位。
              (void)row(c, {.gap = 4.0f, .key = "head:" + head->name}, [&] {
                (void)custom<Button>(c, [this, branch = *head](Button& button) {
                  button.set_id("git-branch-head-" + branch.name);
                  button.set_align(Button::Align::Start);
                  button.set_label("    " + std::string(gbcode::short_branch_name(branch)));
                  button.set_variant(Button::Variant::Soft);
                  button.set_size(Button::Size::Small);
                  // HEAD 也走同一套：点它就是“只看当前分支的历史”（等价于打开“仅当前分支”）。
                  button.on_click = [this, branch] { filter_log_by_branch(branch); };
                }, {.height = 24.0f, .grow = true});
              });
            }
          }

          // —— ② 本地 ——
          //
          // 含**当前分支**（它同时出现在 HEAD 组里）：两处职责不同——
          // HEAD 组回答“我现在在哪”（一条、置顶、不可删），本地组回答
          // “这座仓有哪些本地分支”（完整清单，当前分支用 Soft 高亮标出）。
          // 早期实现在这里 `continue` 跳过当前分支，结果本地组少一条、
          // 用户在本地清单里找不到自己在用的那条（目录与计数都对不上）。
          if (!locals.empty()) {
            group_header("local", "本地", locals.size(), branch_local_open_, "⏣");
            if (branch_local_open_.value()) {
              for (const gbcode::BranchInfo* branch : locals) {
                const bool current = branch->is_current;
                (void)row(c, {.gap = 4.0f, .key = "branch:" + branch->name}, [&] {
                  (void)custom<Button>(c, [this, branch = *branch, current](Button& button) {
                    button.set_id("git-branch-" + branch.name);
                    button.set_align(Button::Align::Start);
                    button.set_label("    " + std::string(gbcode::short_branch_name(branch)));
                    // 当前分支**高亮**（Soft = 主题主色的浅底，与 HEAD 组一致）。
                    // 不给高亮就只剩缩进，一屏十几条时看不出自己在哪条上。
                    button.set_variant(current ? Button::Variant::Soft
                                               : Button::Variant::Ghost);
                    button.set_size(Button::Size::Small);
                    // 点击 = **筛日志**（不是检出）：检出会改工作区，是写操作，
                    // 不该由“在侧栏看一眼这条分支的历史”触发。
                    button.on_click = [this, branch] { filter_log_by_branch(branch); };
                  }, {.height = 24.0f, .grow = true});
                  // 当前分支不可删（删了就没地方站了）；远程分支的删除在别处管。
                  if (!current) {
                    scm_icon_button(c, "delbranch-" + branch->name, "trash", "删除分支",
                                    [this, name = branch->name] { ask_delete_branch(name); });
                  }
                });
              }
            }
          }

          // —— ③ 远程（按 remote 再分一层）——
          //
          // IDEA 的层级是 `远程 > origin > master/feature`。多一层缩进的意义：
          // 一个仓库常配多个 remote（origin / upstream / 同事的 fork），
          // 平铺时 `origin/xxx` 与 `upstream/xxx` 混在一起，看不出谁是同一台远端。
          if (!remotes.empty()) {
            group_header("remote", "远程", remotes.size(), branch_remote_open_, "☁");
            if (branch_remote_open_.value()) {
              // 按 remote 名分组（保持首次出现顺序）。
              std::vector<std::pair<std::string, std::vector<const gbcode::BranchInfo*>>> by_remote;
              for (const gbcode::BranchInfo* branch : remotes) {
                const std::string remote = gbcode::remote_of_branch(*branch);
                auto slot = std::find_if(by_remote.begin(), by_remote.end(),
                                         [&remote](const auto& pair) { return pair.first == remote; });
                if (slot == by_remote.end()) {
                  by_remote.emplace_back(remote, std::vector<const gbcode::BranchInfo*>{});
                  slot = by_remote.end() - 1;
                }
                slot->second.push_back(branch);
              }
              for (const auto& [remote, entries] : by_remote) {
                // remote 父节点：图标 + 名字 + 计数（IDEA 只显示名字，计数帮判断“有没有同步过”）。
                (void)text(c, [remote, count = entries.size()] {
                  return std::format("  ▾ {} ({})", remote, count);
                }, {.color = Tone::Muted, .id = "git-remote-" + remote});
                for (const gbcode::BranchInfo* branch : entries) {
                  (void)row(c, {.gap = 4.0f, .key = "branch:" + branch->name}, [&] {
                    (void)custom<Button>(c, [this, branch = *branch](Button& button) {
                      button.set_id("git-branch-" + branch.name);
                      button.set_align(Button::Align::Start);
                      // 缩进两级：remote 父节点下再退一格（IDEA 同）。
                      button.set_label("      " + std::string(gbcode::short_branch_name(branch)));
                      button.set_variant(Button::Variant::Ghost);
                      button.set_size(Button::Size::Small);
                      // 同上：点击 = 筛日志，不检出。
                      button.on_click = [this, branch] { filter_log_by_branch(branch); };
                    }, {.height = 24.0f, .grow = true});
                  });
                }
              }
            }
          }
        },
        [](ScrollView& scroll) { scroll.set_id("git-branches-scroll"); },
        {.grow = true, .id = "git-branches-host"});
  }


  /// 子视图 2：标签（新建/删除）。
  void CodeEditorPage::build_tags_pane(Composer& c) {
    (void)row(c, {.gap = 4.0f, .id = "git-tags-actions"}, [&] {
      scm_icon_button(c, "tag-new", "plus", "在当前提交建标签", [this] {
        tag_new_name_.set(std::string{});
        tag_new_open_.set(true);
      });
    });
    (void)custom_container<ScrollView>(
        c,
        [&] {
          if (git_tags_.empty()) {
            (void)text(c, [] { return std::string("（没有标签）"); });
            return;
          }
          for (const auto& tag : git_tags_) {
            (void)row(c, {.gap = 4.0f, .key = "tag:" + tag.name}, [&] {
              (void)custom<Button>(c, [this, tag](Button& b) {
                b.set_id("git-tag-" + tag.name);
                b.set_align(Button::Align::Start);   // 列表行贴左缘（居中会参差不齐）
b.set_label(std::format("{}{}  {}  {}", tag.annotated ? "⚑" : "  ", tag.name,
                                        tag.target, tag.subject));
                b.set_variant(Button::Variant::Ghost);
                b.set_size(Button::Size::Small);
                b.on_click = [this, tag] { open_commit_by_hash(tag.target, tag.name); };
              }, {.height = 24.0f, .grow = true});
              scm_icon_button(c, "deltag-" + tag.name, "trash", "删除标签", [this, name = tag.name] {
                ask_delete_tag(name);
              });
            });
          }
        },
        [](ScrollView& scroll) { scroll.set_id("git-tags-scroll"); },
        {.grow = true, .id = "git-tags-host"});
  }


  /// 子视图 3：暂存（stash）——弹出/应用/删除。
  void CodeEditorPage::build_stashes_pane(Composer& c) {
    (void)row(c, {.gap = 4.0f, .id = "git-stash-actions"}, [&] {
      (void)custom<Button>(c, [this](Button& b) {
        b.set_id("stash-push");
        b.set_label("暂存当前改动");
        b.set_variant(Button::Variant::Neutral);
        b.set_size(Button::Size::Small);
        b.on_click = [this] { (void)run_write({.op = gbcode::WriteOp::StashPush, .message = "st"},
                                              "stash push"); };
      }, {.height = 24.0f, .key = "stash-push"});
      scm_icon_button(c, "stash-refresh", "refresh", "刷新", [this] { git_log_loaded_ = false; });
    });
    (void)custom_container<ScrollView>(
        c,
        [&] {
          if (git_stashes_.empty()) {
            (void)text(c, [] { return std::string("（没有暂存记录）"); });
            return;
          }
          for (const auto& stash : git_stashes_) {
            (void)row(c, {.gap = 4.0f, .key = "stash:" + stash.index}, [&] {
              (void)text(c, [stash] {
                return std::format("{}  {}", stash.index, stash.message);
              }, {.grow = true, .id = "stash-label-" + stash.index});
              scm_icon_button(c, "stash-pop-" + stash.index, "check", "弹出（应用并删除）",
                              [this, index = stash.index] {
                                (void)run_write({.op = gbcode::WriteOp::StashPop, .ref = index},
                                                "stash pop");
                              });
              scm_icon_button(c, "stash-apply-" + stash.index, "plus", "应用（保留记录）",
                              [this, index = stash.index] {
                                (void)run_write({.op = gbcode::WriteOp::StashApply, .ref = index},
                                                "stash apply");
                              });
              scm_icon_button(c, "stash-drop-" + stash.index, "trash", "删除暂存",
                              [this, index = stash.index] {
                                ask_drop_stash(index);
                              });
            });
          }
        },
        [](ScrollView& scroll) { scroll.set_id("git-stashes-scroll"); },
        {.grow = true, .id = "git-stashes-host"});
  }


  /// 子视图 4：远程（拉取/推送/强制推送（带 lease）/新建）。
  void CodeEditorPage::build_remotes_pane(Composer& c) {
    (void)row(c, {.gap = 4.0f, .id = "git-remote-actions"}, [&] {
      struct RemoteAction {
        const char* id;
        const char* label;
        gbcode::WriteOp op;
      };
      static constexpr RemoteAction kActions[] = {{"remote-fetch", "取回", gbcode::WriteOp::Fetch},
                                                  {"remote-pull", "拉取", gbcode::WriteOp::Pull},
                                                  {"remote-push", "推送", gbcode::WriteOp::Push}};
      for (const auto& action : kActions) {
        (void)custom<Button>(c, [this, action, upstream = git_upstream_](Button& b) {
          b.set_id(action.id);
          b.set_label(action.label);
          b.set_variant(Button::Variant::Neutral);
          b.set_size(Button::Size::Small);
          // 无上游时推送带上当前分支名（`git push -u <branch>`）：否则首推会失败，
          // 而“失败”与“没有可推送的”在界面上长得一样。
          const std::string branch =
              action.op == gbcode::WriteOp::Push && upstream.empty() ? git_.branch : std::string{};
          b.on_click = [this, op = action.op, branch] {
            (void)run_write({.op = op, .name = branch}, "remote");
          };
        }, {.height = 24.0f, .key = std::string("remote:") + action.id});
      }
      (void)custom<Button>(c, [this](Button& b) {
        b.set_id("remote-force-push");
        b.set_label("强制推送");
        b.set_variant(Button::Variant::Ghost);
        b.set_size(Button::Size::Small);
        b.on_click = [this] { ask_force_push(); };
      }, {.height = 24.0f, .key = "remote-force-push"});
    });
    if (!git_upstream_.empty()) {
      (void)text(c, [this] { return "上游：" + git_upstream_; },
                 {.color = Tone::Muted, .id = "git-upstream-label"});
    }
    (void)custom_container<ScrollView>(
        c,
        [&] {
          if (git_remotes_.empty()) {
            (void)text(c, [] { return std::string("（没有远程仓库）"); });
            return;
          }
          for (const auto& remote : git_remotes_) {
            (void)text(c, [remote] {
              return std::format("{}  {} → {}", remote.name, remote.fetch_url, remote.push_url);
            }, {.id = "remote-label-" + remote.name});
          }
        },
        [](ScrollView& scroll) { scroll.set_id("git-remotes-scroll"); },
        {.id = "git-remotes-host"});
    // 写操作审计（最近 20 条）——「谁在什么时候做了什么」的唯一可查证据。
    (void)text(c, [] { return std::string("最近操作"); }, {.color = Tone::Muted,
                                                          .id = "git-audit-title"});
    if (git_audit_.empty()) {
      (void)text(c, [] { return std::string("（还没有记录）"); }, {.id = "git-audit-empty"});
    } else {
      for (std::size_t index = 0; index < git_audit_.size(); ++index) {
        (void)text(c, [this, index] { return git_audit_[git_audit_.size() - 1 - index]; },
                   {.id = std::format("git-audit-{}", index)});
      }
    }
  }


  /// 中栏：日志（标题 + 过滤 + 泳道图 + 范围提示）。
  ///
  /// 与右栏的分工：中栏回答“**发生了什么**”（一笔笔提交），右栏回答
  /// “**这一笔改了什么**”（文件与具体差异）。两者同屏才能“选中即看内容”，
  /// 而互斥标签页会把这条动线打断。
  void CodeEditorPage::build_log_pane(Composer& c) {
    // —— 标题与筛选**共一行** ——
    //
    // 旧形态是两行（26px 标题行 + 30px 过滤行）：标题只剩一个“日志 N · 分支”的
    // 短语，却独占一整行，而中栏本身不高（与右栏分屏）——一屏日志少两行。
    // 合成一行后：左边是标题（窄、不收缩），右边是过滤框 + “仅当前分支” + 刷新。
    //
    // ⚠ `#git-log-head` 这个 id 保留在**这一行**上：`tools/gbcode_git_e2e.py`
    // 与自动化靠它判“中栏在不在”（换成别的 id 会让既有断言静默变成“找不到”）。
    (void)row(c, {.gap = 6.0f, .height = 30.0f, .id = "git-log-head"}, [&] {
      (void)text(c, [this] {
        return std::format("日志{}",
                           git_log_.empty() ? std::string{} : std::format(" {}", git_log_.size()));
      }, {.weight = FontWeight::Regular, .id = "git-log-head-title"});
      // 当前分支名另起一个弱化小字：它是**范围提示**，不是标题的一部分
      //（标题里的数字会变，把分支拼进去会让它每切一次分支就整段重排）。
      //
      // 有**分支筛选**时改显示那条筛选：否则日志少了一大量提交，
      // 而头部只有当前分支名（会让人以为“提交丢了”）。
      if (!log_filter_branch_.value().empty()) {
        (void)text(c, [this] {
          const std::string& ref = log_filter_branch_.value();
          // 去 `refs/remotes/` 一类前缀：列表里看到的就是短名，要一致。
          const std::size_t slash = ref.find('/');
          const std::string shown =
              slash == std::string::npos ? ref : ref.substr(slash + 1);
          return std::format("仅 {}（点分支取消）", shown);
        }, {.color = Tone::Muted, .id = "git-log-head-filter"});
      } else if (!git_.branch.empty()) {
        (void)text(c, [this] { return git_.branch; },
                   {.color = Tone::Muted, .id = "git-log-head-branch"});
      }
      (void)custom<Input>(c, [this](Input& field) {
        field.set_id("log-filter-text");
        field.set_placeholder("提交信息 / 作者 / 路径");
        field.style().grow = true;
        field.on_change = [this](std::string_view value) {
          log_filter_text_.set(std::string(value));
        };
      }, {.height = 24.0f, .grow = true, .key = "log-filter-text"});
      (void)custom<Button>(c, [this](Button& b) {
        b.set_id("log-filter-current");
        b.set_label("仅当前分支");
        b.set_variant(log_filter_current_branch_.value() ? Button::Variant::Soft
                                                         : Button::Variant::Ghost);
        b.set_size(Button::Size::Small);
        b.on_click = [this] {
          const bool turn_on = !log_filter_current_branch_.value();
          log_filter_current_branch_.set(turn_on);
          // 与“按分支筛”**二选一**：两个都开就变成交集，结果看着像“日志少了”。
          if (turn_on) clear_log_branch_filter();
        };
      }, {.height = 24.0f, .key = "log-filter-current"});
      scm_icon_button(c, "log-refresh", "refresh", "重新读取日志", [this] {
        git_log_loaded_ = false;
      });
    });
    // —— 多列表格（IDEA 同款）——
    //
    // 旧形态是“一行一个 Button + 空格拼起来的字符串”，问题是：
    // 提交信息/作者/日期挤成一串，**列不对齐**（不同长度就错位）——而日志表
    // 恰恰是靠列对齐来扫的（“谁改的 / 什么时候” 是两列独立信息）。
    //
    // 泳道列由**节点-边图**（`Table::set_graph`）表示，不走文本列。
    //
    // 数据层只有一份提交列表（`filtered_log`）+ 一份拓扑图（`commit_graph`），
    // 没有任何“行”概念——字符网格通道已彻底移除。
    (void)custom<Table>(c, [this](Table& table) {
      table.set_id("git-log-table");
      // **不要表头**：这四列的含意自明（提交信息/作者/日期/哈希），扫日志时
      // 列宽固定、不依赖列名对齐；而表头要吃掉 36px 的黄金视野，还是唯一
      // 不随滚动移动的色带（IDEA 的日志页也没有列头）。
      table.set_show_header(false);
      // 行 = 提交（过滤后的）。过滤会把某些提交去掉，所以行号要**重新算**：
      // 拓扑图里的 `row` 是“未过滤拓扑序里的下标”，不能直接当表行号用。
      // 下面按 hash 建一次映射（O(n)），同时维护“保留提交在拓扑图里的原下标”
      // 供边重映射使用。
      const auto rows = filtered_log();
      table.clear_rows();   // 每次重组重建：日志量与过滤条件都会变
      // 泳道带宽：默认值（28）只够画一条线，而“圆点+线”与首字之间还需要一档空白。
      table.set_lane_column_width(34.0f);
      const auto graph = commit_graph();
      std::vector<Table::GraphNodeSpec> nodes;
      std::vector<Table::GraphEdgeSpec> edges;
      // 行号对齐：拓扑图的 `node.row` 是“**未过滤**拓扑序里的下标”，
      // 而过滤/仅当前分支会去掉一些提交——直接拿来当表行号会整体错位。
      // 按 hash 建一次映射（O(n)，每帧一次不心疼），并把被去掉的行映射到
      // `rows.size()`（哨兵值，下面跳过引用它的边）。
      if (!graph.nodes.empty()) {
        std::map<std::string, std::size_t> row_of_hash;
        for (std::size_t index = 0; index < rows.size(); ++index) {
          row_of_hash.emplace(rows[index].hash, index);
        }
        std::vector<std::size_t> table_row_of_graph(graph.nodes.size(), rows.size());
        for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
          const auto found = row_of_hash.find(graph.nodes[index].hash);
          if (found == row_of_hash.end()) continue;
          table_row_of_graph[index] = found->second;
          nodes.push_back(Table::GraphNodeSpec{.row = found->second,
                                               .lane = graph.nodes[index].lane});
        }
        for (const auto& edge : graph.edges) {
          if (edge.from_row >= table_row_of_graph.size() ||
              edge.to_row >= table_row_of_graph.size()) {
            continue;
          }
          const std::size_t from_row = table_row_of_graph[edge.from_row];
          const std::size_t to_row = table_row_of_graph[edge.to_row];
          // 被过滤掉的行（哨兵 = rows.size()）不能成边：那会在两行真实提交之间
          // 拉一根“跳过了一堆被隐藏提交”的线，让人以为它们是直系父子。
          if (from_row >= rows.size() || to_row >= rows.size()) continue;
          edges.push_back(Table::GraphEdgeSpec{.from_row = from_row,
                                               .from_lane = edge.from_lane,
                                               .to_row = to_row,
                                               .to_lane = edge.to_lane});
        }
      }
      table.set_graph(std::move(nodes), std::move(edges));
      // 行高：全部 1.0（每行都是一笔提交；不再有“为画斜边而占的空行”）。
      std::vector<float> scales(rows.size(), 1.0f);
      table.set_row_scales(std::move(scales));
      // 窄栏压缩优先级：先把哈希/日期让出去，再是作者；提交信息（主列）最后。
      //
      // 为何要显式声明：右栏把日志挤到 380px 宽时，110+130+76 = 316px 全被这三列
      // 占满，主列只剩十几字（实测：提交信息全变成省略号，而日期/哈希一字不少）。
      // 下限取“还看得懂”的宽度：日期 96（`2026-11-06 15:04` 约 106，再窄就不如省略）、
      // 哈希 62（短哈希 7 位 + 一点余量）、作者 76（中文作者三四字）。
      table.set_columns({
          {.name = "提交信息", .width = 0.0f},
          {.name = "作者", .width = 110.0f, .align = TextAlign::Start, .min_width = 76.0f,
           .shrink = 3},
          {.name = "日期", .width = 130.0f, .align = TextAlign::Start, .min_width = 96.0f,
           .shrink = 1},
          {.name = "哈希", .width = 76.0f, .align = TextAlign::Start, .min_width = 62.0f,
           .shrink = 2},
      });
      // 列色调：提交信息主色（用户在看的东西）、其余弱化（背景信息）。
      // 不这样做就只能全表一个颜色，靠列位置猜重点。
      table.set_column_tones({Table::CellTone::Normal, Table::CellTone::Muted,
                              Table::CellTone::Muted, Table::CellTone::Muted});
      // 行高（= 提交线的纵向间距）。
      //
      // 从 24 调到 28 的理由是**图而不是文字**：分支图的斜边要跨越相邻两行，
      // 行高太矮时曲线只能压在一两个像素里完成横向位移——“换道”看上去就像
      // 线被撞了一下（实测截图：曲线几乎看不见，只剩一个凸起）。28px 给了曲线
      // 上下各半行的缓冲，仍能 4 屏看到 ~20 条提交（原来约 23 条）。
      table.set_row_height(28.0f);
      for (const auto& entry : rows) {
        // refs（分支/标签胶囊）拼在提交信息尾部：它是**这条提交的身份装饰**，
        // 单独开一列会在窄视图里把真正的内容挤没。
        std::string subject = entry.subject;
        if (!entry.refs.empty()) {
          std::string refs;
          for (const auto& ref : entry.refs) refs += " [" + ref + "]";
          subject += refs;
        }
        table.add_row({subject, entry.author, entry.date, entry.short_hash});
      }
      // 选中行与左栏的“当前提交”同源（同一条高亮，不是两个独立状态）。
      if (!git_selected_hash_.empty()) {
        for (std::size_t index = 0; index < rows.size(); ++index) {
          if (rows[index].short_hash == git_selected_hash_) {
            table.set_selected_row(index);
            break;
          }
        }
      }
      // 点行 = 看这条提交的详情。行下标就是 `rows` 下标（行 = 提交，无偏移）。
      table.set_on_row_click([this, rows](std::size_t row) {
        if (row < rows.size()) show_commit(rows[row]);
      });
    }, {.grow = true, .id = "git-log-host"});
    // 给 e2e 用的断言面：当前可见行区间（“表格真的在显示数据”而非空壳）。
    (void)text(c, [this] {
      const std::size_t total = filtered_log().size();
      return total == 0U ? std::string("（没有提交，或读取失败）")
                         : std::string{};
    }, {.id = "git-log-empty"});
  }


  /// 一栏的标题行（标题 + 右侧一个可选图标钮）——三栏各一份，职责一眼可辨。
  void CodeEditorPage::build_pane_head(Composer& c, const char* id, std::function<std::string()> title,
                       const char* action_id) {
    (void)row(c, {.gap = 4.0f, .height = 26.0f, .id = id}, [&] {
      (void)text(c, [title = std::move(title)] { return title(); },
                 {.grow = true, .weight = FontWeight::Regular, .id = std::string(id) + "-title"});
      if (action_id != nullptr) {
        scm_icon_button(c, action_id, "refresh", "重新读取", [this] { git_log_loaded_ = false; });
      }
    });
  }



  /// 过滤后的日志（提交信息/作者/hash 三个条件都作用在同一条上）。
  ///
  /// 这是日志表的**唯一**数据入口：行 = 提交，没有“连线行”这回事
  ///（字符网格通道已移除；跨道连线由拓扑图 `set_graph` 画，本就跨行）。
  auto CodeEditorPage::filtered_log() const -> std::vector<gbcode::LogEntry> {
    const std::string needle = std::string(st::trim(log_filter_text_.value()));
    const bool current_only = log_filter_current_branch_.value();
    // **分支筛选**（点分支列表设的那个）：按 hash 集合判，不看泳道号。
    //
    // 为何不能用泳道：泳道号是绘制层的分配结果，“道 0 = 当前分支”只在
    // 恰好看 HEAD 那条时成立；用户点的是**任意一条**分支（包括远程分支），
    // 正确的集合是 `git rev-list <ref>`。
    const std::string& branch_ref = log_filter_branch_.value();
    const bool by_branch = !branch_ref.empty() && !log_filter_branch_commits_.empty();
    std::set<std::string> branch_set;
    if (by_branch) {
      branch_set.insert(log_filter_branch_commits_.begin(), log_filter_branch_commits_.end());
    }
    // “仅当前分支”的判据：**拓扑图里道号 0**。
    //
    // 道 0 的定义就是“从 HEAD 一路走第一个父提交的那条线”（见 `git_graph.cpp`），
    // 也就是当前分支本身，所以它天然是“当前分支上的提交”这个集合。
    // 道号要从未过滤的图里查（过滤后的行号已经重排过）。
    std::map<std::string, std::size_t> lane_of_hash;
    if (current_only) {
      for (const auto& node : git_graph_.nodes) lane_of_hash.emplace(node.hash, node.lane);
    }
    std::vector<gbcode::LogEntry> out;
    out.reserve(git_log_.size());
    for (const auto& entry : git_log_) {
      if (by_branch && !branch_set.contains(entry.hash)) continue;
      if (current_only) {
        const auto found = lane_of_hash.find(entry.hash);
        if (found == lane_of_hash.end() || found->second != 0) continue;
      }
      if (!needle.empty()) {
        const bool hit = entry.subject.find(needle) != std::string::npos ||
                         entry.author.find(needle) != std::string::npos ||
                         entry.short_hash.find(needle) != std::string::npos;
        if (!hit) continue;
      }
      out.push_back(entry);
    }
    return out;
  }


  /// 右栏：**选中提交的内容**（元信息 + 汇总 + 文件清单；点文件开并列差异）。
  ///
  /// 为何独立成一栏（而不是塞在日志下方）：看历史是“扫很多笔 → 挑一笔 → 看它改了什么”，
  /// 而“改了什么”是**当前唯一要看的东西**——把它压在日志下面会被迫滚动。
  void CodeEditorPage::build_commit_detail(Composer& c) {
    // 标题栏：未选中时也显示（告知“这里是什么、怎么用”），选中时带短 hash。
    // 空态不是“没有这一栏”，而是“这一栏里还没有内容”——两码事（自动化也靠 id 判存在）。
    build_pane_head(c, "git-detail-head", [this] {
      return git_selected_hash_.empty() ? std::string("提交内容")
                                        : "提交 " + git_selected_hash_;
    }, nullptr);
    if (git_selected_hash_.empty()) {
      (void)text(c, [] { return std::string("在中栏选一笔提交，这里显示它改动的文件。"); },
                 {.color = Tone::Muted, .id = "git-detail-empty"});
      return;
    }
    // —— 整栏可垂直滚动 ——
    //
    // 为何必须包一层 `ScrollView`：提交信息是**多行**的（用户会把一整段说明写进提交
    // 消息），文件清单也随改动文件数变长。旧版把两层堆在一个固定高度的列里，
    // 超额部分直接被裁掉（没有滚动，下面的文件根本看不到）。
    //
    // 而 `ScrollView` 的宿主是 `custom_container`：它的子声明回调用**外层 `c`**
    //（不是组件的 composer）——写错会直接编译报错。
    (void)custom_container<ScrollView>(
        c,
        [&] {
          (void)column(c, {.gap = 6.0f, .padding_x = 2.0f, .id = "git-detail-body"}, [&] {
            // —— 元信息分级（IDEA 同款）——
            //
            // 三级层次，靠字号/字重/色调三个正交手段区分（不是只靠颜色）：
            //   ① **提交标题**：最大、最重、主色——它回答“这笔提交干了什么”；
            //   ② **作者 · 日期**：小、中灰——背景信息；
            //   ③ **增删统计**：右缘、+/− 分色——回答“改动多大”。
            //
            // ⚠ 提交信息用多行 `Text`（`set_multiline`）：单行 `Text` 会 `ellipsize`，
            // 长句被截成一行省略号（“消息看不全”）。多行的高度由折行结果自然撑开，
            // 即“高度适应内容”。
            (void)custom<Text>(c, [this](Text& t) {
              t.set_id("git-commit-summary");
              t.set_content(git_commit_summary_.value());
              t.set_weight(FontWeight::SemiBold);
              t.set_font_size(14.0f);
              t.set_multiline(true);
            }, {.id = "git-commit-summary"});
            (void)row(c, {.gap = 8.0f, .height = 20.0f}, [&] {
              (void)custom<Text>(c, [this](Text& t) {
                t.set_id("git-commit-meta");
                // 作者与日期放在同一行（它们回答同一个问题：“谁、什么时候”）。
                t.set_content(std::format("{} · {}", git_selected_author_, git_selected_date_));
                t.set_tone(Tone::Muted);
                t.set_font_size(12.0f);
              }, {.grow = true});
              // 增删统计：从**存下来的**统计层读（不在渲染期重算：那是“同一量两处推导”的温床）。
              (void)custom<Text>(c, [this](Text& t) {
                t.set_id("git-commit-stat");
                t.set_content(std::format("{} 个文件  +{} −{}", git_selected_stat_.files,
                                          git_selected_stat_.added, git_selected_stat_.removed));
                t.set_tone(Tone::Muted);
                t.set_font_size(12.0f);
              }, {.id = "git-commit-stat"});
            });
            if (git_commit_files_.empty()) {
              (void)text(c, [] { return std::string("（这笔提交没有改动文件）"); },
                         {.id = "git-detail-no-files"});
              return;
            }
            // —— 文件清单：**表格**（路径 / 增 / 删）——
            //
            // 旧版是一行一个 Button，文本拼成 `path  +N −M`：路径长短不一时
            // 增删数字**参差不齐**，而“哪些文件动得多”恰恰要靠这两列对齐才扫得出来。
            //
            // 表格不 `grow`：放进 `ScrollView` 后它该按**行数**撑开自然高
            //（grow 会把它拉到视口高，短清单下面挂一大片空白，长清单又被压回去）。
            (void)custom<Table>(c, [this](Table& table) {
              table.set_id("git-commit-files-table");
              table.clear_rows();
              table.set_columns({
                  {.name = "文件", .width = 0.0f},
                  {.name = "+", .width = 52.0f},
                  {.name = "−", .width = 52.0f},
              });
              table.set_column_tones({Table::CellTone::Normal, Table::CellTone::Muted,
                                      Table::CellTone::Muted});
              table.set_row_height(24.0f);
              for (const auto& [path, plus, minus] : git_commit_files_) {
                table.add_row({path, std::format("+{}", plus), std::format("−{}", minus)});
              }
              // 点文件 → 看该文件在这笔提交里的差异（与旧版同一动线）。
              const auto files = git_commit_files_;
              table.set_on_row_click([this, files](std::size_t row) {
                if (row < files.size()) {
                  open_commit_file(git_selected_hash_, std::get<0>(files[row]));
                }
              });
            }, {.id = "git-commit-files"});
          });
        },
        [](ScrollView& scroll) { scroll.set_id("git-detail-scroll"); },
        {.grow = true, .id = "git-detail-host"});
  }


  /// 确认一个**不可逆/需确认**的写操作（统一入口：弹确认 → 批准后才执行）。
  ///
  /// 与直接执行的区别：`Danger::None` 的操作不弹（弹多了人会盲目点“确定”，
  /// 那才是真危险）；这里只收 `Confirm`/`Backup` 两级。
  void CodeEditorPage::ask_write(const gbcode::WriteIntent& intent, std::string title, std::string body) {
    const auto args = gbcode::write_args(intent);
    if (!args.has_value()) {
      status_.set("操作参数不合法——已拒绝执行");
      return;
    }
    git_confirm_title_ = std::move(title);
    git_confirm_body_ = std::format("{}\n\n将执行：{}", body, gbcode::to_display_line(*args));
    if (gbcode::needs_backup(intent)) {
      git_confirm_body_ += "\n（执行前会自动建备份分支：可手动回滚）";
    }
    pending_git_confirm_ = [this, intent] { (void)run_write(intent, "confirmed"); };
    git_confirm_open_.set(true);
  }


  /// 删分支（`-d` 拒删未合并——想强删得再点一次）。
  void CodeEditorPage::ask_delete_branch(const std::string& name) {
    ask_write({.op = gbcode::WriteOp::DeleteBranch, .name = name},
              std::format("删除分支 {}", name),
              "`-d` 只删已合并的分支（未合并会被 git 拒绝）。");
  }


  /// 强删分支（`-D`）——丢掉未合并的提交，必须说清楚。
  void CodeEditorPage::ask_force_delete_branch(const std::string& name) {
    ask_write({.op = gbcode::WriteOp::DeleteBranch, .name = name, .hard = true},
              std::format("强制删除分支 {}", name),
              "`-D` 会丢掉**未合并的提交**（不可恢复）。");
  }


  /// 删标签。
  void CodeEditorPage::ask_delete_tag(const std::string& name) {
    ask_write({.op = gbcode::WriteOp::TagDelete, .name = name},
              std::format("删除标签 {}", name), "标签本身可重建，但引用会丢。");
  }


  /// 删一条 stash。
  void CodeEditorPage::ask_drop_stash(const std::string& index) {
    ask_write({.op = gbcode::WriteOp::StashDrop, .ref = index},
              std::format("删除暂存记录 {}", index), "暂存内容将不可恢复地丢失。");
  }


  /// 强制推送（**带 lease**）。
  ///
  /// 为何不无脑用 `--force`：无 lease 的强推会直接盖掉别人的提交；
  /// `--force-with-lease` 在远端已经前进时**拒推**——这才是安全网。
  void CodeEditorPage::ask_force_push() {
    ask_write({.op = gbcode::WriteOp::PushForceWithLease}, "强制推送",
              "会改写远端历史（仅在与远端分歧且确认无人共享时使用）。");
  }


  /// 重置到某个提交（三种模式）。
  void CodeEditorPage::ask_reset(const std::string& ref, gbcode::WriteOp op) {
    const bool hard = op == gbcode::WriteOp::ResetHard;
    ask_write({.op = op, .ref = ref}, std::format("重置到 {}", ref),
              hard ? "`--hard` 会丢弃当前工作区与暂存区的全部改动（不可恢复）。"
                   : "`--soft/--mixed` 只移动 HEAD 与索引，工作区内容保留。");
  }


  /// 建分支（浮层提交）。
  void CodeEditorPage::create_branch() {
    const std::string name = std::string(st::trim(branch_new_name_.value()));
    if (name.empty()) {
      status_.set("分支名不能为空");
      return;
    }
    branch_new_open_.set(false);
    (void)run_write({.op = gbcode::WriteOp::CheckoutNew, .name = name}, "branch -b");
    status_.set("已新建并切换到分支 " + name);
  }


  /// 建标签（浮层提交）。
  void CodeEditorPage::create_tag() {
    const std::string name = std::string(st::trim(tag_new_name_.value()));
    if (name.empty()) {
      status_.set("标签名不能为空");
      return;
    }
    const bool annotated = tag_new_annotated_.value();
    tag_new_open_.set(false);
    (void)run_write({.op = gbcode::WriteOp::TagCreate, .name = name,
                     .message = annotated ? "release " + name : std::string{},
                     .annotated = annotated},
                    "tag");
    status_.set("已建标签 " + name + (annotated ? "（注解）" : "（轻量）"));
  }


  /// 按 hash 打开一个提交（标签行点开用）。
  void CodeEditorPage::open_commit_by_hash(const std::string& hash, const std::string& label) {
    for (const auto& entry : git_log_) {
      if (entry.hash.starts_with(hash) || entry.short_hash == hash) {
        show_commit(entry);
        return;
      }
    }
    // 不在已读的 200 条里：只给状态提示（不额外起进程——列表里没有的提交
    // 本来也不是当前上下文）。
    status_.set("提交 " + label + "（" + hash + "）不在已读取的日志里");
  }


  /// 切换到分支（终端代敲——checkout 会改工作区，输出必须让用户看见）。
  void CodeEditorPage::checkout_branch(const gbcode::BranchInfo& branch) {
    if (branch.is_current || branch.is_remote) {
      // 远程分支：检出它的本地跟踪分支（`git checkout <短名>` 自动建）。
      if (branch.is_remote) {
        const std::size_t slash = branch.name.find('/');
        const std::string short_name =
            slash == std::string::npos ? branch.name : branch.name.substr(slash + 1);
        if (terminal_ptr != nullptr) {
          ensure_terminal_visible();
          terminal_ptr->run("git checkout " + short_name);
          git_refresh_pending_ = true;
          git_log_loaded_ = false;
        }
      }
      return;
    }
    pending_git_confirm_ = [this, name = branch.name] {
      if (terminal_ptr != nullptr) {
        ensure_terminal_visible();
        terminal_ptr->run("git checkout " + name);
        git_refresh_pending_ = true;
        git_log_loaded_ = false;
      }
    };
    git_confirm_title_ = std::format("切换到分支 {}", branch.name);
    git_confirm_body_ = "未提交的改动会跟着走（冲突时 git 会拒绝）。切换？";
    git_confirm_open_.set(true);
  }


  /// 按分支筛日志（**不检出**，见头文件里的取舍说明）。
  void CodeEditorPage::filter_log_by_branch(const gbcode::BranchInfo& branch) {
    const std::string ref = branch.name;
    // 再点同一条 = 取消（筛选开关的常见词形，与侧栏视图按钮一致）。
    if (log_filter_branch_.value() == ref) {
      clear_log_branch_filter();
      // 回到“全部提交”的范围：**同步**重读（与筛选时同一做法）。
      //
      // 为何不用 `git_log_loaded_ = false` 走异步泵：那会留下一帧“筛选已取消、
      // 但列表还是旧数据”的中间态（实测：标题写 master、第一行却是别的分支的提交）。
      // 同步读 200 条实测 ~10ms，代价可以接受。
      git_log_ = gbcode::read_log(workspace_, 200);
      git_graph_ = gbcode::read_commit_graph(workspace_, 200);
      status_.set("日志：已取消分支筛选");
      return;
    }
    const std::vector<std::string> commits =
        gbcode::read_branch_commits(workspace_, ref, git_log_.size() + 200U);
    if (commits.empty()) {
      // 读不到就不改状态（而不是“筛成空列表”让用户以为那条分支没提交）。
      status_.set(std::format("日志：读不到分支 {}", gbcode::short_branch_name(branch)));
      return;
    }
    log_filter_branch_commits_ = commits;
    log_filter_branch_.set(ref);
    // 两个筛选**二选一**：“仅当前分支”开关与“具体某条分支”表达的是同一件事，
    // 同时生效会让结果变成两者的交集（用户看到的是“点了一条分支却没几条提交”）。
    log_filter_current_branch_.set(false);

    // ⚠ **必须重新读日志**，不能只靠本地已加载的那批（默认 200 条 `--all`）。
    // 实测：点 `origin/dev-finops`（它的提交全在 200 条之外）→ 本地集合里一个
    // 都不命中 → 日志表空、而状态栏说“252 条提交”。用户看到的是“这条分支没提交”。
    // `read_log` 支持 `ref` 参数，直接按该分支重取就是正确范围。
    git_log_ = gbcode::read_log(workspace_, 200, ref);
    git_graph_ = gbcode::read_commit_graph(workspace_, 200, ref);
    status_.set(std::format("日志：只看 {}（{} 条提交）",
                            gbcode::short_branch_name(branch), git_log_.size()));
  }

  void CodeEditorPage::clear_log_branch_filter() {
    log_filter_branch_.set(std::string{});
    log_filter_branch_commits_.clear();
  }


  /// 查看一个提交：与它的**父提交**逐文件并列对比（首个提交则与空树比）。
  void CodeEditorPage::show_commit(const gbcode::LogEntry& entry) {
    git_selected_hash_ = entry.short_hash;
    auto copy = entry;
    const std::string diff = gbcode::read_commit_detail(workspace_, copy);
    std::size_t added = 0;
    std::size_t removed = 0;
    for (const auto& [path, plus, minus] : copy.files) {
      (void)path;
      added += plus;
      removed += minus;
    }
    // 增删总量另行保存：它属于**统计层**，不属于标题（见上面的说明）。
    git_selected_stat_ = {.files = copy.files.size(), .added = added, .removed = removed};
    // 标题 = **提交信息原文**（它回答“这笔提交干了什么”）。
    //
    // 为什么不能拼上 hash/统计：那些是**另一类信息**（身份与体量），
    // 把它们塞进标题会让标题变成一串难读的元数据——而详情栏的分级排版
    // 正是为了让“标题 / 元信息 / 统计”各占一层（见 `build_commit_detail`）。
    // 旧版在这里拼过“hash · N 个文件 · +A −R”，导致标题与统计重复两次。
    git_commit_summary_.set(entry.subject);
    git_selected_author_ = entry.author;
    git_selected_date_ = entry.date;
    // 文件清单存下来（渲染期只消费）：它是“看具体改动”的入口。
    git_commit_files_ = copy.files;
    status_.set("提交 " + entry.short_hash + "：" + entry.subject.substr(0, 40));
  }


  /// 打开某个提交里一个文件的并列差异（父提交 ↔ 该提交）。
  ///
  /// 根提交没有父：`HEAD^` 会报错，而“与不存在的父对比”在语义上就是“与空树对比”
  /// ——歌白那边实测过“不归一时被当成了无差异”。
  void CodeEditorPage::open_commit_file(const std::string& hash, const std::string& path) {
    gbcode::CompareOptions options;
    const bool root = gbcode::is_root_commit(workspace_, hash);
    options.from = gbcode::DiffEndpoint::parse(root ? std::string(gbcode::kEmptyTree)
                                                    : hash + "^");
    options.to = gbcode::DiffEndpoint::parse(hash);
    open_diff(std::format("@{} {}", hash.substr(0, 7), st::fs::file_name(path)), options, path);
  }


  // 两个共用小工具（终端/搜索/面包屑都用）：直接在这里定义——
  // 只写声明的话，后面若有任何分支未走到它们的定义，编译器会报
  // “used but never defined”（`static` 成员函数在文件作用域里就是这个语义）。
  /// 把用户输入的文件名解析为绝对路径（相对终端当前目录 → 工作区）。
  [[nodiscard]] auto CodeEditorPage::resolve_path(std::string_view raw) const -> std::string {
    std::string path(st::trim(raw));
    if (st::fs::is_absolute(path)) return st::fs::normalize(path);
    const std::string base = terminal_exec_dir();
    if (base.empty()) return path;
    return st::fs::normalize(st::fs::join(base, path));
  }


  /// 文本的行数（空串算 0 行）。
  [[nodiscard]] auto CodeEditorPage::line_count_of(std::string_view text) -> std::size_t {
    if (text.empty()) return 0;
    std::size_t count = 1;
    for (const char c : text) {
      if (c == '\n') ++count;
    }
    return count;
  }


  /// 终端的工作目录（相对路径按它解析；未显式设置过就用工作区根）。
  [[nodiscard]] auto CodeEditorPage::terminal_exec_dir() const -> std::string {
    if (terminal_ptr != nullptr) {
      const std::string cwd = terminal_ptr->get_property("working_directory").value_or("");
      if (!cwd.empty()) return cwd;
    }
    return workspace_;
  }


  /// 问题面板（VSCode / IDEA 同位置：底部停靠槽的第三页）。
  ///
  /// 内容 = **两份真数据源的合并**：
  ///   · `lsp_.all_diagnostics()`：语言服务按文件推的语义诊断（跨文件）
  ///   · `problems_`：当前文件的**内置轻量检查**（行尾空白/Tab/TODO/超长行）
  ///
  /// 后者在 LSP 就绪时仍然保留——TODO 标记这类项 LSP 不会报，而它们是本仓的
  /// 实际工作流（见 `Problem` 的注释）。两者同一行一条目，按“文件 → 行号”排；
  /// 点行即跳（跳当前文件的直接定位，跨文件的先 `open_path` 再跳，否则光标会
  /// 落在别的标签上）。
  void CodeEditorPage::build_problems_view(Composer& c) {
    build_pane_head(c, "problems-head", [this] {
      return std::format("问题  错误 {} · 警告 {}", error_count(), warning_count());
    }, nullptr);

    struct Row {
      std::string key{};
      std::string path{};
      std::size_t line{1};
      bool warning{true};
      std::string message{};
      std::string source{};
    };
    std::vector<Row> rows;
    for (const auto& [path, list] : lsp_.all_diagnostics()) {
      for (const auto& problem : list) {
        rows.push_back(Row{.key = std::format("lsp:{}:{}:{}", path, problem.line, rows.size()),
                           .path = path,
                           .line = problem.line,
                           .warning = problem.warning,
                           .message = problem.message,
                           .source = problem.source.empty() ? std::string("lsp")
                                                            : problem.source});
      }
    }
    const std::string current = current_buffer_path();
    for (const auto& problem : problems_) {
      rows.push_back(Row{.key = std::format("lint:{}:{}", problem.line, rows.size()),
                         .path = current,
                         .line = problem.line,
                         .warning = problem.warning,
                         .message = problem.message,
                         .source = "内置检查"});
    }

    if (rows.empty()) {
      (void)text(c, [] { return std::string("当前工作区没有诊断问题。"); },
                 {.color = Tone::Muted, .id = "problems-empty"});
      return;
    }

    // 行**必须带业务 key**（由“来源+路径+行号+序号”拼，不含下标）——面板每帧重算，
    // 不带 key 时元素 id 随序号漂移，“刷新后点了另一条”这类错很难查。
    std::vector<ListItemData> items;
    items.reserve(rows.size());
    for (const Row& row : rows) {
      const std::string leaf = st::fs::file_name(row.path);
      items.push_back(ListItemData{
          .key = row.key,
          .label = std::format("{}:{}  {}", leaf, row.line, row.message),
          .subtitle = std::format("{} · {}", row.path, row.source),
          .icon = row.warning ? "warning" : "error"});
    }
    const auto snapshot = rows;   // 回调按快照取值（重组后 rows 会被重建）
    (void)custom_container<ScrollView>(
        c,
        [&] {
          (void)list(c, items,
                     [this, snapshot](std::size_t index) {
                       if (index >= snapshot.size()) return;
                       jump_to_diagnostic(snapshot[index].path, snapshot[index].line);
                     },
                     // 行高 24：问题面板是“扫一眼”的清单，比常规列表紧凑。
                     {.gap = 0.0f, .height = 24.0f, .id = "problems-list",
                      .key = "problems-list"});
        },
        [](ScrollView& scroll) { scroll.set_id("problems-scroll"); },
        {.grow = true, .id = "problems-host"});
  }


  auto CodeEditorPage::context_items() -> std::vector<MenuItem> {
    // 条目 id 与 `run_context_action` 的派发**一一对应**（两边同源，增删同步）。
    // 顺序按 IDEA / VSCode 归组：撤销重做 → 剪贴板 → 选中 → 注释 → 导航/重构。
    return {{.id = "undo", .label = "撤销"},
            {.id = "redo", .label = "重做"},
            {.separator = true},
            {.id = "cut", .label = "剪切"},
            {.id = "copy", .label = "复制"},
            {.id = "paste", .label = "粘贴"},
            {.id = "select_all", .label = "全选"},
            {.separator = true},
            {.id = "comment", .label = "切换行注释"},
            {.separator = true},
            // 导航与重构（IDEA 把它们放在右键菜单——“看到这个词就想跳/改”是最高频的组合）。
            {.id = "goto-definition", .label = "转到定义（F12）"},
            {.id = "find-references", .label = "查找引用（Shift+F12）"},
            {.separator = true},
            {.id = "format", .label = "格式化文档（Shift+Alt+F）"},
            {.id = "rename", .label = "重命名符号（F2）"},
            {.separator = true},
            {.id = "history", .label = "文件历史…"},
            {.separator = true},
            {.id = "find", .label = "查找…"},
            {.id = "palette", .label = "命令面板…"}};
  }

}  // namespace gbcode_ui
