#pragma once

/// 字体引擎 → UI 文本端口的适配层（依赖倒置：`ui` 层不认识 `text` 层）。
///
/// 独立成文件的原因：**测试与示例都需要真实的文本端口**（度量/绘制），若它藏在 `app.cpp` 的
/// 匿名命名空间里，任何脱离 `Application` 的组件测试就只能退化用 `NullTextPort`（宽度恒为 0），
/// 布局与命中相关断言全部失效。

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "st/text/text.hpp"
#include "st/ui/text_port.hpp"

namespace st::app {

/// 用 `text::TextRenderer` 驱动的文本端口。
///
/// DPI：入参一律**逻辑坐标**；`TextRenderer` 内部按画布 `device_scale` 以物理分辨率栅格化字形
/// （布局稳定、字形锐利），此处不做换算，避免重复缩放。
class RendererTextPort final : public ui::TextPort {
 public:
  explicit RendererTextPort(const text::TextRenderer& renderer) : renderer_(renderer) {}

  [[nodiscard]] auto measure(std::string_view utf8, float size) const -> math::Size override;
  [[nodiscard]] auto measure_width(
      std::string_view utf8, float size,
      text::FontRole role = text::FontRole::Proportional) const -> float override;
  [[nodiscard]] auto line_height(float size) const -> float override;
  [[nodiscard]] auto ascent(float size) const -> float override;
  [[nodiscard]] auto descent(float size) const -> float override;
  [[nodiscard]] auto shaped_ascent(std::string_view utf8, float size,
                                   text::FontRole role = text::FontRole::Proportional) const
      -> float override;
  [[nodiscard]] auto ink_metrics(std::string_view utf8, float size,
                                 text::FontRole role = text::FontRole::Proportional) const
      -> std::optional<InkMetrics> override;
  [[nodiscard]] auto has_real_bold() const -> bool override {
    return renderer_.stack().has_bold();
  }
  void draw(raster::Surface& canvas, std::string_view utf8, math::Point origin, float size,
            math::Color color, text::FontRole role = text::FontRole::Proportional,
            float embolden = 0.0f, bool bold = false) const override;
  [[nodiscard]] auto ellipsize(std::string_view utf8, float size, float max_width) const
      -> std::string override;
  [[nodiscard]] auto wrap(std::string_view utf8, float size, float max_width) const
      -> std::vector<std::string_view> override;
  [[nodiscard]] auto wrap_limited(std::string_view utf8, float size, float max_width,
                                  std::size_t max_lines) const -> std::vector<std::string> override;

 private:
  const text::TextRenderer& renderer_;
};

/// 便捷工厂。
[[nodiscard]] inline auto make_text_port(const text::TextRenderer& renderer)
    -> std::unique_ptr<ui::TextPort> {
  return std::make_unique<RendererTextPort>(renderer);
}

/// **隔离缓存**的文本端口：给后台离屏排版用（2026-10-19，实测驱动的框架修）。
///
/// ## 为什么必须隔离（这是「内容搬到后台、界面照样卡」的真根因）
///
/// `RendererTextPort` 直接转发到主线程那个 `TextRenderer`，共用同一份
/// **文本宽度 LRU 缓存**（上限 32k 条）。后台排版 447KB 文档要写十万级条目，
/// 每次插入都会把 LRU 淘汰到上限之下——把主线程正在用的条目**全部挤掉**。
/// 于是主线程下一帧绘制的每一段文字都要**重新整形**（逐码点 face 查找 + 字距 +
/// runs 分配）：实测一帧 `measure` 从 1.4ms 涨到 **35ms**（gbcode 切预览）。
///
/// 实测对照（`tools/lock_contention_probe.cpp`）：后台狂调 `measure_width`
/// 时主线程同工作量慢 **2.2~2.9×**——不是抢锁，是被**刷空缓存**。
///
/// ## 修法
///
/// 后台用**自己的一整套** `TextRenderer`（同字体栈、同 supersample，缓存互不相干）：
/// 排版结果只依赖度量数值（与哪个实例算出来的无关），而主线程的缓存始终保持热。
/// 代价是后台要多占一份缓存内存（按 `memory::scale_entry_limit` 随预算伸缩），
/// 换来的是**主线程彻底不受后台排版影响**——这才是「框架层让慢组件不拖主窗」。
///
/// 用法：宿主（如 `gbcode` 的 md 预览）为离屏排版持有一个
/// `OffscreenTextPort`，把它塞进 `SlowLayoutSnapshot::port`。
class OffscreenTextPort final : public ui::TextPort {
 public:
  /// `stack` 与主线程共用一个**只读**字体栈（字体数据不可变，可安全共享）；
  /// `supersample` 要与主线程一致，否则度量会差（字形栅格化的缩放口径不同）。
  explicit OffscreenTextPort(const text::FontStack& stack, float supersample = 1.0f)
      : renderer_(stack, supersample), port_(renderer_) {}

  [[nodiscard]] auto measure(std::string_view utf8, float size) const -> math::Size override {
    return port_.measure(utf8, size);
  }
  [[nodiscard]] auto measure_width(std::string_view utf8, float size,
                                   text::FontRole role = text::FontRole::Proportional) const
      -> float override {
    return port_.measure_width(utf8, size, role);
  }
  [[nodiscard]] auto line_height(float size) const -> float override {
    return port_.line_height(size);
  }
  [[nodiscard]] auto ascent(float size) const -> float override { return port_.ascent(size); }
  [[nodiscard]] auto descent(float size) const -> float override { return port_.descent(size); }
  [[nodiscard]] auto shaped_ascent(std::string_view utf8, float size,
                                   text::FontRole role = text::FontRole::Proportional) const
      -> float override {
    return port_.shaped_ascent(utf8, size, role);
  }
  [[nodiscard]] auto ink_metrics(std::string_view utf8, float size,
                                 text::FontRole role = text::FontRole::Proportional) const
      -> std::optional<InkMetrics> override {
    return port_.ink_metrics(utf8, size, role);
  }
  [[nodiscard]] auto has_real_bold() const -> bool override { return port_.has_real_bold(); }
  void draw(raster::Surface& canvas, std::string_view utf8, math::Point origin, float size,
            math::Color color, text::FontRole role = text::FontRole::Proportional,
            float embolden = 0.0f, bool bold = false) const override {
    port_.draw(canvas, utf8, origin, size, color, role, embolden, bold);
  }
  [[nodiscard]] auto ellipsize(std::string_view utf8, float size, float max_width) const
      -> std::string override {
    return port_.ellipsize(utf8, size, max_width);
  }
  [[nodiscard]] auto wrap(std::string_view utf8, float size, float max_width) const
      -> std::vector<std::string_view> override {
    return port_.wrap(utf8, size, max_width);
  }
  [[nodiscard]] auto wrap_limited(std::string_view utf8, float size, float max_width,
                                  std::size_t max_lines) const
      -> std::vector<std::string> override {
    return port_.wrap_limited(utf8, size, max_width, max_lines);
  }

  /// 后台缓存统计（诊断：命中/未命中，见 `tools/lock_contention_probe.cpp`）。
  [[nodiscard]] auto stats() const { return renderer_.width_cache_stats(); }

 private:
  text::TextRenderer renderer_;      ///< 后台独占（缓存与主线程完全隔离）
  RendererTextPort port_;            ///< 转发到上面那个实例
};

}  // namespace st::app
