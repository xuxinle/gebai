/// FileDialog（打开/保存文件对话框）测试。
///
/// 覆盖：FillViewport 形态布局（遮罩铺满/卡片居中）、目录列表与排序、目录导航
/// （进入子目录/返回上级）、键盘选中与 Enter 确认路径拼接、Save 预填、
/// 取消与空文件名不触发、fs 失败不崩溃。
/// 测试数据用临时目录（`make_temp_dir`），用例结束 `remove_all` 清理。

#include "st/test/test.hpp"

#include <memory>
#include <string>
#include <vector>

#include "st/core/fs.hpp"
#include "st/ui/components/file_dialog.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::ui::FileDialog;
using st::ui::UiRoot;

/// 临时目录 + 预置文件树（RAII 清理）。
struct Sandbox {
  std::string dir{};

  Sandbox() {
    auto made = st::fs::make_temp_dir("st-file-dialog");
    ST_REQUIRE(made.has_value());
    dir = *made;
    // 结构：/a.txt(3B) /b.md(5B) /sub/（内含 c.txt） /zdir/（空目录）
    (void)st::fs::write_text(st::fs::join(dir, "a.txt"), "abc");
    (void)st::fs::write_text(st::fs::join(dir, "b.md"), "hello");
    (void)st::fs::create_directories(st::fs::join(dir, "sub"));
    (void)st::fs::write_text(st::fs::join(st::fs::join(dir, "sub"), "c.txt"), "xyz");
    (void)st::fs::create_directories(st::fs::join(dir, "zdir"));
  }
  ~Sandbox() { (void)st::fs::remove_all(dir); }
};

/// 挂好 overlay 的根（FillViewport 形态）。
struct Hosted {
  UiRoot root{};
  FileDialog* dialog{nullptr};
  std::unique_ptr<FileDialog> owned{};

  explicit Hosted(FileDialog::Mode mode = FileDialog::Mode::Open) {
    root.set_viewport(st::math::Size{1000.0f, 700.0f});
    root.set_content(std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column));
    owned = FileDialog::make(mode, "对话框");
    dialog = owned.get();
    root.add_overlay(std::move(owned), UiRoot::OverlayLayout::FillViewport);
  }

  void layout() { root.layout(true); }
};

[[nodiscard]] auto key(const std::string& name) -> st::ui::Event {
  st::ui::Event event;
  event.kind = st::ui::EventKind::KeyDown;
  event.key = name;
  return event;
}

}  // namespace

ST_TEST(file_dialog_layout_covers_mask_and_centers_card) {
  Hosted hosted;
  hosted.dialog->set_directory(".");
  hosted.layout();

  // 遮罩铺满视口（bounds = FillViewport 分到的矩形）。
  ST_CHECK_EQ(hosted.dialog->bounds().width, 1000.0f);
  ST_CHECK_EQ(hosted.dialog->bounds().height, 700.0f);
  // 卡片居中且在视口内。
  const st::math::Rect card = hosted.dialog->card_rect();
  ST_CHECK(card.width >= FileDialog::kMinWidth - 1.0f);
  ST_CHECK(card.height >= FileDialog::kMinHeight - 1.0f);
  const float dx = (card.x + card.width * 0.5f) - 500.0f;
  const float dy = (card.y + card.height * 0.5f) - 350.0f;
  ST_CHECK(dx < 1.0f && dx > -1.0f);
  ST_CHECK(dy < 1.0f && dy > -1.0f);
  // 内部分区有效：列表区在输入区上方、都在卡片内。
  ST_CHECK(hosted.dialog->list_rect().width > 0.0f);
  ST_CHECK(hosted.dialog->list_rect().bottom() <= hosted.dialog->input_rect().y + 0.5f);
  const st::math::Rect list = hosted.dialog->list_rect();
  const st::math::Rect input = hosted.dialog->input_rect();
  ST_CHECK(card.contains(st::math::Point{list.x + list.width * 0.5f, list.y + list.height * 0.5f}));
  ST_CHECK(card.contains(st::math::Point{input.x + input.width * 0.5f, input.y + input.height * 0.5f}));
}

