// 平台边界：Wayland 窗口后端（`CONVENTIONS.md` §10 第 1 条：系统头/平台 API 只在此）。
//
// ## 与 X11 后端的结构对照（同一份 `shell::Backend` 契约，两套完全不同的窗口模型）
//
// | 能力 | X11（服务端绘制） | Wayland（客户端绘制） |
// |---|---|---|
// | 建窗 | `XCreateWindow` + `_MOTIF_WM_HINTS` | `wl_compositor` + `xdg_surface`/`xdg_toplevel` |
// | 帧缓冲 | `XImage`（`XPutImage` 推给服务端） | `wl_shm` 共享内存 + `wl_buffer`（合成器直接读） |
// | 尺寸 | `XGetWindowAttributes`（随时可问） | **只有 configure 事件给**（服务端说了算） |
// | 上屏 | 立即 `XPutImage` | `attach`+`commit`，等 `wl_buffer.release` 才可复用 |
// | 拖动/缩放 | `_NET_WM_MOVERESIZE` | `xdg_toplevel.move`/`resize`（同样交给合成器） |
// | 去装饰 | `_MOTIF_WM_HINTS` | `zxdg_toplevel_decoration_v1.set_mode(client_side)` |
//
// ## 为什么手写 ABI + `dlopen`（与 X11 同因）
//
// 本机（以及任何没装 `libwayland-dev` 的机器）**没有 `<wayland-client.h>`、
// 没有 `wayland-scanner`、没有协议 XML 目录**（实测三者皆无）。而框架硬约束是
// 「系统能力一律运行时 `dlopen` 可选加载」（§10 第 1 条）。因此：
//   - 协议接口表由 `tools/wayland_bindgen.py` 自研生成（`wayland_protocol.hpp`；
//     输入是 `third_party/wayland/*.xml` 官方副本，**已入库**）；
//   - 通过 `wl_proxy_marshal_array_flags` 这套**泛型调用**接口发请求：不依赖 scanner
//     生成的包装函数，也不链接 libwayland-client；
//   - 键盘映射走 `libxkbcommon`（同样 `dlopen`）——Wayland 的键盘是"服务端发 keymap、
//     客户端自己译键"，自己解析 xkb keymap 是重造一个大轮子。
//
// **ABI 依据**（逐条核对，不凭记忆）：事件回调的确切签名取官方 `connection.c` 的
// `convert_arguments_to_ffi`；`wl_message`/`wl_interface` 字段顺序取 `wayland-util.h`。
// 两份参考副本留档在 `third_party/wayland/refs/`（不参与构建，只为"这些假设可被复查"）。
//
// ## 本版如实的能力边界（**不假装成功**）
//
//   - 窗口图标：xdg-shell 没有 `set_icon`（需 `xdg-toplevel-icon-v3`）→ 未实现，
//     基类如实报 `Unsupported`（与 X11 侧同一姿态）；
//   - 跨进程剪贴板（`wl_data_device`：data_source/data_offer + MIME 协商）→ 未实现，
//     进程内闭环可用（与 headless 同口径）；跨进程实现见 `docs/BACKLOG.md`；
//   - 分数缩放（`wp_fractional_scale_v1`）→ 未实现，用 `wl_output.scale` 的整数倍。
//   三者都不影响"界面能起、能被控制通道驱动、能截图验证"这条主线。

#if !defined(_WIN32)

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <poll.h>      // lint-allow: L14 平台单点封装（事件泵的非阻塞可读判断）
#include <span>
#include <string>
#include <string_view>
#include <sys/mman.h>  // lint-allow: L14 平台单点封装（wl_shm 的共享内存）
#include <time.h>      // lint-allow: L14 平台单点封装（nanosleep）
#include <unistd.h>    // lint-allow: L14 平台单点封装（memfd/ftruncate/close）
#include <vector>

#include <dlfcn.h>  // lint-allow: L14 平台单点封装

#include "st/core/error.hpp"
#include "st/core/log.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/simd.hpp"  // 像素重排下沉点（与 X11 后端共用同一语义）
#include "st/shell/shell.hpp"
#include "st/ui/window_control.hpp"
#include "backend_util.hpp"
#include "wayland_protocol.hpp"

namespace st::shell {

/// 协议接口表（`wayland_protocol.hpp` 生成物）的简写：本文件里出现几十次，
/// 写全 `st::shell::proto::` 只会淹没代码本身。
namespace proto = wayland::proto;

namespace {

// ————————————————————————————————————————————————————————————————————————————
// libwayland-client 的最小 ABI（照 `wayland-client-core.h` / `wayland-util.h` 抄写）
// ————————————————————————————————————————————————————————————————————————————

using WlDisplay = void;
using WlProxy = void;

/// `wl_argument`：泛型调用的参数联合体（`wayland-util.h` 逐字段抄写）。
///
/// 为什么要它（而不是传可变参数）：`wl_proxy_marshal_array_flags` 收**数组**——
/// 参数显式摆好就不必依赖 C 的可变参数提升规则（整数提升、指针 vs 整数是这条路上
/// 最容易踩的坑）。
union WlArgument {
  std::int32_t i;
  std::uint32_t u;
  std::int32_t f;   ///< wl_fixed_t
  const char* s;
  WlProxy* o;       ///< object / new_id：代理对象指针
  std::uint32_t n;  ///< new_id 的**对象 id**（仅服务端侧用得着）
  void* a;          ///< wl_array*
  std::int32_t h;   ///< fd
};

struct WlArray {
  std::size_t size;
  std::size_t alloc;
  void* data;
};

/// `wl_shm` 的格式枚举。
constexpr std::uint32_t kWlShmFormatXrgb8888{1};
/// `wl_keyboard.keymap` 的 `format`。
constexpr std::uint32_t kWlKeyboardKeymapFormatXkbV1{1};
/// `wl_pointer.button` 的 Linux 按钮码（evdev）。
constexpr std::uint32_t kBtnLeft{0x110};
constexpr std::uint32_t kBtnRight{0x111};
constexpr std::uint32_t kBtnMiddle{0x112};
/// `wl_pointer.axis` 的方向与 `wl_pointer.axis_source`。
constexpr std::uint32_t kWlPointerAxisVertical{0};

/// libwayland-client 函数表（一次性取全；缺任何一个即整体视为不可用——
/// 否则"库版本太旧少了某个函数"会变成运行到某条路径才崩，而不是启动时一句明确的话）。
struct WaylandApi {
  std::string library{};
  void* handle{nullptr};

