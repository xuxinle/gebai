#pragma once

/// 应用运行时：装配 后端 + 字体 + UI 树 + 控制通道 + 帧循环。
/// 无头模式与窗口模式共用同一条渲染路径（软件光栅器 → 帧缓冲 → 后端 present）。

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "st/control/control.hpp"
#include "st/core/error.hpp"
#include "st/shell/shell.hpp"
#include "st/ui/ui_root.hpp"

namespace st::app {

struct AppOptions {
  int width{1280};   ///< 逻辑宽（UI 坐标）
  int height{720};   ///< 逻辑高
  float scale{0.0f}; ///< DPI 缩放：0 = 自动（ST_SCALE 环境变量 → 1.0）；2.0 = 200%（HiDPI）
  std::string title{"霜天应用"};
  bool headless{false};
  std::string backend{};              ///< 空=自动（无显示则 headless）
  std::string control_bind{"127.0.0.1"};
  std::uint16_t control_port{0};      ///< 0=自动分配
  std::string control_file{};         ///< 写入 {port,pid,...} 供客户端发现
  std::string screenshot_dir{};       ///< 控制通道 `encode=file` 的落盘目录（空=会话临时目录）
  std::string font_latin{};           ///< 显式指定拉丁字体文件（空=自动探测）
  std::string font_cjk{};             ///< 显式指定 CJK 字体文件（空=自动探测）
  ui::ThemeMode theme{ui::ThemeMode::Light};
  std::string log_level{"info"};
  std::uint32_t max_frames{0};        ///< >0 时跑满即退出（无头冒烟/CI 用）
  bool exit_on_ready{false};          ///< 首帧后立即退出（自检用）
  double frame_budget_ms{16.0};       ///< 帧间隔目标
};

/// 应用：实现控制通道所需的宿主能力（`control::Host`）。
class Application final : public control::Host {
 public:
  Application(std::string name, std::string version, AppOptions options = {});
  ~Application() override;
  Application(const Application&) = delete;
  auto operator=(const Application&) -> Application& = delete;

  /// 设置根组件并运行（返回进程退出码）。
  auto run(std::unique_ptr<ui::Element> content) -> Result<int>;
  /// 设置根组件（`run()` 内部同样调用；用 `start()`+`tick()` 自驱主循环时先调用本方法）。
  void set_content(std::unique_ptr<ui::Element> content);
  /// 请求退出（控制通道 `app.quit`、快捷键、信号均走此处）。
  void request_quit() override;
  void request_repaint() override;

  // —— control::Host ——
  [[nodiscard]] auto root() -> ui::UiRoot& override { return root_; }
  [[nodiscard]] auto app_name() const -> std::string override { return name_; }
  [[nodiscard]] auto app_version() const -> std::string override { return version_; }
  [[nodiscard]] auto backend_name() const -> std::string_view override;
  [[nodiscard]] auto headless() const -> bool override;
  [[nodiscard]] auto viewport() const -> math::Size override { return root_.viewport(); }
  [[nodiscard]] auto device_scale() const -> float override;
  auto set_device_scale(float scale) -> Status override;
  [[nodiscard]] auto metrics() const -> control::Metrics override;
  void set_theme_mode(ui::ThemeMode mode) override;
  [[nodiscard]] auto capture_to_file(std::string_view path, math::IntRect region)
      -> Result<std::string> override;
  [[nodiscard]] auto capture_png(math::IntRect region)
      -> Result<std::vector<std::uint8_t>> override;
  [[nodiscard]] auto log_lines(std::size_t limit) const -> std::vector<std::string> override;

  /// 单帧推进（自检与外部驱动用）：布局 → 绘制 → present。
  void render_frame();
  /// 查询控制通道端口（0=未启动）。
  [[nodiscard]] auto control_port() const noexcept -> std::uint16_t;
  /// 受控提前启动（`run` 内部会调用；自检可先启动再注入事件）。
  auto start() -> Status;
  [[nodiscard]] auto started() const noexcept -> bool { return started_; }
  /// 主循环单步（自检用）。
  void tick();
  /// 是否收到退出请求（控制通道 `app.quit` / 自检终止条件）。
  [[nodiscard]] auto quit_requested() const noexcept -> bool;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::string name_{};
  std::string version_{};
  AppOptions options_{};
  ui::UiRoot root_{};
  bool started_{false};
};

}  // namespace st::app