ST_TEST(file_dialog_lists_directory_entries_sorted) {
  Sandbox sandbox;
  Hosted hosted;
  hosted.dialog->set_directory(sandbox.dir);
  hosted.layout();

  // 目录在前、同组按名排序：sub、zdir、a.txt、b.md
  ST_CHECK_EQ(hosted.dialog->entry_count(), std::size_t{4});
  ST_CHECK_EQ(hosted.dialog->entry(0)->name, std::string("sub"));
  ST_CHECK(hosted.dialog->entry(0)->is_dir);
  ST_CHECK_EQ(hosted.dialog->entry(1)->name, std::string("zdir"));
  ST_CHECK(hosted.dialog->entry(1)->is_dir);
  ST_CHECK_EQ(hosted.dialog->entry(2)->name, std::string("a.txt"));
  ST_CHECK(!hosted.dialog->entry(2)->is_dir);
  ST_CHECK_EQ(hosted.dialog->entry(2)->size, std::uint64_t{3});
  ST_CHECK_EQ(hosted.dialog->entry(3)->name, std::string("b.md"));
  // 语义值带目录与条目数（控制通道可见）。
  ST_CHECK(hosted.dialog->semantics_value().find("4") != std::string::npos);
}

ST_TEST(file_dialog_navigates_into_directory_and_back) {
  Sandbox sandbox;
  Hosted hosted;
  hosted.dialog->set_directory(sandbox.dir);
  hosted.layout();

  // 双击目录行进入 sub。
  const st::math::Rect sub_row = hosted.dialog->entry_rect(0);
  st::ui::Event dbl;
  dbl.kind = st::ui::EventKind::DoubleClick;
  dbl.position = st::math::Point{sub_row.x + 30.0f, sub_row.y + sub_row.height * 0.5f};
  (void)hosted.dialog->on_event(hosted.root.render_context(), dbl);
  ST_CHECK_EQ(hosted.dialog->directory(), st::fs::join(sandbox.dir, "sub"));
  ST_CHECK_EQ(hosted.dialog->entry_count(), std::size_t{1});
  ST_CHECK_EQ(hosted.dialog->entry(0)->name, std::string("c.txt"));

  // 点击 `..` 行返回上级。
  const st::math::Rect up = hosted.dialog->parent_row_rect();
  st::ui::Event click;
  click.kind = st::ui::EventKind::Click;
  click.position = st::math::Point{up.x + 30.0f, up.y + up.height * 0.5f};
  (void)hosted.dialog->on_event(hosted.root.render_context(), click);
  ST_CHECK_EQ(hosted.dialog->directory(), sandbox.dir);
  ST_CHECK_EQ(hosted.dialog->entry_count(), std::size_t{4});
}

ST_TEST(file_dialog_keyboard_select_and_enter_confirms_path) {
  Sandbox sandbox;
  Hosted hosted;
  hosted.dialog->set_directory(sandbox.dir);
  hosted.layout();

  const st::ui::RenderContext context = hosted.root.render_context();
  st::ui::Event down = key("ArrowDown");
  (void)hosted.dialog->on_event(context, down);  // 选中首项（sub 目录）
  ST_CHECK(hosted.dialog->selected_entry() != nullptr);
  ST_CHECK_EQ(hosted.dialog->selected_entry()->name, std::string("sub"));

  st::ui::Event down2 = key("ArrowDown");
  (void)hosted.dialog->on_event(context, down2);  // zdir
  st::ui::Event down3 = key("ArrowDown");
  (void)hosted.dialog->on_event(context, down3);  // a.txt（文件：回填文件名）
  ST_CHECK_EQ(hosted.dialog->filename(), std::string("a.txt"));

  // Enter：目录则进入（当前选中 a.txt 文件——先退回目录再验证 Enter 进入语义）
  std::string confirmed;
  hosted.dialog->on_confirm = [&confirmed](const std::string& path) { confirmed = path; };
  st::ui::Event up = key("ArrowUp");
  (void)hosted.dialog->on_event(context, up);  // 回到 zdir？—— 从 a.txt 上移是 zdir（目录）
  ST_CHECK(hosted.dialog->selected_entry()->is_dir);
  st::ui::Event enter = key("Enter");
  (void)hosted.dialog->on_event(context, enter);  // Enter 目录 = 进入
  ST_CHECK_EQ(hosted.dialog->directory(), st::fs::join(sandbox.dir, "zdir"));

  // 空目录里 Enter（无选中）→ confirm 走输入框（文件名 a.txt 已回填）
  (void)hosted.dialog->on_event(context, enter);
  ST_CHECK_EQ(confirmed, st::fs::join(st::fs::join(sandbox.dir, "zdir"), "a.txt"));
}

