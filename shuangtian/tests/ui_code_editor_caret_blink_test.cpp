// 编辑器光标**闪烁**契约（缺陷史：终端与输入框各自修过两次，编辑器漏了）。
//
// ### 缺陷形态（用户报「编辑器光标不闪烁」）
//
// `CodeEditor::paint_content` 每帧按 `context.time_seconds` 判显隐地画光标，
// 却**从不登记唤醒点**（`request_wake_at`）。而「要不要出下一帧」由
// `UiRoot::needs_frame()` 汇总：静止的编辑器没有损坏区、没有动画 ⇒ 不再有帧
// ⇒ 光标**冻在它恰好可见的那一相位上**——肉眼看到的是「光标一直在，但不闪」。
//
// ### 为何必须把这组断言写死
//
// ① 它**不会让任何数据出错**（文本对、光标位置对、诊断对），只有盯着屏幕的人发现；
// ② 反过来，修法若把闪烁挂到 `request_animation()`（连续续帧），会立刻退化成
//    60fps 空转重绘——那是 2026-10-29 用户报「gbcode 占 CPU 70%」的根因。
//    所以「**既不能冻结、也不能空转**」必须成对钉住。
//
// 判据口径与 `Terminal`/`Input` 一致：1.0s 周期、前半可见；
// 元素只在**相位真翻转**的那一帧上报损坏区（每帧报会与帧间损坏判据互相拉成续帧环）。

#include "st/test/test.hpp"

#include <memory>
#include <string>

#include "st/raster/canvas.hpp"
#include "st/ui/components/code_editor.hpp"
#include "st/ui/text_port.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::ui::CodeEditor;

/// 最小文本端口：绘制路径要量行高/字宽，不需要真字形。
class BlankPort final : public st::ui::TextPort {
 public:
  static constexpr float kLineHeight{18.0f};
  [[nodiscard]] auto measure(std::string_view utf8, float) const -> st::math::Size override {
    return st::math::Size{8.0f * static_cast<float>(utf8.size()), kLineHeight};
  }
  [[nodiscard]] auto measure_width(std::string_view utf8, float,
                                   st::text::FontRole) const -> float override {
    return 8.0f * static_cast<float>(utf8.size());
  }
  [[nodiscard]] auto line_height(float) const -> float override { return kLineHeight; }
  [[nodiscard]] auto ascent(float) const -> float override { return kLineHeight * 0.75f; }
  [[nodiscard]] auto descent(float) const -> float override { return kLineHeight * 0.25f; }
  void draw(st::raster::Surface& canvas, std::string_view utf8, st::math::Point origin, float,
            st::math::Color color, st::text::FontRole, float, bool) const override {
    canvas.fill_rect(st::math::Rect{origin.x, origin.y, 8.0f * static_cast<float>(utf8.size()),
                                    kLineHeight * 0.7f},
                     st::raster::Paint::solid(color));
  }
  [[nodiscard]] auto ellipsize(std::string_view text, float, float) const
      -> std::string override {
    return std::string{text};
  }
  [[nodiscard]] auto wrap(std::string_view utf8, float, float) const
      -> std::vector<std::string_view> override {
    return {utf8};
  }
  [[nodiscard]] auto wrap_limited(std::string_view utf8, float, float, std::size_t) const
      -> std::vector<std::string> override {
    return {std::string{utf8}};
  }
};

void expect(bool ok, std::string message) {
  ::st::test::Registry::instance().count_check();
  if (!ok) ::st::test::Registry::instance().record_failure(__FILE__, __LINE__, std::move(message));
}

/// 搭一个“编辑器住在普通容器里”的夹具。
///
/// ⚠ 不能把编辑器直接当**根内容**：`UiRoot::tree_layout_dirty()` 读的是根元素的
/// `layout_dirty_`，而 `CodeEditor::mark_layout_dirty()` 故意不外冒
///（只置自己的标记）——直接当根时那个标记就永远没人清，帧循环恒清不下来。
/// 真实应用里它总是嵌在页面里（本夹具必须同构，否则测的是另一样东西）。
struct Fixture {
  BlankPort port{};
  st::ui::UiRoot root{};
  CodeEditor* editor{nullptr};
  std::unique_ptr<st::raster::Canvas> canvas{};

