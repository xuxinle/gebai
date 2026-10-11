// 浮层族：命令面板/查找条/转到行/文件对话框/确认框/快捷键一览/重命名/
// 文件历史/补全弹层/右键菜单 的声明与聚焦落点。拆分自 main.cpp，零逻辑改动。

#include <algorithm>
#include <cstdio>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "page.hpp"
#include "st/core/string.hpp"
#include "st/ui/components/command_palette.hpp"

namespace gbcode_ui {


  /// 打开命令面板（`files_table=true` 走 Ctrl+P 的快速打开文件表）。
  auto CodeEditorPage::open_palette(bool files_table, std::string query) -> void {
    palette_shows_files_.set(files_table);
    palette_shows_symbols_.set(false);   // 命令/文件面板与符号面板互斥
    palette_query_.set(std::move(query));
    palette_open_.set(true);
  }


  auto CodeEditorPage::open_find() -> void {
    find_open_.set(true);
    if (editor == nullptr) return;
    // 预填当前选中文本（VSCode 同族行为）
    const std::string picked = editor->selected_text();
    if (!picked.empty() && picked.find('\n') == std::string::npos) {
      editor->set_find(picked);
      editor->find_next(false);
    }
    update_find_counter();
    // 查找条出现后把焦点交给查找输入框：否则敲的字会落到底层编辑器里
    //（与命令面板 `grab_focus()` 同一回事；查找条的构建在下一帧，故走待办标记）。
    focus_find_pending_ = true;
  }


  /// 帧首处理“查找条已出现→把焦点交给它”（构建期拿不到输入框实例）。
  void CodeEditorPage::apply_pending_focus() {
    if (!focus_find_pending_) return;
    if (find_needle_input == nullptr) return;
    focus_find_pending_ = false;
    if (!find_open_.value()) return;
    // 走宿主焦点契约（与 `CommandPalette::grab_focus` 同一路径）：
    // 直接把 `Input::activate()` 当“拿焦点”是错的——它的语义是“触发默认动作”
    // （对 Input 就是 `on_submit`，会当场多执行一次查找）。
    if (auto* host = find_needle_input->host(); host != nullptr) {
      (void)host->set_keyboard_focus(find_needle_input);
    }
  }


  // —— 转到行（Ctrl+G / 选择菜单）：一个小浮层 + 输入框，不靠“猜” ——
  //
  // 此前这个菜单项被实现成“跳到第一个问题行”，并与用户按下的 Ctrl+G 毫无关系——
  // 菜单项叫“转到行…”而行为是别的，属于可信度问题（用户试一次就不再信任何菜单项）。
  // 现在它是一条真链路：输入行号 → 回车 → 光标与视图都到那一行。
  void CodeEditorPage::open_goto_line() {
    if (editor == nullptr) {
      status_.set("转到行：没有打开的编辑器");
      return;
    }
    goto_line_open_.set(true);
    goto_line_error_.set("");
    goto_line_pending_focus_ = true;
  }


  /// 提交行号（“3” 或 “3:8” 行:列）。
  ///
  /// 输入以**纯文本**传进来：浮层里输入框的 Enter 可能来自键盘、也可能来自
  /// 控制通道的 `invoke submit`——两条路都是 `Input::on_submit`，语义相同。
  void CodeEditorPage::submit_goto_line(std::string_view text) {
    if (editor == nullptr) return;
    const std::string spec(st::trim(std::string(text)));
    if (spec.empty()) {
      goto_line_error_.set("请输入行号");
      return;
    }
    std::size_t line = 0;
    std::size_t column = 0;
    if (const std::size_t colon = spec.find(':'); colon != std::string::npos) {
      line = st::parse_u64(spec.substr(0, colon)).value_or(0);
      column = st::parse_u64(spec.substr(colon + 1)).value_or(0);
    } else {
      line = st::parse_u64(spec).value_or(0);
    }
    // 越界如实拒绝并报出**真实行数**：静默夹取会让用户以为“跳到末尾了”，
    // 而他打的是一串无效字符。
    if (line == 0) {
      goto_line_error_.set("行号必须是正整数");
      return;
    }
    const std::size_t total = editor->line_count();
    if (line > total) {
      goto_line_error_.set(std::format("超出范围（共 {} 行）", total));
      return;
    }
    goto_line_open_.set(false);
    goto_line_error_.set("");
    editor->goto_line(line);
    editor->scroll_to_line(line);
    if (column > 0) {
      const std::size_t start = editor->cursor_index() + column - 1;
      editor->set_selection(start, start);
    }
    on_cursor_moved();   // 状态栏的 Ln/Col 跟着走（不只依赖编辑器的回调）
    // 状态栏报**实际落点**（而不是用户输入的那串数字）：填了行:列时，列号会被
    // 行尾/换行夹取（空行上尤其明显），报“用户输入的”会与画面上的光标对不上。
    status_.set(std::format("已跳到第 {} 行 第 {} 列", editor->cursor_line() + 1,
                            editor->cursor_column() + 1));
  }


