#pragma once

/// **取色器**（`ColorPicker`）：色相条 + 饱和度/明度块 + 十六进制输入 + 吸管。
///
/// ## 为什么值得在框架层
///
/// 镂月裁云（`examples/louyue`）先写过一份示例级自绘的 HSV 三滑杆 + 十六进制标签，
/// 取色逻辑也在应用侧。而「挑一个颜色」是**任何创作类应用都要**的能力
/// （图像、视频、主题编辑、图表配色），却没有两个应用共享过它。
///
/// ## 交互为什么用 HSV 而不是 RGB
///
/// - RGB 三滑杆上「我要一个暗一点的蓝」要同时动两三个滑杆，而 HSV 里就是 V 一条；
/// - 「同色系变亮/变暗」在 HSV 是沿一条轴走，在 RGB 里是一条斜线——滑杆无法表达；
/// - 色相是**环形**的，HSV 让它在一条条上线性铺开。
///
/// 存的是 `math::Color`（RGB），HSV 只是交互用的中间量：**真值源是 RGB**——
/// 这样 `set_color` 与 `color()` 之间不会出现「来回换算掉精度」的问题
/// （灰阶在 HSV 里 H 无定义，来回换会把 H 冲掉，颜色的"身份"就变了）。
///
/// ## 十六进制输入的契约
///
/// 只认 `#RRGGBB` 与 `#RRGGBBAA`（大小写均可，`#` 可省）。**解析失败如实拒绝并保留原值**
/// ——静默兜底成黑色是这类控件最糟的失败方式：用户以为设上了，画面却是黑的，
/// 而两者之间没有任何提示。
///
/// 不用 `math::Color::from_hex`：它**按数值大小猜格式**，带前导零的 `RRGGBBAA`
/// 会被当成 RGB（`#0000008C` 会变成纯蓝不透明——`color.hpp` 的注释里记着这个坑）。

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/math/color.hpp"
#include "st/math/geometry.hpp"
#include "st/ui/element.hpp"

namespace st::ui {

class ColorPicker : public Element {
 public:
  /// 组件缺省尺寸（SV 块 128 + 色相条 16 + 间隙与输入行）。
  static constexpr float kDefaultWidth{168.0f};
  static constexpr float kDefaultHeight{196.0f};
  static constexpr float kHueBarHeight{14.0f};
  static constexpr float kHexRowHeight{26.0f};
  static constexpr float kGap{6.0f};
  static constexpr float kSwatchSize{26.0f};

  ColorPicker();

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "ColorPicker"; }
  [[nodiscard]] auto role() const noexcept -> Role override { return Role::Slider; }

  // —— 颜色 ——
  /// 设颜色（`notify = false` 时**不**触发 `on_change`）。
  ///
  /// 为何要这个开关：宿主「用程序把当前颜色同步进控件」时不该收到回调——
  /// 否则会与用户操作混在一起（表现为「设一次颜色触发了两次应用逻辑」）。
  void set_color(math::Color color, bool notify = false);
  [[nodiscard]] auto color() const noexcept -> math::Color { return color_; }

  /// 颜色变化回调（用户操作触发的；程序 `set_color(..., false)` 不触发）。
  std::function<void(math::Color)> on_change{};

  // —— HSV（交互用的中间量；真值源仍是 RGB）——
  [[nodiscard]] auto hue() const noexcept -> float { return hue_; }
  [[nodiscard]] auto saturation() const noexcept -> float { return saturation_; }
  [[nodiscard]] auto value() const noexcept -> float { return value_; }
  /// 直接设 HSV（如「色相环」外部控件联动）。会更新 RGB 并触发回调。
  void set_hsv(float hue, float saturation, float value, bool notify = true);

  // —— 吸管 ——
  /// 取色源：给一个屏幕坐标，返回那里的颜色（`nullopt` = 该处取不到）。
  ///
  /// 为何由宿主注入而不是组件自己去截屏：截屏是**平台能力**（`desktop`/后台才有），
  /// 而组件在无头环境里也要能工作。没设源时吸管按钮**如实禁用**（不假装可用）。
  void set_eyedropper_source(std::function<std::optional<math::Color>(math::Point)> source) {
    eyedropper_ = std::move(source);
  }
  /// 吸管是否可用（有源即为真）。宿主可据此决定要不要显示这个按钮。
  [[nodiscard]] auto has_eyedropper() const noexcept -> bool {
    return static_cast<bool>(eyedropper_);
  }

  // —— 十六进制文本（可读；输入框里显示的就是它）——
  [[nodiscard]] auto hex_text() const -> std::string;
  /// 尝试用十六进制文本设色。失败返回 `false` 且**保留原值**（见文件头）。
  auto set_hex_text(std::string_view text) -> bool;
  /// 输入框内容是否有解析错误（宿主可据此标红）。
  [[nodiscard]] auto hex_invalid() const noexcept -> bool { return hex_invalid_; }

  void apply_theme(const Theme& theme) override;
  void measure(const RenderContext& context, const Constraints& constraints) override;
  void arrange(const RenderContext& context, math::Rect rect) override;
  void paint_content(const RenderContext& context, raster::Surface& canvas) const override;
  auto on_event(const RenderContext& context, Event& event) -> bool override;

  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  [[nodiscard]] auto invoke_action(std::string_view action,
                                   std::string_view argument) -> bool override;
  [[nodiscard]] auto semantics_value() const -> std::string override { return hex_text(); }

  // —— 几何（命中与绘制共用同一份；避免"测试复刻布局公式"的假绿）——
  [[nodiscard]] auto hue_bar_rect() const -> math::Rect;
  [[nodiscard]] auto sv_square_rect() const -> math::Rect;
  [[nodiscard]] auto swatch_rect() const -> math::Rect;
  [[nodiscard]] auto hex_rect() const -> math::Rect;
  [[nodiscard]] auto eyedropper_rect() const -> math::Rect;

 private:
  /// HSV → RGB（`math::Color` 只有 `from_hsl`，没有 HSV 版本）。
  [[nodiscard]] static auto hsv_to_rgb(float hue, float saturation, float value) -> math::Color;
  /// RGB → HSV。返回的 hue 是 `[0,360)`（灰阶时返回**上一次的 hue**，
  /// 因为灰阶的色相在数学上无定义）。
  void rgb_to_hsv(math::Color color);
  /// 缓存 `color_` 到 HSV（并保留灰阶时的旧色相）。
  void sync_from_color();
  [[nodiscard]] auto current_hex_valid() const -> bool;

  math::Color color_{math::Color::rgb(0, 0, 0)};
  float hue_{0.0f};
  float saturation_{0.0f};
  float value_{0.0f};

  /// 十六进制输入框的**编辑中**文本（与 `color_` 可能暂时不一致——
  /// 用户敲到一半时不该把颜色跟着改）。
  std::string hex_buffer_{};
  bool hex_invalid_{false};

  std::function<std::optional<math::Color>(math::Point)> eyedropper_{};

  /// 拖拽中的部件（一次只可能拖一个）。
  enum class Drag : std::uint8_t { None, Hue, Sv };
  Drag dragging_{Drag::None};

  math::Color surface_{};
  math::Color border_{};
  math::Color text_color_{};
  math::Color text_muted_{};
  math::Color focus_ring_{};
  float font_size_{13.0f};
};

}  // namespace st::ui
