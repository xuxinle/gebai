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
      // 与日志工具栏的筛选框**同一形态**（与侧栏同底、保留描边），
      // 但不带前置图标：侧栏窄，图标白占一格宽度，而 placeholder
      // 已经把“这里能筛什么”说清楚了。
      input.set_seamless_background(true);
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
          // 一组的形态：`[▾/▸] [图标] 标题` + 展开时的条目。
          // 折叠箭头在左（IDEA 同位置），整行可点（点击目标远大于那个小三角）。
          //
          // **不显示计数**：分支数量不是读者要看的信息（IDEA 也不显示），
          // 而它占在标题右侧会与分支名抢注意力，还让每组标题长短不一。
          const auto group_header = [&](std::string_view key, std::string_view title,
                                        State<bool>& open, std::string_view icon_name) {
            (void)custom<Button>(c, [&open, key, title, icon_name](Button& button) {
              button.set_id("git-group-" + std::string(key));
              button.set_align(Button::Align::Start);
              button.set_label(std::format("{} {}  {}", open.value() ? "▾" : "▸",
                                           icon_name, title));
              button.set_variant(Button::Variant::Ghost);
              button.set_size(Button::Size::Small);
              button.on_click = [&open] { open.set(!open.value()); };
            }, {.height = 24.0f, .grow = true, .key = "group:" + std::string(key)});
          };

          // —— ① HEAD ——
          //
          // **一行呈现，不下钻、不计入列表**：HEAD 不是“一组分支”，它就是
          // “我现在在哪”这一个事实。做成可展开的组会在下面再排一条分支，
          // 与“本地”组里的同一条分支重复，读者要自己判断“这俩是不是一回事”。
          // 这里直接写成 `HEAD → master`，点它仍是“只看当前分支的历史”。
          if (head != nullptr) {
            (void)custom<Button>(c, [this, branch = *head](Button& button) {
              button.set_id("git-branch-head");
              button.set_align(Button::Align::Start);
              button.set_label("●  HEAD  →  " + std::string(gbcode::short_branch_name(branch)));
              button.set_variant(Button::Variant::Soft);
              button.set_size(Button::Size::Small);
              button.on_click = [this, branch] { filter_log_by_branch(branch); };
            }, {.height = 24.0f, .grow = true, .key = "head-line"});
          }

          // —— ② 本地 ——
          //
          // 含**当前分支**（它同时出现在 HEAD 组里）：两处职责不同——
          // HEAD 组回答“我现在在哪”（一条、置顶、不可删），本地组回答
          // “这座仓有哪些本地分支”（完整清单，当前分支用 Soft 高亮标出）。
          // 早期实现在这里 `continue` 跳过当前分支，结果本地组少一条、
          // 用户在本地清单里找不到自己在用的那条（目录与计数都对不上）。
          if (!locals.empty()) {
            group_header("local", "本地", branch_local_open_, "⏣");
            if (branch_local_open_.value()) {
              for (const gbcode::BranchInfo* branch : locals) {
                (void)custom<Button>(c, [this, branch = *branch](Button& button) {
                  button.set_id("git-branch-" + branch.name);
                  button.set_align(Button::Align::Start);
                  button.set_label("    " + std::string(gbcode::short_branch_name(branch)));
                  // 当前分支**高亮**（Soft = 主题主色的浅底，与 HEAD 行一致）。
                  // 不给高亮就只剩缩进，一屏十几条时看不出自己在哪条上。
                  button.set_variant(branch.is_current ? Button::Variant::Soft
                                                       : Button::Variant::Ghost);
                  button.set_size(Button::Size::Small);
                  // 左键 = **筛日志**（不是检出）：检出会改工作区，是写操作，
                  // 不该由“在侧栏看一眼这条分支的历史”触发。
                  button.on_click = [this, branch] { filter_log_by_branch(branch); };
                  // 右键 = **全部分支操作**（检出/只看日志/复制名/删除）。
                  button.on_context_menu = [this, name = branch.name](st::math::Point at) {
                    branch_menu_target_.set(name);
                    branch_menu_anchor_ = st::math::Point{at.x + 4.0f, at.y + 4.0f};
                    branch_menu_open_.set(true);
                  };
                }, {.height = 24.0f, .grow = true, .key = "branch:" + branch->name});
              }
            }
          }

          // —— ③ 远程（按 remote 再分一层）——
          //
          // IDEA 的层级是 `远程 > origin > master/feature`。多一层缩进的意义：
          // 一个仓库常配多个 remote（origin / upstream / 同事的 fork），
          // 平铺时 `origin/xxx` 与 `upstream/xxx` 混在一起，看不出谁是同一台远端。
          if (!remotes.empty()) {
            group_header("remote", "远程", branch_remote_open_, "☁");
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
                // remote 父节点：**可点开/收起**（IDEA 同）。
                //
                // 不用 `group_header`（它绑一个 State<bool>，而这里是**每个 remote 一个**
                // 折叠态）：所有 remote 共用一份开关，收起 github 时会连 origin 一起没了。
                const bool collapsed = branch_remote_collapsed_.value().contains(remote);
                (void)custom<Button>(c, [this, remote, collapsed](Button& button) {
                  button.set_id("git-remote-" + remote);
                  button.set_align(Button::Align::Start);
                  button.set_label(std::format("{} {}", collapsed ? "  ▸" : "  ▾", remote));
                  button.set_variant(Button::Variant::Ghost);
                  button.set_size(Button::Size::Small);
                  button.on_click = [this, remote] {
                    auto next = branch_remote_collapsed_.value();
                    if (!next.insert(remote).second) { next.erase(remote); }
                    branch_remote_collapsed_.set(std::move(next));
                  };
                }, {.height = 24.0f, .grow = true, .key = "remote:" + remote});
                if (collapsed) { continue; }
                for (const gbcode::BranchInfo* branch : entries) {
                  // ⚠ key 带 **`remote:` 前缀**：本地组里也可能有同名分支
                    // （`feature` 与 `origin/feature` 的短名都是 `feature`），
                    // 同 key 会让声明式把两个位置对齐到同一个实例。
                    (void)custom<Button>(c, [this, branch = *branch](Button& button) {
                      button.set_id("git-branch-" + branch.name);
                      button.set_align(Button::Align::Start);
                      // 缩进两级：remote 父节点下再退一格（IDEA 同）。
                      button.set_label("      " + std::string(gbcode::short_branch_name(branch)));
                      button.set_variant(Button::Variant::Ghost);
                      button.set_size(Button::Size::Small);
                      // 同上：点击 = 筛日志，不检出。
                      button.on_click = [this, branch] { filter_log_by_branch(branch); };
                      // 右键菜单对远程分支同样开放（条目已按种类裁剪：无检出/无删除）。
                      button.on_context_menu = [this, name = branch.name](st::math::Point at) {
                        branch_menu_target_.set(name);
                        branch_menu_anchor_ = st::math::Point{at.x + 4.0f, at.y + 4.0f};
                        branch_menu_open_.set(true);
                      };
                    }, {.height = 24.0f, .grow = true, .key = "remote:" + branch->name});
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
    // —— 日志工具栏（**参照 IDEA 的秩序**，从左到右）——
    //
    //   ① 文本或哈希输入（带放大镜）
    //   ② 分支筛选下拉（HEAD 开关 / 具体分支）
    //   ③ 用户下拉    ④ 日期下拉    ⑤ 路径下拉
    //   ⑥ 一个「排序/倒序」切换图标
    //   ⑦ 右侧一排图标：刷新 / 美化（优选）/ 显示选项
    //
    // 为何不再显示“日志”两个字：中栏本身就是日志（三栏布局里位置就是语义），
    // 标题词占着首屏最宝贵的水平空间，而条目数在每个筛选器上都看得出来。
    //
    // ⚠ `#git-log-head` 保留在**这一行**上：`tools/gbcode_git_e2e.py` 与自动化
    // 靠它判“中栏在不在”（换 id 会让既有断言静默变成“找不到”）。
    (void)row(c, {.gap = 6.0f, .height = 32.0f, .id = "git-log-head"}, [&] {
      // ① 文本/哈希筛选（一个输入框干三件事：信息、作者、短哈希）。
      //
      // 放大镜用 `Input::set_icon_prefix` **画在框内**（而不是旁边排一个图标元素）：
      // 框外图标会多占一份横向间距，且与框的边框形成“两个不相关的东西”，
      // 而搜索框的标准形态是“图标在框里”。
      //
      // 底色用主题的主背景（默认是 `surface_sunken` 的凹陷变体）：
      // 工具栏是一排控件，输入框凹陷成一块深色会把它从那一行里“扒”出来，
      // 而 IDEA 的日志工具栏上输入框与工具栏同底。
      (void)custom<Input>(c, [this](Input& field) {
        field.set_id("log-filter-text");
        field.set_placeholder("文本或哈希");
        field.set_icon_prefix("search");
        // 底色与工具栏**同一块**（不铺凹陷色、不画边框）：
        // 一行控件里只有它凹陷下去，会显得不像同一个工具条。
        field.set_seamless_background(true);
        field.style().grow = true;
        field.set_text(log_filter_text_.value());
        field.on_change = [this](std::string_view value) {
          log_filter_text_.set(std::string(value));
        };
      }, {.height = 24.0f, .grow = true, .key = "log-filter-text"});

      // —— 四个筛选器的公共构件 ——
      //
      // 形态：`[文字：值 ▾]`，有值时文字变主色、带 `×` 清除（IDEA 同形）。
      // 为何不用 `Select` 组件：它要求调用方注入 `overlay_host`，而声明式
      // 已有 `dsl::overlay`——面板内容能跟随帧重组，少一份跨帧状态。
      const auto filter_button = [&](const char* id, std::string_view title,
                                     std::string_view value, bool open,
                                     State<bool>& open_state, st::math::Point& anchor) {
        (void)custom<Button>(c, [id, title, value, open, &open_state, &anchor](Button& b) {
          b.set_id(id);
          // 有值：`标题：值`；无值：只显示标题（对应 IDEA 的灰字“用户”）。
          std::string label = std::string(title);
          if (!value.empty()) label += "：" + std::string(value);
          label += open ? "  ▴" : "  ▾";
          b.set_label(label);
          // 有值时用 Soft（= 正在生效的筛选，一眼看出“日志被筛过”）。
          b.set_variant(!value.empty() ? Button::Variant::Soft : Button::Variant::Ghost);
          b.set_size(Button::Size::Small);
          // 展开时**拍下按钮当前矩形**作为面板锚点。
          //
          // 这个回调跑在事件派发里，此时布局已完成，`bounds()` 就是用户看到的位置；
          // 若改在 `build()` 里读，拿到的会是上一帧甚至全 0（声明式重组发生在测量之前）。
          b.on_click = [&open_state, &anchor, button = &b] {
            const bool turn_on = !open_state.value();
            if (turn_on) {
              const st::math::Rect rect = button->bounds();
              anchor = st::math::Point{rect.x, rect.y};
            }
            open_state.set(turn_on);
          };
        }, {.height = 24.0f, .key = id});
      };

      // ② 分支筛选（值 = 筛选的分支；「仅当前分支」也算一种值）。
      const std::string branch_value = [this] {
        if (!log_filter_branch_.value().empty()) {
          const std::string& ref = log_filter_branch_.value();
          const std::size_t slash = ref.find('/');
          return slash == std::string::npos ? ref : ref.substr(slash + 1);
        }
        return log_filter_current_branch_.value() ? std::string("HEAD") : std::string{};
      }();
      filter_button("log-filter-branch", "分支", branch_value, log_branch_menu_open_.value(),
                    log_branch_menu_open_, log_branch_menu_anchor_);
      // ③ 用户筛选
      filter_button("log-filter-user", "用户", log_filter_author_.value(),
                    log_author_menu_open_.value(), log_author_menu_open_,
                    log_author_menu_anchor_);
      // ④ 日期筛选（值是“最近 N 天”）
      const std::string date_value = [this] {
        const int days = log_filter_days_.value();
        return days > 0 ? std::format("近 {} 天", days) : std::string{};
      }();
      filter_button("log-filter-date", "日期", date_value, log_date_menu_open_.value(),
                    log_date_menu_open_, log_date_menu_anchor_);

      // ⑥ 全部为弹性空隙：左侧是**筛选器**，右侧是**动作**。
      //
      // IDEA 的工具栏就是这个分区：左“改看什么”，右“做什么”。
      // 中间留白把两组拉开，避免“刷新”看上去像第五个筛选器。
      (void)spacer(c);

      // ⑦ 右侧一排：刷新 / 优选（cherry-pick）/ 显示选项。
      scm_icon_button(c, "log-refresh", "refresh", "重新读取日志", [this] {
        git_log_loaded_ = false;
      });
      // 「优选」= **cherry-pick 选中的提交**到当前分支（不是筛选器）。
      //
      // 为何图标不用星：星在这类工具栏里的语义是“收藏/只看重点”，与
      // “把这个提交搬到当前分支”毫无关联（用户原话：“优选是 cherrypick
      // 不是筛选当前分支（图标也改下）”）。用 `git-merge` 家族的“分叉合入”
      // 图形（`arrow-down-circle` → 这里用 `git-cherry-pick` 的弯箭头语义）。
      // 没选中提交时按钮**置灰**（而不是隐藏）：位置稳定，用户能看出“要先选一条”。
      (void)custom<Button>(c, [this](Button& b) {
        b.set_id("log-cherry-pick");
        b.set_icon("cherry-pick");
        b.set_variant(Button::Variant::Ghost);
        b.set_size(Button::Size::Small);
        b.set_enabled(!git_selected_hash_.empty());
        b.set_hint(git_selected_hash_.empty() ? "先选中一条提交，再 cherry-pick"
                                              : "Cherry-pick 选中的提交到当前分支");
        b.on_click = [this] { ask_cherry_pick_selected(); };
      }, {.width = 30.0f, .height = 24.0f, .key = "log-cherry-pick"});
      // 「显示选项」：列显隐（作者/时间/哈希）。
      //
      // 这里不用 `scm_icon_button`：它不把“按钮矩形”回调出来，而这个面板
      // 要锚在按钮下方。自带 `on_click` 里读 `bounds()` 即可（与四个筛选器同法）。
      (void)custom<Button>(c, [this](Button& b) {
        b.set_id("log-columns");
        b.set_icon("list");
        b.set_variant(Button::Variant::Ghost);
        b.set_size(Button::Size::Small);
        b.on_click = [this, &b] {
          const bool turn_on = !log_columns_menu_open_.value();
          if (turn_on) {
            const st::math::Rect rect = b.bounds();
            log_columns_menu_anchor_ = st::math::Point{rect.x, rect.y};
          }
          log_columns_menu_open_.set(turn_on);
        };
      }, {.width = 30.0f, .height = 24.0f, .key = "log-columns"});
    });
    // —— 多列表格（IDEA 同款）——
    //
    // 旧形态是“一行一个 Button + 空格拼起来的字符串”，问题是：
    // 提交信息/作者/日期挤成一串，**列不对齐**（不同长度就错位）——而日志表
    // 恰恰是靠列对齐来扫的（“谁改的 / 什么时候” 是两列独立信息）。
    //
    // 泳道列由**节点-边图**（`Table::set_graph`）表示，不走文本列。
    //
      // 数据层只有一份提交列表（`git_log_`，过滤结果是它的下标）+ 一份拓扑图
  //（`commit_graph`），没有任何“行”概念——字符网格通道已彻底移除。
    (void)custom<Table>(c, [this](Table& table) {
      table.set_id("git-log-table");
      // **不要表头**：这四列的含意自明（提交信息/作者/日期/哈希），扫日志时
      // 列宽固定、不依赖列名对齐；而表头要吃掉 36px 的黄金视野，还是唯一
      // 不随滚动移动的色带（IDEA 的日志页也没有列头）。
      table.set_show_header(false);
      // 行 = 提交（过滤后的）。过滤会把某些提交去掉，所以行号要**重新算**：
      // 拓扑图里的 `row` 是“未过滤拓扑序里的下标”，不能直接当表行号用。
      // `rows` 拿到的是 `git_log_` 的**下标**（不拷贝提交本体，见 `filtered_log_indices`）。
      const auto rows = filtered_log_indices();
      table.clear_rows();   // 每次重组重建：日志量与过滤条件都会变
      // 泳道带宽：默认值（28）只够画一条线，而“圆点+线”与首字之间还需要一档空白。
      table.set_lane_column_width(34.0f);
      const auto& graph = commit_graph();
      std::vector<Table::GraphNodeSpec> nodes;
      std::vector<Table::GraphEdgeSpec> edges;
      // 行号对齐：拓扑图的 `node.row` 是“**未过滤**拓扑序里的下标”，
      // 而过滤/仅当前分支会去掉一些提交——直接拿来当表行号会整体错位。
      //
      // 映射是**平面数组**（`graph_row → table_row`）而不是 hash→行的 `std::map`：
      // 两边同序时按下标直填（零哈希、零分配），只有对不上时才退回建表。
      // （旧写法每帧建一次 `std::map<std::string, size_t>`：200 条就是 200 次
      // 字符串拷贝 + 树插入，而它唯一的用途是把图的行号换算成表行号。）
      if (!graph.nodes.empty()) {
        std::vector<std::size_t> table_row_of_graph(graph.nodes.size(), rows.size());
        const bool aligned = graph.nodes.size() == git_log_.size();
        if (aligned) {
          // 同序：`rows` 里的值就是日志的全局下标，也是图的行号。
          for (std::size_t index = 0; index < rows.size(); ++index) {
            table_row_of_graph[rows[index]] = index;
          }
        } else {
          std::unordered_map<std::string_view, std::size_t> row_of_hash;
          row_of_hash.reserve(rows.size());
          for (std::size_t index = 0; index < rows.size(); ++index) {
            row_of_hash.emplace(git_log_[rows[index]].hash, index);
          }
          for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
            const auto found = row_of_hash.find(graph.nodes[index].hash);
            if (found != row_of_hash.end()) table_row_of_graph[index] = found->second;
          }
        }
        nodes.reserve(rows.size());
        for (std::size_t index = 0; index < graph.nodes.size(); ++index) {
          if (table_row_of_graph[index] >= rows.size()) continue;
          nodes.push_back(Table::GraphNodeSpec{.row = table_row_of_graph[index],
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
      // 列宽：时间与哈希都 **不许缩略**（`width == min_width` 即压不动）。
      //
      // 为何要“压不动”：这两列的内容是**定长**的（`2026-11-17 14:20` 16 字、
      // 哈希 8 位），缩略后变成 `2026-11-1…` / `7c0b3…`——既看不出时间，
      // 也认不出提交，这一列等于没了。宁可窄视图触发横向滚动，也不截断它们。
      //
      // 宽度按**实测字宽**给，不要估算：
      // 字号 15 下数字字形宽约 9px，`2026-11-17 14:20`（16 字）要 144px，
      // 加左右内边距（`kCellPadding × 2` = 24）共 168px；哈希 8 字同理 96px。
      // （实测教训：先按 6.8px/字 给 136px，结果仍然是 `2026-11-17 1…`——
      // 估算偏小，而截断后的列看着就像“没设置过宽度”。）
      // 列显隐（「显示选项」面板控制）：**列定义/色调/行数据三处必须同步**。
      //
      // 只改其中一处就会错位：例如只去掉列定义、行数据仍传四个值，
      // 结果就是“作者列里显示的是时间”。所以下面三者都由同一组开关生成。
      std::vector<TableColumn> columns;
      std::vector<Table::CellTone> tones;
      // 主列始终存在（它是这张表的主题；关掉就只剩元信息）。
      columns.push_back({.name = "提交信息", .width = 0.0f});
      tones.push_back(Table::CellTone::Normal);
      if (log_show_author_.value()) {
        columns.push_back({.name = "作者", .width = 110.0f, .align = TextAlign::Start,
                           .min_width = 76.0f, .shrink = 3});
        tones.push_back(Table::CellTone::Muted);
      }
      if (log_show_date_.value()) {
        columns.push_back({.name = "时间", .width = 172.0f, .align = TextAlign::Start,
                           .min_width = 172.0f, .shrink = 0});
        tones.push_back(Table::CellTone::Muted);
      }
      if (log_show_hash_.value()) {
        columns.push_back({.name = "哈希", .width = 100.0f, .align = TextAlign::Start,
                           .min_width = 100.0f, .shrink = 0});
        tones.push_back(Table::CellTone::Muted);
      }
      table.set_columns(std::move(columns));
      // 列色调：提交信息主色（用户在看的东西）、其余弱化（背景信息）。
      // 不这样做就只能全表一个颜色，靠列位置猜重点。
      table.set_column_tones(std::move(tones));
      // 行高（= 提交线的纵向间距）。
      //
      // 从 24 调到 28 的理由是**图而不是文字**：分支图的斜边要跨越相邻两行，
      // 行高太矮时曲线只能压在一两个像素里完成横向位移——“换道”看上去就像
      // 线被撞了一下（实测截图：曲线几乎看不见，只剩一个凸起）。28px 给了曲线
      // 上下各半行的缓冲，仍能 4 屏看到 ~20 条提交（原来约 23 条）。
      table.set_row_height(28.0f);
      for (const std::size_t row_index : rows) {
        const gbcode::LogEntry& entry = git_log_[row_index];
        // refs（分支/标签胶囊）拼在提交信息尾部：它是**这条提交的身份装饰**，
        // 单独开一列会在窄视图里把真正的内容挤没。
        std::string subject = entry.subject;
        if (!entry.refs.empty()) {
          std::string refs;
          for (const auto& ref : entry.refs) refs += " [" + ref + "]";
          subject += refs;
        }
        std::vector<std::string> cells{std::move(subject)};
        if (log_show_author_.value()) cells.push_back(entry.author);
        if (log_show_date_.value()) cells.push_back(entry.date);
        if (log_show_hash_.value()) cells.push_back(entry.short_hash);
        table.add_row(std::move(cells));
      }
      // 选中行与左栏的“当前提交”同源（同一条高亮，不是两个独立状态）。
      if (!git_selected_hash_.empty()) {
        for (std::size_t index = 0; index < rows.size(); ++index) {
          if (git_log_[rows[index]].short_hash == git_selected_hash_) {
            table.set_selected_row(index);
            break;
          }
        }
      }
      // 点行 = 看这条提交的详情。行下标就是 `rows` 下标（行 = 提交，无偏移）。
      table.set_on_row_click([this, rows](std::size_t row) {
        if (row < rows.size()) show_commit(git_log_[rows[row]]);
      });
    }, {.grow = true, .id = "git-log-host"});
    // 给 e2e 用的断言面：当前可见行区间（“表格真的在显示数据”而非空壳）。
    (void)text(c, [this] {
      const std::size_t total = filtered_log_indices().size();
      return total == 0U ? std::string("（没有提交，或读取失败）")
                         : std::string{};
    }, {.id = "git-log-empty"});
  }


  /// 一栏的标题行（标题 + 右侧一个可选图标钮）——三栏各一份，职责一眼可辨。
    void CodeEditorPage::build_pane_head(Composer& c, const char* id, std::function<std::string()> title,
                                     const char* action_id) {
    (void)row(c, {.gap = 4.0f, .height = 26.0f, .id = id}, [&] {
      (void)custom<Text>(c, [title = std::move(title)](Text& t) {
        t.set_content(title());
        t.set_weight(FontWeight::Regular);
        // 标题（日志栏是短哈希、详情栏也是哈希）要能**选中复制**：
        // 哈希是贴到 issue/聊天里最常用的信息，不能选就只能手写 8 位。
        t.set_selectable(true);
      }, {.grow = true, .id = std::string(id) + "-title"});
      if (action_id != nullptr) {
        scm_icon_button(c, action_id, "refresh", "重新读取", [this] { git_log_loaded_ = false; });
      }
    });
  }



  /// 大小写不敏感的子串包含（作者筛选用；中文不受影响）。
  auto CodeEditorPage::contains_ci(std::string_view haystack, std::string_view needle) -> bool {
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;
    const auto lower = [](char ch) {
      return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
    };
    for (std::size_t start = 0; start + needle.size() <= haystack.size(); ++start) {
      bool hit = true;
      for (std::size_t offset = 0; offset < needle.size(); ++offset) {
        if (lower(haystack[start + offset]) != lower(needle[offset])) {
          hit = false;
          break;
        }
      }
      if (hit) return true;
    }
    return false;
  }
  auto CodeEditorPage::filtered_log_indices() const -> std::vector<std::size_t> {
    const std::string needle = std::string(st::trim(log_filter_text_.value()));
    const bool current_only = log_filter_current_branch_.value();
    // 作者 / 日期两个筛选（IDEA 工具栏同形）。
    const std::string author_needle = std::string(st::trim(log_filter_author_.value()));
    const int days = log_filter_days_.value();
    // 日期筛选的**最早可接受前缀**（`YYYY-MM-DD`）：
    //
    // `entry.date` 是 `YYYY-MM-DD HH:MM`，ISO 格式的**字典序等于时间序**，
    // 所以直接字符串比较就能筛“最近 N 天”，不必解析时间戳也不用引入时区
    // （提交时间本就是作者本地时间，而用户要的是“最近几天”这类粗粒度）。
    std::string earliest_date{};
    if (days > 0) {
      // 用 `git` 给的同口径时间：取日志里最新一条的日期作为“今天”，
      // 而不是系统时钟——否则**在旧仓库上导出快照/离线看历史**时，
      // “最近 7 天”会变成空列表（实测代价：看着像“日志坏了”）。
      // 最新一条不在头部时不猜：直接用系统日期（正常仓库都满足）。
      std::string newest = git_log_.empty() ? std::string{} : git_log_.front().date;
      if (newest.size() >= 10) {
        newest.resize(10);
      } else {
        const auto now = std::chrono::system_clock::now();
        const std::time_t stamp = std::chrono::system_clock::to_time_t(now);
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &stamp);
#else
        localtime_r(&stamp, &local);
#endif
        newest = std::format("{:04d}-{:02d}-{:02d}", local.tm_year + 1900, local.tm_mon + 1,
                             local.tm_mday);
      }
      // 回退 N 天：用 `struct tm` 做日历运算（跨月/跨年自动进位，不用手写月末表）。
      std::tm base{};
      base.tm_year = std::stoi(newest.substr(0, 4)) - 1900;
      base.tm_mon = std::stoi(newest.substr(5, 2)) - 1;
      base.tm_mday = std::stoi(newest.substr(8, 2)) - days;
      base.tm_hour = 12;   // 正午基准：避开夏令时/零点进位边界
      std::mktime(&base);
      earliest_date = std::format("{:04d}-{:02d}-{:02d}", base.tm_year + 1900, base.tm_mon + 1,
                                  base.tm_mday);
    }
    // **分支筛选**（点分支列表设的那个）：按 hash 集合判，不看泳道号。
    //
    // 为何不能用泳道：泳道号是绘制层的分配结果，“道 0 = 当前分支”只在
    // 恰好看 HEAD 那条时成立；用户点的是**任意一条**分支（包括远程分支），
    // 正确的集合是 `git rev-list <ref>`。
    const std::string& branch_ref = log_filter_branch_.value();
    const bool by_branch = !branch_ref.empty() && !log_filter_branch_commits_.empty();
    // 分支集合用**视图**建一次（不拷贝 200 个 40 位 hash 字符串——
    // 旧实现是 `std::set<std::string>`：200 次分配 + 树查找）。
    std::vector<std::string_view> branch_hashes;
    if (by_branch) {
      branch_hashes.reserve(log_filter_branch_commits_.size());
      for (const std::string& hash : log_filter_branch_commits_) branch_hashes.emplace_back(hash);
      std::ranges::sort(branch_hashes);
    }
    // “仅当前分支”的判据：**拓扑图里道号 0**。
    //
    // 道 0 的定义就是“从 HEAD 一路走第一个父提交的那条线”（见 `git_graph.cpp`），
    // 也就是当前分支本身，所以它天然是“当前分支上的提交”这个集合。
    // 道号要从未过滤的图里查（过滤后的行号已经重排过）。
    //
    // 用**平面数组**而不是 hash→lane 的 `std::map`：`LogEntry` 的 hash 是 40 位定长，
    // 而 `git_log_` 与 `git_graph_.nodes` 在正常路径下**同序同长**（两者都是
    // `--all --topo-order` 的同一份输出）。先试按下标直取，只有真的对不上
    // （`ref` 变了、图被裁过）才退回建表。
    std::unordered_map<std::string_view, std::size_t> lane_of_hash;
    if (current_only) {
      bool aligned = git_graph_.nodes.size() == git_log_.size();
      for (std::size_t index = 0; aligned && index < git_log_.size(); ++index) {
        if (git_graph_.nodes[index].hash != git_log_[index].hash) aligned = false;
      }
      if (!aligned) {
        lane_of_hash.reserve(git_graph_.nodes.size());
        for (const auto& node : git_graph_.nodes) lane_of_hash.emplace(node.hash, node.lane);
      }
    }
    std::vector<std::size_t> out;
    out.reserve(git_log_.size());
    for (std::size_t index = 0; index < git_log_.size(); ++index) {
      const gbcode::LogEntry& entry = git_log_[index];
      if (by_branch &&
          !std::ranges::binary_search(branch_hashes, std::string_view(entry.hash))) {
        continue;
      }
      if (current_only) {
        std::size_t lane = 0;
        if (lane_of_hash.empty()) {
          lane = index < git_graph_.nodes.size() ? git_graph_.nodes[index].lane : 1U;
        } else {
          const auto found = lane_of_hash.find(entry.hash);
          if (found == lane_of_hash.end()) continue;
          lane = found->second;
        }
        if (lane != 0) continue;
      }
      if (!needle.empty()) {
        const bool hit = entry.subject.find(needle) != std::string::npos ||
                         entry.author.find(needle) != std::string::npos ||
                         entry.short_hash.find(needle) != std::string::npos;
        if (!hit) continue;
      }
      if (!author_needle.empty() && !contains_ci(entry.author, author_needle)) continue;
      // 日期筛选：字典序前缀比较（见上面的 `earliest_date` 说明）。
      if (!earliest_date.empty() && entry.date.size() >= 10 &&
          entry.date.compare(0, 10, earliest_date) < 0) {
        continue;
      }
      out.push_back(index);
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
    // 标题栏：**不再放哈希**（用户要件）。
    //
    // 理由：哈希是“身份信息”，与作者/日期同一类，应该和它们排在一起；
    // 单独占一行标题会让它看起来像栏名，而不是这笔提交的属性。
    // 而栏名在三栏布局里本就不言自明，于是这一行只用来给空态/有态交代一句。
    build_pane_head(c, "git-detail-head", [this] {
      return git_selected_hash_.empty() ? std::string("选中一笔提交") : std::string("提交详情");
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
            // —— 提交信息：**Markdown 渲染 + 可选中** ——
            //
            // 为何不用 `Text`：提交说明本来就用 Markdown 写（标题/列表/代码块），
            // 当纯文本贴出来会把 `## 背景`、`- 修复` 这些标记原样显示，
            // 读者还得自己在脑子里解——而 IDEA/VSCode 都是渲染的。
            //
            // `set_selectable(true)` 是硬需求（用户要件）：提交信息和哈希
            // 都要能选中复制（贴到 issue/聊天里）。
            (void)custom<MarkdownView>(c, [this](MarkdownView& view) {
              view.set_id("git-commit-summary");
              const std::string& text =
                  git_commit_message_.empty() ? git_commit_summary_.value() : git_commit_message_;
              view.set_markdown(text);
              view.set_selectable(true);
              // 链接不接管：提交说明里的链接点开会弹浏览器，
              // 而日志页里误点一下不该把用户带去别处。
              view.set_open_links(false);
              // **不硬写字号**：跟主题正文（`metrics.font_base` = 15）走。
              //
              // 旧代码写 `set_base_font_size(14.0f)`，而 `font_base` 是 15——
              // 于是提交信息比同屏的日志表（15）、元信息（12）看着“小一号”，
              // 而 Markdown 的段落行距又比普通行宽（1.6 倍），视觉上就更显小。
              // 正文基准就该是正文：需要强调的字号交给 Markdown 自己的标题级差。
              // 内边距归**外部布局**管（`column.padding_x`），组件侧清零：
              // 两层叠加会让正文比同栏文字多缩进一段（实测首字 x 差 25px）。
              view.style().padding = st::math::Insets{};
            }, {.id = "git-commit-summary"});
            // —— 元信息行：**哈希 · 作者 · 日期** ——
            //
            // 为何排在一行：它们都是“这笔提交的身份”；逐项分级反而把一屏拆成三行。
            // 增删统计**不在这里**——它跟文件清单在一起（见 `build_commit_files`）：
            // 统计描述的就是下面那批文件，放在文件表标题行左侧才对得上。
            (void)custom<Text>(c, [this](Text& t) {
              t.set_id("git-commit-meta");
              // 哈希在前（它是身份，扫一眼就知道是哪笔），作者与日期随后。
              t.set_content(std::format("{} · {} · {}", git_selected_hash_,
                                        git_selected_author_, git_selected_date_));
              t.set_tone(Tone::Muted);
              t.set_font_size(12.0f);
              t.set_selectable(true);   // 哈希/作者/时间都要能选中复制
            }, {.id = "git-commit-meta"});
            // —— 文件清单：列表 / 树 两种视图 ——
            //
            // 旧版是一行一个 Button，文本拼成 `path  +N −M`：路径长短不一时
            // 增删数字**参差不齐**，而“哪些文件动得多”恰恰要靠这两列对齐才扫得出来。
            build_commit_files(c);
          });
        },
        [](ScrollView& scroll) { scroll.set_id("git-detail-scroll"); },
        {.grow = true, .id = "git-detail-host"});
  }


  /// 提交详情里的**文件清单**（列表 / 树 两种视图）。
  ///
  /// 为何两种都要：
  /// * 列表：扫“一共改了哪些文件”最快，一屏二十多条，且路径完整可读；
  /// * 树：回答“改动集中在哪个目录”——几十个文件排成树形才看得出来。
  ///
  /// 两者共用同一个 `Table`（列对齐才能比“哪个文件改得多”），
  /// 树视图把层级编进**第一列文本**（缩进 + 展开箭头）而不是另开列：
  /// 另开一列会在窄栏里把路径挤没，而缩进本来就是文本的一部分。
  void CodeEditorPage::build_commit_files(Composer& c) {
    // —— 视图标题行：标签 + 两态切换 ——
    //
    // 切换按钮用 `Soft/Ghost` 区分当前态（与分支面板的组标题同一手法）。
    // —— 标题行：**左侧数量统计，右侧视图切换** ——
    //
    // 为何把统计放这里而不是元信息行：`N 个文件 +A −R` 描述的正是下面
    // 这批文件，与它们同在一行才对得上；而且切换按钮就在右侧，
    // 一眼就是“这批文件的统计 + 看它们的两种方式”。
    //
    // 切换按钮用 `Soft/Ghost` 区分当前态（与分支面板的组标题同一手法）。
    (void)row(c, {.gap = 4.0f, .height = 22.0f, .id = "git-commit-files-head"}, [&] {
      // 数量统计（可选中复制：常被拿去汇报）。
      (void)custom<Text>(c, [this](Text& t) {
        t.set_id("git-commit-stat");
        t.set_content(std::format("{} 个文件  +{} −{}", git_selected_stat_.files,
                                  git_selected_stat_.added, git_selected_stat_.removed));
        t.set_tone(Tone::Muted);
        t.set_font_size(12.0f);
        t.set_selectable(true);
      }, {.grow = true});
      const bool is_list = commit_files_view_.value() == FileViewMode::List;
      (void)custom<Button>(c, [this, is_list](Button& b) {
        b.set_id("commit-files-list");
        b.set_icon("list");
        b.set_variant(is_list ? Button::Variant::Soft : Button::Variant::Ghost);
        b.set_size(Button::Size::Small);
        b.set_hint("列表视图");
        b.on_click = [this] { commit_files_view_.set(FileViewMode::List); };
      }, {.width = 26.0f, .height = 20.0f, .key = "commit-files-list"});
      (void)custom<Button>(c, [this, is_list](Button& b) {
        b.set_id("commit-files-tree");
        b.set_icon("folder-open");
        b.set_variant(!is_list ? Button::Variant::Soft : Button::Variant::Ghost);
        b.set_size(Button::Size::Small);
        b.set_hint("树视图（按目录分组）");
        b.on_click = [this] { commit_files_view_.set(FileViewMode::Tree); };
      }, {.width = 26.0f, .height = 20.0f, .key = "commit-files-tree"});
    });

    if (git_commit_files_.empty()) {
      (void)text(c, [] { return std::string("（这笔提交没有改动文件）"); },
                 {.id = "git-detail-no-files"});
      return;
    }

    // —— 行数据：由视图模式决定 ——
    //
    // 每行 = `{显示文本, 路径（点它开差异）, 是否可点}`。
    // 目录行不可点（点它是展开/收起，不是看差异）。
    struct FileRow {
      std::string label{};
      std::string path{};        ///< 空 = 目录行（不可开差异）
      std::size_t plus{0};
      std::size_t minus{0};
      std::string key{};         ///< 目录行 = 目录路径（供展开状态查）
    };
    std::vector<FileRow> rows;
    if (commit_files_view_.value() == FileViewMode::List) {
      rows.reserve(git_commit_files_.size());
      for (const auto& [path, plus, minus] : git_commit_files_) {
        rows.push_back({.label = path, .path = path, .plus = plus, .minus = minus});
      }
    } else {
      // —— 树：按 `/` 分段建一层目录，再按路径顺序深度优先展开 ——
      //
      // 不构建真实的树结构（`unordered_map<dir, node>`）：改动文件量级 ≤ 几百，
      // 而“按排序后的完整路径依次判断公共前缀”完全够用且**行数可控**——
      // 路径已经排好序（`git show --numstat` 的输出就是字典序），
      // 同一目录的条目必然相邻。
      std::set<std::string> emitted_dirs;
      for (const auto& [path, plus, minus] : git_commit_files_) {
        // 逐级重放祖先目录（已发过的跳过）。
        std::size_t slash = 0;
        bool hidden = false;
        while ((slash = path.find('/', slash)) != std::string::npos) {
          const std::string dir = path.substr(0, slash + 1);
          ++slash;
          if (hidden) break;
          if (emitted_dirs.insert(dir).second) {
            // 目录行的名字只要**末段**（缩进已表达层级）。
            const std::size_t prev = dir.rfind('/', dir.size() - 2);
            const std::string leaf = prev == std::string::npos
                                         ? dir.substr(0, dir.size() - 1)
                                         : dir.substr(prev + 1, dir.size() - prev - 2);
            const int depth = static_cast<int>(std::count(dir.begin(), dir.end(), '/')) - 1;
            const bool collapsed = commit_tree_collapsed_.value().contains(dir);
            rows.push_back({.label = std::string(static_cast<std::size_t>(std::max(0, depth)) * 2, ' ') +
                                     (collapsed ? "▸ " : "▾ ") + leaf,
                            .key = dir});
          }
          // 祖先被收起：本行与其余子孙都不出现（树的标准行为）。
          if (commit_tree_collapsed_.value().contains(dir)) {
            hidden = true;
            break;
          }
        }
        if (hidden) continue;
        const std::size_t depth = std::count(path.begin(), path.end(), '/');
        rows.push_back({.label = std::string(depth * 2, ' ') + st::fs::file_name(path),
                        .path = path,
                        .plus = plus,
                        .minus = minus});
      }
    }

    // —— 表格 ——
    //
    // 不 `grow`：放进 `ScrollView` 后它该按**行数**撑开自然高（grow 会把它
    // 拉到视口高，短清单下面挂一大片空白）。
    //
    // **无表头**（用户要件）：这两列的含意自明（路径 / 增 / 减），
    // 而表头要吃掉 36px 的黄金视野。
    (void)custom<Table>(c, [this, rows](Table& table) {
      table.set_id("git-commit-files-table");
      table.set_show_header(false);
      table.clear_rows();
      // 增/减列宽度**按实际最长内容算**，并且 `width == min_width`（压不动）：
      //
      // 这两列的内容是**定长但变宽**的（`+3` 两字，`+8446` 五字，`+12345` 六字），
      // 写死一个数字总会在某个量级上被截成 `+…`——而“改了多少行”正是这两列
      // 唯一的信息，截了就等于没有（用户报的“修改文件行数不能缩略”）。
      //
      // 宽度 = 最长串的字宽 + 左右内边距（`kCellPadding × 2`）+ 余量。
      // 量字宽靠 `measure` 拿不到 `RenderContext`，所以按**字符数×最大字宽**估：
      // 数字字形在字号 15 下最宽约 9px，`+` 更窄——偏大一点是安全的（宁可宽不可截）。
      std::size_t plus_chars = 2;
      std::size_t minus_chars = 2;
      for (const FileRow& row : rows) {
        plus_chars = std::max(plus_chars, std::format("+{}", row.plus).size());
        minus_chars = std::max(minus_chars, std::format("−{}", row.minus).size());
      }
      const auto column_width = [](std::size_t chars) {
        // UTF-8：`+`/数字都是单字节，`−`（U+2212）是三字节 —— 按码点算。
        return static_cast<float>(chars) * 9.5f + Table::kCellPadding * 2.0f + 4.0f;
      };
      const float plus_width = column_width(plus_chars);
      const float minus_width = column_width(minus_chars);
      table.set_columns({
          {.name = "文件", .width = 0.0f},
          {.name = "+", .width = plus_width, .min_width = plus_width, .shrink = 0},
          {.name = "−", .width = minus_width, .min_width = minus_width, .shrink = 0},
      });
      table.set_column_tones({Table::CellTone::Normal, Table::CellTone::Muted,
                              Table::CellTone::Muted});
      table.set_row_height(24.0f);
      for (const FileRow& row : rows) {
        table.add_row({row.label, std::format("+{}", row.plus), std::format("−{}", row.minus)});
      }
      // 点行：文件行 → 看该文件的并列差异；目录行 → 展开/收起。
      table.set_on_row_click([this, rows](std::size_t index) {
        if (index >= rows.size()) return;
        const FileRow& row = rows[index];
        if (!row.path.empty()) {
          open_commit_file(git_selected_hash_, row.path);
          return;
        }
        // 目录行：翻折叠态。
        auto next = commit_tree_collapsed_.value();
        if (!next.insert(row.key).second) next.erase(row.key);
        commit_tree_collapsed_.set(std::move(next));
      });
    }, {.id = "git-commit-files"});
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


  /// Cherry-pick **当前选中的提交**到当前分支。
  ///
  /// 为何要确认：cherry-pick 会**新增一笔提交**、可能产生冲突（冲突后仓库
  /// 进入 `CHERRY_PICK_HEAD` 多步状态），不是只读操作。
  ///
  /// 没选中提交时按钮是置灰的，这里仍然再拦一道：
  /// 控制通道/快捷键可能绕过 `enabled`，而“不知道要 pick 哪个”必有错。
  void CodeEditorPage::ask_cherry_pick_selected() {
    if (git_selected_hash_.empty()) {
      status_.set("先选中一条提交，再 cherry-pick");
      return;
    }
    // 拿完整 hash：`git_selected_hash_` 存的是 8 位短哈希（展示用），
    // 而 `git cherry-pick` 接受短哈希也能工作——但从日志里取完整值更稳
    //（目标仓巨大时 8 位在极端情况下可能不唯一）。
    std::string ref = git_selected_hash_;
    for (const gbcode::LogEntry& entry : git_log_) {
      if (entry.short_hash == git_selected_hash_) {
        ref = entry.hash;
        break;
      }
    }
    ask_write({.op = gbcode::WriteOp::CherryPick, .ref = ref},
              std::format("Cherry-pick 提交 {}", git_selected_hash_),
              "会在当前分支上**新建一笔提交**；冲突时仓库进入 cherry-pick 状态（可继续/跳过/中止）。");
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
        // 同步读 200 条实测 ~100 ms，代价可以接受。
        //
        // 日志与拓扑**一条命令拿齐**（`read_log_and_graph`）：分别调
        // `read_log` 与 `read_commit_graph` 会把同一段历史读两遍。
        auto both = gbcode::read_log_and_graph(workspace_, 200);
        git_log_ = std::move(both.log);
        git_graph_ = std::move(both.graph);
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
    auto both = gbcode::read_log_and_graph(workspace_, 200, ref);
    git_log_ = std::move(both.log);
    git_graph_ = std::move(both.graph);
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
    // 完整提交信息（含正文）：详情栏按 Markdown 渲它。
    // 为空时回退到 subject（`write_args`/假数据路径可能没填 message）。
    git_commit_message_ = copy.message.empty() ? entry.subject : copy.message;
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