  /// 帧首处理“转到行浮层已出现→把焦点交给输入框”（构建期拿不到输入框实例）。
  /// 与查找条同一机制（见 `apply_pending_focus`）。
  void CodeEditorPage::apply_pending_goto_focus() {
    if (!goto_line_pending_focus_) return;
    if (goto_line_input == nullptr) return;
    goto_line_pending_focus_ = false;
    if (!goto_line_open_.value()) return;
    if (auto* host = goto_line_input->host(); host != nullptr) {
      (void)host->set_keyboard_focus(goto_line_input);
    }
  }


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
  void CodeEditorPage::build_branch_new_dialog(Composer& c) {
    if (!branch_new_open_.value()) return;
    (void)dsl::overlay(c, "branch-new", {}, [&] {
      (void)column(c, {.gap = 8.0f, .padding = 14.0f, .width = 420.0f, .id = "branch-new"}, [&] {
        (void)text(c, [] { return std::string("新建分支（从当前 HEAD）"); },
                   {.weight = FontWeight::Medium, .id = "branch-new-title"});
        (void)custom<Input>(c, [this](Input& field) {
          field.set_id("branch-new-name");
          field.set_placeholder("分支名，如 feat/diff-view");
          field.style().grow = true;
          field.on_change = [this](std::string_view value) { branch_new_name_.set(std::string(value)); };
          field.on_submit = [this](std::string_view) { create_branch(); };
        }, {.height = 26.0f, .grow = true, .id = "branch-new-name"});
        (void)row(c, {.gap = 6.0f}, [&] {
          (void)custom<Button>(c, [this](Button& b) {
            b.set_id("branch-new-cancel");
            b.set_label("取消");
            b.set_variant(Button::Variant::Ghost);
            b.set_size(Button::Size::Small);
            b.on_click = [this] { branch_new_open_.set(false); };
          }, {.height = 26.0f, .grow = true, .key = "branch-new-cancel"});
          (void)custom<Button>(c, [this](Button& b) {
            b.set_id("branch-new-go");
            b.set_label("新建并切换");
            b.set_variant(Button::Variant::Primary);
            b.set_size(Button::Size::Small);
            b.on_click = [this] { create_branch(); };
          }, {.height = 26.0f, .grow = true, .key = "branch-new-go"});
        });
      });
    });
  }


  /// 建标签浮层（名字 + 是否注解）。
  void CodeEditorPage::build_tag_new_dialog(Composer& c) {
    if (!tag_new_open_.value()) return;
    (void)dsl::overlay(c, "tag-new", {}, [&] {
      (void)column(c, {.gap = 8.0f, .padding = 14.0f, .width = 420.0f, .id = "tag-new"}, [&] {
        (void)text(c, [] { return std::string("在当前提交建标签"); },
                   {.weight = FontWeight::Medium, .id = "tag-new-title"});
        (void)custom<Input>(c, [this](Input& field) {
          field.set_id("tag-new-name");
          field.set_placeholder("标签名，如 v1.2.0");
          field.style().grow = true;
          field.on_change = [this](std::string_view value) { tag_new_name_.set(std::string(value)); };
          field.on_submit = [this](std::string_view) { create_tag(); };
        }, {.height = 26.0f, .grow = true, .id = "tag-new-name"});
        (void)custom<Button>(c, [this](Button& b) {
          b.set_id("tag-new-annotated");
          b.set_label(tag_new_annotated_.value() ? "注解标签 ✓" : "轻量标签");
          b.set_variant(tag_new_annotated_.value() ? Button::Variant::Soft : Button::Variant::Ghost);
          b.set_size(Button::Size::Small);
          b.on_click = [this] { tag_new_annotated_.set(!tag_new_annotated_.value()); };
        }, {.height = 24.0f, .key = "tag-new-annotated"});
        (void)row(c, {.gap = 6.0f}, [&] {
          (void)custom<Button>(c, [this](Button& b) {
            b.set_id("tag-new-cancel");
            b.set_label("取消");
            b.set_variant(Button::Variant::Ghost);
            b.set_size(Button::Size::Small);
            b.on_click = [this] { tag_new_open_.set(false); };
          }, {.height = 26.0f, .grow = true, .key = "tag-new-cancel"});
          (void)custom<Button>(c, [this](Button& b) {
            b.set_id("tag-new-go");
            b.set_label("创建");
            b.set_variant(Button::Variant::Primary);
            b.set_size(Button::Size::Small);
            b.on_click = [this] { create_tag(); };
          }, {.height = 26.0f, .grow = true, .key = "tag-new-go"});
        });
      });
    });
  }


