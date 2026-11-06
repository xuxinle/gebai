// 底部面板族：终端/Git 工具窗（分支|日志|标签|暂存|远程）与提交详情、
// 不可逆写操作的确认浮层触发（ask_*）。拆分自 main.cpp，零逻辑改动。

#include <algorithm>
#include <cstdio>
#include <format>
#include <functional>
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
          panel.set_visible(bottom_tab_.value() == 0);
        },
        {.grow = true, .key = "pane:terminal"});
    (void)custom_container<Panel>(
        c,
        [&] { build_git_view(c); },
        [this](Panel& panel) {
          panel.set_id("git-panel");
          panel.set_surface(Element::Surface::Content);
          panel.set_visible(bottom_tab_.value() != 0);
        },
        {.grow = true, .key = "pane:git"});
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
                            [](Panel& panel) { panel.set_id("git-log-panel"); },
                            {.grow = true, .key = "git-log"});
                        (void)custom_container<Panel>(
                            c, [&] { build_commit_detail(c); },
                            [](Panel& panel) { panel.set_id("git-detail-panel"); },
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
    (void)column(c, {.gap = 4.0f, .grow = true, .id = "git-refs-body"}, [&] {
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
    // 高 30 → **34**、距离 2 → **6**：与编辑器标签条和侧栏头部同高，
    // 且选中块之间留缝（不留缝的圆角胶囊会粘成一条，IDEA 新 UI 也是分开的）。
    (void)row(c, {.gap = 6.0f, .height = 34.0f, .id = "git-tabs"}, [&] {
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
                                                             : Button::Variant::Secondary);
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
  void CodeEditorPage::build_branches_pane(Composer& c) {
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
          for (const auto& branch : git_branches_) {
            (void)row(c, {.gap = 4.0f, .key = "branch:" + branch.name}, [&] {
              (void)custom<Button>(c, [this, branch](Button& b) {
                b.set_id("git-branch-" + branch.name);
                b.set_align(Button::Align::Start);   // 列表行贴左缘（居中会参差不齐）
b.set_label(std::format("{}{}{}", branch.is_current ? "● " : "  ", branch.name,
                                        branch.is_remote ? "（远程）" : ""));
                b.set_variant(branch.is_current ? Button::Variant::Soft : Button::Variant::Ghost);
                b.set_size(Button::Size::Small);
                b.on_click = [this, branch] { checkout_branch(branch); };
              }, {.height = 24.0f, .grow = true});
              if (!branch.is_current && !branch.is_remote) {
                scm_icon_button(c, "delbranch-" + branch.name, "trash", "删除分支",
                                [this, name = branch.name] { ask_delete_branch(name); });
              }
            });
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
        b.set_variant(Button::Variant::Secondary);
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
          b.set_variant(Button::Variant::Secondary);
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
      if (!git_.branch.empty()) {
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
          log_filter_current_branch_.set(!log_filter_current_branch_.value());
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
    // 泳道列由图型圆点 + 竖线表示（`Table::set_row_lanes`），不走文本列：
    // 用字符画分支线（`●`/`○`）在有向无环图上根本表达不出合并关系。
    (void)custom<Table>(c, [this](Table& table) {
      table.set_id("git-log-table");
      const auto entries = filtered_log();
      table.clear_rows();   // 每次重组重建：日志量与过滤条件都会变
      // 泳道带宽：默认值（28）只够画一条线，而“圆点+线”与首字之间还需要一档空白。
      // 旧版**根本没给泳道留宽度**（`column_x` 从 `bounds_.x` 起算），圆点与竖线直接
      // 压在提交信息首字上（“提交线和信息贴太紧”的那个现象）。现在框架的 `Table`
      // 会把这块宽度真的让出来（见 `Table::lane_inset`），这里只调宽。
      table.set_lane_column_width(34.0f);
      table.set_columns({
          {.name = "提交信息", .width = 0.0f},
          {.name = "作者", .width = 110.0f},
          {.name = "日期", .width = 130.0f},
          {.name = "哈希", .width = 76.0f},
      });
      // 列色调：提交信息主色（用户在看的东西）、其余弱化（背景信息）。
      // 不这样做就只能全表一个颜色，靠列位置猜重点。
      table.set_column_tones({Table::CellTone::Normal, Table::CellTone::Muted,
                              Table::CellTone::Muted, Table::CellTone::Muted});
      table.set_row_height(24.0f);
      std::vector<int> lanes;
      lanes.reserve(entries.size());
      for (const auto& entry : entries) {
        // refs（分支/标签胶囊）拼在提交信息尾部：它是**这条提交的身份装饰**，
        // 单独开一列会在窄视图里把真正的内容挤没。
        std::string subject = entry.subject;
        if (!entry.refs.empty()) {
          std::string refs;
          for (const auto& ref : entry.refs) refs += " [" + ref + "]";
          subject += refs;
        }
        table.add_row({subject, entry.author, entry.date, entry.short_hash});
        lanes.push_back(static_cast<int>(entry.lane));
      }
      table.set_row_lanes(std::move(lanes));
      // 选中行与左栏的“当前提交”同源（同一条高亮，不是两个独立状态）。
      if (!git_selected_hash_.empty()) {
        for (std::size_t index = 0; index < entries.size(); ++index) {
          if (entries[index].short_hash == git_selected_hash_) {
            table.set_selected_row(index);
            break;
          }
        }
      }
      // 点行 = 看这条提交的详情（与旧版同一动线）。
      table.set_on_row_click([this, entries](std::size_t row) {
        if (row < entries.size()) show_commit(entries[row]);
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


  /// 日志行的展示文本（泳道标记 + 短 hash + 主题 + refs 胶囊）。
  auto CodeEditorPage::log_line_label(const gbcode::LogEntry& entry) const -> std::string {
    std::string refs;
    for (const auto& ref : entry.refs) refs += "[" + ref + "] ";
    return std::format("{} {}  {}  {}", entry.lane == 0 ? "●" : "○", entry.short_hash,
                       entry.subject, refs);
  }


  /// 过滤后的日志（提交信息/作者/路径三个条件都作用在同一条上）。
  auto CodeEditorPage::filtered_log() const -> std::vector<gbcode::LogEntry> {
    const std::string needle = std::string(st::trim(log_filter_text_.value()));
    std::vector<gbcode::LogEntry> out;
    out.reserve(git_log_.size());
    for (const auto& entry : git_log_) {
      if (log_filter_current_branch_.value() && entry.lane != 0) continue;
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


  auto CodeEditorPage::context_items() -> std::vector<MenuItem> {
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
            {.id = "history", .label = "文件历史…"},
            {.separator = true},
            {.id = "find", .label = "查找…"},
            {.id = "palette", .label = "命令面板…"}};
  }

}  // namespace gbcode_ui
