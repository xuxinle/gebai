#pragma once

/// 控制通道（TCP）：协议 `st-control/1`，帧 = 4 字节大端长度 + UTF-8 JSON。
/// 规范见 `DESIGN.md` §6。设计要点：
/// - **非阻塞**：所有请求在 `poll()` 内处理，`wait` 类请求挂起后由后续 `poll()` 续判（不阻塞应用主循环）；
/// - **无任意代码入口**：只有数据与动作（没有 eval）；
/// - **默认只绑回环**：`ServerOptions::bind` 默认 `127.0.0.1`，绑定其他地址需显式指定。

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"
#include "st/ext/json.hpp"
#include "st/ext/script.hpp"
#include "st/raster/canvas.hpp"
#include "st/ui/script_host.hpp"
#include "st/ui/ui_root.hpp"

namespace st::control {

struct ServerOptions {
  std::string bind{"127.0.0.1"};
  std::uint16_t port{0};              ///< 0 = 由内核分配
  std::string control_file{};         ///< 非空时写入握手信息（port/pid/app/token），供客户端发现
  std::size_t max_frame{64u * 1024u * 1024u};
  bool log_calls{true};
  /// 连接鉴权 token：`nullopt`（默认）= `start()` 自动生成随机 token 并写入控制文件。
  /// 客户端必须在首个 `hello` 的 `params.token` 里携带同值，否则拒绝并断连。
  /// 为什么必须有：回环绑定只防远程，**不防本机其他进程**——多用户主机上任何本地进程
  /// 都能连上端口注入键鼠、读屏、杀进程。token 随控制文件分发（文件权限由 OS 管），
  /// 未拿到文件的进程连不上。显式传空串 `""` 可关闭鉴权（单用户开发机兼容旧客户端）。
  ///
  /// 为什么用 `optional` 而不是哨兵字符串：曾用 `std::string token{"\0"}` 当「自动生成」
  /// 哨兵，但 `std::string` 从 `"\0"` 构造会**坍缩成空串**（遇 NUL 即止），
  /// 于是「显式空串关闭鉴权」与「未设置」无法区分——鉴权测试当场抓住（无法关闭）。
  std::optional<std::string> token{};
  /// `capture` 落盘路径白名单（目录前缀，UTF-8，比较前统一归一化分隔符）。
  /// 默认：系统 temp 目录 + 可执行文件同目录 + **控制文件所在目录**（智能体会话
  /// 目录在那里——截图直落会话免搬运）。空向量 = 禁止一切落盘（仍可 base64 回传）。
  std::vector<std::string> capture_dirs{};
  /// 是否开放 `script` 方法与脚本宿主能力。
  ///
  /// **默认关闭**且不随其它开关联动：脚本 = 在应用进程内执行任意代码，
  /// 而控制通道原本的设计前提是"没有任意代码执行入口"。开启它是一次**显式的姿态变更**，
  /// 必须由宿主应用主动决定（`--enable-script`），而不是"监听端口就顺带有了"。
  bool enable_script{false};
  /// 脚本配额（仅 `enable_script` 时生效）。
  ext::ScriptLimits script_limits{};

  /// 计算默认白名单（temp + 可执行文件目录 + 控制文件目录）。`start()` 内部使用。
  [[nodiscard]] auto default_capture_dirs() const -> std::vector<std::string>;
};

struct Metrics {
  std::uint64_t frames{0};
  double last_frame_ms{0.0};
  double frame_p50_ms{0.0};
  double frame_p95_ms{0.0};
  /// 最后一帧的**分阶段耗时**（同一次 `render_frame` 内的三段）：
  /// `layout`（测量 + 排版）、`paint`（把控件画进画布）、`present`（清屏 + 送显/翻转）。
  ///
  /// 为什么必须分开报："帧耗时 20ms" 本身不指向任何行动——是排版重复算了、
  /// 还是阴影/文字光栅化太重、还是送显在等垂直同步，只有分段数据能区分。
  double layout_ms{0.0};
  double paint_ms{0.0};
  double present_ms{0.0};
  std::int64_t uptime_ms{0};
  std::string backend{};
  /// 实际使用的渲染器（"software" / "gpu"）与选择理由（`auto` 时含实测结果）。
  std::string renderer{};
  std::string renderer_note{};
  /// 文字抗锯齿形态："lcd"（亚像素）或 "grayscale"。
  ///
  /// 为什么上报而不是只写日志："字看着糊/看着带彩边"这类观感问题，
  /// 第一件要确认的事就是**当前到底在用哪种形态**——它跟后端（有窗口/无头）
  /// 与启动参数都相关，不报就变成猜。
  std::string text_renderer{};
  /// 字形网格拟合模式：`"off"` / `"light"` / `"normal"`（见 `AppOptions::text_fit`）。
  ///
  /// 与 `text_renderer` 并列上报的理由一样：同一段文字在不同拟合档下**字形边沿不同**，
  /// 它是一份渲染结果的组成部分——不报就无法复现一个像素现场。
  std::string text_fit{};
  bool headless{true};
  float device_scale{1.0f};
  int physical_width{0};
  int physical_height{0};
  std::size_t nodes{0};
  std::uint64_t requests{0};
};

/// 像素缓冲（截图像素口径：**物理像素**、RGBA8、行优先无填充）。
struct PixelView {
  int width{0};
  int height{0};
  std::vector<std::uint8_t> rgba{};
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
  /// 截取像素（`region` 为空=全屏；逻辑坐标入参、物理像素回传）。
  ///
  /// 与 `capture_png` 的区别：不编码 PNG，直接给 RGBA8 原始字节——供**视觉断言**
  /// （`capture.hash` / `visual.diff`）在应用进程内比较像素用，省去编解码一圈。
  [[nodiscard]] virtual auto capture_pixels(math::IntRect region) -> Result<PixelView> = 0;
  /// 截图 PNG 字节（`region` 为空=全屏；base64 回传用）。
  [[nodiscard]] virtual auto capture_png(math::IntRect region)
      -> Result<std::vector<std::uint8_t>> = 0;
  [[nodiscard]] virtual auto log_lines(std::size_t limit) const -> std::vector<std::string> = 0;
  /// 脚本宿主（**宿主应用拥有**；未启用脚本能力时返回 `nullptr`）。
  ///
  /// 由宿主注入而不是 Server 自建：JS 环境必须是**唯一一份**——否则协议里的 `script` 与
  /// 应用内部的事件/定时器会落在两个互不可见的 JS 世界里，脚本绑的事件永远收不到 UI 事件。
  [[nodiscard]] virtual auto script() -> ui::ScriptHost* { return nullptr; }
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
  void publish(std::string_view event, const st::Json& data);
  [[nodiscard]] auto port() const noexcept -> std::uint16_t;
  /// 生效的鉴权 token（`start()` 自动生成后由此读回；显式关闭鉴权时为空串）。
  /// 客户端从控制文件读；测试与嵌入场景可直接取（`start()` 的 options 是 const 引用，
  /// 调用方拿不到写回值，故提供本访问器）。
  [[nodiscard]] auto token() const -> std::string;
  [[nodiscard]] auto client_count() const noexcept -> std::size_t;
  [[nodiscard]] auto running() const noexcept -> bool;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace st::control