  /// 端点选择浮层：A/B 两端可以从**分支 / 最近提交**里选（也可直接粘 rev）。
  ///
  /// 为何要有它：比较视图的价值就是“任意两端”，而只给几个常用端点的按钮
  /// 等于把“任意”锁在那几个选项上（歌白工作台的端点选择器就是为此存在）。
  void CodeEditorPage::build_compare_picker(Composer& c) {
    const std::size_t which = compare_pick_.value();
    if (which > 1) return;
    (void)dsl::overlay(c, "compare-picker", {}, [&] {
      (void)column(c, {.gap = 6.0f, .padding = 14.0f, .width = 520.0f,
                       .id = "compare-picker"}, [&] {
        (void)text(c, [which] {
          return which == 0 ? std::string("选择 A 端点（左 / 旧侧）")
                            : std::string("选择 B 端点（右 / 新侧）");
        }, {.weight = FontWeight::Medium, .id = "compare-picker-title"});
        // 保留端点（工作区 / 暂存区）。
        (void)row(c, {.gap = 6.0f}, [&] {
          struct Reserved {
            const char* label;
            const char* value;
          };
          static constexpr Reserved kReserved[] = {{"工作区", "WORKTREE"}, {"暂存区", "INDEX"}};
          for (const auto& item : kReserved) {
            (void)custom<Button>(c, [this, which, item](Button& b) {
              b.set_id(std::format("pick-end-{}", item.value));
              b.set_label(item.label);
              b.set_variant(Button::Variant::Secondary);
              b.set_size(Button::Size::Small);
              b.on_click = [this, which, value = std::string(item.value)] {
                pick_endpoint(which, value);
              };
            }, {.height = 28.0f, .grow = true, .key = std::string("pick:") + item.value});
          }
        });
        // 分支 + 最近提交（两者都是可解析的 rev）。
        (void)custom_container<ScrollView>(
            c,
            [&] {
              const auto group =
                  [&](const char* title,
                      const std::vector<std::pair<std::string, std::string>>& items) {
                    if (items.empty()) return;
                    (void)text(c, [title] { return std::string(title); }, {.color = Tone::Muted});
                    for (const auto& [label, value] : items) {
                      (void)custom<Button>(c, [this, which, label, value](Button& b) {
                        b.set_id("pick-ref-" + value);
                        b.set_label(label);
                        b.set_variant(Button::Variant::Ghost);
                        b.set_size(Button::Size::Small);
                        b.on_click = [this, which, value] { pick_endpoint(which, value); };
                      }, {.key = "pickref:" + value});
                    }
                  };
              std::vector<std::pair<std::string, std::string>> branches;
              for (const auto& branch : git_branches_) {
                branches.emplace_back(branch.name, branch.name);
              }
              std::vector<std::pair<std::string, std::string>> commits;
              for (const auto& entry : git_log_) {
                if (commits.size() >= 20) break;
                commits.emplace_back(std::format("{}  {}", entry.short_hash,
                                                 entry.subject.substr(0, 48)),
                                     entry.hash);
              }
              group("分支", branches);
              group("最近提交", commits);
            },
            [](ScrollView& scroll) { scroll.set_id("compare-picker-scroll"); },
            {.grow = true, .id = "compare-picker-list"});
        (void)custom<Button>(c, [this](Button& b) {
          b.set_id("compare-picker-cancel");
          b.set_label("取消");
          b.set_variant(Button::Variant::Ghost);
          b.set_size(Button::Size::Small);
          b.on_click = [this] { compare_pick_.set(2); };
        }, {.height = 28.0f, .key = "compare-picker-cancel"});
      });
    });
  }


  /// 选好一个端点（写入 A 或 B、关浮层、触发重读）。
  void CodeEditorPage::pick_endpoint(std::size_t which, const std::string& value) {
    if (which == 0) {
      compare_from_.set(value);
    } else {
      compare_to_.set(value);
    }
    compare_pick_.set(2);
    compare_loaded_ = false;
    compare_wanted_ = true;
  }


  /// 危险操作确认浮层（写入前先让用户看一眼**将要执行的命令**）。
  void CodeEditorPage::build_git_confirm(Composer& c) {
    if (!git_confirm_open_.value()) return;
    (void)dsl::overlay(c, "git-confirm", {}, [&] {
      (void)custom<Dialog>(c, [this](Dialog& dialog) {
        dialog.set_id("git-confirm");
        dialog.set_title(git_confirm_title_);
        dialog.set_body(git_confirm_body_);
        dialog.set_actions({"取消", "确定"});
        dialog.on_action = [this](std::size_t index) {
          git_confirm_open_.set(false);
          if (index == 1 && pending_git_confirm_) pending_git_confirm_();
        };
      }, {.id = "git-confirm-dialog"});
    });
  }


  void CodeEditorPage::build_rename_dialog(Composer& c) {
    if (!rename_open_.value()) return;
    (void)overlay(c, "rename", {}, [&] {
      (void)card(c, {.gap = 6.0f, .padding = 8.0f, .id = "rename-bar"}, [&] {
        (void)row(c, {.gap = 6.0f}, [&] {
          (void)text(c, [] { return std::string("重命名"); });
          (void)custom<Input>(c, [this](Input& field) {
            field.set_id("rename-input");
            field.set_placeholder("新名字");
            field.style().width = 220.0f;
            // 预填当前名字：**只在首次构建时写**。
            //
            // ⚠ 判据不能用"每帧写"：那会覆盖用户正在敲的内容。
            // 也不能用"这个 Input 对象是否被填过"（`rename_field_seeded_` 的旧写法）——
            // 浮层里的元素**每帧重建**，新对象没被填过，而标记已经是 true，
            // 于是输入框永远是空的（实测：预填名字正确打印、输入框内容却是空）。
            // 正确判据是"这一轮浮层是否已经开过"：用 `rename_open_` 上升沿。
            if (!rename_field_seeded_) {
              field.set_text(rename_text_.value());
              rename_field_seeded_ = true;
            }
            rename_input = &field;
            field.on_submit = [this](std::string_view value) {
              rename_text_.set(std::string(value));
              submit_rename();
            };
            field.on_change = [this](std::string_view value) { rename_text_.set(std::string(value)); };
          }, {.key = "rename-input"});
          (void)button(c, "重命名", [this] {
            if (rename_input != nullptr) rename_text_.set(rename_input->value());
            submit_rename();
          }, {.id = "rename-go"});
          (void)button(c, "×", [this] {
            rename_open_.set(false);
            rename_field_seeded_ = false;   // 下次打开时重新预填
          }, {.id = "rename-close"});
        });
      });
    });
  }


