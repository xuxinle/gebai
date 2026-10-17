#pragma once

/// 裁云（Nimbus）· **AI 控制通道桥**（`CaiyunDirector` 元素）。
///
/// ## 为什么需要一个专门的元素来承载动作面
///
/// 霜天的控制通道是「元素 id + 动作/属性」模型：智能体要驱动这个应用（换脚本、
/// 开渲染、查进度），必须有一个**稳定的寻址点**。挂在时间轴或监视器上语义不对
/// （那些是人的操作面），所以本元素是纯控制面：**不画任何东西**，只做两件事——
///
/// 1. `invoke_action`：`load_script` / `apply_script` / `render` / `render_stop` / `save_script`；
/// 2. `get_property`：脚本与渲染的**只读快照**（`script` / `render_stage` /
///    `render_progress` / `render_log` …），智能体据此判断该等还是该停。
///
/// 这样「AI 控制」就是一条**可断言**的路径：`invoke render` → 轮询
/// `get render_stage` → `done` 后断言 `render_output` 文件存在。而不是靠截图猜。
///
/// ## 为什么不给它一个可见的卡片
///
/// 界面上的 AI 面板（`#director-scroll`）已经把这些能力以按钮形式给出；再画一遍
/// 就是两套展示、两份文字要同步。控制面只出现在控制通道里。

#include <functional>
#include <string>
#include <vector>

#include "st/ui/element.hpp"

namespace caiyun {

class DirectorBridge final : public st::ui::Element {
 public:
  DirectorBridge() = default;

  [[nodiscard]] auto type() const noexcept -> std::string_view override { return "CaiyunDirector"; }
  [[nodiscard]] auto role() const noexcept -> st::ui::Role override { return st::ui::Role::None; }

  /// 宿主注入的能力（本项目里由 `CaiyunPage` 提供）。
  struct Hooks {
    /// 应用一段脚本（json 文本）；返回失败信息（空 = 成功）。
    std::function<std::string(const std::string& json)> apply_script{};
    /// 从文件装载脚本；返回失败信息（空 = 成功）。
    std::function<std::string(const std::string& path)> load_script{};
    /// 保存当前脚本；返回失败信息。
    std::function<std::string(const std::string& path)> save_script{};
    /// 启动渲染（`spec` 形如 `out.mp4` 或 `out.mp4 1280x720`）；返回失败信息。
    std::function<std::string(const std::string& spec)> start_render{};
    /// 中止渲染。
    std::function<void()> stop_render{};
    /// 当前脚本 JSON。
    std::function<std::string()> script_json{};
    /// 工程摘要（`scene_count` / `duration` / `fps` / `size` 等一条键值）。
    std::function<std::string(const std::string& key)> project_value{};
    /// 渲染状态里的一项。
    std::function<std::string(const std::string& key)> render_value{};
  };

  void set_hooks(Hooks hooks) { hooks_ = std::move(hooks); }

  void measure(const st::ui::RenderContext& context, const st::ui::Constraints& constraints) override;
  void paint_content(const st::ui::RenderContext& context, st::raster::Surface& canvas) const override;
  [[nodiscard]] auto get_property(std::string_view name) const -> std::optional<std::string> override;
  auto set_property(std::string_view name, std::string_view value) -> bool override;
  [[nodiscard]] auto property_names() const -> std::vector<std::string_view> override;
  auto invoke_action(std::string_view action, std::string_view argument) -> bool override;

  /// 最近一次动作的结果（成功为空；失败为可读原因）。控制通道侧据此判断成败。
  [[nodiscard]] auto last_error() const -> const std::string& { return last_error_; }
  [[nodiscard]] auto last_action() const -> const std::string& { return last_action_; }

 private:
  Hooks hooks_{};
  std::string last_action_{};
  std::string last_error_{};
};

}  // namespace caiyun
