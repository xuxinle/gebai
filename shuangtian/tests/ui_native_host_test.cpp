// `NativeHost`（原生子窗口嵌入）的回归测试。
//
// ## 这些用例在钉什么
//
// 本组件的能力是**可选**的：大部分环境（无头 / 未实现平台 / 测试桩）都没有后端支持。
// 所以真正需要钉死的第一条不变量不是"能嵌入"，而是：
//
//   **不可用时，界面其余部分完全不受影响。**
//
// 这条比"能用"更重要——嵌入能力失败的场景远多于成功的场景（CI 无头、Linux 无后端、
// 外部程序没起来…），而它一旦让布局塌了/抛异常/崩溃，整个应用就废了。
//
// 其余用例钉宿主契约：几何下发、去重（避免每帧 SetWindowPos）、外部窗口消失后的收敛、
// 以及"占位块确实被画出来"（否则用户会以为组件没渲染）。
//
// ## 为什么不用像素断言
//
// 嵌入内容的像素**不进 `framebuffer`**（由平台合成器上屏），`capture` 拍不到。
// 因此这里全部走**属性面与调用记录**：桩宿主记录被调用的入参，断言其正确性。

#include "st/test/test.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <optional>
#include <memory>
#include <string>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/ui/components/basic.hpp"
#include "st/ui/components/native_host.hpp"
#include "st/ui/element.hpp"
#include "st/ui/ui_root.hpp"

#include "tests/support/text_port_fixtures.hpp"

using st::math::Rect;
using st::math::Size;
using st::ui::Element;
using st::ui::NativeHost;

namespace {

/// 假的外部窗口句柄。
///
/// 为什么用一个"不是真指针但非空"的值而不是 `nullptr`：被测代码只把它当**不透明
/// 句柄**原样透传，从不解引用。用固定值能让断言直接比对"传下去的到底是不是同一个"。
///
/// 用局部函数而不是 `constexpr` 全局量：`reinterpret_cast` 不能出现在常量表达式里，
/// 而地址只要**同一个值**就够，是不是编译期常量无关。
constexpr std::uintptr_t kFakeChildAddr = 0x1234;
constexpr std::uintptr_t kFakeChild2Addr = 0x5678;

[[nodiscard]] auto fake_child() -> void* {
  // 测试桩刻意构造一个“非空但不解引用”的不透明句柄值：被测契约就是 `void*` 只透传。
  return reinterpret_cast<void*>(kFakeChildAddr);  // lint-allow: L6 不透明句柄桩
}
[[nodiscard]] auto fake_child2() -> void* {
  return reinterpret_cast<void*>(kFakeChild2Addr);  // lint-allow: L6 不透明句柄桩
}

/// `Rect` 没有 `operator==`（浮点矩形按位比较本来就不该当默认语义），
/// 测试里用容差比较：坐标是 `float`，`0.01` 在逻辑像素尺度上远小于一个像素。
[[nodiscard]] auto rect_near(Rect a, Rect b) -> bool {
  constexpr float kEpsilon = 0.01F;
  return std::abs(a.x - b.x) < kEpsilon && std::abs(a.y - b.y) < kEpsilon &&
         std::abs(a.width - b.width) < kEpsilon && std::abs(a.height - b.height) < kEpsilon;
}

/// 可编程的宿主桩：记录所有原生子窗口调用。
///
/// ## 桩必须复现被测的那一段真实行为（CONVENTIONS.md §7.2）
///
/// 这里最容易写成"永远返回 true"的假桩——那样"组件有没有真的检查返回值"就测不出来了。
/// 所以 `supports` / `adopt_ok` / `attached` 都是**可编程**的，各用例自己摆。
struct HostStub final : Element::HostFocus {
  bool supports{false};
  bool adopt_ok{true};
  /// 每次 `host_child_view_probe` 返回的 attached（模拟外部窗口还在/没了）。
  bool attached{true};
  /// 模拟“外部窗口重排是异步的”：每次 `sync` 把内容矩形往里推一格，直到与宿主一致。
  ///
  /// 为何必须能模拟：被测的收敛逻辑就是为了对付这件事。桩若一上来就返回
  /// “已就位”的值，那么「有没有收敛」「是否请求下一帧」都测不出来
  ///（CONVENTIONS.md §7.2：桩把差异抹平了）。
  int reflow_steps{0};
  /// 每次 probe 时内容相对宿主的偏移（由 `reflow_steps` 递减到 0）。
  Element::ChildViewProbe probe_state{};