  /// 文件操作浮层（右键菜单的落点）：新建 / 重命名 用输入框，删除用确认。  ///
  /// 形态与 `build_goto_line` / `build_rename_dialog` **完全一致**（同一条浮层套路）：
  /// `overlay` + `card` + `custom<Input>` + 一个“干”按钮。
  /// 名字的读音只从 `fs_input` 指针取（它在每次重组时被重新赋值）——
  /// 不要跨帧存元素指针（浮层里的元素每帧重建，那是悬垂访问）。
  void CodeEditorPage::build_fs_action(Composer& c) {
    if (!fs_action_open_.value()) return;
    const FsAction action = fs_action_kind_.value();
    const bool confirming = action == FsAction::Delete;
    (void)overlay(c, "fs-action", {}, [&] {
      (void)card(c, {.gap = 6.0f, .padding = 8.0f, .id = "fs-action-bar"}, [&] {
        (void)row(c, {.gap = 6.0f}, [&] {
          (void)text(c, [confirming, this] {
            if (confirming) return std::string("删除（不可恢复）");
            return fs_action_kind_.value() == FsAction::Rename
                       ? std::string("重命名")
                       : (fs_action_kind_.value() == FsAction::NewFolder
                              ? std::string("新建文件夹")
                              : std::string("新建文件"));
          }, {.id = "fs-action-title"});
          if (!confirming) {
            (void)custom<Input>(c, [this](Input& field) {
              field.set_id("fs-action-input");
              field.set_placeholder("名字");
              field.style().width = 240.0f;
              fs_input = &field;
              field.on_submit = [this](std::string_view value) { submit_fs_action(value); };
            }, {.key = "fs-action-input"});
          }
          (void)button(c, confirming ? "删除" : "确定", [this, confirming] {
            const std::string value = fs_input != nullptr ? fs_input->value() : std::string{};
            submit_fs_action(confirming ? std::string_view{} : std::string_view{value});
          }, {.id = "fs-action-go"});
          (void)button(c, "×", [this] {
            fs_action_open_.set(false);
            fs_action_kind_.set(FsAction::None);
          }, {.id = "fs-action-close"});
        });
  /// 目标目录就住在浮层里：用户要能看见“我要建/改/删的是什么”。
  ///
  /// 它同时是**用户敲出来的名字的实时提示**：导航后两者拼起来就是最终落盘位置。
  /// 值为空字符串时（理论上不会发生）不声明这一行——空白行看着像渲染坏了。
  const std::string target = fs_action_target_.value();
  if (!target.empty()) {
    (void)text(c, [this] { return fs_action_target_.value(); }, {.id = "fs-action-target"});
  }
      });
    });
  }


  /// 新建/重命名浮层里的输入框聚焦待办（与 `apply_pending_goto_focus` 同一套路）。
  void CodeEditorPage::apply_pending_fs_focus() {
    if (!fs_focus_pending_) return;
    if (fs_input == nullptr) return;
    fs_focus_pending_ = false;
    if (!fs_action_open_.value()) return;
    if (auto* host = fs_input->host(); host != nullptr) {
      (void)host->set_keyboard_focus(fs_input);
    }
  }


  void CodeEditorPage::build_goto_line(Composer& c) {
    if (!goto_line_open_.value()) return;
    (void)overlay(c, "goto-line", {}, [&] {
      (void)card(c, {.gap = 6.0f, .padding = 8.0f, .id = "goto-bar"}, [&] {
        (void)row(c, {.gap = 6.0f}, [&] {
          (void)text(c, [] { return std::string("转到行"); });
          (void)custom<Input>(c, [this](Input& field) {
            field.set_id("goto-input");
            field.set_placeholder("行号（或 行:列）");
            field.style().width = 160.0f;
            goto_line_input = &field;
            field.on_submit = [this](std::string_view value) {
              // **按值**拷贝：提交会关掉浮层，下一次重组就销毁这个输入框，
              // 再靠引用/指针读它的缓冲就是悬垂访问（跨帧存裸指针的同一类坑）。
              submit_goto_line(std::string(value));
            };
          }, {.key = "goto-input"});
          (void)button(c, "跳转", [this] {
            submit_goto_line(goto_line_input != nullptr ? goto_line_input->value() : std::string{});
          }, {.id = "goto-go"});
          (void)button(c, "×", [this] {
            goto_line_open_.set(false);
            goto_line_error_.set("");
          }, {.id = "goto-close"});
        });
        // 出错信息就住在同一个浮层里（而不是把浮层关掉、把话丢到状态栏——
        // 那样用户得往窗口底部去找，还找不到是“哪一次输入”的错）。
        if (!goto_line_error_.value().empty()) {
          (void)text(c, [this] { return goto_line_error_.value(); }, {.id = "goto-error"});
        }
      });
    });
  }