ST_TEST(file_dialog_save_mode_prefills_and_confirms) {
  Sandbox sandbox;
  Hosted hosted(FileDialog::Mode::Save);
  ST_CHECK_EQ(hosted.dialog->filename(), std::string("untitled.txt"));
  hosted.dialog->set_directory(sandbox.dir);
  hosted.layout();

  // 键盘把文件名改掉：聚焦输入行（点击输入区）→ Backspace 删尾 → TextInput 追加
  const st::ui::RenderContext context = hosted.root.render_context();
  const st::math::Rect input = hosted.dialog->input_rect();
  st::ui::Event focus;
  focus.kind = st::ui::EventKind::Click;
  focus.position = st::math::Point{input.x + 10.0f, input.y + input.height * 0.5f};
  (void)hosted.dialog->on_event(context, focus);

  for (int i = 0; i < 4; ++i) {  // 删掉 ".txt"
    st::ui::Event backspace = key("Backspace");
    (void)hosted.dialog->on_event(context, backspace);
  }
  st::ui::Event typed;
  typed.kind = st::ui::EventKind::TextInput;
  typed.text = ".md";
  (void)hosted.dialog->on_event(context, typed);
  ST_CHECK_EQ(hosted.dialog->filename(), std::string("untitled.md"));

  std::string confirmed;
  hosted.dialog->on_confirm = [&confirmed](const std::string& path) { confirmed = path; };
  st::ui::Event enter = key("Enter");
  (void)hosted.dialog->on_event(context, enter);
  ST_CHECK_EQ(confirmed, st::fs::join(sandbox.dir, "untitled.md"));
}

ST_TEST(file_dialog_cancel_and_empty_filename_no_confirm) {
  Sandbox sandbox;
  Hosted hosted;
  hosted.dialog->set_directory(sandbox.dir);
  hosted.layout();

  int cancelled = 0;
  hosted.dialog->on_cancel = [&cancelled]() { ++cancelled; };
  int confirmed = 0;
  hosted.dialog->on_confirm = [&](const std::string&) { ++confirmed; };

  // Esc 取消。
  const st::ui::RenderContext context = hosted.root.render_context();
  st::ui::Event escape = key("Escape");
  (void)hosted.dialog->on_event(context, escape);
  ST_CHECK_EQ(cancelled, 1);

  // 遮罩点击取消。
  st::ui::Event mask;
  mask.kind = st::ui::EventKind::Click;
  mask.position = st::math::Point{5.0f, 5.0f};  // 卡片外
  (void)hosted.dialog->on_event(context, mask);
  ST_CHECK_EQ(cancelled, 2);

  // 空文件名：Enter 不触发确认。
  hosted.dialog->set_filename("");
  st::ui::Event enter = key("Enter");
  (void)hosted.dialog->on_event(context, enter);
  ST_CHECK_EQ(confirmed, 0);

  // invoke 动作面：cancel/confirm 等效。
  (void)hosted.dialog->invoke_action("cancel", "");
  ST_CHECK_EQ(cancelled, 3);
  hosted.dialog->set_filename("x.txt");
  (void)hosted.dialog->invoke_action("confirm", "");
  ST_CHECK_EQ(confirmed, 1);
}