  std::vector<void*> adopted{};
  std::vector<Rect> adopted_bounds{};
  std::vector<void*> synced{};
  std::vector<Rect> synced_bounds{};
  /// 每次 sync 下发的**可见区**（`NativeHost` 得保证它随隐藏/裁剪一起变）。
  std::vector<Rect> synced_clips{};
  std::vector<void*> dismissed{};

  [[nodiscard]] auto set_keyboard_focus(Element*) -> bool override { return false; }

  [[nodiscard]] auto host_supports_child_view() const -> bool override { return supports; }

  auto host_adopt_child_view(void* child, Rect bounds) -> bool override {
    adopted.push_back(child);
    adopted_bounds.push_back(bounds);
    if (!adopt_ok) return false;
    // 真实后端嵌入成功后 probe 会看到 attached=true。
    attached = true;
    if (reflow_steps > 0) {
      // 只有显式开启“异步重排”的用例才摆出错位初值（否则与真实后端的
      // “嵌好就已就位”情形不符），避免把默认行为搞成“永远要收敛”。
      probe_state = {};
      probe_state.attached = true;
      probe_state.title_bar_height = 107.0F;
      probe_state.content_y = -107.0F;
      probe_state.content_width = 2304.0F;
      probe_state.content_height = 1024.0F;
    } else {
      // 默认：嵌上就已经就位（同步后端的形态）。尺寸也必须摆对，
      // 否则被测代码的 `content_fits()` 会把它当成“还没重排完”（假报警）。
      probe_state = {};
      probe_state.attached = true;
      probe_state.content_x = 0.0F;
      probe_state.content_y = 0.0F;
      probe_state.content_width = bounds.width;
      probe_state.content_height = bounds.height;
    }
    probe_state.host_width = bounds.width;
    probe_state.host_height = bounds.height;
    return true;
  }

  void host_sync_child_view(void* child, Rect bounds, Rect clip) override {
    synced.push_back(child);
    synced_bounds.push_back(bounds);
    synced_clips.push_back(clip);
    // 宿主尺寸**立即**变成新矩形（真实后端确实如此：宿主是我们自己的窗口，
    // `SetWindowPos` 当场生效）；而内容子窗口要等外部程序重排，那一段才是滞后的。
    // 桩若不这么分，`content_fits()` 就会拿“旧宿主 vs 旧内容”判定为已就位，
    // 收敛永远不会开始（实测：那一版的 resize 用例假绿）。
    probe_state.host_width = bounds.width;
    probe_state.host_height = bounds.height;
    if (reflow_steps > 0) {
      --reflow_steps;
      return;   // 内容还是旧值（滞后）
    }
    // 重排完成：内容落在 (0,0) 且尺寸等于宿主。
    probe_state.attached = true;
    probe_state.content_x = 0.0F;
    probe_state.content_y = 0.0F;
    probe_state.content_width = bounds.width;
    probe_state.content_height = bounds.height;
  }

  void host_dismiss_child_view(void* child) override {
    dismissed.push_back(child);
    attached = false;
    probe_state = {};
  }