  // —— 6. 查找替换条（Ctrl+F / Ctrl+H；可见性由 find_open_ 驱动）——
  void CodeEditorPage::build_find_bar(Composer& c) {
    if (!find_open_.value()) return;
    (void)overlay(c, "find", {}, [&] {
      (void)card(c, {.gap = 6.0f, .padding = 8.0f, .id = "find-bar"}, [&] {
        (void)row(c, {.gap = 6.0f}, [&] {
          (void)custom<Input>(c, [this](Input& field) {
            field.set_id("find-needle");
            field.set_placeholder("查找");
            field.style().width = 170.0f;
            find_needle_input = &field;
            field.on_change = [this](std::string_view value) {
              if (editor == nullptr) return;
              // 选项（大小写/全词）由组件自己管——**不再改写查找词**：
              // 旧写法给全词塞 `\b词\b`，而组件做的是字面量匹配，于是“勾了全词
              // 就一处也搜不到”，却什么错也不报（用户只能看到“搜不到”）。
              editor->set_find(std::string(value), find_options());
              editor->find_next(false);
              update_find_counter();
            };
            field.on_submit = [this](std::string_view) {
              if (editor == nullptr) return;
              editor->find_next(false);
              update_find_counter();
            };
          }, {.key = "find-needle"});
          (void)text(c, [this] { return find_counter_.value(); }, {.id = "find-counter"});
          (void)button(c, "↑", [this] { step_find(true); }, {.id = "find-prev"});
          (void)button(c, "↓", [this] { step_find(false); }, {.id = "find-next"});
          (void)custom<Input>(c, [this](Input& field) {
            field.set_id("find-replace");
            field.set_placeholder("替换为");
            field.style().width = 150.0f;
            find_replace_input = &field;   // 替换文本不进状态（不参与重组）
          }, {.key = "find-replace"});
          (void)button(c, "替换", [this] { replace_one(); }, {.id = "find-replace-one"});
          (void)button(c, "全部", [this] { replace_all(); }, {.id = "find-replace-all"});
          // 查找选项三开关：与搜索视图同一套胶囊（大小写 / 全词 / 正则）。
          // 改选项后**重跑查找**：结果集变了，不重跑就会“勾了正则但命中数还是旧的”。
          toggle_chip(c, "find-case", "Aa", find_case_.value(), [this] {
            find_case_.set(!find_case_.value());
            refind();
          });
          toggle_chip(c, "find-word", "ab", find_word_.value(), [this] {
            find_word_.set(!find_word_.value());
            refind();
          });
          // 正则胶囊：**如实置灰**。编辑器不做正则，而“按了没反应”与“不支持”
          // 是两件事——按钮文案直接写出为什么（与组件"动作面如实报能力"同一姿态）。
          toggle_chip(c, "find-regex", ".*", find_regex_.value(), [this] {
            status_.set("本编辑器的查找不做正则（只支持大小写与全词）");
          });
          (void)button(c, "×", [this] { close_find(); }, {.id = "find-close"});
        });
      });
    });
  }


  /// 用当前选项重跑查找（选项变化时调）。
  ///
  /// 大小写与全词都是**组件的一等选项**（`CodeEditor::FindOptions`）：直接交给
  /// 查找器、在“命中那一刻”判——而不是把查找词改写成 `\b词\b` 这种
  /// “看着支持、实际给出错结果”的做法（本编辑器不做正则，`\b` 只会被当成
  /// 两个字面字符，一处也匹配不到，而且什么错也不报）。
  void CodeEditorPage::refind() {
    if (editor == nullptr) return;
    const std::string needle = find_needle_input != nullptr ? find_needle_input->value()
                                                            : std::string{};
    editor->set_find(needle, find_options());
    editor->find_next(false);
    update_find_counter();
  }


  /// 把页面的两个查找开关翻译成组件的查找选项（**唯一转换点**）。
  [[nodiscard]] auto CodeEditorPage::find_options() const -> CodeEditor::FindOptions {
    return CodeEditor::FindOptions{.case_sensitive = find_case_.value(),
                                   .whole_word = find_word_.value()};
  }


  // —— 编辑器右键菜单（`ContextMenu` overlay；锚点 = 最后一次鼠标位置）——
  void CodeEditorPage::build_editor_context(Composer& c) {
    if (!context_open_.value()) return;
    (void)overlay(c, "editor-context", {}, [&] {
      // `ContextMenu` 的条目只能在构造后写入（声明式要求无参构造）：
      // 走 `custom<T>` 逃生舱 + `MenuPanel::set_items`，与 `ContextMenu::make` 同一套接线。
      (void)custom<ContextMenu>(c, [this](ContextMenu& menu) {
        menu.anchor_ = context_anchor_;
        if (menu.panel() != nullptr) {
          menu.panel()->set_items(context_items());
          menu.panel()->on_activate = [this](std::size_t index) {
            context_open_.set(false);
            run_context_action(index);
          };
          menu.panel()->on_close = [this] { context_open_.set(false); };
        }
        menu.on_close = [this] { context_open_.set(false); };
      }, {.id = "editor-context-menu"});
    });
  }


  /// 右键菜单条目（序号 = `kContextItems` 的下标）。
  void CodeEditorPage::run_context_action(std::size_t index) {
    const auto click = [this](const char* action) {
      if (editor != nullptr) (void)editor->invoke_action(action, {});
    };
    switch (index) {
      case 0: click("undo"); status_.set("已撤销"); break;
      case 1: click("redo"); status_.set("已重做"); break;
      case 2: click("cut"); status_.set("已剪切"); break;
      case 3: click("copy"); status_.set("已复制"); break;
      case 4: click("paste"); status_.set("已粘贴"); break;
      case 5: click("select_all"); status_.set("已全选"); break;
      case 6:
        if (editor != nullptr) status_.set(editor->toggle_comment() ? "已切换行注释"
                                                                   : "本语言无行注释标记");
        break;
      case 7: open_find(); break;
      case 8: open_file_history(); break;
      case 9: open_palette(false); break;
      default: break;
    }
  }