ST_TEST(file_dialog_fs_failure_shows_error_without_crash) {
  Hosted hosted;
  hosted.dialog->set_directory("/definitely/not/exist/12345");
  hosted.layout();

  ST_CHECK(hosted.dialog->error_text().size() > 0U);
  ST_CHECK_EQ(hosted.dialog->entry_count(), std::size_t{0});
  ST_CHECK(hosted.dialog->selected_entry() == nullptr);

  // 错误状态下键盘/滚轮/点击不崩溃。
  const st::ui::RenderContext context = hosted.root.render_context();
  st::ui::Event down = key("ArrowDown");
  (void)hosted.dialog->on_event(context, down);
  st::ui::Event wheel;
  wheel.kind = st::ui::EventKind::Wheel;
  wheel.wheel_delta = 1.0f;
  (void)hosted.dialog->on_event(context, wheel);
  st::ui::Event click;
  click.kind = st::ui::EventKind::Click;
  click.position = st::math::Point{hosted.dialog->list_rect().x + 10.0f,
                                   hosted.dialog->list_rect().y + 10.0f};
  (void)hosted.dialog->on_event(context, click);

  // 恢复到有效目录：错误清空、列表就绪。
  Sandbox sandbox;
  hosted.dialog->set_directory(sandbox.dir);
  ST_CHECK(hosted.dialog->error_text().empty());
  ST_CHECK_EQ(hosted.dialog->entry_count(), std::size_t{4});
}

ST_TEST(file_dialog_property_and_semantics_surface) {
  Sandbox sandbox;
  Hosted hosted;
  hosted.dialog->set_directory(sandbox.dir);
  hosted.layout();

  auto mode = hosted.dialog->get_property("mode");
  ST_REQUIRE(mode.has_value());
  ST_CHECK_EQ(*mode, std::string("open"));
  auto directory = hosted.dialog->get_property("directory");
  ST_REQUIRE(directory.has_value());
  ST_CHECK_EQ(*directory, sandbox.dir);

  ST_CHECK(hosted.dialog->set_property("filename", "hello.md"));
  ST_CHECK_EQ(hosted.dialog->filename(), std::string("hello.md"));

  const auto names = hosted.dialog->property_names();
  bool has_entry = false;
  for (std::string_view name : names) {
    if (name == "directory") has_entry = true;
  }
  ST_CHECK(has_entry);
}

// ── 无头 / 自动化选路（`pending_path` / `select`）─────────────────────────────
//
// 起因（真实应用）：`FileDialog` 选路全靠鼠标点列表 + 在文件名行打字，而 **headless
// 下没有真实鼠标键盘**——智能体根本"选不了文件"（实测「点选择图片无反应」），
// 整条 OCR 流程端到端验证不了。于是补一条程序化入口。
//
// 判据是**端到端**：注入路径 → `on_confirm` 真收到 → 拼出的全路径就是注入的那个。
// 只断言"`set_pending_path` 返回 true"不够——那是"接口存在"，不是"流程跑得通"。

ST_TEST(file_dialog_pending_path_completes_open_flow) {
  Sandbox sandbox;
  Hosted hosted;
  hosted.dialog->set_directory(sandbox.dir);
  hosted.layout();

  std::string confirmed;
  hosted.dialog->on_confirm = [&](const std::string& path) { confirmed = path; };

  // ① 注入一个**已存在的文件** → 进它所在目录 + 回填文件名
  const std::string target = st::fs::join(sandbox.dir, "a.txt");
  ST_CHECK(hosted.dialog->set_pending_path(target));
  ST_CHECK_EQ(hosted.dialog->directory(), sandbox.dir);
  ST_CHECK_EQ(hosted.dialog->filename(), std::string("a.txt"));
  // 列表里那一项也应被选中（程序化状态与"用户在界面上看到的"必须一致）
  const st::fs::DirEntry* selected = hosted.dialog->selected_entry();
  ST_REQUIRE(selected != nullptr);
  ST_CHECK_EQ(selected->name, std::string("a.txt"));

  // ② 确认 → `on_confirm` 收到**同一个**路径（整条链路打通）
  ST_CHECK(hosted.dialog->invoke_action("confirm", {}));
  ST_CHECK_EQ(confirmed, target);
}