  Fixture() {
    root.set_theme(st::ui::Theme{st::ui::Theme::dark()});
    root.set_viewport(st::math::Size{800.0f, 400.0f});
    root.set_text_port(&port);
    auto host = std::make_unique<st::ui::Panel>();
    host->set_id("host");
    host->style().grow = true;
    auto owned = std::make_unique<CodeEditor>();
    editor = owned.get();
    editor->set_id("editor-blink");
    editor->style().grow = true;
    editor->set_text("line one\nline two\nline three\n");
    host->add_child(std::move(owned));
    root.set_content(std::move(host));
    root.layout(true);
    canvas = std::make_unique<st::raster::Canvas>(800, 400, 1.0f);
  }

  /// 画一帧并推进时间轴（返回本帧是否真的画了）。
  auto draw(double seconds) -> bool {
    root.set_time(seconds);
    const bool painted = root.paint_frame(*canvas);
    root.clear_dirty();
    return painted;
  }
};

}  // namespace

ST_TEST(code_editor_caret_blinks_while_focused) {
  Fixture fixture;
  expect(fixture.root.set_focus(fixture.editor), "无法把焦点给编辑器");
  expect(fixture.editor->focused(), "焦点没落在编辑器上");

  // 让它在已聚焦状态下画一帧：此时才会登记下一个翻转点。
  fixture.draw(10.0);

  // ① 聚焦静止：帧循环必须收敛（不能靠连续续帧来“假装会闪”）。
  int idle_frames = 0;
  while (fixture.root.dirty() && idle_frames < 40) {
    fixture.draw(10.0);   // 时间不动：唤醒点未到，不该再要帧
    ++idle_frames;
  }
  expect(idle_frames < 3,
         std::format("聚焦静止时还在续帧（{} 帧）——闪烁挂到了连续动画上，会烧满帧预算",
                     idle_frames));

  // ② 但必须有唤醒点（否则就是「光标不闪」的原缺陷）。
  const double wake_in_ms = fixture.root.next_wake_in_ms();
  expect(wake_in_ms > 0.0 && wake_in_ms <= 1000.0,
         std::format("没有登记唤醒点（next_wake_in_ms={}）——光标会冻结在某个相位上",
                     wake_in_ms));

  // ③ 时间推进到唤醒点之后：必须真的要求一帧（翻转要继续）。
  fixture.root.set_time(10.0 + wake_in_ms / 1000.0 + 0.001);
  expect(fixture.root.needs_frame(), "到唤醒点后仍不要帧 —— 光标不会翻转");
}

ST_TEST(code_editor_caret_flips_phase_across_wake_points) {
  Fixture fixture;
  expect(fixture.root.set_focus(fixture.editor), "无法把焦点给编辑器");
  fixture.draw(1.1);   // 相位 0.1 → 可见（duty 前半）

  const auto phase = [&fixture] {
    return fixture.editor->get_property("caret_blink").value_or("");
  };
  expect(phase() == "1", std::format("t=1.1 该处于可见半周期，实际 caret_blink={}", phase()));

  // 推进到不可见半周期：必须翻到 0。
  fixture.draw(1.6);
  expect(phase() == "0", std::format("t=1.6 该处于不可见半周期，实际 caret_blink={}", phase()));

  // 再推进回可见半周期。
  fixture.draw(2.1);
  expect(phase() == "1", std::format("t=2.1 该回到可见半周期，实际 caret_blink={}", phase()));
}

ST_TEST(code_editor_caret_does_not_blink_when_unfocused) {
  // 反向护栏：**失焦**（含只读）时不该登记唤醒点——那是纯浪费帧预算。
  Fixture fixture;
  fixture.draw(10.0);
  expect(!fixture.editor->focused(), "夹具起始就不该有焦点");

  int idle_frames = 0;
  while (fixture.root.dirty() && idle_frames < 40) {
    fixture.draw(10.0);
    ++idle_frames;
  }
  expect(idle_frames < 3, std::format("失焦时仍在续帧（{} 帧）", idle_frames));
  expect(fixture.root.next_wake_in_ms() < 0.0,
         std::format("失焦时还登记了唤醒点（{}ms）——会白拉帧",
                     fixture.root.next_wake_in_ms()));
  expect(fixture.editor->get_property("caret_blink").value_or("x") == "-1",
         "失焦时 caret_blink 该报 -1（未闪）");
}