  /// 关闭脏标签确认对话框（`Dialog` overlay；三个按钮与 VSCode 同序）。
  void CodeEditorPage::build_close_confirm(Composer& c) {
    if (!pending_close_) return;
    (void)overlay(c, "close-confirm", {}, [&] {
      (void)custom<Dialog>(c, [this](Dialog& dialog) {
        dialog.set_id("close-confirm-dialog");
        dialog.set_title("未保存的修改");
        dialog.set_body(std::format("“{}”有未保存的修改。要保存吗？", pending_close_label_));
        dialog.set_actions({"取消", "不保存", "保存"});
        dialog.on_action = [this](std::size_t index) {
          if (index == 0) confirm_close_cancel();
          else if (index == 1) confirm_close_discard();
          else confirm_close_save();
        };
        dialog.on_dismiss = [this] { confirm_close_cancel(); };
      }, {.id = "close-confirm"});
    });
  }


  /// 键盘快捷键一览（`Ctrl+K` 或帮助菜单；内容与代码里注册的表同源，不手写第二份）。
  void CodeEditorPage::build_shortcuts_card(Composer& c) {
    if (!shortcuts_open_.value()) return;
    (void)overlay(c, "shortcuts", {}, [&] {
      (void)custom<Dialog>(c, [this](Dialog& dialog) {
        dialog.set_id("shortcuts-dialog");
        dialog.set_title("键盘快捷键");
        dialog.set_body(
            "文件\n"
            "  Ctrl+S          保存            Ctrl+W   关闭当前编辑器\n"
            "  Ctrl+Shift+S    另存为          Ctrl+O   打开文件\n"
            "  Ctrl+N          新建文件\n"
            "\n"
            "编辑\n"
            "  Ctrl+Z/Y        撤销 / 重做     Ctrl+/   切换行注释\n"
            "  Ctrl+A          全选            Ctrl+D   选中下一处同词\n"
            "  Alt+↑ / Alt+↓   上/下移当前行\n"
            "\n"
            "导航\n"
            "  Ctrl+F/Ctrl+H   查找 / 替换     Ctrl+G   转到行…\n"
            "  F8              下一处问题\n"
            "  Ctrl+P          快速打开文件    Ctrl+Shift+P  命令面板\n"
            "  Ctrl+Shift+E/F/G 资源管理器 / 搜索 / 变更\n"
            "  Ctrl+Tab        循环切换标签    Ctrl+B   切换侧栏\n"
            "  Ctrl+J          切换底部面板    Ctrl+K   本帮助\n"
            "  Esc             关闭浮层\n");
        dialog.set_actions({"知道了"});
        dialog.on_action = [this](std::size_t) { shortcuts_open_.set(false); };
        dialog.on_dismiss = [this] { shortcuts_open_.set(false); };
      }, {.id = "shortcuts"});
    });
  }


  // —— 打开 / 新建 / 另存为：真实 `FileDialog`（框架组件，支持程序化选路）——


  void CodeEditorPage::open_file_dialog() {
    pending_open_mode_ = 0;
    new_file_open_.set(true);
  }

  void CodeEditorPage::save_as() {
    pending_open_mode_ = 1;
    new_file_open_.set(true);
  }

  /// 打开**文件夹**（换工作区）：同一条对话框链路，用 `FileDialog` 的目录模式。
  void CodeEditorPage::open_folder_dialog() {
    pending_open_mode_ = 3;
    new_file_open_.set(true);
  }


  /// 文件对话框（打开 / 另存为 / 新建共用；`pending_open_mode_` 区分）。
  ///
  /// 为什么用真 `FileDialog` 而不是自绘输入框：目录浏览/上级/双击进入这些行为
  /// 组件已经写好且**在无头下可程序化选路**（`set_pending_path`），自绘会全部重写一遍。
  void CodeEditorPage::build_open_dialog(Composer& c) {
    if (!new_file_open_.value()) {
      dialog_seen_ = false;   // 对话框不在树上：下次重新打开要重新初始化目录
      return;
    }
    const bool saving = pending_open_mode_ == 1;
    const bool picking_folder = pending_open_mode_ == 3;
    // 本帧才被声明（从无到有）= 一次新的“打开”动作。
    const bool fresh_open = !dialog_seen_;
    dialog_seen_ = true;
    (void)overlay(c, "file-dialog", {}, [&] {
      (void)custom<FileDialog>(c, [this, saving, picking_folder, fresh_open](FileDialog& dialog) {
        dialog.set_id("file-dialog");
        // 标题（声明式 `custom<T>` 是无参构造，标题得构造后设）：
        // 没有标题时顶部会空一块（旧版就是这样，像"内容没加载出来"）。
        dialog.set_title(picking_folder ? "打开文件夹"
                                       : (saving ? "另存为" : "打开文件"));
        // 目录模式：没有文件名行、按钮是「选择此文件夹」、确认返回**当前目录**。
        if (picking_folder) dialog.set_mode(FileDialog::Mode::Directory);
        // ⚠ **只在一次新的“打开”动作里设置目录**：本 setup 每帧重组都跑，
        // 而对话框自己的导航（双击进目录/点面包屑/点侧栏）会改当前目录——
        // 每帧无条件 `set_directory` 会把用户导航打回起点（实测：面包屑只有
        // 一个 `.` 点、进不去任何目录）。
        if (fresh_open) {
          // 起始目录：记住上次（同一会话连开几个文件不用每次从工作区爬）；
          // 首次用**绝对路径**（相对路径 `.` 在面包屑里只有一段，看不出层级）。
          std::string start = dialog_start_dir_;
          if (start.empty()) start = workspace_.empty() ? std::string(".") : workspace_;
          if (const auto absolute = st::fs::absolute(start); absolute.has_value()) {
            dialog.set_directory(*absolute);
          } else {
            dialog.set_directory(start);
          }
        }
        // 打开模式：过滤到**可编辑的代码/文本文件**（框架组件的后缀白名单；
        // 目录不过滤——导航不受影响）。另存为/新建不过滤（目标名字任意）。
        if (!saving && pending_open_mode_ == 0) {
          dialog.set_name_filters({".cpp", ".hpp", ".c", ".h", ".cc", ".cxx",
                                   ".py", ".js", ".ts", ".rs", ".go", ".java",
                                   ".md", ".txt", ".json", ".yaml", ".yml",
                                   ".toml", ".sh", ".stlog"});
        }
        // 位置侧栏：工作区永远在（跨盘跳转的高频目标）。
        if (!workspace_.empty()) {
          dialog.set_places({{"工作区", workspace_}});
        }
        if (saving && editor != nullptr) {
          const auto list = buffers_.value();
          if (active_.value() < list.size()) dialog.set_filename(list[active_.value()].label);
        }
        // 确认时记下当前目录（下次打开从这起）。
        dialog.on_confirm = [this, saving](const std::string& path) {
          new_file_open_.set(false);
          // 记住这次所在的目录（下次打开从这起）。
          dialog_start_dir_ = st::fs::parent(path).empty() ? std::string(".")
                                                           : std::string(st::fs::parent(path));
          if (pending_open_mode_ == 3) {
            switch_workspace(path);
            return;
          }
          if (pending_open_mode_ == 2) {
            // 新建：写一个空文件并打开它
            if (st::fs::write_text(path, "")) {
              refresh_tree();
              open_path(path);
            } else {
              status_.set("新建失败：" + path);
            }
            return;
          }
          if (saving) {
            auto list = buffers_.value();
            if (active_.value() < list.size()) {
              list[active_.value()].path = path;
              list[active_.value()].key = path;
              const std::size_t slash = path.find_last_of("/\\");
              list[active_.value()].label =
                  slash == std::string::npos ? path : path.substr(slash + 1);
              list[active_.value()].language =
                  st::text::language_from_path(path).value_or(list[active_.value()].language);
              buffers_.set(std::move(list));
              save();
            }
            return;
          }
          open_path(path);
        };
        dialog.on_cancel = [this] { new_file_open_.set(false); };
      }, {.id = "file-dialog"});
    });
  }