ST_TEST(file_dialog_pending_path_enters_directory) {
  Sandbox sandbox;
  Hosted hosted;
  hosted.dialog->set_directory(sandbox.dir);
  hosted.layout();

  // 目录路径：进入该目录，**不改文件名**（与双击目录同语义）
  ST_CHECK(hosted.dialog->set_pending_path(st::fs::join(sandbox.dir, "sub")));
  ST_CHECK_EQ(hosted.dialog->directory(), st::fs::join(sandbox.dir, "sub"));
  ST_CHECK_EQ(hosted.dialog->filename(), std::string{});
  ST_CHECK_EQ(hosted.dialog->entry_count(), std::size_t{1});   // 只有 c.txt
  ST_CHECK_EQ(hosted.dialog->entry(0)->name, std::string("c.txt"));
}

ST_TEST(file_dialog_pending_path_rejects_missing_without_moving) {
  Sandbox sandbox;
  Hosted hosted;
  hosted.dialog->set_directory(sandbox.dir);
  hosted.layout();

  // 不存在的路径：**返回 false 且不改变任何状态**。
  // 这条比"能选文件"更重要：自动化最怕的是"调了没报错但去了别处"——
  // 那会让下游拿到一个看似正常的错目录，错误在很远的地方才炸。
  ST_CHECK(!hosted.dialog->set_pending_path(st::fs::join(sandbox.dir, "nope.txt")));
  ST_CHECK_EQ(hosted.dialog->directory(), sandbox.dir);
  ST_CHECK_EQ(hosted.dialog->filename(), std::string{});
  ST_CHECK(!hosted.dialog->set_pending_path(""));

  // 属性面（控制通道 `set` 的落点）也如实报错，不静默成功
  ST_CHECK(!hosted.dialog->set_property("pending_path", st::fs::join(sandbox.dir, "nope.txt")));
  ST_CHECK(hosted.dialog->set_property("pending_path", st::fs::join(sandbox.dir, "b.md")));
  ST_CHECK_EQ(hosted.dialog->filename(), std::string("b.md"));
}

ST_TEST(file_dialog_select_action_matches_mouse_click) {
  Sandbox sandbox;
  Hosted hosted;
  hosted.dialog->set_directory(sandbox.dir);
  hosted.layout();

  // `select 2` = 第 2 项（排序后是 a.txt）：语义 = **鼠标单击**（选中 + 回填文件名），
  // **不确认**。旧实现直接走 `activate_entry`（双击语义）：自动化调 `select`
  // 想"点一下那一行"，结果对话框当场确认关闭并把文件打开了——实测（打开真实项目）踩到。
  std::string confirmed;
  hosted.dialog->on_confirm = [&](const std::string& path) { confirmed = path; };
  ST_CHECK(hosted.dialog->invoke_action("select", "2"));
  ST_CHECK_EQ(hosted.dialog->filename(), std::string("a.txt"));
  ST_CHECK_EQ(confirmed, std::string{});          // 单击：不确认
  ST_REQUIRE(hosted.dialog->selected_entry() != nullptr);
  ST_CHECK_EQ(hosted.dialog->selected_entry()->name, std::string("a.txt"));

  // 越界如实返回 false（不假装成功）
  ST_CHECK(!hosted.dialog->invoke_action("select", "99"));
  ST_CHECK(!hosted.dialog->invoke_action("select", "not-a-number"));
  // 选目录项 = 进入目录（不触发 on_confirm），与单击目录行同语义
  confirmed.clear();
  ST_CHECK(hosted.dialog->invoke_action("select", "0"));   // 排序后第 0 项是 sub/
  ST_CHECK_EQ(hosted.dialog->directory(), st::fs::join(sandbox.dir, "sub"));
  ST_CHECK_EQ(confirmed, std::string{});
}

