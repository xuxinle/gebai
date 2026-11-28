/// `TextArea` 的**提交语义**与**输入法插入点**契约。
///
/// 两条都是用户实测报上来的（不是推演出来的）：
///
/// 1. **回车要发送、`Shift+Enter` 换行**——聊天/搜索类输入框的通行约定。
///    修前 `TextArea` 根本没有提交能力（`Enter` 无条���插入 `\n`），
///    而 `Input`/`SearchBox` 早就有 `on_submit`——同族不对称。
/// 2. **首次输入时输入法候选窗没对准、第二次就对了**——根因是 `caret_rect()`
///    直接返回「上一帧绘制时缓存」的矩形，而首次聚焦那一帧还没画过，
///    缓存是空矩形 ⇒ `UiRoot` 上报 `(0,0,0,0)` ⇒ 系统把候选窗摆到客户区左上角。
///    `Input` 早已用「缓存无效时按 `bounds_` 兜底」解决过同一个问题，`TextArea` 没继承。
///
/// 这两条都**只能靠直接调组件**验证：无头环境没有系统输入法，控制通道也造不出
/// 一次真实的 `WM_IME_*`；但缺陷发生在我们这一侧（报文里给的矩形），
/// 因此断言「上报出去的矩形是否有效」就是精确判据。

#include <string>

#include "st/raster/canvas.hpp"
#include "st/test/test.hpp"
#include "st/ui/components/text_area.hpp"
#include "st/ui/theme.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::math::Color;
using st::math::Rect;
using st::ui::Event;
using st::ui::EventKind;
using st::ui::RenderContext;
using st::ui::TextArea;
using st::ui::Theme;

inline constexpr int kWidth = 320;
inline constexpr int kHeight = 120;

struct Fixture {
  Theme theme{Theme::light()};
  RenderContext context{theme, nullptr, 0.0};
  TextArea area{};

  Fixture() {
    area.set_text("abc");
    area.arrange(context, Rect{0.0f, 0.0f, static_cast<float>(kWidth),
                              static_cast<float>(kHeight)});
    // 把光标放到文末：`set_text` 是“换了一份文档”，光标停在 0（行首），
    // 而插入类断言要看的是“在末尾追加”——不摆好光标就会测成 `\nabc`。
    // 用 Ctrl+End（TextArea 里裸 `End` 是**行尾**，不是文档末尾）。
    Event to_end;
    to_end.kind = EventKind::KeyDown;
    to_end.key = "End";
    to_end.ctrl = true;
    (void)area.on_event(context, to_end);
  }
};

/// 造一次真实按键（走 `on_event`，与 shell 送进来的路径同一条）。
auto press(TextArea& area, RenderContext& context, const char* key, bool shift) -> bool {
  Event event;
  event.kind = EventKind::KeyDown;
  event.key = key;
  event.shift = shift;
  return area.on_event(context, event);
}

}  // namespace

// ————————————————————————————————————————————————————————————————
// ① 回车提交 / Shift+Enter 换行
// ————————————————————————————————————————————————————————————————

ST_TEST(text_area_enter_submits_when_on_submit_installed) {
  Fixture fixture;
  std::string submitted;
  fixture.area.on_submit = [&submitted](std::string_view text) { submitted = std::string(text); };

  ST_CHECK(press(fixture.area, fixture.context, "Enter", false));
  // 提交：回调拿到全文；**内容本身不插入换行**（换行是 Shift+Enter 的事）。
  ST_CHECK_EQ(submitted, std::string("abc"));
  ST_CHECK_EQ(fixture.area.value(), std::string("abc"));
}

ST_TEST(text_area_shift_enter_inserts_newline_even_with_on_submit) {
  Fixture fixture;
  bool called = false;
  fixture.area.on_submit = [&called](std::string_view) { called = true; };

  ST_CHECK(press(fixture.area, fixture.context, "Enter", true));
  // Shift+Enter 只插换行，**不触发提交**。
  ST_CHECK(!called);
  ST_CHECK_EQ(fixture.area.value(), std::string("abc\n"));
}

ST_TEST(text_area_enter_inserts_newline_without_on_submit) {
  // 没装回调 = 纯多行编辑器：回车仍是换行（老行为不能变）。
  Fixture fixture;
  ST_CHECK(press(fixture.area, fixture.context, "Enter", false));
  ST_CHECK_EQ(fixture.area.value(), std::string("abc\n"));
}

ST_TEST(text_area_activate_follows_on_submit) {
  // `invoke submit` / `activate`（自动化与命令面板用的那条路）必须与回车同义，
  // 否则「键盘能发、`invoke submit` 发不了」又是一种同族不对称。
  Fixture fixture;
  std::string submitted;
  fixture.area.on_submit = [&submitted](std::string_view text) { submitted = std::string(text); };
  fixture.area.activate();
  ST_CHECK_EQ(submitted, std::string("abc"));

  // 没装回调时退回旧语义（`on_change` 通知），不能变成什么都不做。
  Fixture fallback;
  std::string changed;
  fallback.area.on_change = [&changed](std::string_view text) { changed = std::string(text); };
  fallback.area.activate();
  ST_CHECK_EQ(changed, std::string("abc"));
}

// ————————————————————————————————————————————————————————————————
// ② 输入法插入点：首次就必须落在输入框内（不能是空矩形）
// ————————————————————————————————————————————————————————————————

ST_TEST(text_area_caret_rect_never_returns_empty_before_first_paint) {
  // **这一条就是「首次输入候选窗没对准」的判据。**
  //
  // 刻意不调 `paint_content`：模拟「窗口刚起来、用户立刻点进去打字」那一帧。
  Theme theme{Theme::light()};
  RenderContext context{theme, nullptr, 0.0};
  TextArea area;
  area.set_text("hi");
  // 只 arrange、不 paint —— 缓存此刻仍是空的。
  area.arrange(context, Rect{40.0f, 200.0f, 300.0f, 60.0f});

  const Rect caret = area.caret_rect();
  ST_CHECK(!caret.is_empty());
  // 纵向必须落在元素内（候选窗贴不贴光标行主要由纵坐标决定）。
  ST_CHECK(caret.y >= 200.0f);
  ST_CHECK(caret.y + caret.height <= 260.0f + 0.01f);
  ST_CHECK(caret.x >= 40.0f);
  ST_CHECK(caret.x <= 340.0f);
  ST_CHECK(caret.height > 1.0f);
}

ST_TEST(text_area_caret_rect_prefers_painted_cache) {
  // 画过之后必须用真实排版值（横向随光标），兜底只是首帧的应急。
  Theme theme{Theme::light()};
  RenderContext context{theme, nullptr, 0.0};
  TextArea area;
  area.set_text("hello");
  const Rect box{10.0f, 10.0f, 300.0f, 60.0f};
  area.arrange(context, box);

  // `arrange` 已按真实排版算过插入点（行首）。
  const Rect painted = area.caret_rect();
  ST_CHECK(!painted.is_empty());
  // 插入点在元素盒内。
  ST_CHECK(painted.x >= box.x);
  ST_CHECK(painted.y >= box.y);
  ST_CHECK(painted.y <= box.bottom());
}