  [[nodiscard]] auto host_child_view_probe() const -> Element::ChildViewProbe override {
    Element::ChildViewProbe probe;
    probe.attached = attached;
    if (!attached) return probe;
    probe = probe_state;
    probe.attached = true;
    return probe;
  }
};

/// 测试夹具：一个 UiRoot + 一块 NativeHost + 装在 root 上的**可编程宿主能力**。
///
/// ## 为什么把桩接在 `set_child_view_providers` 上，而不是自己 `set_host(&stub)`
///
/// `UiRoot::set_content` 会把**它自己**作为宿主契约写到整棵子树（真实的依赖方向：
/// 组件只认宿主，而宿主就是 UiRoot）。绕过它去 `set_host` 一个外部桩，测到的就不是
/// 生产路径了——而且实测会被重新覆盖，于是所有 attach 都莫名其妙地返回 false。
///
/// 所以这里走**和 `Application` 完全一样的那条路**：往 `UiRoot` 装 provider。
/// 这同时也验证了 provider 装配本身是通的（`Application` 里的那段代码同理生效）。
struct Fixture {
  /// 声明顺序刻意如此：C++ 按**逆序**析构，`stub` 必须比 `root` 活得久。
  ///
  /// 原因：`root` 析构会销毁内容树 → `NativeHost` 析构 → `detach()` → provider
  /// 回调 → 读写 `stub`。若 `stub` 先生销，这就是**访问已销毁对象**（实测表现：
  /// 进程以 `0xC0000374`（堆损坏）退出，没有断言、没有任何栈上部线索）。
  HostStub stub{};
  st::test::FixedAdvanceTextPort port{};
  st::ui::UiRoot root{};
  NativeHost* host{nullptr};

  explicit Fixture(bool supported = true) {
    stub.supports = supported;
    root.set_text_port(&port);
    root.set_viewport(Size{800.0F, 600.0F});

    // 与 `Application` 的装配同构：五个 provider 全部指向一个桩。
    root.set_child_view_providers(
        [this]() -> bool { return stub.host_supports_child_view(); },
        [this](void* child, Rect bounds) -> bool {
          return stub.host_adopt_child_view(child, bounds);
        },
        [this](void* child, Rect bounds, Rect clip) { stub.host_sync_child_view(child, bounds, clip); },
        [this](void* child) { stub.host_dismiss_child_view(child); },
        [this]() -> Element::ChildViewProbe { return stub.host_child_view_probe(); });

    auto panel = std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column);
    auto owned = std::make_unique<NativeHost>();
    host = owned.get();
    panel->add_child(std::move(owned));
    root.set_content(std::move(panel));
    root.layout(true);
  }
};

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 第一组：**不可用时不影响基础功能**（本组件最重要的契约）
// ─────────────────────────────────────────────────────────────────────────────

ST_TEST(native_host_without_backend_support_degrades_to_placeholder) {
  Fixture fx(/*supported=*/false);

  // attach 必须如实失败（不假装成功）。
  ST_CHECK(!fx.host->attach(fake_child()));
  ST_CHECK(!fx.host->attached());

  // 关键：**一次宿主调用都不该发生**——不支持时组件连试都不试。
  ST_CHECK(fx.stub.adopted.empty());
  ST_CHECK(fx.stub.synced.empty());

  // 布局必须照常完成：组件有尺寸、在正确位置。
  fx.root.layout(true);
  ST_CHECK(!fx.host->bounds().is_empty());
  ST_CHECK(fx.host->bounds().width == 800.0F);
}

ST_TEST(native_host_without_host_attached_fails_cleanly) {
  // 完全没有宿主（离屏渲染 / 未上树）：attach 返回 false，且不崩。
  NativeHost host;
  ST_CHECK(host.attach(fake_child()) == false);
  ST_CHECK(!host.attached());

  // 没有宿主时调 detach / 布局也必须安全（常态调用路径）。
  host.detach();  // lint-allow: L9 本类方法（解除嵌入），非 std::thread::detach
  st::test::FixedAdvanceTextPort port;
  st::ui::UiRoot root;
  root.set_text_port(&port);
  root.set_viewport(Size{400.0F, 300.0F});
  root.set_content(std::make_unique<NativeHost>());
  root.layout(true);
  ST_CHECK(root.content() != nullptr);
}

