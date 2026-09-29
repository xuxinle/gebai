#pragma once

/// 控制通道（TCP）：协议 `st-control/1`，帧 = 4 字节大端长度 + UTF-8 JSON。
/// 规范见 `DESIGN.md` §6。设计要点：
/// - **非阻塞**：所有请求在 `poll()` 内处理，`wait` 类请求挂起后由后续 `poll()` 续判（不阻塞应用主循环）；
/// - **无任意代码入口**：只有数据与动作（没有 eval）；
/// - **默认只绑回环**：`ServerOptions::bind` 默认 `127.0.0.1`，绑定其他地址需显式指定。

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"
#include "st/core/json.hpp"
#include "st/raster/canvas.hpp"
#include "st/ui/ui_root.hpp"

namespace st::control {

struct ServerOptions {
  std::string bind{"127.0.0.1"};
  std::uint16_t port{0};              ///< 0 = 由内核分配
  std::string control_file{};         ///< 非空时写入握手信息（port/pid/app），供客户端发现
  std::size_t max_frame{64u * 1024u * 1024u};
  bool log_calls{true};
};

struct Metrics {
  std::uint64_t frames{0};
  double last_frame_ms{0.0};
  double frame_p50_ms{0.0};
  double frame_p95_ms{0.0};
  std::int64_t uptime_ms{0};
  std::string backend{};
  bool headless{true};
  float device_scale{1.0f};
  int physical_width{0};
  int physical_height{0};
  std::size_t nodes{0};
  std::uint64_t requests{0};
};

/// 应用侧能力（由 `app::App` 实现）——控制层不直接触碰具体组件类型。
class Host {
 public:
  virtual ~Host() = default;
  [[nodiscard]] virtual auto root() -> ui::UiRoot& = 0;
  [[nodiscard]] virtual auto app_name() const -> std::string = 0;
  [[nodiscard]] virtual auto app_version() const -> std::string = 0;
  [[nodiscard]] virtual auto backend_name() const -> std::string_view = 0;
  [[nodiscard]] virtual auto headless() const -> bool = 0;
  [[nodiscard]] virtual auto viewport() const -> math::Size = 0;
  /// 逻辑尺寸（= viewport）；`viewport` 即逻辑像素，物理像素 = 逻辑 × device_scale。
  [[nodiscard]] virtual auto device_scale() const -> float = 0;
  virtual auto set_device_scale(float scale) -> Status = 0;
  [[nodiscard]] virtual auto metrics() const -> Metrics = 0;
  virtual void request_quit() = 0;
  virtual void request_repaint() = 0;
  virtual void set_theme_mode(ui::ThemeMode mode) = 0;
  /// 截图并写 PNG 到 `path`（空=自动命名；`region` 为空=全屏）；返回实际路径。
  [[nodiscard]] virtual auto capture_to_file(std::string_view path, math::IntRect region)
      -> Result<std::string> = 0;
  /// 截图 PNG 字节（`region` 为空=全屏；base64 回传用）。
  [[nodiscard]] virtual auto capture_png(math::IntRect region)
      -> Result<std::vector<std::uint8_t>> = 0;
  [[nodiscard]] virtual auto log_lines(std::size_t limit) const -> std::vector<std::string> = 0;
};

class Server {
 public:
  explicit Server(Host& host);
  ~Server();
  Server(const Server&) = delete;
  auto operator=(const Server&) -> Server& = delete;

  [[nodiscard]] auto start(const ServerOptions& options) -> Result<std::uint16_t>;
  void stop();
  /// 非阻塞轮询：接受连接、读取并处理请求、续判等待、推送事件。
  void poll();
  /// 向已订阅客户端推送事件。
  void publish(std::string_view event, const Value& data);
  [[nodiscard]] auto port() const noexcept -> std::uint16_t;
  [[nodiscard]] auto client_count() const noexcept -> std::size_t;
  [[nodiscard]] auto running() const noexcept -> bool;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace st::control