  // —— 命令面板（overlay；命令表 / 文件表两种）——
  void CodeEditorPage::build_palette(Composer& c) {
    (void)overlay(c, "palette", {}, [&] {
      (void)custom<CommandPalette>(c, [this](CommandPalette& palette) {
        palette.set_id("command-palette");
        palette.set_commands(palette_shows_files_.value()
                                 ? file_commands()
                                 : (palette_shows_symbols_.value() ? symbol_commands()
                                                                   : command_table()));
        // 面板每次重新挂上时把过滤词同步过去。`set_query` 自带相等早退，
        // 所以“每帧调”是安全的；但不能无条件 `set_text`：那会把用户正在敲的字
        // 每帧覆写回去（组件已把这条写进它自己的注释）。
        if (palette.query() != palette_query_.value()) {
          palette.set_query(palette_query_.value());
        }
        palette.set_visible(true);
        palette.on_command = [this](std::string_view id) {
          // 符号面板不关面板（与“文件表”同族）：跳转是导航，
          // 用户很可能接着看下一个符号。
          if (id.rfind("symbol:", 0) != 0) palette_open_.set(false);
          run_command(std::string(id));
        };
        palette.on_close = [this] { palette_open_.set(false); };
        palette_ptr = &palette;
        // 面板显示后必须把焦点交给过滤框——不调的话敲的字会跑进底层编辑器里（实测踩到）。
        // 这一步要在**挂树之后**调：`Element::add_child` 会把宿主契约一路继承下去，
        // 此时 `grab_focus()` 才能经 `UiRoot::set_focus` 真把键盘焦点转过来。
        palette.grab_focus();
      }, {.id = "command-palette"});
    });
  }


  /// 文件历史弹窗（`Dialog` overlay；条目点一下 → 打开该提交对本文件的差异）。
  void CodeEditorPage::build_file_history(Composer& c) {
    if (!history_open_.value()) return;
    (void)overlay(c, "file-history", {}, [&] {
      (void)card(c, {.gap = 6.0f, .padding = 10.0f, .id = "file-history-card"}, [&] {
        (void)row(c, {.gap = 8.0f}, [&] {
          (void)text(c, [this] {
            return std::format("文件历史 · {}",
                               std::filesystem::path(history_path_).filename().string());
          }, {.id = "file-history-title"});
          (void)spacer(c);
          auto& close = button(c, "关闭", [this] { history_open_.set(false); },
                               {.id = "file-history-close"});
          close.style().width = 72.0f;
        });
        const auto& entries = history_entries_.value();
        if (entries.empty()) {
          (void)text(c, [] { return std::string("（没有历史）"); }, {.id = "file-history-empty"});
          return;
        }
        // 条目列表：短 hash + 日期 + 作者 + 标题。
        // 用现成的 DSL `list`（数据驱动、按 key 复用）+ `on_click`——手搓 `custom<List>`
        // 要自己管条目与回调，而这两件事 DSL 已经做完了。
        std::vector<dsl::ListItemData> items;
        items.reserve(entries.size());
        for (const auto& entry : entries) {
          // 用**短** hash（长 hash 40 位会把这行撑满，标题反而看不见——
          // 实测踩到：截图里只剩一串十六进制）。作者/日期放副行。
          items.push_back(dsl::ListItemData{
              .key = "fh-" + entry.hash,
              .label = std::format("{}  {}",
                                   entry.short_hash.empty() ? entry.hash : entry.short_hash,
                                   entry.subject),
              .subtitle = std::format("{} · {}", entry.author, entry.date)});
        }
        // 回调按**下标**取（`list` 给的是扁平序号）；副本捕获避免生命周期问题
        // （`entries` 是 `State` 里那份的引用，而回调可能在状态变更后触发）。
        Element& history_list = list(c, items, [this, snapshot = entries](std::size_t index) {
                    if (index < snapshot.size()) open_history_entry(snapshot[index]);
                  },
                  {.id = "file-history-list"});
        history_list.style().width = 560.0f;
        history_list.style().height = 260.0f;
      });
    });
  }