ST_TEST(file_dialog_activate_action_is_double_click) {
  // `activate` = 双击语义（目录 → 进入；文件 → 确认）。它与 `select`（单击）分开：
  // 自动化里"选中看看"与"就这样打开"是两件事。
  Sandbox sandbox;
  Hosted hosted;
  hosted.dialog->set_directory(sandbox.dir);
  hosted.layout();
  std::string confirmed;
  hosted.dialog->on_confirm = [&](const std::string& path) { confirmed = path; };
  ST_CHECK(hosted.dialog->invoke_action("activate", "2"));   // a.txt
  ST_CHECK_EQ(confirmed, st::fs::join(sandbox.dir, "a.txt"));
  ST_CHECK(!hosted.dialog->invoke_action("activate", "99"));
}

ST_TEST(file_dialog_directory_mode_confirms_the_current_directory) {
  // `Mode::Directory`（2026-10-07 新增，为「打开文件夹／换工作区」）：
  // 确认返回的是**当前目录本身**，不是「目录/文件名」拼接，也不要求文件名非空。
  // 回退 `confirm()` 的目录分支（让它走原来那条 `filename_.empty()` 早退）即变红。
  Sandbox sandbox;
  Hosted hosted;
  hosted.dialog->set_mode(FileDialog::Mode::Directory);
  hosted.dialog->set_directory(sandbox.dir);
  hosted.layout();
  ST_CHECK_EQ(std::string("directory"), std::string("directory"));
  ST_CHECK(hosted.dialog->filename().empty());

  std::string confirmed;
  hosted.dialog->on_confirm = [&](const std::string& path) { confirmed = path; };
  ST_CHECK(hosted.dialog->invoke_action("confirm", ""));
  ST_CHECK_EQ(confirmed, sandbox.dir);   // **目录本身**，不是 join(dir, "")
}

ST_TEST(file_dialog_directory_mode_ignores_files_and_text_input) {
  // 目录模式的两条边界（都是“用户会误触”的路径）：
  // ① 点一个**文件**不该确认——否则浏览时误点就会把“该文件所在目录”当结果交出去；
  // ② 不该接受文本输入——界面上没有文件名行，收下字符等于“打字无声无息地丢了”。
  Sandbox sandbox;
  Hosted hosted;
  hosted.dialog->set_mode(FileDialog::Mode::Directory);
  hosted.dialog->set_directory(sandbox.dir);
  hosted.layout();

  std::string confirmed;
  hosted.dialog->on_confirm = [&](const std::string& path) { confirmed = path; };
  // 排序后：0=sub/ 1=zdir/ 2=a.txt …（目录在前）；挑一个**文件**项
  bool found_file = false;
  for (std::size_t index = 0; index < hosted.dialog->entry_count(); ++index) {
    const st::fs::DirEntry* entry = hosted.dialog->entry(index);
    if (entry == nullptr || entry->is_dir) continue;
    found_file = true;
    ST_CHECK(hosted.dialog->invoke_action("select", std::to_string(index + 1)));
    break;
  }
  ST_CHECK(found_file);              // 夹具里确实有文件，否则本用例没测到东西
  ST_CHECK_EQ(confirmed, std::string{});
  ST_CHECK(hosted.dialog->filename().empty());   // 也不回填文件名

  // 文本输入被忽略
  // ⚠ 走 `root.dispatch`（与其余用例同一条路径）：`RenderContext` 含引用成员，
  // 不能默认构造，直接调 `on_event` 需要一份上下文。
  st::ui::Event typed;
  typed.kind = st::ui::EventKind::TextInput;
  typed.text = "typed";
  (void)hosted.root.dispatch(typed);
  ST_CHECK(hosted.dialog->filename().empty());
}

