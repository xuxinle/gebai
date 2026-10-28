/// 「窗口先用软件画布秒开，后台备好 GPU 再升级」的**判据回归**。
///
/// ## 为什么需要这组用例
///
/// 这条能力有一个**静默失效**的形态：`defer_gpu` 明明写着 `true`，但
/// `AppOptions::renderer` 被填了 `"auto"` 时，后端仍会把它当成“显式指定渲染器”，
/// 于是 GPU 初始化又回到窗口露出**之前**的同步路径上——启动还是 500 ms，
/// 而代码看起来“开关是开的”。
///
/// 实测就踩过：相期的 `Options` 把 `renderer` 默认写成 `"auto"`，
/// 于是 `defer_gpu` 全套逻辑一行都没生效（窗口仍 530 ms），
/// 而日志里没有任何一条能看出“开关被静默关掉了”。
///
/// ## 判据为什么是这两个组合
///
/// 三种输入（空 / `auto` / `gpu`/`software`）对应两种语义：
/// - 空 = “跟框架缺省走” → 允许延迟升级；
/// - `auto` = 显式写出来的 auto → 也算显式（用户的写法意图明确）；
/// - `gpu` / `software` = 绝对明确 → 绝不能被升级逻辑改动。
///
/// 后两条都必须把延迟升级**关掉**，否则 `--renderer software` 的基准会
/// 在 390 ms 后偷偷变成 GPU，数据不可比。
///
/// ## 这组用例为什么在无头下也能跑
///
/// 真正的升级动作需要真实窗口（`CreateSwapChain`），无头下建不出 presenter，
/// 所以**升级本身**无法在这里验（见 `docs/BACKLOG.md` P1 那条）。
/// 但「开关该不该生效」是纯判据，正是最容易错的那一步，它完全可以测。

#include "st/test/test.hpp"

#include "st/app/app.hpp"
#include "st/core/print.hpp"
#include "st/core/log.hpp"

namespace {

using st::app::AppOptions;

}  // namespace

/// 默认（未指定渲染器）：延迟升级**开着**——这是“窗口秒开”的默认形态。
ST_TEST(defer_gpu_is_on_by_default) {
  const AppOptions options;
  ST_CHECK(options.defer_gpu);
  ST_CHECK(options.renderer.empty());
  ST_CHECK(options.defer_gpu_effective());
  st::log::out("[defer-gpu] 默认：defer_gpu={} renderer='{}' → 生效={}\n", options.defer_gpu,
            options.renderer, options.defer_gpu_effective());
}

/// 显式 `--renderer gpu`：延迟升级必须**关掉**（用户要的就是一开始就用 GPU）。
ST_TEST(defer_gpu_is_off_when_renderer_is_explicitly_gpu) {
  AppOptions options;
  options.renderer = "gpu";
  ST_CHECK(options.defer_gpu);            // 字段本身仍是默认 true
  ST_CHECK(!options.defer_gpu_effective());  // 但判据为假
  st::log::out("[defer-gpu] renderer=gpu → 生效={}（必须为 0）\n",
            options.defer_gpu_effective());
}

/// 显式 `--renderer software`：同理必须关掉。
///
/// 这条尤其重要：若它生效，`software` 基准跑 390 ms 后会被偷偷升级成 GPU，
/// 于是“软件渲染的启动耗时/帧耗时”全是假的。
ST_TEST(defer_gpu_is_off_when_renderer_is_explicitly_software) {
  AppOptions options;
  options.renderer = "software";
  ST_CHECK(!options.defer_gpu_effective());
  st::log::out("[defer-gpu] renderer=software → 生效={}（必须为 0）\n",
            options.defer_gpu_effective());
}

/// 显式写成 `auto` 也算“显式指定”——它同样必须关掉。
///
/// 这一条是**实测踩到的那个坑**：相期把 `renderer` 默认写成 `"auto"`，
/// 于是 `defer_gpu` 静默失效、窗口仍要 530 ms。判据必须把 `auto` 归到“显式”这一边，
/// 否则“开关开着但不生效”会变成一个没有任何日志提示的哑弹。
ST_TEST(defer_gpu_is_off_when_renderer_is_explicitly_auto) {
  AppOptions options;
  options.renderer = "auto";
  ST_CHECK(!options.defer_gpu_effective());
  st::log::out("[defer-gpu] renderer=auto → 生效={}（显式 auto 也算指定，必须为 0）\n",
            options.defer_gpu_effective());
}

/// 用户显式关掉 `defer_gpu`（`--no-defer-gpu` 之类）：即使渲染器为空也不生效。
ST_TEST(defer_gpu_flag_can_be_turned_off_explicitly) {
  AppOptions options;
  options.defer_gpu = false;
  ST_CHECK(options.renderer.empty());
  ST_CHECK(!options.defer_gpu_effective());
  st::log::out("[defer-gpu] defer_gpu=false → 生效={}（必须为 0）\n",
            options.defer_gpu_effective());
}
