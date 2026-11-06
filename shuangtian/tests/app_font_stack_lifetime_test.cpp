/// `Application::font_stack()` 的**生命周期契约**：`start()` 之前没有字体。
///
/// ## 为什么需要这组用例
///
/// 旧签名返回**引用**（`const FontStack&`），实现是 `return *impl_->fonts;`。
/// 而字体是在 `Application::start()` 里装载的（那里才拿得到真实 DPI 与后端）。
/// 于是「构造完、start 之前」取一次就是**解空 `unique_ptr`**——实测后果是进程直接死：
///
/// ```
/// unique_ptr<FontStack>::operator*(): Assertion 'get() != pointer()' failed.
/// ```
///
/// 真实受害者是 gbcode：它在 `main` 的装配段就把字体栈交给页面（`page->font_stack_ = &app.font_stack();`），
/// 而 `app.start()` 在那之后才调用 ⇒ **整个应用启动即崩**。从报错里完全看不出
/// “这跟字体装载的时序有关”，所以它被当成“环境问题”挂了两轮。
///
/// ## 判据
///
/// ① 构造后立刻取 → 必须是 `nullptr`（不是解引用、不是崩溃）；
/// ② `start()` 之后取 → 必须非空（否则应用根本没法用离屏排版）；
/// ③ 两条合起来证明「时机」就是语义本身，而不是实现细节。
///
/// ## 为什么逆向验证会红
///
/// 把实现改回 `return *impl_->fonts;` 且签名改回引用：本文件**编译不过**——
/// 那也是一种红，但对「测试能不能抓住缺陷」没有说服力。所以这里断言的是
/// **指针语义**：回退到「返回引用」必须同时改签名，那会让 ①② 两条断言里的
/// `== nullptr` 比较无法成立。真正的护栏见 `tools/window_chrome_reverse_verify.py`
/// 同族脚本的思路：改坏判据、看用例变红。

#include <memory>
#include <string>

#include "st/app/app.hpp"
#include "st/core/log.hpp"
#include "st/test/test.hpp"

namespace {

using st::app::AppOptions;
using st::app::Application;

/// 无头 + 不跑帧：只测「字体装载时机」，不牵涉窗口与呈现。
[[nodiscard]] auto headless_options() -> AppOptions {
  AppOptions options;
  options.headless = true;
  options.max_frames = 1;
  options.defer_gpu = false;
  options.renderer = "software";
  return options;
}

}  // namespace

/// ① 构造之后、`start()` 之前：字体栈必须**如实为空**。
///
/// 这条直接钉住“启动即崩”那个缺陷：它要求这个状态是**可观察的 nullptr**，
/// 而不是一次解引用断言。
ST_TEST(app_font_stack_is_null_before_start) {
  Application app("font-stack-probe", "0.1.0", headless_options());
  ST_CHECK(app.font_stack() == nullptr);
  st::log::out("[font-stack] 构造后取字体栈：{}（必须为空）\n",
               app.font_stack() == nullptr ? "nullptr" : "NON-NULL");
}

/// ② `start()` 之后：字体栈必须就位——否则“提前取是空的”会被实现成“永远为空”。
ST_TEST(app_font_stack_is_ready_after_start) {
  Application app("font-stack-probe", "0.1.0", headless_options());
  const auto started = app.start();
  ST_REQUIRE(started.has_value());
  const auto* stack = app.font_stack();
  ST_REQUIRE(stack != nullptr);
  // 真的能用：至少有一个正文 face（`system_default` 拿不到字体时 `start()` 就该失败）。
  const auto& faces = stack->faces();
  ST_CHECK(!faces.empty());
  st::log::out("[font-stack] start 之后：{} 个 face + {} 个等宽 face\n", faces.size(),
               stack->monospace_faces().size());
}

/// ③ 两者之间的**顺序依赖**本身就是契约：同一个对象，先空后有。
///
/// 单看 ①② 两条，有人可能把它“修”成「构造时就装载字体，于是永远非空」——
/// 那也能让两条都过。这条把时序写死：**同一个实例**在 `start()` 前后必须**不同**。
ST_TEST(app_font_stack_transitions_at_start) {
  Application app("font-stack-probe", "0.1.0", headless_options());
  const bool before = app.font_stack() == nullptr;
  const auto started = app.start();
  ST_REQUIRE(started.has_value());
  const bool after = app.font_stack() != nullptr;
  ST_CHECK(before);
  ST_CHECK(after);
  st::log::out("[font-stack] 时序：start 前为空={} → start 后就位={}\n", before, after);
}