// ————————————————— 跨平台选择器增强（面包屑/侧栏/过滤/隐藏/新建/滚动）—————————————————

ST_TEST(file_dialog_name_filters_hide_non_matching_files) {
  Sandbox sandbox;
  Hosted hosted;
  hosted.dialog->set_directory(sandbox.dir);
  hosted.layout();
  // 全部可见（未设过滤）：a.txt b.md（+两个目录）。
  ST_CHECK(hosted.dialog->entry_count() >= std::size_t{4});
  // 过滤到 .md：只剩 b.md（目录永不过滤）。
  hosted.dialog->set_name_filters({".MD"});   // 大小写归一
  hosted.layout();
  std::size_t files = 0, dirs = 0;
  for (std::size_t index = 0; index < hosted.dialog->entry_count(); ++index) {
    const auto* entry = hosted.dialog->entry(index);
    if (entry == nullptr) continue;
    if (entry->is_dir) ++dirs; else ++files;
  }
  ST_CHECK_EQ(files, std::size_t{1});
  ST_CHECK_EQ(dirs, std::size_t{2});
  // 过滤后 pending_path 指向被过滤的文件：仍能选中（文件名回填，只是列表高亮不到）。
  ST_CHECK(hosted.dialog->set_pending_path(st::fs::join(sandbox.dir, "a.txt")));
  ST_CHECK_EQ(hosted.dialog->filename(), std::string("a.txt"));
  // 清空过滤：恢复全部。
  hosted.dialog->set_name_filters({});
  hosted.layout();
  ST_CHECK(hosted.dialog->entry_count() >= std::size_t{4});
}

ST_TEST(file_dialog_hidden_toggle_shows_dot_files) {
  Sandbox sandbox;
  (void)st::fs::write_text(st::fs::join(sandbox.dir, ".secret"), "x");
  Hosted hosted;
  hosted.dialog->set_directory(sandbox.dir);
  hosted.layout();
  const std::size_t before = hosted.dialog->entry_count();
  // 开隐藏：多出 .secret。
  hosted.dialog->set_show_hidden(true);
  hosted.layout();
  ST_CHECK_EQ(hosted.dialog->entry_count(), before + 1);
  bool found = false;
  for (std::size_t index = 0; index < hosted.dialog->entry_count(); ++index) {
    if (hosted.dialog->entry(index) != nullptr &&
        hosted.dialog->entry(index)->name == ".secret") {
      found = true;
      break;
    }
  }
  ST_CHECK(found);
  // 属性面同口径。
  ST_CHECK_EQ(hosted.dialog->get_property("show_hidden").value_or(""), std::string("true"));
}

ST_TEST(file_dialog_breadcrumbs_navigate_across_levels) {
  Sandbox sandbox;
  Hosted hosted;
  hosted.dialog->set_directory(st::fs::join(sandbox.dir, "sub"));
  hosted.layout();
  // 面包屑几何：至少三段（临时目录链里的最后几段）；首段命中区存在。
  bool any = false;
  for (std::size_t index = 0; index < 12; ++index) {
    if (!hosted.dialog->crumb_rect(index).is_empty()) any = true;
  }
  ST_CHECK(any);
  // 动作面跳转到末段前一段 = 回到 sandbox 根。
  // 数可见段数（末段之后的命中区为空）。
  std::size_t crumb_total = 0;
  while (!hosted.dialog->crumb_rect(crumb_total).is_empty()) ++crumb_total;
  ST_CHECK(crumb_total >= 2);
  ST_CHECK(hosted.dialog->invoke_action("goto_crumb", std::to_string(crumb_total - 2)));
  hosted.layout();
  ST_CHECK_EQ(st::fs::normalize(hosted.dialog->directory()), st::fs::normalize(sandbox.dir));
}

