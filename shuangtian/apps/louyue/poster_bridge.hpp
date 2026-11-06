#pragma once

/// 镂月（Lunaris）· **AI 控制面**（`PosterBridge` 元素）。
///
/// ## 为什么需要它
///
/// 镂月原本只有「像素 + 图层」的控制面（`CanvasView` 的 11 个写动作）——AI 能改像素，
/// 却**没法描述"我在做一张海报"**：改完之后没有结构化真值，人也无法在"改文字"这个
/// 层面接续修改。裁云的结论是：AI 主导的作品必须有一份**脚本**当共享真值，
/// 像素只是它的投影。这个元素就是那份脚本的读写口。
///
/// ## 与 `CanvasView` 的分工
///
/// - `CanvasView`：**像素级**（笔触/图层/合成）——人工绘图面，也供 AI 微调；
/// - `#poster`：**作品级**（应用海报脚本 / 导出 PNG / 控制权）——AI 主导面。
///
/// 两者占同一份 `Document`（脚本渲染成的图层就是文档里的图层），所以不存在
/// "脚本一套、画布一套"的双源。

#include <functional>
#include <string>
#include <vector>

#include "st/ui/element.hpp"

namespace louyue {

class PosterBridge final : public st::ui::Element {
 public:
  PosterBridge() = default;

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "LouyuePoster"; }
  [[nodiscard]] auto role() const noexcept -> st::ui::Role override { return st::ui::Role::None; }

  /// 宿主注入的能力（由 `LouyuePage` 提供；桥内不做业务判断）。
  struct Hooks {
    /// 应用一份海报脚本（json 文本）；返回失败信息（空 = 成功）。
    std::function<std::string(const std::string& json)> apply_poster{};
    /// 从文件装载海报脚本；返回失败信息。
    std::function<std::string(const std::string& path)> load_poster{};
    /// 把当前海报另存为脚本文件；返回失败信息。
    std::function<std::string(const std::string& path)> save_poster{};
    /// 导出 PNG（`spec` 形如 `out.png` 或 `out.png 1080x1080`）；返回失败信息。
    std::function<std::string(const std::string& spec)> export_png{};
    /// 当前海报脚本 JSON。
    std::function<std::string()> poster_json{};
    /// 作品摘要（`blocks` / `layer_count` / `size` / `name` / `export_size`）。
    std::function<std::string(const std::string& key)> poster_value{};
    /// 控制权切换（`true` = AI 接管、人只能看）；返回失败信息。
    std::function<std::string(bool)> set_edit_lock{};
    /// 当前是否 AI 接管。
    std::function<bool()> edit_locked{};
    /// 最近一次导出的路径。
    std::function<std::string()> last_export{};
  };

  void set_hooks(Hooks hooks) { hooks_ = std::move(hooks); }

  void measure(const st::ui::RenderContext& context, const st::ui::Constraints& constraints) override;
  void paint_content(const st::ui::RenderContext& context, st::raster::Surface& canvas) const override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  auto invoke_action(std::string_view action, std::string_view argument) -> bool override;

  /// 最近一次动作的失败原因（空 = 成功）。控制通道侧据此判断成败。
  [[nodiscard]] auto last_error() const -> const std::string& { return last_error_; }

 private:
  Hooks hooks_{};
  std::string last_action_{};
  std::string last_error_{};
};

}  // namespace louyue