  /// 打开“该提交对本文件”的差异（`<hash>^` ↔ `<hash>`）。
  void CodeEditorPage::open_history_entry(const gbcode::LogEntry& entry) {
    history_open_.set(false);
    diff_pending_path_ = history_path_;
    gbcode::CompareOptions options;
    // 根提交没有父：`read_content_at` 会把 `<hash>^` 归一成空树（与服务端同一套规则），
    // 所以这里不必特判（否则跨文件导航按钮会凭空消失——那正是那个坑的形态）。
    options.from = gbcode::DiffEndpoint::parse(entry.hash + "^");
    options.to = gbcode::DiffEndpoint::parse(entry.hash);
    diff_pending_options_ = options;
    diff_wanted_ = true;
    status_.set("文件历史：打开 " + entry.hash);
  }


  /// 资源管理器右键菜单（与编辑器的右键菜单**同一形态**）。
  ///
  /// 为何不复用 `context_open_`：两个菜单的锚点与条目完全不同，共享一个开关会让
  /// “在编辑器里右键”与“在树里右键”互相触发对方的菜单。
  void CodeEditorPage::build_explorer_context(Composer& c) {
    if (!explorer_menu_open_.value()) return;
    (void)overlay(c, "explorer-context", {}, [&] {
      (void)custom<ContextMenu>(c, [this](ContextMenu& menu) {
        menu.anchor_ = explorer_menu_anchor_;
        if (menu.panel() != nullptr) {
          menu.panel()->set_items({{.id = "new-file", .label = "新建文件"},
                                   {.id = "new-folder", .label = "新建文件夹"},
                                   {.separator = true},
                                   {.id = "rename", .label = "重命名"},
                                   {.id = "delete", .label = "删除"},
                                   {.separator = true},
                                   {.id = "reveal", .label = "复制路径"}});
          menu.panel()->on_activate = [this](std::size_t index) {
            explorer_menu_open_.set(false);
            switch (index) {
              case 0: explorer_create(false); break;
              case 1: explorer_create(true); break;
              case 2: explorer_rename(); break;
              case 3: explorer_delete(); break;
              case 4:
                status_.set(std::format("路径：{}", tree_key_to_path(context_key_.value())));
                break;
              default: break;
            }
          };
          menu.panel()->on_close = [this] { explorer_menu_open_.set(false); };
        }
        menu.on_close = [this] { explorer_menu_open_.set(false); };
      }, {.id = "explorer-context-menu"});
    });
  }


  /// 构建补全弹层（overlay；条件声明）。
  void CodeEditorPage::build_completion_popup(Composer& c) {
    if (!completion_open_.value()) return;
    (void)overlay(c, "completion", {}, [&] {
      (void)custom<CompletionPopup>(c, [this](CompletionPopup& popup) {
        popup.set_id("completion-popup");
        // ⚠ **必须显式设可见**：`CompletionPopup` 构造时是隐藏的（浮层默认不显示），
        // 而声明式 overlay 只负责挂载——不设 visible 的话它会**不可见**：
        // 绘制与键盘派发都会被 `!visible()` 跳过（实测：弹层画面出得来但
        // ArrowDown/Enter 全不生效——`dispatch_key_into` 遍历子元素时跳过了它）。
        popup.set_visible(true);
        popup.set_anchor(completion_anchor_);
        std::vector<CompletionItemView> views;
        views.reserve(completion_items_.size());
        for (const auto& entry : completion_items_) {
          CompletionItemView view{};
          view.label = entry.label;
          view.detail = entry.detail;
          view.insert_text = entry.insert_text;
          view.badge = entry.badge;
          view.filter_text = entry.filter_text;
          views.push_back(std::move(view));
        }
        popup.set_items(std::move(views));
        popup.on_accept = [this](const std::string& insert) { accept_completion(insert); };
        popup.on_dismiss = [this](bool) { completion_open_.set(false); };
        popup.on_selection_changed = [this](std::size_t index) {
          // 按需取详情（`completionItem/resolve`）——每次移动都发太浪费，
          // 只在**详情为空**时请求（server 回过后弹层会记住）。
          if (index < completion_items_.size() && completion_items_[index].documentation.empty()) {
            const auto list = buffers_.value();
            if (active_.value() < list.size()) {
              (void)lsp_.resolve_completion(list[active_.value()].path, index);
            }
          }
        };
      }, {.id = "completion-popup"});
    });
  }


  /// 重命名输入浮层的聚焦（与其它输入浮层同一套路）。
  void CodeEditorPage::apply_pending_rename_focus() {
    if (!rename_focus_pending_) return;
    rename_focus_pending_ = false;
    // 浮层内的输入框在下一帧才建好：置标记，让 `build_rename_dialog` 落地时聚焦。
  }

}  // namespace gbcode_ui