  WlDisplay* (*display_connect)(const char*);
  void (*display_disconnect)(WlDisplay*);
  int (*display_get_fd)(WlDisplay*);
  int (*display_flush)(WlDisplay*);
  int (*display_dispatch_pending)(WlDisplay*);
  int (*display_prepare_read)(WlDisplay*);
  int (*display_read_events)(WlDisplay*);
  int (*display_cancel_read)(WlDisplay*);
  int (*display_roundtrip)(WlDisplay*);
  void (*proxy_destroy)(WlProxy*);
  int (*proxy_add_listener)(WlProxy*, void (**)(void), void*);
  std::uint32_t (*proxy_get_version)(const WlProxy*);
  WlProxy* (*proxy_marshal_array_flags)(WlProxy*, std::uint32_t, const proto::Interface*,
                                        std::uint32_t, std::uint32_t, WlArgument*);
};

/// libxkbcommon 函数表（Wayland 的键盘映射必需；缺失时键盘降级为"仅键码"）。
struct XkbApi {
  std::string library{};
  void* handle{nullptr};
  void* (*context_new)(int);
  void (*context_unref)(void*);
  void* (*keymap_new_from_string)(void*, const char*, int, int);
  void (*keymap_unref)(void*);
  void* (*state_new)(void*);
  void (*state_unref)(void*);
  // 官方签名（xkbcommon.h）：`(state, depressed_mods, latched_mods, locked_mods,
  // depressed_layout, latched_layout, locked_layout)`——共 7 个参数。
  // ⚠ 抄成 8 个（多一个掩码）会在编译期就报"实参太少"，而这里按官方逐项对齐。
  void (*state_update_mask)(void*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                            std::uint32_t, std::uint32_t);
  std::uint32_t (*state_key_get_one_sym)(void*, std::uint32_t);
  int (*state_key_get_utf8)(void*, std::uint32_t, char*, std::size_t);
};

/// xkb 常量（`xkbcommon.h`）。
constexpr int kXkbContextNoFlags{0};
constexpr int kXkbKeymapFormatTextV1{1};

// xkb 修饰键**位序**（`XKB_MOD_NAME_*` 的顺序，不是 X11 的 state 位）
constexpr std::uint32_t kXkbModShift{1U << 0};
constexpr std::uint32_t kXkbModControl{1U << 2};
constexpr std::uint32_t kXkbModAlt{1U << 3};  ///< Mod1
constexpr std::uint32_t kXkbModMeta{1U << 6}; ///< Mod4（Super）

template <typename Fn>
[[nodiscard]] auto load_symbol(void* handle, const char* name, Fn& out) -> bool {
  out = reinterpret_cast<Fn>(::dlsym(handle, name));  // lint-allow: L6 dlopen 符号
  return out != nullptr;
}

[[nodiscard]] auto wayland_library() -> const WaylandApi& {
  static const WaylandApi instance = [] {
    WaylandApi api;
    const std::string name = detail::library_available("libwayland-client.so.0");
    if (name.empty()) return api;
    void* handle = ::dlopen(name.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (handle == nullptr) return api;
    const bool ok =
        load_symbol(handle, "wl_display_connect", api.display_connect) &&
        load_symbol(handle, "wl_display_disconnect", api.display_disconnect) &&
        load_symbol(handle, "wl_display_get_fd", api.display_get_fd) &&
        load_symbol(handle, "wl_display_flush", api.display_flush) &&
        load_symbol(handle, "wl_display_dispatch_pending", api.display_dispatch_pending) &&
        load_symbol(handle, "wl_display_prepare_read", api.display_prepare_read) &&
        load_symbol(handle, "wl_display_read_events", api.display_read_events) &&
        load_symbol(handle, "wl_display_cancel_read", api.display_cancel_read) &&
        load_symbol(handle, "wl_display_roundtrip", api.display_roundtrip) &&
        load_symbol(handle, "wl_proxy_destroy", api.proxy_destroy) &&
        load_symbol(handle, "wl_proxy_add_listener", api.proxy_add_listener) &&
        load_symbol(handle, "wl_proxy_get_version", api.proxy_get_version) &&
        load_symbol(handle, "wl_proxy_marshal_array_flags", api.proxy_marshal_array_flags);
    if (!ok) {
      ::dlclose(handle);
      return api;
    }
    api.library = name;
    api.handle = handle;
    return api;
  }();
  return instance;
}

[[nodiscard]] auto xkb_library() -> const XkbApi& {
  static const XkbApi instance = [] {
    XkbApi api;
    const std::string name = detail::library_available("libxkbcommon.so.0:libxkbcommon.so");
    if (name.empty()) return api;
    void* handle = ::dlopen(name.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (handle == nullptr) return api;
    const bool ok = load_symbol(handle, "xkb_context_new", api.context_new) &&
                    load_symbol(handle, "xkb_context_unref", api.context_unref) &&
                    load_symbol(handle, "xkb_keymap_new_from_string", api.keymap_new_from_string) &&
                    load_symbol(handle, "xkb_keymap_unref", api.keymap_unref) &&
                    load_symbol(handle, "xkb_state_new", api.state_new) &&
                    load_symbol(handle, "xkb_state_unref", api.state_unref) &&
                    load_symbol(handle, "xkb_state_update_mask", api.state_update_mask) &&
                    load_symbol(handle, "xkb_state_key_get_one_sym", api.state_key_get_one_sym) &&
                    load_symbol(handle, "xkb_state_key_get_utf8", api.state_key_get_utf8);
    if (!ok) {
      ::dlclose(handle);
      return api;
    }
    api.library = name;
    api.handle = handle;
    return api;
  }();
  return instance;
}

[[nodiscard]] auto wayland_available() -> bool { return wayland_library().handle != nullptr; }

// ————————————————————————————————————————————————————————————————————————————
// 键盘：keysym → 框架键名
//
// 与 X11 后端**必须逐项一致**：组件层拿 `event.key` 与字符串字面量比较。
// 两平台给不同名字就会出现"同一个界面在 Linux 上按键没反应"，而只造 `ui::Event`
// 的单元测试看不出来（它不经过键名生成）。
// ————————————————————————————————————————————————————————————————————————————

constexpr std::uint32_t kXkBackSpace{0xFF08};
constexpr std::uint32_t kXkTab{0xFF09};
constexpr std::uint32_t kXkReturn{0xFF0D};
constexpr std::uint32_t kXkEscape{0xFF1B};
constexpr std::uint32_t kXkHome{0xFF50};
constexpr std::uint32_t kXkLeft{0xFF51};
constexpr std::uint32_t kXkUp{0xFF52};
constexpr std::uint32_t kXkRight{0xFF53};
constexpr std::uint32_t kXkDown{0xFF54};
constexpr std::uint32_t kXkPrior{0xFF55};
constexpr std::uint32_t kXkNext{0xFF56};
constexpr std::uint32_t kXkEnd{0xFF57};
constexpr std::uint32_t kXkInsert{0xFF63};
constexpr std::uint32_t kXkKPEnter{0xFF8D};
constexpr std::uint32_t kXkDelete{0xFFFF};
constexpr std::uint32_t kXkFunction{0xFFBE};

[[nodiscard]] auto key_name_from_keysym(std::uint32_t keysym) -> std::string {
  if (keysym >= 0x21 && keysym <= 0x7E) {
    const auto ch = static_cast<char>(keysym);
    if (ch >= 'A' && ch <= 'Z') return std::string(1, static_cast<char>('a' + (ch - 'A')));
    return std::string(1, ch);
  }
  switch (keysym) {
    case 0x0020: return "Space";
    case kXkBackSpace: return "Backspace";
    case kXkTab: return "Tab";
    case kXkReturn: return "Enter";
    case kXkKPEnter: return "NumpadEnter";
    case kXkEscape: return "Escape";
    case kXkDelete: return "Delete";
    case kXkInsert: return "Insert";
    case kXkHome: return "Home";
    case kXkEnd: return "End";
    case kXkLeft: return "ArrowLeft";
    case kXkRight: return "ArrowRight";
    case kXkUp: return "ArrowUp";
    case kXkDown: return "ArrowDown";
    case kXkPrior: return "PageUp";
    case kXkNext: return "PageDown";
    case 0xFFE1:
    case 0xFFE2: return "Shift";
    case 0xFFE3:
    case 0xFFE4: return "Control";
    case 0xFFE9:
    case 0xFFEA: return "Alt";
    case 0xFFEB:
    case 0xFFEC:
    case 0xFFE7:
    case 0xFFE8: return "Meta";
    default: break;
  }
  if (keysym >= kXkFunction && keysym < kXkFunction + 35) {
    return std::format("F{}", keysym - kXkFunction + 1);
  }
  return {};
}

// ————————————————————————————————————————————————————————————————————————————
// 参数准备
//
// `wl_proxy_marshal_array_flags` 要 `wl_argument` 数组，且**顺序必须与消息签名逐一对齐**
// （与 scanner 生成的代码同一约束）。链式小工厂把类型写清楚，避免调用点直接摆弄联合体——
// 类型写错在 libwayland 里是**静默错读**（它按签名取字段），不是编译错误。
// ————————————————————————————————————————————————————————————————————————————

class Args {
 public:
  auto i(std::int32_t value) -> Args& {
    values_.push_back(WlArgument{.i = value});
    return *this;
  }
  auto u(std::uint32_t value) -> Args& {
    values_.push_back(WlArgument{.u = value});
    return *this;
  }
  auto str(const char* value) -> Args& {
    values_.push_back(WlArgument{.s = value});
    return *this;
  }
  auto obj(WlProxy* value) -> Args& {
    values_.push_back(WlArgument{.o = value});
    return *this;
  }
  /// `new_id`：槽位填 **nullptr**，真正的对象由 `wl_proxy_marshal_array_flags` 的
  /// `interface`/`version` 形参触发创建（它把新对象指针填回该槽并返回给我们）；
  /// 对"接口由运行时决定"的 `new_id`（如 `wl_registry.bind`）还要额外给
  /// `interface_name`(string) 与 `version`(uint) 两个**隐式参数**。
  auto new_id() -> Args& {
    values_.push_back(WlArgument{.o = nullptr});
    return *this;
  }
  auto array(WlArray* value) -> Args& {
    values_.push_back(WlArgument{.a = value});
    return *this;
  }
  auto fd(std::int32_t value) -> Args& {
    values_.push_back(WlArgument{.h = value});
    return *this;
  }
  [[nodiscard]] auto data() const -> WlArgument* {
    return const_cast<WlArgument*>(values_.data());  // lint-allow: L6 libwayland 只读该数组
  }
  [[nodiscard]] auto values() const -> const std::vector<WlArgument>& { return values_; }
  [[nodiscard]] auto values() -> std::vector<WlArgument>& { return values_; }

 private:
  std::vector<WlArgument> values_{};
};

/// 消息在接口请求表里的下标即 opcode（协议按声明顺序编号）。
[[nodiscard]] auto opcode_of(const proto::Interface& owner, const proto::Message* message)
    -> std::uint32_t {
  for (int index = 0; index < owner.request_count; ++index) {
    if (&owner.requests[index] == message) return static_cast<std::uint32_t>(index);
  }
  return 0;
}

[[nodiscard]] auto find_request(const proto::Interface& owner, std::string_view name)
    -> const proto::Message* {
  for (int index = 0; index < owner.request_count; ++index) {
    const proto::Message& message = owner.requests[index];
    if (message.name != nullptr && name == message.name) return &message;
  }
  return nullptr;
}

/// 一行像素的字节数：XRGB8888 是 4 字节/像素，天然 4 字节对齐（`wl_shm` 也要求 ≥4）。
[[nodiscard]] auto row_stride(int width) -> int { return width * 4; }

// ————————————————————————————————————————————————————————————————————————————
// 后端（连同监听器表与监听器实现，一起收在匿名命名空间里）
// ————————————————————————————————————————————————————————————————————————————

namespace {

/// Wayland 窗口后端（与 `X11Backend` 同一契约、两种系统原语；见文件头对照表）。
class WaylandBackend final : public Backend {
 public:
  WaylandBackend() = default;
  ~WaylandBackend() override { teardown(); }

  [[nodiscard]] auto name() const noexcept -> std::string_view override { return "wayland"; }
  [[nodiscard]] auto headless() const noexcept -> bool override { return false; }
  [[nodiscard]] auto close_requested() const noexcept -> bool override { return close_requested_; }

  auto create_window(const WindowOptions& options) -> Status override;
  void show_when_ready() override;
  void present() override;
  [[nodiscard]] auto poll_event() -> std::optional<ui::Event> override;
  void set_title(std::string_view title) override;
  [[nodiscard]] auto supports_window_control() const noexcept -> bool override {
    return xdg_toplevel_ != nullptr;
  }
  [[nodiscard]] auto minimize() -> Status override;
  [[nodiscard]] auto toggle_maximize() -> Status override;
  [[nodiscard]] auto request_close() -> Status override;
  [[nodiscard]] auto begin_move() -> Status override;
  [[nodiscard]] auto begin_resize(WindowEdge edge) -> Status override;
  [[nodiscard]] auto maximized() const noexcept -> bool override { return maximized_; }
  [[nodiscard]] auto clipboard_text() -> Result<std::string> override;
  [[nodiscard]] auto set_clipboard_text(std::string_view text) -> Status override;
  void set_resize_repaint(std::function<void()> repaint) override {
    repaint_ = std::move(repaint);
  }
  [[nodiscard]] auto framebuffer() -> raster::Surface& override;
  [[nodiscard]] auto renderer_name() const noexcept -> std::string_view override {
    return renderer_name_;
  }
  [[nodiscard]] auto renderer_note() const -> std::string override { return renderer_note_; }
  /// 上屏路径 + **本帧实际重排的像素面积**。
  ///
  /// 面积是量出来的事实：“局部上屏到底生效了没有”只能靠它回答
  /// （生效时远小于缓冲尺寸；等于缓冲尺寸就是走了整屏）。
  [[nodiscard]] auto present_note() const -> std::string override {
    return std::format("wl_shm 共享缓冲（{} 块双缓冲；上帧重排 {}x{}）", buffers_.size(),
                       last_present_width_, last_present_height_);
  }
  void set_present_region(math::IntRect region) override { present_region_ = region; }
  [[nodiscard]] auto frame_count() const noexcept -> std::uint64_t override { return frames_; }
  [[nodiscard]] auto device_scale() const noexcept -> float override { return scale_; }
  [[nodiscard]] auto logical_size() const noexcept -> math::Size override {
    return math::Size{logical_width_exact_, logical_height_exact_};
  }
  auto set_device_scale(float scale) -> Status override;

  // —— 协议调用与内部流程（实现见文件末；对外不可见）——

  /// 发一条请求。
  ///
  /// `new_interface`/`version` 只在请求含"**接口由客户端指定**的 `new_id`"时给
  /// （如 `wl_compositor.create_surface`、`xdg_wm_base.get_xdg_surface`）；
  /// **其余情况必须传 `nullptr`/0**，否则 libwayland 会按"要创建新对象"处理，
  /// 在错误的位置建对象（表现为句柄错乱，而不是报错）。
  [[nodiscard]] auto marshal(WlProxy* proxy, const proto::Interface& owner,
                             const proto::Message* message, const proto::Interface* new_interface,
                             std::uint32_t version, const Args& args) -> WlProxy* {
    const WaylandApi& api = wayland_library();
    if (proxy == nullptr || message == nullptr) return nullptr;
    return api.proxy_marshal_array_flags(proxy, opcode_of(owner, message), new_interface, version,
                                         0, args.data());
  }

  // 事件处理需被文件内的静态监听器调用 → 公开；其余实现细节保持私有。
  void on_registry_global(std::uint32_t name, const char* interface, std::uint32_t version);
  void on_shm_format(std::uint32_t format);
  void on_wm_base_ping(std::uint32_t serial);
  void on_xdg_surface_configure(std::uint32_t serial);
  void on_toplevel_configure(std::int32_t width, std::int32_t height, WlArray* states);
  void on_toplevel_close();
  void on_buffer_release(WlProxy* buffer);
  void on_decoration_configure(std::uint32_t mode);
  void on_seat_capabilities(std::uint32_t capabilities);
  void on_keyboard_keymap(std::uint32_t format, std::int32_t fd, std::uint32_t size);
  void on_keyboard_key(std::uint32_t serial, std::uint32_t key, std::uint32_t state);
  void on_keyboard_modifiers(std::uint32_t depressed, std::uint32_t latched,
                             std::uint32_t locked, std::uint32_t group);
  void on_pointer_enter(std::uint32_t serial, std::int32_t sx, std::int32_t sy);
  void on_pointer_motion(std::int32_t sx, std::int32_t sy);
  void on_pointer_button(std::uint32_t serial, std::uint32_t button, std::uint32_t state);
  void on_pointer_axis(std::uint32_t axis, std::int32_t value);
  void on_output_scale(std::int32_t factor);
  void on_frame_callback_done(WlProxy* proxy);
  void on_preferred_buffer_scale(std::int32_t factor);

 private:
  [[nodiscard]] auto bind_globals() -> bool;
  [[nodiscard]] auto create_xdg_window(const WindowOptions& options) -> Status;
  [[nodiscard]] auto ensure_buffers() -> Status;
  void release_buffers();
  [[nodiscard]] auto sync_buffers_to_client() -> Status;
  [[nodiscard]] auto commit_frame() -> Status;
  void pump_events();
  [[nodiscard]] auto flush() -> bool;

  void apply_modifiers(ui::Event& event) const;
  [[nodiscard]] auto to_logical(std::int32_t sx, std::int32_t sy) const -> math::Point;
  void teardown();

  // —— 数据 ——
  WlDisplay* display_{nullptr};
  WlProxy* registry_{nullptr};
  WlProxy* compositor_{nullptr};
  WlProxy* shm_{nullptr};
  WlProxy* xdg_wm_base_{nullptr};
  WlProxy* decoration_manager_{nullptr};
  WlProxy* seat_{nullptr};
  WlProxy* keyboard_{nullptr};
  WlProxy* pointer_{nullptr};
  WlProxy* output_{nullptr};
  WlProxy* surface_{nullptr};
  WlProxy* xdg_surface_{nullptr};
  WlProxy* xdg_toplevel_{nullptr};
  WlProxy* decoration_{nullptr};
  WlProxy* frame_callback_{nullptr};
  WlProxy* shm_pool_{nullptr};

  /// 一块共享内存缓冲（池里切出来的一格）。
  struct ShmBuffer {
    WlProxy* buffer{nullptr};
    std::uint32_t* pixels{nullptr};
    bool busy{false};
  };
  std::vector<ShmBuffer> buffers_{};
  void* mapped_{nullptr};
  std::size_t mapped_bytes_{0};
  int shm_fd_{-1};
  std::uint32_t shm_format_{kWlShmFormatXrgb8888};
  int buffer_width_{0};
  int buffer_height_{0};
  int configure_width_{0};
  int configure_height_{0};

  void* xkb_context_{nullptr};
  void* xkb_keymap_{nullptr};
  void* xkb_state_{nullptr};
  std::uint32_t depressed_{0};
  std::uint32_t latched_{0};
  std::uint32_t locked_{0};
  std::uint32_t group_{0};

  std::unique_ptr<raster::Surface> canvas_{};
  std::deque<ui::Event> events_{};
  std::function<void()> repaint_{};

  std::string title_{};
  std::string renderer_{"auto"};
  std::string renderer_name_{"software"};
  std::string renderer_note_{};
  std::string clipboard_cache_{};

  bool decorations_{false};
  bool window_resizable_{true};
  bool shown_{false};
  bool close_requested_{false};
  bool maximized_{false};
  bool size_dirty_{false};
  /// 本帧被改动的区域（逻辑像素；空 = 整帧）——由 `set_present_region` 写入。
  math::IntRect present_region_{};
  /// 上一帧重排上屏的区域尺寸（诊断用；`present_note` 报它）。
  int last_present_width_{0};
  int last_present_height_{0};
  bool resizing_{false};
  bool explicit_scale_{false};
  bool xdg_configured_{false};

  int logical_width_{1280};
  int logical_height_{720};
  float logical_width_exact_{1280.0f};
  float logical_height_exact_{720.0f};
  float scale_{1.0f};
  float server_scale_{1.0f};
  std::uint64_t frames_{0};
  std::uint32_t last_input_serial_{0};
  int click_streak_{0};
  std::chrono::steady_clock::time_point last_press_at_{};
  std::int32_t last_press_x_{0};
  std::int32_t last_press_y_{0};
  std::int32_t last_pointer_x_{0};
  std::int32_t last_pointer_y_{0};
  bool pointer_inside_{false};
};

// ————————————————————————————————————————————————————————————————————————————
// 监听器表
//
// 形态是 `void (**)(void)`：libwayland 用 libffi 按**协议签名**调用它们，因此
// 每个槽位的函数必须与事件参数逐项一致（依据：官方 `convert_arguments_to_ffi`）。
// ⚠ 数组长度必须**恰好等于该接口的 `event_count`**——少了会越界读函数指针
//   （`wl_closure_invoke` 直接按 opcode 索引），多了无害但说明有误解。
// ————————————————————————————————————————————————————————————————————————————

void registry_global(void* data, void* proxy, std::uint32_t name, const char* interface,
                     std::uint32_t version);
void registry_global_remove(void* data, void* proxy, std::uint32_t name);
void shm_format(void* data, void* proxy, std::uint32_t format);
void wm_base_ping(void* data, void* proxy, std::uint32_t serial);
void xdg_surface_configure(void* data, void* proxy, std::uint32_t serial);
void toplevel_configure(void* data, void* proxy, std::int32_t width, std::int32_t height,
                        WlArray* states);
void toplevel_close(void* data, void* proxy);
void toplevel_configure_bounds(void* data, void* proxy, std::int32_t width, std::int32_t height);
void toplevel_wm_capabilities(void* data, void* proxy, WlArray* capabilities);
void decoration_configure(void* data, void* proxy, std::uint32_t mode);
void buffer_release(void* data, void* proxy);
void seat_capabilities(void* data, void* proxy, std::uint32_t capabilities);
void seat_name(void* data, void* proxy, const char* name);
void keyboard_keymap(void* data, void* proxy, std::uint32_t format, std::int32_t fd,
                     std::uint32_t size);
void keyboard_enter(void* data, void* proxy, std::uint32_t serial, void* surface, WlArray* keys);
void keyboard_leave(void* data, void* proxy, std::uint32_t serial, void* surface);
void keyboard_key(void* data, void* proxy, std::uint32_t serial, std::uint32_t time,
                  std::uint32_t key, std::uint32_t state);
void keyboard_modifiers(void* data, void* proxy, std::uint32_t serial, std::uint32_t depressed,
                        std::uint32_t latched, std::uint32_t locked, std::uint32_t group);
void keyboard_repeat_info(void* data, void* proxy, std::int32_t rate, std::int32_t delay);
void pointer_enter(void* data, void* proxy, std::uint32_t serial, void* surface, std::int32_t sx,
                   std::int32_t sy);
void pointer_leave(void* data, void* proxy, std::uint32_t serial, void* surface);
void pointer_motion(void* data, void* proxy, std::uint32_t time, std::int32_t sx, std::int32_t sy);
void pointer_button(void* data, void* proxy, std::uint32_t serial, std::uint32_t time,
                    std::uint32_t button, std::uint32_t state);
void pointer_axis(void* data, void* proxy, std::uint32_t time, std::uint32_t axis,
                  std::int32_t value);
void pointer_frame(void* data, void* proxy);
void pointer_axis_source(void* data, void* proxy, std::uint32_t source);
void pointer_axis_stop(void* data, void* proxy, std::uint32_t time, std::uint32_t axis);
void pointer_axis_discrete(void* data, void* proxy, std::uint32_t axis, std::int32_t discrete);
void pointer_axis_value120(void* data, void* proxy, std::uint32_t axis, std::int32_t value120);
void pointer_axis_relative_direction(void* data, void* proxy, std::uint32_t axis,
                                     std::uint32_t direction);
void output_geometry(void* data, void* proxy, std::int32_t x, std::int32_t y,
                     std::int32_t physical_width, std::int32_t physical_height,
                     std::int32_t subpixel, const char* make, const char* model,
                     std::int32_t transform);
void output_mode(void* data, void* proxy, std::uint32_t flags, std::int32_t width,
                 std::int32_t height, std::int32_t refresh);
void output_done(void* data, void* proxy);
void output_scale(void* data, void* proxy, std::int32_t factor);
void output_name(void* data, void* proxy, const char* name);
void output_description(void* data, void* proxy, const char* description);
void callback_done(void* data, void* proxy, std::uint32_t callback_data);
void surface_enter(void* data, void* proxy, void* output);
void surface_leave(void* data, void* proxy, void* output);
void surface_preferred_buffer_scale(void* data, void* proxy, std::int32_t factor);
void surface_preferred_buffer_transform(void* data, void* proxy, std::uint32_t transform);

template <typename Fn>
[[nodiscard]] constexpr auto slot(Fn fn) -> void (*)(void) {
  return reinterpret_cast<void (*)(void)>(fn);  // lint-allow: L6 libwayland 回调表
}

/// 取后端实例（所有 `add_listener` 都把 `this` 作为 `data` 传入）。
template <typename T>
[[nodiscard]] auto self_of(void* data) -> T& {
  return *static_cast<T*>(data);
}

[[nodiscard]] auto registry_listener() -> void (**)(void) {
  static void (*table[])(void) = {slot(&registry_global), slot(&registry_global_remove)};
  return table;
}
[[nodiscard]] auto shm_listener() -> void (**)(void) {
  static void (*table[])(void) = {slot(&shm_format)};
  return table;
}
[[nodiscard]] auto wm_base_listener() -> void (**)(void) {
  static void (*table[])(void) = {slot(&wm_base_ping)};
  return table;
}
[[nodiscard]] auto xdg_surface_listener() -> void (**)(void) {
  static void (*table[])(void) = {slot(&xdg_surface_configure)};
  return table;
}
[[nodiscard]] auto toplevel_listener() -> void (**)(void) {
  static void (*table[])(void) = {slot(&toplevel_configure), slot(&toplevel_close),
                                  slot(&toplevel_configure_bounds),
                                  slot(&toplevel_wm_capabilities)};
  return table;
}
[[nodiscard]] auto decoration_listener() -> void (**)(void) {
  static void (*table[])(void) = {slot(&decoration_configure)};
  return table;
}
[[nodiscard]] auto buffer_listener() -> void (**)(void) {
  static void (*table[])(void) = {slot(&buffer_release)};
  return table;
}
[[nodiscard]] auto seat_listener() -> void (**)(void) {
  static void (*table[])(void) = {slot(&seat_capabilities), slot(&seat_name)};
  return table;
}
[[nodiscard]] auto surface_listener() -> void (**)(void) {
  static void (*table[])(void) = {slot(&surface_enter), slot(&surface_leave),
                                  slot(&surface_preferred_buffer_scale),
                                  slot(&surface_preferred_buffer_transform)};
  return table;
}
[[nodiscard]] auto keyboard_listener() -> void (**)(void) {
  static void (*table[])(void) = {slot(&keyboard_keymap),  slot(&keyboard_enter),
                                  slot(&keyboard_leave),   slot(&keyboard_key),
                                  slot(&keyboard_modifiers), slot(&keyboard_repeat_info)};
  return table;
}
[[nodiscard]] auto pointer_listener() -> void (**)(void) {
  static void (*table[])(void) = {
      slot(&pointer_enter),     slot(&pointer_leave),          slot(&pointer_motion),
      slot(&pointer_button),    slot(&pointer_axis),           slot(&pointer_frame),
      slot(&pointer_axis_source), slot(&pointer_axis_stop),    slot(&pointer_axis_discrete),
      slot(&pointer_axis_value120), slot(&pointer_axis_relative_direction)};
  return table;
}
[[nodiscard]] auto output_listener() -> void (**)(void) {
  static void (*table[])(void) = {slot(&output_geometry), slot(&output_mode), slot(&output_done),
                                  slot(&output_scale),    slot(&output_name),
                                  slot(&output_description)};
  return table;
}
[[nodiscard]] auto callback_listener() -> void (**)(void) {
  static void (*table[])(void) = {slot(&callback_done)};
  return table;
}

// ————————————————————————————————————————————————————————————————————————————
// 监听器实现（把事件转给后端）
// ————————————————————————————————————————————————————————————————————————————

void registry_global(void* data, void* /*proxy*/, std::uint32_t name, const char* interface,
                     std::uint32_t version) {
  self_of<WaylandBackend>(data).on_registry_global(name, interface, version);
}
void registry_global_remove(void* /*data*/, void* /*proxy*/, std::uint32_t /*name*/) {}
void shm_format(void* data, void* /*proxy*/, std::uint32_t format) {
  self_of<WaylandBackend>(data).on_shm_format(format);
}
void wm_base_ping(void* data, void* /*proxy*/, std::uint32_t serial) {
  self_of<WaylandBackend>(data).on_wm_base_ping(serial);
}
void xdg_surface_configure(void* data, void* /*proxy*/, std::uint32_t serial) {
  self_of<WaylandBackend>(data).on_xdg_surface_configure(serial);
}
void toplevel_configure(void* data, void* /*proxy*/, std::int32_t width, std::int32_t height,
                        WlArray* states) {
  self_of<WaylandBackend>(data).on_toplevel_configure(width, height, states);
}
void toplevel_close(void* data, void* /*proxy*/) { self_of<WaylandBackend>(data).on_toplevel_close(); }
void toplevel_configure_bounds(void* /*data*/, void* /*proxy*/, std::int32_t /*width*/,
                               std::int32_t /*height*/) {}
void toplevel_wm_capabilities(void* /*data*/, void* /*proxy*/, WlArray* /*capabilities*/) {}
void decoration_configure(void* data, void* /*proxy*/, std::uint32_t mode) {
  self_of<WaylandBackend>(data).on_decoration_configure(mode);
}
void buffer_release(void* data, void* proxy) {
  self_of<WaylandBackend>(data).on_buffer_release(proxy);
}
void seat_capabilities(void* data, void* /*proxy*/, std::uint32_t capabilities) {
  self_of<WaylandBackend>(data).on_seat_capabilities(capabilities);
}
void seat_name(void* /*data*/, void* /*proxy*/, const char* /*name*/) {}
void keyboard_keymap(void* data, void* /*proxy*/, std::uint32_t format, std::int32_t fd,
                     std::uint32_t size) {
  self_of<WaylandBackend>(data).on_keyboard_keymap(format, fd, size);
}
void keyboard_enter(void* /*data*/, void* /*proxy*/, std::uint32_t /*serial*/, void* /*surface*/,
                    WlArray* /*keys*/) {}
void keyboard_leave(void* /*data*/, void* /*proxy*/, std::uint32_t /*serial*/, void* /*surface*/) {}
void keyboard_key(void* data, void* /*proxy*/, std::uint32_t serial, std::uint32_t /*time*/,
                  std::uint32_t key, std::uint32_t state) {
  self_of<WaylandBackend>(data).on_keyboard_key(serial, key, state);
}
void keyboard_modifiers(void* data, void* /*proxy*/, std::uint32_t /*serial*/,
                        std::uint32_t depressed, std::uint32_t latched, std::uint32_t locked,
                        std::uint32_t group) {
  self_of<WaylandBackend>(data).on_keyboard_modifiers(depressed, latched, locked, group);
}
void keyboard_repeat_info(void* /*data*/, void* /*proxy*/, std::int32_t /*rate*/,
                          std::int32_t /*delay*/) {}
void pointer_enter(void* data, void* /*proxy*/, std::uint32_t serial, void* /*surface*/,
                   std::int32_t sx, std::int32_t sy) {
  self_of<WaylandBackend>(data).on_pointer_enter(serial, sx, sy);
}
void pointer_leave(void* /*data*/, void* /*proxy*/, std::uint32_t /*serial*/,
                   void* /*surface*/) {}
void pointer_motion(void* data, void* /*proxy*/, std::uint32_t /*time*/, std::int32_t sx,
                    std::int32_t sy) {
  self_of<WaylandBackend>(data).on_pointer_motion(sx, sy);
}
void pointer_button(void* data, void* /*proxy*/, std::uint32_t serial, std::uint32_t /*time*/,
                    std::uint32_t button, std::uint32_t state) {
  self_of<WaylandBackend>(data).on_pointer_button(serial, button, state);
}
void pointer_axis(void* data, void* /*proxy*/, std::uint32_t /*time*/, std::uint32_t axis,
                  std::int32_t value) {
  self_of<WaylandBackend>(data).on_pointer_axis(axis, value);
}
void pointer_frame(void* /*data*/, void* /*proxy*/) {}
void pointer_axis_source(void* /*data*/, void* /*proxy*/, std::uint32_t /*source*/) {}
void pointer_axis_stop(void* /*data*/, void* /*proxy*/, std::uint32_t /*time*/,
                       std::uint32_t /*axis*/) {}
void pointer_axis_discrete(void* /*data*/, void* /*proxy*/, std::uint32_t /*axis*/,
                           std::int32_t /*discrete*/) {}
void pointer_axis_value120(void* /*data*/, void* /*proxy*/, std::uint32_t /*axis*/,
                           std::int32_t /*value120*/) {}
void pointer_axis_relative_direction(void* /*data*/, void* /*proxy*/, std::uint32_t /*axis*/,
                                     std::uint32_t /*direction*/) {}
void output_geometry(void* /*data*/, void* /*proxy*/, std::int32_t /*x*/, std::int32_t /*y*/,
                     std::int32_t /*physical_width*/, std::int32_t /*physical_height*/,
                     std::int32_t /*subpixel*/, const char* /*make*/, const char* /*model*/,
                     std::int32_t /*transform*/) {}
void output_mode(void* /*data*/, void* /*proxy*/, std::uint32_t /*flags*/, std::int32_t /*width*/,
                 std::int32_t /*height*/, std::int32_t /*refresh*/) {}
void output_done(void* /*data*/, void* /*proxy*/) {}
void output_scale(void* data, void* /*proxy*/, std::int32_t factor) {
  self_of<WaylandBackend>(data).on_output_scale(factor);
}
void output_name(void* /*data*/, void* /*proxy*/, const char* /*name*/) {}
void output_description(void* /*data*/, void* /*proxy*/, const char* /*description*/) {}
void callback_done(void* data, void* proxy, std::uint32_t /*callback_data*/) {
  auto& self = self_of<WaylandBackend>(data);
  self.on_frame_callback_done(proxy);
}
void surface_enter(void* /*data*/, void* /*proxy*/, void* /*output*/) {}
void surface_leave(void* /*data*/, void* /*proxy*/, void* /*output*/) {}
void surface_preferred_buffer_scale(void* data, void* /*proxy*/, std::int32_t factor) {
  // `wl_surface.preferred_buffer_scale`（v6）：合成器**建议**的缓冲缩放。
  // 优先级高于 `wl_output.scale`——它表达的是"这块 surface 该用几倍渲染"。
  self_of<WaylandBackend>(data).on_preferred_buffer_scale(factor);
}
void surface_preferred_buffer_transform(void* /*data*/, void* /*proxy*/,
                                        std::uint32_t /*transform*/) {}

}  // namespace

// ————————————————————————————————————————————————————————————————————————————
// 后端实现
// ————————————————————————————————————————————————————————————————————————————

auto WaylandBackend::create_window(const WindowOptions& options) -> Status {
  const WaylandApi& api = wayland_library();
  if (api.handle == nullptr) {
    return unexpected(ErrorCode::Unsupported, "未探测到 libwayland-client");
  }
  const auto* display_env = std::getenv("WAYLAND_DISPLAY");  // lint-allow: L14 平台边界
  if (display_env == nullptr || *display_env == '\0') {
    return unexpected(ErrorCode::Unsupported, "无 WAYLAND_DISPLAY 环境变量（非 Wayland 会话）");
  }
  display_ = api.display_connect(nullptr);  // nullptr = 用 WAYLAND_DISPLAY
  if (display_ == nullptr) {
    return unexpected(ErrorCode::Unsupported,
                      std::format("wl_display_connect 失败（WAYLAND_DISPLAY={}）", display_env));
  }
  decorations_ = options.decorations;
  window_resizable_ = options.resizable;
  logical_width_ = options.width > 0 ? options.width : 1280;
  logical_height_ = options.height > 0 ? options.height : 720;
  renderer_ = options.renderer.empty() ? "auto" : options.renderer;
  // Wayland 的缩放是**整数倍**（`wl_output.scale`）：先按 1 起，拿到输出后按真实值校正。
  // 显式 `--scale`/`ST_SCALE` 完全接管——与 win32/x11 同一语义（0 = 未指定）。
  explicit_scale_ = options.scale > 0.0f;
  scale_ = explicit_scale_ ? options.scale : 1.0f;

  if (!bind_globals()) {
    teardown();
    return unexpected(ErrorCode::Unsupported,
                      "缺少必需的 Wayland 全局对象（wl_compositor / wl_shm / xdg_wm_base）");
  }
  if (!explicit_scale_ && server_scale_ > 0.0f) scale_ = server_scale_;

  if (auto status = create_xdg_window(options); !status) {
    teardown();
    return status;
  }

  WindowOptions resolved = options;
  resolved.scale = scale_;
  resolved.width = logical_width_;
  resolved.height = logical_height_;
  detail::SurfaceChoice choice = detail::create_surface(resolved, renderer_);
  if (choice.surface == nullptr) {
    teardown();
    return unexpected(ErrorCode::Unsupported, "创建绘制面失败");
  }
  canvas_ = std::move(choice.surface);
  renderer_name_ = std::move(choice.name);
  renderer_note_ = std::move(choice.note);
  canvas_->clear(math::Color{0, 0, 0, 0});
  if (auto status = sync_buffers_to_client(); !status) {
    teardown();
    return status;
  }
  ST_LOG_INFO("wayland 窗口已创建（逻辑 {}x{} · 缩放 {:.2f} · 库 {} · 渲染器 {} · 装饰 {}）",
              logical_width_, logical_height_, static_cast<double>(scale_), api.library,
              renderer_name_, decorations_ ? "服务端" : "客户端自绘");
  return ok();
}

auto WaylandBackend::bind_globals() -> bool {
  // `wl_display.get_registry`：接口由客户端指定 → 用 make-new-id 的通用写法
  const proto::Message* get_registry = find_request(proto::kwl_display, "get_registry");
  if (get_registry == nullptr) return false;
  {
    Args args;
    args.new_id();
    registry_ = marshal(display_, proto::kwl_display, get_registry, &proto::kwl_registry,
                        proto::kwl_registry.version, args);
  }
  if (registry_ == nullptr) return false;
  (void)wayland_library().proxy_add_listener(registry_, registry_listener(), this);
  // 第一轮：收服务端的全局对象广告（`global` 事件）
  if (wayland_library().display_roundtrip(display_) < 0) return false;
  // 第二轮：等我们 bind 出来的对象把初始事件发完（`wl_shm.format`/`wl_output.*`/
  // `wl_seat.capabilities`/`xdg_wm_base.ping`）。**两轮是官方推荐做法**：
  // 少了第二轮会出现"格式列表还是空的就建了缓冲"这类时序缺陷。
  if (wayland_library().display_roundtrip(display_) < 0) return false;
  return compositor_ != nullptr && shm_ != nullptr && xdg_wm_base_ != nullptr;
}

auto WaylandBackend::create_xdg_window(const WindowOptions& options) -> Status {
  // 通用建对象：`new_id` 请求 + 挂监听
  // 建对象：`new_id` 的**位置由签名决定**，必须摆在它真正出现的地方。
  // 本函数覆盖的这几条请求（`create_surface(n)`、`get_xdg_surface(no)`、
  // `get_toplevel(n)`、`get_toplevel_decoration(no)`、`get_keyboard(n)`/`get_pointer(n)`）
  // 都是 **new_id 打头、紧随其后才是参数**——所以这里先放 `new_id()` 再放 `extra`。
  // ⚠ 顺序错了 libwayland 会直接报 "null value passed for arg N"（它按签名取参数）——
  //   实测踩过：把 surface 放到 new_id 前面，`get_xdg_surface` 立刻以
  //   "null value passed for arg 1" 失败（要的是对象、收到的却是别的东西）。
  //   签名里 `new_id` 不在首位的请求（如 `wl_registry.bind` 的 `"usun"`）不走本函数，
  //   各自的调用点按签名顺序手工摆（见 `on_registry_global`）。
  const auto make = [&](WlProxy* parent, const proto::Interface& owner, const char* request,
                        const proto::Interface* interface, void (**listener)(void),
                        const Args& extra) -> WlProxy* {
    const proto::Message* message = find_request(owner, request);
    if (message == nullptr) return nullptr;
    Args args;
    args.new_id();
    Args merged = args;
    (void)merged;
    Args combined;
    combined.new_id();
    // 把 extra 的参数接在 new_id 之后（`Args` 只提供追加，故这里逐项搬）
    for (const WlArgument& value : extra.values()) combined.values().push_back(value);
    WlProxy* created = marshal(parent, owner, message, interface, interface->version, combined);
    if (created != nullptr && listener != nullptr) {
      (void)wayland_library().proxy_add_listener(created, listener, this);
    }
    return created;
  };

  surface_ = make(compositor_, proto::kwl_compositor, "create_surface", &proto::kwl_surface,
                  surface_listener(), {});
  if (surface_ == nullptr) return unexpected(ErrorCode::Io, "wl_compositor.create_surface 失败");
  {
    Args args;
    args.obj(surface_);
    xdg_surface_ = make(xdg_wm_base_, proto::kxdg_wm_base, "get_xdg_surface",
                        &proto::kxdg_surface, xdg_surface_listener(), args);
  }
  if (xdg_surface_ == nullptr) {
    return unexpected(ErrorCode::Io, "xdg_wm_base.get_xdg_surface 失败");
  }
  {
    Args args;
    args.obj(xdg_surface_);
    xdg_toplevel_ = make(xdg_surface_, proto::kxdg_surface, "get_toplevel",
                         &proto::kxdg_toplevel, toplevel_listener(), args);
  }
  if (xdg_toplevel_ == nullptr) return unexpected(ErrorCode::Io, "xdg_surface.get_toplevel 失败");

  // 去装饰（自绘窗框的前提）：`xdg-shell` 本身**没有**"不要服务端装饰"的表示，
  // 而 GNOME 默认给 `xdg_toplevel` 加服务端标题栏——`xdg-decoration` 就是补这个缺口。
  if (!decorations_ && decoration_manager_ != nullptr) {
    Args args;
    args.obj(xdg_toplevel_);
    decoration_ = make(decoration_manager_, proto::kzxdg_decoration_manager_v1,
                       "get_toplevel_decoration", &proto::kzxdg_toplevel_decoration_v1,
                       decoration_listener(), args);
    if (decoration_ != nullptr) {
      const proto::Message* set_mode =
          find_request(proto::kzxdg_toplevel_decoration_v1, "set_mode");
      if (set_mode != nullptr) {
        Args mode;
        mode.u(1);  // zxdg_toplevel_decoration_v1_mode::client_side
        (void)marshal(decoration_, proto::kzxdg_toplevel_decoration_v1, set_mode, nullptr, 0, mode);
      }
    } else {
      ST_LOG_WARN("wayland: 未能取得 toplevel decoration 对象（可能仍会显示服务端标题栏）");
    }
  }
  if (!options.title.empty()) set_title(options.title);
  // 应用标识：合成器/任务栏据此归类（Wayland 没有 `_NET_WM_PID` 那类属性，app_id 就是它）
  {
    const proto::Message* set_app_id = find_request(proto::kxdg_toplevel, "set_app_id");
    if (set_app_id != nullptr) {
      Args args;
      args.str("shuangtian");
      (void)marshal(xdg_toplevel_, proto::kxdg_toplevel, set_app_id, nullptr, 0, args);
    }
  }
  // **建窗后必须先 commit 一次（不带缓冲）**：xdg-shell 的握手起手式——
  // 不 commit，合成器永远不会发第一个 `xdg_surface.configure`，窗口就永远不出现。
  {
    const proto::Message* commit = find_request(proto::kwl_surface, "commit");
    if (commit != nullptr) {
      (void)marshal(surface_, proto::kwl_surface, commit, nullptr, 0, {});
    }
  }
  (void)flush();
  // 等 configure（合成器给出尺寸/状态）——这是 xdg-shell 的**强制**握手：
  // 没收到 configure 就 attach 缓冲是协议错误（合成器会直接断开连接）。
  for (int attempt = 0; attempt < 500 && !xdg_configured_; ++attempt) {
    pump_events();
    (void)flush();
    if (!xdg_configured_) {
      const timespec pause{.tv_sec = 0, .tv_nsec = 2'000'000};  // 2ms
      nanosleep(&pause, nullptr);
    }
  }
  if (!xdg_configured_) {
    return unexpected(ErrorCode::Timeout, "未收到 xdg_surface.configure（合成器未响应）");
  }
  return ok();
}

auto WaylandBackend::ensure_buffers() -> Status {
  const WaylandApi& api = wayland_library();
  if (shm_ == nullptr || canvas_ == nullptr) return ok();
  const int width = canvas_->physical_width();
  const int height = canvas_->physical_height();
  const int stride = row_stride(width);
  const std::size_t frame_bytes = static_cast<std::size_t>(stride) *
                                  static_cast<std::size_t>(height);
  if (!buffers_.empty() && buffer_width_ == width && buffer_height_ == height) return ok();
  release_buffers();
  // `MFD_CLOEXEC`：子进程（PTY 里的 shell）不该继承这块内存。
  // `memfd_create`：Linux 上创建可共享匿名内存的标准手段（无需落盘）。
  // 用 glibc 的声明而不用 `syscall(319)`：后者是 x86_64 私有号，换架构（aarch64 是 279）
  // 就静默错——而这是**静默**的，只在别的架构上表现为"打不开窗口"。
  constexpr unsigned int kMfdCloexec{1U};
  shm_fd_ = ::memfd_create("shuangtian-shm", kMfdCloexec);
  if (shm_fd_ < 0) {
    return unexpected(ErrorCode::Io, "memfd_create 失败（wl_shm 需要共享内存）");
  }
  constexpr int kBufferCount = 2;  // 双缓冲：一块在合成器手里时还能画另一块
  const std::size_t pool_bytes = frame_bytes * kBufferCount;
  if (::ftruncate(shm_fd_, static_cast<off_t>(pool_bytes)) != 0) {
    return unexpected(ErrorCode::Io, "ftruncate 失败");
  }
  void* mapped = ::mmap(nullptr, pool_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0);
  if (mapped == MAP_FAILED) {
    return unexpected(ErrorCode::Io, "mmap 共享内存失败");
  }
  {
    const proto::Message* create_pool = find_request(proto::kwl_shm, "create_pool");
    if (create_pool == nullptr) return unexpected(ErrorCode::Unsupported, "wl_shm 无 create_pool");
    Args args;
    args.new_id().fd(shm_fd_).i(static_cast<std::int32_t>(pool_bytes));
    shm_pool_ = marshal(shm_, proto::kwl_shm, create_pool, &proto::kwl_shm_pool,
                        proto::kwl_shm_pool.version, args);
  }
  if (shm_pool_ == nullptr) return unexpected(ErrorCode::Io, "wl_shm.create_pool 失败");
  buffers_.resize(kBufferCount);
  const proto::Message* create_buffer = find_request(proto::kwl_shm_pool, "create_buffer");
  if (create_buffer == nullptr) {
    return unexpected(ErrorCode::Unsupported, "wl_shm_pool 无 create_buffer");
  }
  auto* base = static_cast<std::uint8_t*>(mapped);
  for (int index = 0; index < kBufferCount; ++index) {
    const std::size_t offset = frame_bytes * static_cast<std::size_t>(index);
    Args args;
    args.new_id()
        .i(static_cast<std::int32_t>(offset))
        .i(width)
        .i(height)
        .i(stride)
        .u(shm_format_);
    ShmBuffer& buffer = buffers_[static_cast<std::size_t>(index)];
    buffer.buffer = marshal(shm_pool_, proto::kwl_shm_pool, create_buffer, &proto::kwl_buffer,
                            proto::kwl_buffer.version, args);
    if (buffer.buffer == nullptr) return unexpected(ErrorCode::Io, "create_buffer 失败");
    (void)api.proxy_add_listener(buffer.buffer, buffer_listener(), this);
    buffer.pixels = reinterpret_cast<std::uint32_t*>(base + offset);  // lint-allow: L6 像素视图
    buffer.busy = false;
  }
  mapped_ = mapped;
  mapped_bytes_ = pool_bytes;
  buffer_width_ = width;
  buffer_height_ = height;
  return ok();
}

void WaylandBackend::release_buffers() {
  const WaylandApi& api = wayland_library();
  for (ShmBuffer& buffer : buffers_) {
    if (buffer.buffer != nullptr) {
      api.proxy_destroy(buffer.buffer);
      buffer.buffer = nullptr;
    }
  }
  buffers_.clear();
  if (shm_pool_ != nullptr) {
    api.proxy_destroy(shm_pool_);
    shm_pool_ = nullptr;
  }
  if (mapped_ != nullptr) {
    ::munmap(mapped_, mapped_bytes_);
    mapped_ = nullptr;
    mapped_bytes_ = 0;
  }
  if (shm_fd_ >= 0) {
    ::close(shm_fd_);
    shm_fd_ = -1;
  }
  buffer_width_ = 0;
  buffer_height_ = 0;
}

auto WaylandBackend::sync_buffers_to_client() -> Status {
  if (canvas_ == nullptr) return ok();
  // 真源与 X11 不同：X11 可随时问服务端，Wayland 的尺寸**只能来自 configure**。
  const int width = configure_width_ > 0 ? configure_width_ : logical_width_;
  const int height = configure_height_ > 0 ? configure_height_ : logical_height_;
  const int physical_width = std::max(1, static_cast<int>(
      std::lround(static_cast<double>(width) * static_cast<double>(scale_))));
  const int physical_height = std::max(1, static_cast<int>(
      std::lround(static_cast<double>(height) * static_cast<double>(scale_))));
  logical_width_exact_ = static_cast<float>(width);
  logical_height_exact_ = static_cast<float>(height);
  logical_width_ = width;
  logical_height_ = height;
  if (canvas_->physical_width() != physical_width ||
      canvas_->physical_height() != physical_height) {
    if (auto status = canvas_->resize(physical_width, physical_height); !status) return status;
    canvas_->clear(math::Color{0, 0, 0, 0});
  }
  if (auto status = ensure_buffers(); !status) return status;
  // 告诉合成器"这块内容哪部分是窗口本体"（有阴影/圆角留白时才有区别；我们的帧缓冲
  // 就是整窗）。**必须**在 attach 之前设，否则合成器按默认几何摆放（窗口位置偏移）。
  if (xdg_surface_ != nullptr) {
    const proto::Message* set_geometry = find_request(proto::kxdg_surface, "set_window_geometry");
    if (set_geometry != nullptr) {
      Args args;
      args.i(0).i(0).i(physical_width).i(physical_height);
      (void)marshal(xdg_surface_, proto::kxdg_surface, set_geometry, nullptr, 0, args);
    }
  }
  return ok();
}

auto WaylandBackend::commit_frame() -> Status {
  if (surface_ == nullptr || canvas_ == nullptr || buffers_.empty()) return ok();
  if (!xdg_configured_) return ok();  // 握手未完成前不 attach（协议要求）
  // 找空闲缓冲：都被合成器占用就跳过本帧——**绝不能改写正在被读的缓冲**
  //（改了就是画面撕裂；Wayland 的模型就是靠 `wl_buffer.release` 表达"用完了"）。
  ShmBuffer* target = nullptr;
  for (ShmBuffer& buffer : buffers_) {
    if (!buffer.busy) {
      target = &buffer;
      break;
    }
  }
  if (target == nullptr) return ok();
  // 画布（预乘 0xRRGGBBAA）→ XRGB8888（小端 0x00RRGGBB）：与 X11 的转换**同源**
  // （共用 `simd::convert_rgba_to_xrgb_row`，AVX2/SSE2/标量三路逐像素一致有测试钉住）；
  // 差异只在 alpha 位（XRGB 的 X 被忽略，填 0 或 0xFF 都行——这里保持与旧实现逐字节相同）。
  //
  // ⚠ **本后端不能做“只重排损坏区”的局部上屏**（与 X11 的关键差异）：
  // Wayland 的 `wl_surface` 是**双缓冲**（`ensure_buffers` 建两块，交替 attach），
  // 而每块缓冲都必须是一张**完整帧**——只重排当前 `target` 的损坏区，另一块还是
  // 上一帧（甚至更早）的像素，下一次 attach 它时就是旧内容（交替提交时表现为闪烁）。
  // 协议里确实有 `wl_surface.damage_buffer` + buffer age 这类增量手段，
  // 但那要求客户端能查询缓冲年龄（`wp_presentation`/EGL 扩展），本后端不用它们。
  // X11 之所以能安全局部：它每次都是把同一块画布内容推给服务端，没有“另一块”。
  const std::span<const std::uint32_t> source = canvas_->pixels();
  const std::size_t count = std::min(
      source.size(),
      static_cast<std::size_t>(buffer_width_) * static_cast<std::size_t>(buffer_height_));
  raster::simd::convert_rgba_to_xrgb_row(target->pixels, source.data(), count);
  last_present_width_ = buffer_width_;
  last_present_height_ = buffer_height_;
  const WaylandApi& api = wayland_library();
  const proto::Message* attach = find_request(proto::kwl_surface, "attach");
  const proto::Message* damage = find_request(proto::kwl_surface, "damage");
  const proto::Message* commit = find_request(proto::kwl_surface, "commit");
  const proto::Message* frame = find_request(proto::kwl_surface, "frame");
  if (attach == nullptr || damage == nullptr || commit == nullptr) return ok();
  {
    Args args;
    args.obj(target->buffer).i(0).i(0);
    (void)marshal(surface_, proto::kwl_surface, attach, nullptr, 0, args);
  }
  {
    Args args;
    args.i(0).i(0).i(buffer_width_).i(buffer_height_);
    (void)marshal(surface_, proto::kwl_surface, damage, nullptr, 0, args);
  }
  // 帧回调：合成器下次合成完会发 `done`——既是节流也是"帧真被呈现"的凭据。
  if (frame != nullptr) {
    if (frame_callback_ != nullptr) {
      api.proxy_destroy(frame_callback_);
      frame_callback_ = nullptr;
    }
    Args args;
    args.new_id();
    frame_callback_ = marshal(surface_, proto::kwl_surface, frame, &proto::kwl_callback,
                              proto::kwl_callback.version, args);
    if (frame_callback_ != nullptr) {
      (void)api.proxy_add_listener(frame_callback_, callback_listener(), this);
    }
  }
  (void)marshal(surface_, proto::kwl_surface, commit, nullptr, 0, {});
  target->busy = true;
  ++frames_;
  (void)flush();
  return ok();
}

void WaylandBackend::pump_events() {
  const WaylandApi& api = wayland_library();
  if (display_ == nullptr) return;
  (void)api.display_dispatch_pending(display_);
  if (api.display_prepare_read(display_) != 0) {
    // 队列里还有待派发事件：先派发，不读（prepare 失败表示"读已经在进行"）
    (void)api.display_dispatch_pending(display_);
    return;
  }
  // **非阻塞**读取：用 poll 判断有没有数据可读。
  // 为什么不用 `wl_display_dispatch`：那会阻塞到有事件为止，而应用主循环还要渲染、
  // 跑控制通道、推进动画（`prepare_read`/`read_events`/`cancel_read` 三件套正是
  // libwayland 官方给出的"与自己的事件循环集成"方式）。
  pollfd descriptor{};
  descriptor.fd = api.display_get_fd(display_);
  descriptor.events = POLLIN;
  if (::poll(&descriptor, 1, 0) > 0 && (descriptor.revents & POLLIN) != 0) {
    if (api.display_read_events(display_) != 0) {
      (void)api.display_dispatch_pending(display_);
      return;
    }
  } else {
    api.display_cancel_read(display_);
  }
  (void)api.display_dispatch_pending(display_);
}

auto WaylandBackend::flush() -> bool {
  const WaylandApi& api = wayland_library();
  if (display_ == nullptr) return false;
  // EAGAIN 表示内核缓冲满（下次再冲），不是错误——libwayland 返回 -1 且 errno=EAGAIN
  return api.display_flush(display_) >= 0 || errno == EAGAIN;
}

void WaylandBackend::show_when_ready() {
  // Wayland 没有"映射窗口"这一步：surface 在**首次带内容的 commit** 后出现。
  // 与 win32/x11 同一时序契约：首帧画完再让它可见。
  if (shown_ || xdg_surface_ == nullptr) return;
  shown_ = true;
  (void)commit_frame();
  size_dirty_ = true;  // 让主循环下一帧再画一次（尺寸/内容都对齐）
}

void WaylandBackend::present() {
  pump_events();
  if (xdg_surface_ == nullptr) return;
  if (size_dirty_) {
    size_dirty_ = false;
    (void)sync_buffers_to_client();
  }
  (void)commit_frame();
}

auto WaylandBackend::poll_event() -> std::optional<ui::Event> {
  pump_events();
  if (events_.empty()) return std::nullopt;
  ui::Event event = std::move(events_.front());
  events_.pop_front();
  return event;
}

void WaylandBackend::set_title(std::string_view title) {
  title_ = std::string(title);
  if (xdg_toplevel_ == nullptr) return;
  const proto::Message* message = find_request(proto::kxdg_toplevel, "set_title");
  if (message == nullptr) return;
  Args args;
  args.str(title_.c_str());
  (void)marshal(xdg_toplevel_, proto::kxdg_toplevel, message, nullptr, 0, args);
  (void)flush();
}

auto WaylandBackend::minimize() -> Status {
  if (xdg_toplevel_ == nullptr) return unexpected(ErrorCode::Io, "窗口未创建");
  const proto::Message* message = find_request(proto::kxdg_toplevel, "set_minimized");
  if (message == nullptr) {
    return unexpected(ErrorCode::Unsupported, "xdg_toplevel 无 set_minimized");
  }
  Args args;
  args.i(1);
  (void)marshal(xdg_toplevel_, proto::kxdg_toplevel, message, nullptr, 0, args);
  (void)flush();
  return ok();
}

auto WaylandBackend::toggle_maximize() -> Status {
  // `set_maximized`/`unset_maximized` 是**一对独立请求**（xdg-shell 没有 toggle）。
  // "当前是否最大化"以收到的 **configure 状态**为准（见 `on_toplevel_configure`）——
  // 不能拿"我们发过什么请求"记状态：合成器可以拒绝或改写，改的是它说了算。
  if (xdg_toplevel_ == nullptr) return unexpected(ErrorCode::Io, "窗口未创建");
  const proto::Message* message =
      find_request(proto::kxdg_toplevel, maximized_ ? "unset_maximized" : "set_maximized");
  if (message == nullptr) return unexpected(ErrorCode::Unsupported, "xdg_toplevel 无最大化请求");
  (void)marshal(xdg_toplevel_, proto::kxdg_toplevel, message, nullptr, 0, {});
  (void)flush();
  return ok();
}

auto WaylandBackend::request_close() -> Status {
  // xdg_toplevel **没有**"请求关闭自己"的请求（关闭是合成器发给我们的 `close` 事件）。
  // 与 win32/x11 同姿态：走应用内**同一条收尾路径**——置 `close_requested_`，
  // 由应用主循环退出（进程内还有控制通道/脚本宿主要正常停止）。
  if (xdg_toplevel_ == nullptr) return unexpected(ErrorCode::Io, "窗口未创建");
  close_requested_ = true;
  return ok();
}

auto WaylandBackend::begin_move() -> Status {
  if (xdg_toplevel_ == nullptr) return unexpected(ErrorCode::Io, "窗口未创建");
  if (seat_ == nullptr) return unexpected(ErrorCode::Unsupported, "无 wl_seat（无法发起移动）");
  // 交合成器的手势：`xdg_toplevel.move(seat, serial)`。serial **必须**取自最近一次真实
  // 输入事件——合成器拿它校验"这次请求是否对应一次真实按下"（与 X11 的
  // `_NET_WM_MOVERESIZE` 需要事件时间戳同理，少了它请求被静默丢弃）。
  if (last_input_serial_ == 0) {
    return unexpected(ErrorCode::Unsupported, "尚无输入事件 serial（无法发起移动）");
  }
  const proto::Message* message = find_request(proto::kxdg_toplevel, "move");
  if (message == nullptr) return unexpected(ErrorCode::Unsupported, "xdg_toplevel 无 move");
  Args args;
  args.obj(seat_).u(last_input_serial_);
  (void)marshal(xdg_toplevel_, proto::kxdg_toplevel, message, nullptr, 0, args);
  (void)flush();
  return ok();
}

auto WaylandBackend::begin_resize(WindowEdge edge) -> Status {
  if (xdg_toplevel_ == nullptr) return unexpected(ErrorCode::Io, "窗口未创建");
  if (!window_resizable_) return unexpected(ErrorCode::Unsupported, "窗口声明为不可缩放");
  if (seat_ == nullptr) return unexpected(ErrorCode::Unsupported, "无 wl_seat（无法发起缩放）");
  if (last_input_serial_ == 0) {
    return unexpected(ErrorCode::Unsupported, "尚无输入事件 serial（无法发起缩放）");
  }
  // xdg-shell 的边缘枚举（`xdg_toplevel.resize` 的 edge）：
  //   1=top 2=bottom 4=left 5=top_left 6=bottom_left 8=right 9=top_right 10=bottom_right
  // ⚠ 这是**位掩码式**枚举，与 Win32 的 HT*（13/14/15/16/17…）和 EWMH 的 0..10
  //   方向序号都不同——三套各自对照，抄错的表现是"拖右下角却往左上收"。
  std::uint32_t code = 0;
  switch (edge) {
    case WindowEdge::Top: code = 1; break;
    case WindowEdge::Bottom: code = 2; break;
    case WindowEdge::Left: code = 4; break;
    case WindowEdge::TopLeft: code = 5; break;
    case WindowEdge::BottomLeft: code = 6; break;
    case WindowEdge::Right: code = 8; break;
    case WindowEdge::TopRight: code = 9; break;
    case WindowEdge::BottomRight: code = 10; break;
    case WindowEdge::None: break;
  }
  if (code == 0) return unexpected(ErrorCode::Invalid, "非边缘（无可缩放方向）");
  const proto::Message* message = find_request(proto::kxdg_toplevel, "resize");
  if (message == nullptr) return unexpected(ErrorCode::Unsupported, "xdg_toplevel 无 resize");
  Args args;
  args.obj(seat_).u(last_input_serial_).u(code);
  (void)marshal(xdg_toplevel_, proto::kxdg_toplevel, message, nullptr, 0, args);
  (void)flush();
  return ok();
}

auto WaylandBackend::clipboard_text() -> Result<std::string> {
  // 如实说明范围：Wayland 剪贴板要 `wl_data_device`（data_source/data_offer + MIME 协商，
  // 还需合成器支持）。本版未实现跨进程剪贴板——进程内闭环可用（与 headless 同口径：
  // 无头下"复制→粘贴"必须能用）；跨进程实现见 BACKLOG。
  if (clipboard_cache_.empty()) {
    return unexpected(ErrorCode::Unsupported,
                      "Wayland 跨进程剪贴板尚未实现（本进程内复制粘贴可用）");
  }
  return clipboard_cache_;
}

auto WaylandBackend::set_clipboard_text(std::string_view text) -> Status {
  clipboard_cache_ = std::string(text);
  return ok();
}

auto WaylandBackend::framebuffer() -> raster::Surface& {
  // 与 win32/x11 同一时序结论：尺寸重建在**取画布**处（"画"之前），不在 present。
  if (size_dirty_) {
    size_dirty_ = false;
    (void)sync_buffers_to_client();
  }
  return *canvas_;
}

auto WaylandBackend::set_device_scale(float scale) -> Status {
  if (scale <= 0.0f) return unexpected(ErrorCode::Invalid, "DPI 缩放必须为正数");
  scale_ = scale;
  size_dirty_ = true;
  return sync_buffers_to_client();
}

// —— 事件处理 ——

void WaylandBackend::on_registry_global(std::uint32_t name, const char* interface,
                                        std::uint32_t version) {
  if (interface == nullptr) return;
  const std::string_view iface(interface);
  // 只 bind 需要的全局对象；其余（wl_data_device_manager/dnd 等）本版不使用——
  // bind 了就得欠着实现它的回调。
  const proto::Interface* wanted = nullptr;
  void (**listener)(void) = nullptr;
  if (iface == "wl_compositor" && compositor_ == nullptr) {
    wanted = &proto::kwl_compositor;
  } else if (iface == "wl_shm" && shm_ == nullptr) {
    wanted = &proto::kwl_shm;
    listener = shm_listener();
  } else if (iface == "xdg_wm_base" && xdg_wm_base_ == nullptr) {
    wanted = &proto::kxdg_wm_base;
    listener = wm_base_listener();
  } else if (iface == "zxdg_decoration_manager_v1" && decoration_manager_ == nullptr) {
    wanted = &proto::kzxdg_decoration_manager_v1;
  } else if (iface == "wl_seat" && seat_ == nullptr) {
    wanted = &proto::kwl_seat;
    listener = seat_listener();
  } else if (iface == "wl_output" && output_ == nullptr) {
    wanted = &proto::kwl_output;
    listener = output_listener();
  }
  if (wanted == nullptr) return;
  // `wl_registry.bind` 是"接口由运行时给"的 new_id：额外要 `interface`(string) 与
  // `version`(uint) 两个**隐式参数**（见生成器注释与官方 `emit_messages`）。
  const proto::Message* message = find_request(proto::kwl_registry, "bind");
  if (message == nullptr) return;
  Args args;
  const auto usable_version = static_cast<std::uint32_t>(
      std::min(version, static_cast<std::uint32_t>(wanted->version)));
  args.u(name).str(interface).u(usable_version).new_id();
  WlProxy* bound = marshal(registry_, proto::kwl_registry, message, wanted, usable_version, args);
  if (bound == nullptr) return;
  if (listener != nullptr) {
    (void)wayland_library().proxy_add_listener(bound, listener, this);
  }
  if (iface == "wl_compositor") compositor_ = bound;
  else if (iface == "wl_shm") shm_ = bound;
  else if (iface == "xdg_wm_base") xdg_wm_base_ = bound;
  else if (iface == "zxdg_decoration_manager_v1") decoration_manager_ = bound;
  else if (iface == "wl_seat") seat_ = bound;
  else if (iface == "wl_output") output_ = bound;
}

void WaylandBackend::on_shm_format(std::uint32_t format) {
  // 优先 XRGB8888（不透明 32 位，几乎所有合成器都支持）
  if (format == kWlShmFormatXrgb8888) shm_format_ = format;
}

void WaylandBackend::on_wm_base_ping(std::uint32_t serial) {
  // `xdg_wm_base.ping`：合成器探活，**必须**用 `pong` 回应，否则它认为客户端无响应
  const proto::Message* message = find_request(proto::kxdg_wm_base, "pong");
  if (message == nullptr) return;
  Args args;
  args.u(serial);
  (void)marshal(xdg_wm_base_, proto::kxdg_wm_base, message, nullptr, 0, args);
  (void)flush();
}

void WaylandBackend::on_xdg_surface_configure(std::uint32_t serial) {
  // xdg-shell 握手：收到 configure 必须 `ack_configure`，之后才能 attach 缓冲。
  const proto::Message* message = find_request(proto::kxdg_surface, "ack_configure");
  if (message != nullptr) {
    Args args;
    args.u(serial);
    (void)marshal(xdg_surface_, proto::kxdg_surface, message, nullptr, 0, args);
  }
  xdg_configured_ = true;
}

void WaylandBackend::on_toplevel_configure(std::int32_t width, std::int32_t height,
                                           WlArray* states) {
  // 尺寸：0/0 表示"你自己定"（保持当前逻辑尺寸）
  if (width > 0 && height > 0) {
    configure_width_ = width;
    configure_height_ = height;
  }
  // 状态位（`xdg_toplevel.state`）：1=maximized 2=fullscreen 3=resizing 4=activated
  bool maximized = false;
  if (states != nullptr && states->data != nullptr && states->size > 0) {
    const auto* values = static_cast<const std::uint32_t*>(states->data);  // lint-allow: L6
    const std::size_t count = states->size / sizeof(std::uint32_t);
    for (std::size_t index = 0; index < count; ++index) {
      if (values[index] == 1) maximized = true;
    }
  }
  maximized_ = maximized;
  if (configure_width_ > 0 && configure_height_ > 0) {
    const int previous_w = logical_width_;
    const int previous_h = logical_height_;
    logical_width_exact_ = static_cast<float>(configure_width_);
    logical_height_exact_ = static_cast<float>(configure_height_);
    logical_width_ = configure_width_;
    logical_height_ = configure_height_;
    size_dirty_ = true;
    // 尺寸真变了才驱动重绘（与 X11 的 ConfigureNotify 同姿态：拖动/缩放在合成器侧
    // 是模态的，主循环可能被堵住，这里是唯一的执行机会）。
    if ((configure_width_ != previous_w || configure_height_ != previous_h) &&
        repaint_ != nullptr && !resizing_) {
      resizing_ = true;
      repaint_();
      resizing_ = false;
    }
  }
}

void WaylandBackend::on_toplevel_close() {
  // 合成器要求关闭（用户点了它的关闭按钮/快捷键）——与应用内 request_close 同一条路
  close_requested_ = true;
}

void WaylandBackend::on_buffer_release(WlProxy* buffer) {
  for (ShmBuffer& item : buffers_) {
    if (item.buffer == buffer) {
      item.busy = false;
      return;
    }
  }
}

void WaylandBackend::on_decoration_configure(std::uint32_t mode) {
  // 合成器回报它**实际接受**的模式（可能与我们请求的 client_side 不同）——
  // 不覆盖我们的决定（自绘窗框是框架契约），但记一条日志便于排查外观差异。
  if (mode != 1) {
    ST_LOG_WARN("wayland: 合成器未接受 client_side 装饰（mode={}）——可能仍会画服务端标题栏",
                mode);
  }
}

void WaylandBackend::on_frame_callback_done(WlProxy* proxy) {
  const WaylandApi& api = wayland_library();
  if (frame_callback_ == proxy) {
    api.proxy_destroy(frame_callback_);
    frame_callback_ = nullptr;
  }
}

void WaylandBackend::on_preferred_buffer_scale(std::int32_t factor) {
  // 优先级高于 `wl_output.scale`：它表达的是"这块 surface 该用几倍渲染"。
  // 显式 `--scale` 仍完全接管。
  if (!explicit_scale_ && factor > 0) {
    server_scale_ = static_cast<float>(factor);
    if (scale_ != server_scale_) {
      scale_ = server_scale_;
      size_dirty_ = true;
    }
  }
}

void WaylandBackend::on_seat_capabilities(std::uint32_t capabilities) {
  // `wl_seat.capability`：1=pointer 2=keyboard 4=touch
  const bool has_pointer = (capabilities & 1U) != 0;
  const bool has_keyboard = (capabilities & 2U) != 0;
  const WaylandApi& api = wayland_library();
  const auto make = [&](const char* request, const proto::Interface* interface,
                        void (**listener)(void)) -> WlProxy* {
    const proto::Message* message = find_request(proto::kwl_seat, request);
    if (message == nullptr) return nullptr;
    Args args;
    args.new_id();
    WlProxy* created = marshal(seat_, proto::kwl_seat, message, interface, interface->version, args);
    if (created != nullptr && listener != nullptr) {
      (void)api.proxy_add_listener(created, listener, this);
    }
    return created;
  };
  if (has_keyboard && keyboard_ == nullptr) {
    keyboard_ = make("get_keyboard", &proto::kwl_keyboard, keyboard_listener());
  } else if (!has_keyboard && keyboard_ != nullptr) {
    api.proxy_destroy(keyboard_);
    keyboard_ = nullptr;
  }
  if (has_pointer && pointer_ == nullptr) {
    pointer_ = make("get_pointer", &proto::kwl_pointer, pointer_listener());
  } else if (!has_pointer && pointer_ != nullptr) {
    api.proxy_destroy(pointer_);
    pointer_ = nullptr;
  }
}

void WaylandBackend::on_keyboard_keymap(std::uint32_t format, std::int32_t fd, std::uint32_t size) {
  if (format != kWlKeyboardKeymapFormatXkbV1 || fd < 0) {
    if (fd >= 0) ::close(fd);
    return;
  }
  // keymap 通过 fd 传来（NUL 结尾的 xkb 文本）；映射后即可关闭 fd。
  // ⚠ 必须 `munmap` 而不是 `free`——这是映射，不是堆分配。
  void* mapped = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (mapped == MAP_FAILED) {
    ::close(fd);
    return;
  }
  const XkbApi& xkb = xkb_library();
  if (xkb.handle != nullptr) {
    if (xkb_context_ == nullptr) xkb_context_ = xkb.context_new(kXkbContextNoFlags);
    if (xkb_context_ != nullptr) {
      void* keymap =
          xkb.keymap_new_from_string(xkb_context_, static_cast<const char*>(mapped),
                                     kXkbKeymapFormatTextV1, 0);
      if (keymap != nullptr) {
        if (xkb_state_ != nullptr) xkb.state_unref(xkb_state_);
        if (xkb_keymap_ != nullptr) xkb.keymap_unref(xkb_keymap_);
        xkb_keymap_ = keymap;
        xkb_state_ = xkb.state_new(keymap);
      } else {
        ST_LOG_WARN("wayland: xkb keymap 解析失败（键盘退化为仅键码）");
      }
    }
  } else {
    ST_LOG_WARN("wayland: 未探测到 libxkbcommon（键盘退化为仅键码）");
  }
  ::munmap(mapped, size);
  ::close(fd);
}

void WaylandBackend::on_keyboard_key(std::uint32_t serial, std::uint32_t key, std::uint32_t state) {
  last_input_serial_ = serial;
  ui::Event event;
  event.kind = state == 1 ? ui::EventKind::KeyDown : ui::EventKind::KeyUp;
  // Wayland 的键码 = evdev keycode + 8（xkb 的约定）
  const std::uint32_t keycode = key + 8;
  const XkbApi& xkb = xkb_library();
  std::uint32_t keysym = 0;
  if (xkb.handle != nullptr && xkb_state_ != nullptr) {
    keysym = xkb.state_key_get_one_sym(xkb_state_, keycode);
  }
  event.key = key_name_from_keysym(keysym);
  apply_modifiers(event);
  events_.push_back(std::move(event));
  if (state == 1 && xkb.handle != nullptr && xkb_state_ != nullptr) {
    // 文本输入走 xkb（与 X11 的 `XLookupString` 对应）：比自己按 keysym 拼正确得多，
    // 中文/日文输入法与重音组合键都靠它。
    char buffer[64] = {};
    const int length = xkb.state_key_get_utf8(xkb_state_, keycode, buffer, sizeof(buffer));
    if (length > 0) {
      ui::Event text;
      text.kind = ui::EventKind::TextInput;
      text.text.assign(buffer, static_cast<std::size_t>(length));
      if (!text.text.empty() && static_cast<unsigned char>(text.text[0]) >= 32) {
        apply_modifiers(text);
        events_.push_back(std::move(text));
      }
    }
  }
}

void WaylandBackend::on_keyboard_modifiers(std::uint32_t depressed, std::uint32_t latched,
                                           std::uint32_t locked, std::uint32_t group) {
  depressed_ = depressed;
  latched_ = latched;
  locked_ = locked;
  group_ = group;
  const XkbApi& xkb = xkb_library();
  if (xkb.handle != nullptr && xkb_state_ != nullptr) {
    xkb.state_update_mask(xkb_state_, depressed, latched, locked, 0, 0, group);
  }
}

void WaylandBackend::apply_modifiers(ui::Event& event) const {
  // xkb 的修饰位序与 X11 的 state 位不同（Shift=0, Lock=1, Control=2, Mod1=3, Mod4=6）
  const std::uint32_t effective = depressed_ | latched_ | locked_;
  event.shift = (effective & kXkbModShift) != 0;
  event.ctrl = (effective & kXkbModControl) != 0;
  event.alt = (effective & kXkbModAlt) != 0;
  event.meta = (effective & kXkbModMeta) != 0;
}

void WaylandBackend::on_pointer_enter(std::uint32_t serial, std::int32_t sx, std::int32_t sy) {
  last_input_serial_ = serial;
  pointer_inside_ = true;
  last_pointer_x_ = sx;
  last_pointer_y_ = sy;
  ui::Event event;
  event.kind = ui::EventKind::HoverIn;
  event.position = to_logical(sx, sy);
  apply_modifiers(event);
  events_.push_back(std::move(event));
}

void WaylandBackend::on_pointer_motion(std::int32_t sx, std::int32_t sy) {
  last_pointer_x_ = sx;
  last_pointer_y_ = sy;
  ui::Event event;
  event.kind = ui::EventKind::MouseMove;
  event.position = to_logical(sx, sy);
  apply_modifiers(event);
  events_.push_back(std::move(event));
}

void WaylandBackend::on_pointer_button(std::uint32_t serial, std::uint32_t button,
                                       std::uint32_t state) {
  last_input_serial_ = serial;
  // Wayland 的按钮码是 Linux evdev 码：0x110=左 0x111=右 0x112=中。
  // 框架侧与 Win32 对齐：1=左 2=右 3=中（**顺序不同**，必须换算）。
  int code = 1;
  switch (button) {
    case kBtnLeft: code = 1; break;
    case kBtnRight: code = 2; break;
    case kBtnMiddle: code = 3; break;
    default: code = static_cast<int>(button); break;
  }
  const bool pressed = state == 1;
  ui::Event event;
  event.kind = pressed ? ui::EventKind::MouseDown : ui::EventKind::MouseUp;
  event.position = to_logical(last_pointer_x_, last_pointer_y_);
  event.button = code;
  apply_modifiers(event);
  if (pressed) {
    // 双击/三击：与 X11 后端同一套自算逻辑（Wayland 也没有 Win32 的 `CS_DBLCLKS`）
    const auto now = std::chrono::steady_clock::now();
    const bool near = std::abs(last_pointer_x_ - last_press_x_) <= 4 &&
                      std::abs(last_pointer_y_ - last_press_y_) <= 4;
    const bool quick = (now - last_press_at_) < std::chrono::milliseconds(400);
    click_streak_ = (code == 1 && near && quick) ? click_streak_ + 1 : 1;
    last_press_at_ = now;
    last_press_x_ = last_pointer_x_;
    last_press_y_ = last_pointer_y_;
  }
  events_.push_back(std::move(event));
  if (!pressed && code == 1) {
    // 与 win32/x11 同一份事件序列（Down → Up → Click）：`Element::activate()` 只在
    // `UiRoot::dispatch` 的 `Click` 分支里被调用——只发 Down/Up 会让按钮
    // "有按压效果但点了没反应"。
    ui::Event click;
    click.kind = click_streak_ >= 3 ? ui::EventKind::TripleClick
                : click_streak_ == 2 ? ui::EventKind::DoubleClick
                                     : ui::EventKind::Click;
    click.position = to_logical(last_pointer_x_, last_pointer_y_);
    click.button = 1;
    click.click_count = click_streak_;
    apply_modifiers(click);
    events_.push_back(std::move(click));
  }
}

void WaylandBackend::on_pointer_axis(std::uint32_t axis, std::int32_t value) {
  ui::Event event;
  event.kind = ui::EventKind::Wheel;
  event.position = to_logical(last_pointer_x_, last_pointer_y_);
  apply_modifiers(event);
  // `wl_pointer.axis` 的值是 wl_fixed（8.8 定点）；正值 = **向下/向右**滚
  //（`wayland.xml` 里 axis 的正方向定义）。而 `Event::wheel_delta` 的约定是
  // **向上为正 / 向下为负**——所以这里取负（与 X11 后端 button 4/5 的映射同源）。
  const float delta = -static_cast<float>(value) / 256.0f;
  event.wheel_delta = delta;
  (void)axis;  // 水平/垂直都用同一个字段表达（框架侧口径，与 X11 一致）
  events_.push_back(std::move(event));
}

void WaylandBackend::on_output_scale(std::int32_t factor) {
  if (factor > 0) server_scale_ = static_cast<float>(factor);
}

auto WaylandBackend::to_logical(std::int32_t sx, std::int32_t sy) const -> math::Point {
  return math::Point{static_cast<float>(static_cast<double>(sx) / static_cast<double>(scale_)),
                     static_cast<float>(static_cast<double>(sy) / static_cast<double>(scale_))};
}

void WaylandBackend::teardown() {
  const WaylandApi& api = wayland_library();
  release_buffers();
  if (display_ == nullptr) return;
  const auto drop = [&](WlProxy*& proxy) {
    if (proxy != nullptr) {
      api.proxy_destroy(proxy);
      proxy = nullptr;
    }
  };
  const XkbApi& xkb = xkb_library();
  if (xkb.handle != nullptr) {
    if (xkb_state_ != nullptr) xkb.state_unref(xkb_state_);
    if (xkb_keymap_ != nullptr) xkb.keymap_unref(xkb_keymap_);
    if (xkb_context_ != nullptr) xkb.context_unref(xkb_context_);
  }
  xkb_state_ = nullptr;
  xkb_keymap_ = nullptr;
  xkb_context_ = nullptr;
  drop(frame_callback_);
  drop(decoration_);
  drop(xdg_toplevel_);
  drop(xdg_surface_);
  drop(surface_);
  drop(keyboard_);
  drop(pointer_);
  drop(seat_);
  drop(output_);
  drop(decoration_manager_);
  drop(xdg_wm_base_);
  drop(shm_);
  drop(compositor_);
  drop(registry_);
  api.display_disconnect(display_);
  display_ = nullptr;
  canvas_.reset();
}

}  // namespace

/// 工厂入口（声明见 `shell.hpp`；与 `create_x11_backend`/`create_win32_backend` 同形）。
///
/// 只做"能力探测 + 构造"：真正的可用性判断在 `create_window`（那里才知道
/// `WAYLAND_DISPLAY` 能不能连上、合成器有没有给必需全局对象）——与 X11 侧同姿态。
[[nodiscard]] auto create_wayland_backend() -> Result<std::unique_ptr<Backend>> {
  if (!wayland_available()) {
    return unexpected(ErrorCode::Unsupported, "未探测到 libwayland-client");
  }
  return std::unique_ptr<Backend>(std::make_unique<WaylandBackend>());
}

}  // namespace st::shell

#endif  // !defined(_WIN32)