ST_TEST(native_host_failed_adopt_leaves_no_half_state) {
  Fixture fx(/*supported=*/true);
  fx.stub.adopt_ok = false;  // 后端支持但嵌入失败

  ST_CHECK(!fx.host->attach(fake_child()));
  ST_CHECK(!fx.host->attached());

  // 失败后**不能留下**本地"已嵌入"标志：再次 attach 应当重新尝试（而不是
  // 因为 `child_ == child` 走幂等捷径直接返回 true）。
  fx.stub.adopt_ok = true;
  ST_CHECK(fx.host->attach(fake_child()));
  ST_CHECK(fx.stub.adopted.size() == 2);
  ST_CHECK(fx.host->attached());
}

ST_TEST(native_host_null_child_is_rejected) {
  Fixture fx(/*supported=*/true);
  ST_CHECK(!fx.host->attach(nullptr));
  ST_CHECK(!fx.host->attached());
  ST_CHECK(fx.stub.adopted.empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// 第二组：宿主契约（几何下发 / 去重 / 还原）
// ─────────────────────────────────────────────────────────────────────────────

ST_TEST(native_host_adopt_carries_current_bounds) {
  Fixture fx(/*supported=*/true);
  fx.root.layout(true);
  const Rect expected = fx.host->bounds();
  ST_CHECK(!expected.is_empty());  // 前提：布局确实给了矩形，否则下面的断言没意义

  ST_CHECK(fx.host->attach(fake_child()));
  ST_CHECK(fx.stub.adopted.size() == 1);
  ST_CHECK(fx.stub.adopted[0] == fake_child());
  // 下发的矩形必须就是布局给出的那个（不多不少）。
  ST_CHECK(rect_near(fx.stub.adopted_bounds[0], expected));
}

ST_TEST(native_host_sync_is_skipped_when_bounds_unchanged) {
  Fixture fx(/*supported=*/true);
  fx.root.layout(true);
  ST_CHECK(fx.host->attach(fake_child()));
  // 基线取在**首帧布局之后**：`attach` 只带矩形，可见区要等第一次 `arrange` 才能算
  //（它依赖祖先链），所以那一帧一定会下发一次。这也正是“首帧必然同步”的契约。
  fx.root.layout(true);
  const std::size_t after_attach = fx.stub.synced.size();
  ST_CHECK(after_attach > 0U);

  // 布局没变 → 不该再下发 SetWindowPos（每帧下发会让嵌入内容持续闪）。
  fx.root.layout(true);
  fx.root.layout(true);
  ST_CHECK(fx.stub.synced.size() == after_attach);

  // 改视口 → 矩形变了 → 必须下发一次，且带的是新矩形。
  fx.root.set_viewport(Size{640.0F, 480.0F});
  fx.root.layout(true);
  ST_CHECK(fx.stub.synced.size() == after_attach + 1);
  ST_CHECK(fx.stub.synced.back() == fake_child());
  ST_CHECK(std::abs(fx.stub.synced_bounds.back().width - 640.0F) < 0.01F);
}

ST_TEST(native_host_detach_restores_and_clears_state) {
  Fixture fx(/*supported=*/true);
  fx.root.layout(true);
  ST_CHECK(fx.host->attach(fake_child()));
  ST_CHECK(fx.host->attached());

  fx.host->detach();
  // 必须真的把"还原"告诉宿主（后端据此 reparent 回桌面）：
  // 只清本地标志是不够的，外部窗口会挂在正在消失的窗口下变成孤儿。
  ST_CHECK(fx.stub.dismissed.size() == 1);
  ST_CHECK(fx.stub.dismissed[0] == fake_child());
  ST_CHECK(!fx.host->attached());

  // 重复 detach 是幂等的（不重复下发）。
  fx.host->detach();
  ST_CHECK(fx.stub.dismissed.size() == 1);
}

ST_TEST(native_host_destructor_detaches) {
  Fixture fx(/*supported=*/true);
  fx.root.layout(true);
  ST_CHECK(fx.host->attach(fake_child()));

  // 换掉整个内容树 → NativeHost 析构。析构里必须 detach（否则外部窗口变孤儿）。
  fx.root.set_content(std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column));
  ST_CHECK(fx.stub.dismissed.size() == 1);
  ST_CHECK(fx.stub.dismissed[0] == fake_child());
}

ST_TEST(native_host_attach_another_child_releases_previous) {
  Fixture fx(/*supported=*/true);
  fx.root.layout(true);
  ST_CHECK(fx.host->attach(fake_child()));

  // 换一个句柄：必须先还原旧的（否则旧窗口永久挂在宿主下）。
  fx.host->attach(fake_child2());
  ST_CHECK(fx.stub.dismissed.size() == 1);
  ST_CHECK(fx.stub.dismissed[0] == fake_child());
  ST_CHECK(fx.stub.adopted.back() == fake_child2());
}

ST_TEST(native_host_attach_same_child_is_idempotent) {
  Fixture fx(/*supported=*/true);
  fx.root.layout(true);
  ST_CHECK(fx.host->attach(fake_child()));
  const std::size_t after_first = fx.stub.adopted.size();

  // 同一个句柄重复 attach：不该重复 adopt（否则会重复建宿主窗口）。
  ST_CHECK(fx.host->attach(fake_child()));
  ST_CHECK(fx.stub.adopted.size() == after_first);
  ST_CHECK(fx.stub.dismissed.empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// 第三组：外部窗口消失后的收敛（这是 capture 看不到、只能靠属性面发现的场景）
// ─────────────────────────────────────────────────────────────────────────────

ST_TEST(native_host_converges_when_external_window_disappears) {
  Fixture fx(/*supported=*/true);
  fx.root.layout(true);
  ST_CHECK(fx.host->attach(fake_child()));
  ST_CHECK(fx.host->attached());

  // 外部程序退出：后端读数变 false。
  fx.stub.attached = false;
  ST_CHECK(!fx.host->attached());

  // 下一次布局必须**自我收敛**（清掉本地状态，回到占位块），而不是每帧反复尝试同步。
  fx.root.set_viewport(Size{700.0F, 500.0F});
  fx.root.layout(true);
  ST_CHECK(!fx.host->attached());
  // 收敛后不该再对失效句柄下发 sync。
  ST_CHECK(fx.stub.synced.empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// 第四组：占位块的可见性（"组件看起来没渲染"是最容易被误判成缺陷的状态）
// ─────────────────────────────────────────────────────────────────────────────

ST_TEST(native_host_placeholder_is_painted_when_unattached) {
  Fixture fx(/*supported=*/false);
  fx.root.layout(true);

  // 用真实画布渲染，断言那块矩形**确实有像素**（不是留空）。
  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(800, 600, 1.0F);
  canvas.clear(st::math::Color{255, 255, 255, 255});
  fx.root.paint(canvas);

  // 取组件矩形中心的一个像素：未嵌入时必须被占位底色覆盖（不等于纯白）。
  const Rect box = fx.host->bounds();
  ST_CHECK(!box.is_empty());
  const int px = static_cast<int>(box.center().x);
  const int py = static_cast<int>(box.center().y);
  const auto color = canvas.pixel_at(px, py);
  const bool is_white = color.r > 250 && color.g > 250 && color.b > 250;
  ST_CHECK(!is_white);
}

ST_TEST(native_host_placeholder_can_be_disabled) {
  Fixture fx(/*supported=*/false);
  fx.host->set_placeholder_visible(false);
  fx.root.layout(true);

  st::raster::Canvas canvas = st::raster::Canvas::for_logical_size(800, 600, 1.0F);
  canvas.clear(st::math::Color{255, 255, 255, 255});
  fx.root.paint(canvas);

  // 关掉占位块 = 这块区域留空（保持背景白）。
  const Rect box = fx.host->bounds();
  const auto color = canvas.pixel_at(static_cast<int>(box.center().x),
                                     static_cast<int>(box.center().y));
  ST_CHECK(color.r > 250 && color.g > 250 && color.b > 250);
}

// ─────────────────────────────────────────────────────────────────────────────
// 第五组：外部窗口“重排是异步的”引起的那类错位
// ─────────────────────────────────────────────────────────────────────────────
//
// 这一组的来历：实测把浏览器嵌进 gallery 后，属性面读数显示
// `content_y = -107`、`content_height = 1024` 而 `host_height = 525`——
// 内容被顶出去 107px、底部溢出近百像素。根因不是布局算错，而是**量得太早**：
// 外部窗口刚被 reparent/resize 时还没重排，此时量到的偏移是旧布局的值。

ST_TEST(native_host_keeps_refining_until_content_fits) {
  Fixture fx(/*supported=*/true);
  fx.stub.reflow_steps = 3;   // 外部窗口要 3 次同步才把内容摆到位
  fx.root.layout(true);
  ST_CHECK(fx.host->attach(fake_child()));

  // 刚嵌入：读数还是错的（内容偏 -107、尺寸也不对）——**不假装正常**。
  ST_CHECK(fx.host->child_view_probe().content_y < 0.0F);

  const std::size_t after_adopt = fx.stub.synced.size();
  // 后续几帧会自己重测重同步；每帧一次，直到读数到位。
  for (int frame = 0; frame < 8; ++frame) fx.root.layout(true);

  ST_CHECK(fx.stub.synced.size() > after_adopt);   // 真的重试了
  const auto final_probe = fx.host->child_view_probe();
  ST_CHECK(std::abs(final_probe.content_y) < 1.0F);
  ST_CHECK(std::abs(final_probe.content_height - final_probe.host_height) < 1.0F);
}

ST_TEST(native_host_stops_refining_once_content_fits) {
  // 收敛必须**停下**：嵌入物稳定后还每帧 SetWindowPos = 持续闪烁 + 白耗。
  //
  // ⚠ `reflow_steps` 不能为 0：那样桩一开始就“已就位”，收敛路径根本进不去，
  // 本用例会因为“从未开始”而假绿（实测踩到）。必须让它**真的跑一段**再停下。
  Fixture fx(/*supported=*/true);
  fx.stub.reflow_steps = 2;
  fx.root.layout(true);
  ST_CHECK(fx.host->attach(fake_child()));

  // 跑足够多帧让它收敛（每次 sync 消耗一格 reflow_steps）。
  for (int frame = 0; frame < 8; ++frame) fx.root.layout(true);
  ST_CHECK(fx.host->child_view_probe().content_height > 0.0F);   // 前提：真的嵌上了
  const std::size_t settled = fx.stub.synced.size();
  ST_CHECK(settled > 0U);   // 前提：收敛真的跑过（否则下面的断言恒成立）

  // 再跑若干帧：不该再有新增同步。
  for (int frame = 0; frame < 8; ++frame) fx.root.layout(true);
  ST_CHECK(fx.stub.synced.size() == settled);
}

ST_TEST(native_host_does_not_sync_when_already_placed) {
  // 不变量：**已就位时收敛不下发任何东西**。
  //
  // 为何单独一条：“稳定后不再同步”那条只能证明“帧数用完了”；
  // 而“已就位却还多发一次同步”会在帧数耗尽前被掩盖（实测：变异体仍绿）。
  // 这里直接盯住那个边界：找到“首次就位”的那一帧，下一帧不该再有同步。
  Fixture fx(/*supported=*/true);
  fx.stub.reflow_steps = 1;   // 嵌上时可视为旧布局，需要一次同步才就位
  fx.root.layout(true);
  ST_CHECK(fx.host->attach(fake_child()));

  // 逐帧跑，找“内容刚好就位”的那一刻。
  std::size_t synced_when_placed = 0;
  bool found = false;
  for (int frame = 0; frame < 10; ++frame) {
    fx.root.layout(true);
    const auto probe = fx.host->child_view_probe();
    if (probe.attached && std::abs(probe.content_height - probe.host_height) < 1.0F) {
      synced_when_placed = fx.stub.synced.size();
      found = true;
      break;
    }
  }
  ST_CHECK(found);
  ST_CHECK(synced_when_placed > 0U);   // 前提：确实同步过（否则断言恒成立）

  // 已就位之后的下一帧：不该再下发同步。
  fx.root.layout(true);
  ST_CHECK(fx.stub.synced.size() == synced_when_placed);
}

ST_TEST(native_host_resize_restarts_refining) {
  // 改尺寸这条路径必须也能触发收敛：`sync_bounds` 里判“没到位就重新收敛”。
  // 这条单独一个用例，因为 `attach` 那条路径有自己的触发点——只靠 attach
  // 起收敛的话，删掉 sync_bounds 里的判据也测不出来（实测：确实是假绿）。
  Fixture fx(/*supported=*/true);
  fx.root.layout(true);
  ST_CHECK(fx.host->attach(fake_child()));
  for (int frame = 0; frame < 4; ++frame) fx.root.layout(true);   // 先收敛到稳态

  // 改视口：后端这一轮又会“重排滞后”（用 reflow_steps 模拟），
  // 而矩形变化本身必须足以触发新的收敛。
  fx.stub.reflow_steps = 3;
  fx.root.set_viewport(Size{640.0F, 480.0F});
  fx.root.layout(true);   // 这一帧下发新矩形，但读数还不准
  const std::size_t after_resize = fx.stub.synced.size();

  for (int frame = 0; frame < 10; ++frame) fx.root.layout(true);
  ST_CHECK(fx.stub.synced.size() > after_resize);   // 后续帧确实在收敛
  const auto probe = fx.host->child_view_probe();
  ST_CHECK(std::abs(probe.content_width - probe.host_width) < 1.0F);
}

ST_TEST(native_host_refine_survives_disappearing_window) {
  // 收敛中外部窗口没了：必须**安静停下**（不是继续对失效句柄下发 sync）。
  Fixture fx(/*supported=*/true);
  fx.stub.reflow_steps = 5;
  fx.root.layout(true);
  ST_CHECK(fx.host->attach(fake_child()));
  fx.root.layout(true);

  fx.stub.attached = false;   // 外部程序退出
  const std::size_t before = fx.stub.synced.size();
  for (int frame = 0; frame < 6; ++frame) fx.root.layout(true);
  ST_CHECK(fx.stub.synced.size() == before);
  ST_CHECK(!fx.host->attached());
}

// ─────────────────────────────────────────────────────────────────────────────
// 第六组：属性面（嵌入内容不进 capture，这是唯一的机读通道）
// ─────────────────────────────────────────────────────────────────────────────

ST_TEST(native_host_exposes_probe_readings_as_properties) {
  Fixture fx(/*supported=*/true);
  fx.root.layout(true);
  ST_CHECK(fx.host->attach(fake_child()));
  for (int frame = 0; frame < 6; ++frame) fx.root.layout(true);

  // 名字必须出现在只读属性表里（否则 `get` 遍不到，控制通道读不到）。
  const auto names = fx.host->readonly_property_names();
  for (const std::string_view expected : {"attached", "content_x", "content_y",
                                          "content_width", "content_height", "host_width",
                                          "host_height", "title_bar_height"}) {
    ST_CHECK(std::ranges::find(names, expected) != names.end());
  }
  // 值必须与组件自己的读数**同口径**（两处各算一遍必然漂）。
  ST_CHECK(fx.host->get_property("attached") == std::optional<std::string>("true"));
  const auto probe = fx.host->child_view_probe();
  ST_CHECK(fx.host->get_property("host_height") ==
           std::optional<std::string>(std::format("{:.1f}", probe.host_height)));
}

ST_TEST(native_host_properties_report_unattached_honestly) {
  Fixture fx(/*supported=*/false);   // 后端不支持：占位块状态
  fx.root.layout(true);
  ST_CHECK(fx.host->get_property("attached") == std::optional<std::string>("false"));
  ST_CHECK(fx.host->get_property("content_y") == std::optional<std::string>("0.0"));
}

// ─────────────────────────────────────────────────────────────────────────────
// 第七组：**嵌入物必须遵守可见性与裁剪**（平台子窗口不认 UI 树，厂厂会漏）
// ─────────────────────────────────────────────────────────────────────────────
//
// 这组覆的是两个实测缺陷：切到别的页面后，嵌入的网页仍盖在新页面上；
// 把嵌入区滚出视口，它也不消失。根因同一条——平台子窗口由合成器直接上屏，
// 既不看 `visible()` 也不走 `paint` 的 clip 核。

ST_TEST(native_host_hiding_ancestor_pushes_empty_clip) {
  Fixture fx(/*supported=*/true);
  fx.root.layout(true);
  ST_CHECK(fx.host->attach(fake_child()));
  fx.root.layout(true);
  const std::size_t before = fx.stub.synced.size();

  // 隐藏**整个内容树**（等价于 gallery 的切页）：组件自己不动，但它不可见了。
  ST_CHECK(fx.root.content() != nullptr);
  fx.root.content()->set_visible(false);

  // 必须**主动**下发一次同步，而且可见区是空的——否则后端不知道要藏。
  ST_CHECK(fx.stub.synced.size() > before);
  ST_CHECK(fx.stub.synced_clips.back().is_empty());
  ST_CHECK(fx.stub.synced_bounds.back().width > 0.0F);   // 前提：自身矩形没变
  ST_CHECK(fx.host->get_property("shown") == std::optional<std::string>("false"));

  // 重新可见：必须再下发一次非空可见区（否则“切回来网页不见了”）。
  fx.root.content()->set_visible(true);
  ST_CHECK(!fx.stub.synced_clips.empty());
  ST_CHECK(!fx.stub.synced_clips.back().is_empty());
}

ST_TEST(native_host_clip_is_ancestor_scroll_view_not_own_bounds) {
  // 可见区必须是**祖先 `clip_children` 与本元素矩形的交集**，而不是自身矩形。
  // 否则“滚出视口”完全测不出来（自身矩形根本没变）。
  Fixture fx(/*supported=*/true);
  fx.root.layout(true);
  ST_CHECK(fx.host->attach(fake_child()));
  fx.root.layout(true);

  // 在宿主与根之间插一个“带裁剪的视口”并把它的矩形改小（模拟滚动容器）。
  // 不真搭 ScrollView：这里测的是**可见区计算口径**，不是滚动行为。
  st::ui::Panel* parent = dynamic_cast<st::ui::Panel*>(fx.host->parent());
  ST_CHECK(parent != nullptr);
  parent->style().clip_children = true;
  const st::ui::RenderContext context{fx.root.theme(), &fx.port};
  parent->arrange(context, Rect{0.0F, 0.0F, 800.0F, 100.0F});
  const Rect visible = fx.host->visible_rect();
  ST_CHECK(!visible.is_empty());
  ST_CHECK(visible.height <= 100.0F + 0.01F);   // 被父级裁到 100 高

  // 把裁剪父级送出视口（高 0）：可见区必须为空，而不是“还是那块”。
  parent->arrange(context, Rect{0.0F, 0.0F, 800.0F, 0.0F});
  ST_CHECK(fx.host->visible_rect().is_empty());
}

ST_TEST(native_host_visible_rect_ignores_unclipped_ancestors) {
  // 反例：**不裁剪**的祖先不得硬裁子元素（凹陷/阴影/凸出标签都是合法用法）。
  Fixture fx(/*supported=*/true);
  fx.root.layout(true);
  st::ui::Panel* parent = dynamic_cast<st::ui::Panel*>(fx.host->parent());
  ST_CHECK(parent != nullptr);
  ST_CHECK(!parent->style().clip_children);   // 默认不裁
  const Rect own = fx.host->bounds();
  ST_CHECK(std::abs(own.width - 800.0F) < 0.01F);   // 前提：它原本铺满父级
  const st::ui::RenderContext context{fx.root.theme(), &fx.port};
  parent->arrange(context, Rect{0.0F, 0.0F, 10.0F, 10.0F});
  // 取**重排之后**的自身矩形：`arrange` 会把子元素重新摆进 10×10 里。
  const Rect after = fx.host->bounds();
  ST_CHECK(std::abs(after.width - 10.0F) < 0.01F);   // 前提：它确实被摆窄了
  const Rect visible = fx.host->visible_rect();
  ST_CHECK(std::abs(visible.width - after.width) < 0.01F);
  ST_CHECK(std::abs(visible.height - after.height) < 0.01F);
}
