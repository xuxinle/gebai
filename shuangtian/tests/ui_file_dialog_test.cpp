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

  // `select 2` = 第 2 项（排序后是 a.txt）；与鼠标点它**同一条** `activate_entry`，
  // 因此文件名回填、选中态、`on_confirm` 全部一致。
  std::string confirmed;
  hosted.dialog->on_confirm = [&](const std::string& path) { confirmed = path; };
  ST_CHECK(hosted.dialog->invoke_action("select", "2"));
  ST_CHECK_EQ(hosted.dialog->filename(), std::string("a.txt"));
  ST_CHECK_EQ(confirmed, st::fs::join(sandbox.dir, "a.txt"));

  // 越界如实返回 false（不假装成功）
  ST_CHECK(!hosted.dialog->invoke_action("select", "99"));
  ST_CHECK(!hosted.dialog->invoke_action("select", "not-a-number"));
  // 选目录项 = 进入目录（不触发 on_confirm），与双击目录同语义
  confirmed.clear();
  ST_CHECK(hosted.dialog->invoke_action("select", "0"));   // 排序后第 0 项是 sub/
  ST_CHECK_EQ(hosted.dialog->directory(), st::fs::join(sandbox.dir, "sub"));
  ST_CHECK_EQ(confirmed, std::string{});
}