ST_TEST(file_dialog_create_folder_enters_it) {
  Sandbox sandbox;
  Hosted hosted;
  hosted.dialog->set_directory(sandbox.dir);
  hosted.layout();
  const std::string created = hosted.dialog->create_folder("mydir");
  ST_CHECK(!created.empty());
  ST_CHECK(hosted.dialog->directory().find("mydir") != std::string::npos);
  ST_CHECK(st::fs::is_directory(created));
  // 冲突自动后缀：先回到父目录（create_folder 建完就进入了 mydir），
  // 再在同目录建同名——第二次应得 mydir-1。
  hosted.dialog->set_directory(sandbox.dir);
  const std::string second = hosted.dialog->create_folder("mydir");
  ST_CHECK(second.find("mydir-1") != std::string::npos);
}

ST_TEST(file_dialog_places_include_home_and_custom) {
  Hosted hosted;
  const auto list = hosted.dialog->places();
  // 至少有主目录（跨平台都有）；自定义项追加在后。
  ST_CHECK(!list.empty());
  bool has_home = false;
  for (const auto& place : list) {
    if (place.label == "主目录") has_home = true;
  }
  ST_CHECK(has_home);
  hosted.dialog->set_places({{"沙箱标记", "/绝对/不存在的路径"}, {"根", "/"}});
  const auto list2 = hosted.dialog->places();
  bool has_root = false, has_missing = false;
  for (const auto& place : list2) {
    if (place.label == "根") has_root = true;
    if (place.label == "沙箱标记") has_missing = true;   // 不存在的被过滤掉
  }
  ST_CHECK(has_root);
  ST_CHECK(!has_missing);
}

ST_TEST(file_dialog_keyboard_selection_scrolls_into_view) {
  Sandbox sandbox;
  // 造 60 个文件（远超一屏）。
  for (int i = 0; i < 60; ++i) {
    (void)st::fs::write_text(st::fs::join(sandbox.dir, std::format("f{:02}.txt", i)), "x");
  }
  Hosted hosted;
  hosted.dialog->set_directory(sandbox.dir);
  hosted.layout();
  // End：选中末项且行在可视区内。
  st::ui::Event end = key("End");
  (void)hosted.root.dispatch(end);
  hosted.layout();
  const auto* picked = hosted.dialog->selected_entry();
  ST_REQUIRE(picked != nullptr);
  ST_CHECK_EQ(picked->name, std::string("f59.txt"));   // 目录在前、文件按名序：末项是最后的文件
  // Home：回到首项（目录排最前，sub 字典序最靠前的目录）。
  st::ui::Event home = key("Home");
  (void)hosted.root.dispatch(home);
  const auto* top = hosted.dialog->selected_entry();
  ST_REQUIRE(top != nullptr);
  ST_CHECK_EQ(top->name, std::string("sub"));
}

ST_TEST(file_dialog_fills_viewport_rect_for_mask) {
  // 回归：作为 overlay 内容时，FileDialog 必须拿到**整个视口矩形**——
  // 遮罩要盖满屏、卡片才会居中。旧缺陷：宿主 Panel 里无 grow → 高度塌成
  // 自然尺寸（实测卡片贴顶、遮罩只盖上半屏）。
  st::ui::UiRoot root;
  root.set_viewport(st::math::Size{1000.0f, 700.0f});
  root.set_content(std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column));
  auto owned = FileDialog::make(FileDialog::Mode::Open, "打开");
  FileDialog* dialog = owned.get();
  root.add_overlay(std::move(owned), UiRoot::OverlayLayout::FillViewport);
  root.layout(true);
  ST_CHECK_EQ(dialog->bounds().width, 1000.0f);
  ST_CHECK_EQ(dialog->bounds().height, 700.0f);
  // 卡片居中：上下留白大致相等，且高度是设计值（420）。
  const auto card = dialog->card_rect();
  ST_CHECK(card.width > 500.0f);
  ST_CHECK(card.height > 300.0f);
  const float above = card.y;
  const float below = 700.0f - card.bottom();
  ST_CHECK(std::fabs(above - below) < 2.0f);
}
