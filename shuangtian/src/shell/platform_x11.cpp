// 平台边界：本文件集中封装 X11 窗口与输入 API（`CONVENTIONS.md` §10 第 1 条：
// 系统头 / 平台宏 / `char*` 边界只出现在 `platform_*`）。
//
// ## 为什么**不含 `<X11/*.h>`**，而是手写 ABI + `dlopen`
//
// 框架的硬约束是「系统能力一律**运行时 `dlopen`** 可选加载（见 §10 第 1 条），
// 缺失即回退或明确报错」。X11 天然适合这条：装了桌面就有 `libX11.so.6`，
// 而**开发头文件（`libx11-dev`）在服务器上常常没有**——实测本机（Ubuntu + GNOME）
// 就是这个形态：`libX11.so.6` 在、`/usr/include/X11/Xlib.h` 不在。
// 若改成编译期 `#include <X11/Xlib.h>` + `-lX11`，则：
//   ① 没装 `-dev` 包的机器**根本编不出霜天**（连无头模式都跑不了）；
//   ② 探测到库但符号不全时是链接期失败，而不是"如实报 `Unsupported` 并回退 headless"。
// 这与 Windows 侧同构（那边也是运行时 `LoadLibrary`，只是 Win32 头文件随编译器走，
// 所以看起来像"直接调 API"——真正的判据是**有没有链接期依赖**，不是"写没写函数名"）。
//
// 代价（如实说）：函数签名要自己抄一遍，抄错了是运行期崩溃而不是编译期报错。
// 因此这里只声明本文件真正用到的函数，且**每个都按 Xlib 官方头文件与实现逐项核对**：
//   - `XEvent` 必须是 192 字节（`Xlib.h` 的 `long pad[24]`），已用 `static_assert` 钉住；
//   - `XImage` 字段顺序按 `Xlib.h`（`ImUtil.c` 的 `_XInitImageFuncPtrs` 会按它解释内存）；
//   - 内存所有权规则见 `ensure_backbuffer()` 的注释（这一条抄错的后果是越界崩溃）。
//
// ## 职责（与 `platform_win32.cpp` 同一份契约、逐条对齐）
//
// | 能力 | Win32 | 本文件（X11） |
// |---|---|---|
// | 去装饰建窗 | `WS_POPUP`（去 `WS_CAPTION`） | `_MOTIF_WM_HINTS` decorations=0 |
// | 客户区 == 整窗 | `WM_NCCALCSIZE` 返回 0 | 天然成立（WM 不画框） |
// | 拖动 / 缩放 | `WM_NCLBUTTONDOWN` 模态循环 | `_NET_WM_MOVERESIZE`（EWMH） |
// | 最小化 | `ShowWindow(SW_MINIMIZE)` | `WM_CHANGE_STATE` = IconicState |
// | 最大化切换 | `ShowWindow(SW_MAXIMIZE)` | `_NET_WM_STATE_MAXIMIZED_*` |
// | 关闭 | `PostMessage(WM_CLOSE)` | `WM_DELETE_WINDOW` |
// | 窗口图标 | `WM_SETICON` | **本版未实现**（如实报 `Unsupported`，见下） |
// | 剪贴板 | 打开/写/读剪贴板 | CLIPBOARD 选择（进程内闭环；跨进程见下） |
// | 上屏 | `BitBlt`（DIB） | `XPutImage` |
//
// **本版如实的能力边界**（不假装成功，与 `CONVENTIONS §10 第 7 条` 同姿态）：
//   - 窗口图标（`_NET_WM_ICON`/`WM_HINTS`）：未实现 → 基类默认返回 `Unsupported`；
//   - 跨进程剪贴板读写（完整 ICCCM SelectionRequest/Notify 往返）：未实现，
//     进程内闭环可用（与无头后端同口径）；跨进程路径见 `docs/BACKLOG.md`。
//   两项都不影响"界面能起、能被控制通道驱动、能截图验证"这条主线。

#if !defined(_WIN32)

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <dlfcn.h>  // lint-allow: L14 本文件即 platform_* 单点封装（§10 第 1 条）
#include <sys/ipc.h>   // lint-allow: L14 MIT-SHM 共享内存（System V IPC）
#include <sys/shm.h>   // lint-allow: L14 同上

#include "st/core/error.hpp"
#include "st/core/log.hpp"
#include "st/core/process.hpp"
#include "st/math/geometry.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/simd.hpp"
#include "st/shell/present_slots.hpp"
#include "st/shell/shell.hpp"
#include "st/ui/window_control.hpp"
#include "backend_util.hpp"

namespace st::shell {
namespace {

// ————————————————————————————————————————————————————————————————————————————
// Xlib 最小 ABI（只声明用到的部分；不自带任何系统头）
// ————————————————————————————————————————————————————————————————————————————

using XDisplay = void;      ///< opaque `Display`
using XId = unsigned long;  ///< `Window`/`Atom`/`Pixmap`/`KeySym`（都是 XID）
using XTime = unsigned long;
using XPointer = char*;

/// X 事件联合体。**尺寸是 ABI 的一部分**：`Xlib.h` 里是 `long pad[24]`，
/// x86_64 = 192 字节。开小了 `XNextEvent` 会写出界（内存破坏，不是花屏）。
struct XEvent {
  int type;
  std::int64_t pad[23];
};

/// 键/按钮/指针事件共用的前 96 字节。
///
/// 为什么一条结构体覆盖三种事件：`XKeyEvent`/`XButtonEvent`/`XMotionEvent` 在 Xlib 里
/// 是**同布局的三个名字**（`x`/`y`/`state` 位置完全一致，只有尾字段语义不同）。
/// 分开写三份只会让"抄错一处"变成三倍的机会。
struct XInputEvent {
  int type;
  unsigned long serial;
  int send_event;
  XDisplay* display;
  XId window;
  XId root;
  XId subwindow;
  XTime time;
  int x;
  int y;
  int x_root;
  int y_root;
  unsigned int state;
  unsigned int detail;  ///< `keycode` / `button` / `is_hint`
  int same_screen;
};

struct XExposeEvent {
  int type;
  unsigned long serial;
  int send_event;
  XDisplay* display;
  XId window;
  int x;
  int y;
  int width;
  int height;
  int count;
};

struct XConfigureEvent {
  int type;
  unsigned long serial;
  int send_event;
  XDisplay* display;
  XId event;
  XId window;
  int x;
  int y;
  int width;
  int height;
  int border_width;
  XId above;
  int override_redirect;
};

struct XClientMessageEvent {
  int type;
  unsigned long serial;
  int send_event;
  XDisplay* display;
  XId window;
  XId message_type;
  int format;
  union {
    char b[20];
    short s[10];
    long l[5];
  } data;
};

struct XPropertyEvent {
  int type;
  unsigned long serial;
  int send_event;
  XDisplay* display;
  XId window;
  XId atom;
  XTime time;
  int state;
};

struct XSelectionClearEvent {
  int type;
  unsigned long serial;
  int send_event;
  XDisplay* display;
  XId window;
  XId selection;
  XTime time;
};

/// `XWindowAttributes`：只用到 geometry/depth/visual，其余按 ABI 占位。
struct XWindowAttributes {
  int x;
  int y;
  int width;
  int height;
  int border_width;
  int depth;
  void* visual;
  XId root;
  int window_class;
  int bit_gravity;
  int win_gravity;
  int backing_store;
  unsigned long backing_planes;
  unsigned long backing_pixel;
  int save_under;
  XId colormap;
  int map_installed;
  int map_state;
  long all_event_masks;
  long your_event_mask;
  long do_not_propagate_mask;
  int override_redirect;
  void* screen;
};

/// `XImage`：`XPutImage` 的载体。字段顺序**必须**与 `Xlib.h` 一致——
/// Xlib 用 `bytes_per_line`/`bits_per_pixel`/`byte_order` 解释 `data`；
/// 错一个是屏幕上"斜着的彩色条纹"（而不是报错）。生产途径是 `XCreateImage`。
struct XImage {
  int width;
  int height;
  int xoffset;
  int format;
  char* data;
  int byte_order;
  int bitmap_unit;
  int bitmap_bit_order;
  int bitmap_pad;
  int depth;
  int bytes_per_line;
  int bits_per_pixel;
  unsigned long red_mask;
  unsigned long green_mask;
  unsigned long blue_mask;
  void* obdata;
  struct Funcs {
    void* create_image;
    void* destroy_image;
    void* get_pixel;
    void* put_pixel;
    void* sub_image;
    void* add_pixel;
  } f;
};

/// `XClassHint`（`XSetClassHint`）。
struct XClassHint {
  char* res_name;
  char* res_class;
};

/// `XSetWindowAttributes`（`XCreateWindow`）。本文件只用 `background_pixel` 与
/// `event_mask`；**结构体大小必须对**（Xlib 按 `valuemask` 读其中若干字段）。
struct XSetWindowAttributes {
  XId background_pixmap;
  unsigned long background_pixel;
  XId border_pixmap;
  unsigned long border_pixel;
  int bit_gravity;
  int win_gravity;
  int backing_store;
  unsigned long backing_planes;
  unsigned long backing_pixel;
  int save_under;
  long event_mask;
  long do_not_propagate_mask;
  int override_redirect;
  XId colormap;
  void* cursor;
};

using XErrorHandler = int (*)(XDisplay*, XEvent*);

/// `XShmSegmentInfo`（`X11/extensions/XShm.h`）：共享内存段的四元组。
///
/// 字段顺序与 `XShm.h` 逐项一致——Xlib 按它解释 `shmaddr` 与 `shmseg`，
/// 抄错一处（比如把 `read_only` 提前）会把段 ID 读成指针（直接崩）。
struct XShmSegmentInfo {
  XId shmseg;   ///< 服务端侧的段标识（`XShmAttach` 后由服务端填）
  int shmid;    ///< 客户端 System V 共享内存 ID（`shmget` 的返回值）
  char* shmaddr;
  int read_only;
};

/// `XShmCompletionEvent`（`X11/extensions/shmstr.h`）：共享段“已被服务端读走”的回执。
///
/// 字段布局**按实测钉住**：在本机 Xwayland `:0` 上收一枚真实完成事件，逐 8 字节槽位比对得出
/// `serial`=8 / `send_event`=16 / `display`=24 / `drawable`=32 / `major_code`=40 /
/// `minor_code`=44 / **`shmseg`=48**（8 字节对齐，不紧接 44）/ `offset`=56 / `data`=64。
/// 抄错一处就是把段 ID 读成别的字段——而失效方式是“完成事件永远认不出自己那一块”，
/// 表现为上屏静默退化（不崩、不报错），是最难查的那类手写 ABI 错误。
struct XShmCompletionEvent {
  int type;
  unsigned long serial;
  int send_event;
  XDisplay* display;
  XId drawable;
  int major_code;
  int minor_code;
  XId shmseg;
  unsigned long offset;
  unsigned long data;
};

static_assert(offsetof(XShmCompletionEvent, shmseg) == 48,
              "XShmCompletionEvent.shmseg 必须在偏移 48（X 协议 + 8 字节对齐）");
static_assert(offsetof(XShmCompletionEvent, drawable) == 32,
              "XShmCompletionEvent.drawable 必须在偏移 32");

/// libXext（MIT-SHM 扩展）函数表。**独立于 libX11**：扩展库不在时不影响
/// X11 后端本身可用（上屏回退到 `XPutImage` 并如实记原因）——这正是
/// 「能力运行期探测、缺失如实回退」的同一条姿态。
struct XextApi {
  std::string library{};
  void* handle{nullptr};

  int (*query_extension)(XDisplay*, int*, int*, int*);
  XImage* (*create_image)(XDisplay*, void*, unsigned int, int, char*, XShmSegmentInfo*,
                          unsigned int, unsigned int);
  int (*attach)(XDisplay*, XShmSegmentInfo*);
  int (*detach)(XDisplay*, XShmSegmentInfo*);
  int (*put_image)(XDisplay*, XId, void*, XImage*, int, int, int, int, unsigned int, unsigned int,
                   int);
  int (*get_event_base)(XDisplay*);
};

/// libX11 函数表：`dlopen` 一次，全进程共用。
///
/// 取符号的策略：**一次性取全**。缺任何一个就整体视为不可用——
/// 否则"库版本太旧少了某个函数"会变成**运行到某条路径才崩**（空函数指针调用），
/// 而不是启动时一句明确的话。
struct XlibApi {
  std::string library{};
  void* handle{nullptr};

  XDisplay* (*open_display)(const char*);
  int (*close_display)(XDisplay*);
  int (*default_screen)(XDisplay*);
  XId (*root_window)(XDisplay*, int);
  void* (*default_visual)(XDisplay*, int);
  int (*default_depth)(XDisplay*, int);
  unsigned long (*black_pixel)(XDisplay*, int);
  XId (*create_window)(XDisplay*, XId, int, int, unsigned int, unsigned int, unsigned int, int,
                       unsigned int, void*, unsigned long, XSetWindowAttributes*);
  int (*destroy_window)(XDisplay*, XId);
  int (*map_window)(XDisplay*, XId);
  int (*resize_window)(XDisplay*, XId, unsigned int, unsigned int);
  int (*flush)(XDisplay*);
  int (*sync)(XDisplay*, int);
  int (*pending)(XDisplay*);
  int (*next_event)(XDisplay*, XEvent*);
  XId (*intern_atom)(XDisplay*, const char*, int);
  unsigned char* (*get_atom_name)(XDisplay*, XId);
  int (*change_property)(XDisplay*, XId, XId, XId, int, int, const unsigned char*, int);
  // 官方签名（Xlib.h）：
  //   int XGetWindowProperty(Display*, Window, Atom property, long long_offset,
  //       long long_length, Bool delete, Atom req_type, Atom* actual_type_return,
  //       int* actual_format_return, unsigned long* nitems_return,
  //       unsigned long* bytes_after_return, unsigned char** prop_return);
  // ⚠ **`req_type` 不能漏**：漏抄一位就是整条参数链错位（读出垃圾或直接崩），
  //   而且因为返回 0 而看起来"属性不存在"——最难查的那类手写 ABI 错误。
  int (*get_window_property)(XDisplay*, XId, XId, long, long, int, XId, XId*, int*,
                             unsigned long*, unsigned long*, unsigned char**);
  int (*get_window_attributes)(XDisplay*, XId, XWindowAttributes*);
  int (*set_input_focus)(XDisplay*, int, XId, XTime);
  int (*set_class_hint)(XDisplay*, XId, XClassHint*);
  int (*set_selection_owner)(XDisplay*, XId, XId, XTime);
  XId (*get_selection_owner)(XDisplay*, XId);
  int (*store_name)(XDisplay*, XId, const char*);
  void* (*create_gc)(XDisplay*, XId, unsigned long, void*);
  int (*free_gc)(XDisplay*, void*);
  XImage* (*create_image)(XDisplay*, void*, unsigned int, int, int, char*, unsigned int,
                          unsigned int, int, int);
  int (*destroy_image)(XImage*);
  int (*put_image)(XDisplay*, XId, void*, XImage*, int, int, int, int, unsigned int, unsigned int);
  int (*send_event)(XDisplay*, XId, int, long, XEvent*);
  int (*lookup_string)(XEvent*, char*, int, XId*, void*);
  XId (*keycode_to_keysym)(XDisplay*, unsigned int, int);
  int (*queue_length)(XDisplay*);
  char* (*resource_manager_string)(XDisplay*);
  XErrorHandler (*set_error_handler)(XErrorHandler);
  int (*free_memory)(void*);  ///< `XFree`
};

template <typename Fn>
[[nodiscard]] auto load_symbol(void* handle, const char* name, Fn& out) -> bool {
  out = reinterpret_cast<Fn>(::dlsym(handle, name));  // lint-allow: L6 dlopen 符号
  return out != nullptr;
}

/// 探测并加载 libX11。
///
/// 探测结果（含失败）**缓存一次**：`probe_backend()` 与 `create_backend()` 都会问，
/// 而两处给不同说法是最难查的一类不一致（"探测说能用、创建说没有"）。
/// 同时安装进程级 X 错误处理器：Xlib 的默认处理器遇到协议错误会**直接退出进程**
/// （`exit(1)`，连控制通道都来不及收尾）——而"窗口管理器不支持某个属性"这类错误
/// 在真实桌面里是常态，绝不能让它杀掉应用。
[[nodiscard]] auto x11_library() -> const XlibApi& {
  static const XlibApi instance = [] {
    XlibApi api;
    const std::string name = detail::library_available("libX11.so.6:libX11.so");
    if (name.empty()) return api;
    void* handle = ::dlopen(name.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (handle == nullptr) return api;
    const bool ok =
        load_symbol(handle, "XOpenDisplay", api.open_display) &&
        load_symbol(handle, "XCloseDisplay", api.close_display) &&
        load_symbol(handle, "XDefaultScreen", api.default_screen) &&
        load_symbol(handle, "XRootWindow", api.root_window) &&
        load_symbol(handle, "XDefaultVisual", api.default_visual) &&
        load_symbol(handle, "XDefaultDepth", api.default_depth) &&
        load_symbol(handle, "XBlackPixel", api.black_pixel) &&
        load_symbol(handle, "XCreateWindow", api.create_window) &&
        load_symbol(handle, "XDestroyWindow", api.destroy_window) &&
        load_symbol(handle, "XMapWindow", api.map_window) &&
        load_symbol(handle, "XResizeWindow", api.resize_window) &&
        load_symbol(handle, "XFlush", api.flush) &&
        load_symbol(handle, "XSync", api.sync) &&
        load_symbol(handle, "XPending", api.pending) &&
        load_symbol(handle, "XNextEvent", api.next_event) &&
        load_symbol(handle, "XInternAtom", api.intern_atom) &&
        load_symbol(handle, "XGetAtomName", api.get_atom_name) &&
        load_symbol(handle, "XChangeProperty", api.change_property) &&
        load_symbol(handle, "XGetWindowProperty", api.get_window_property) &&
        load_symbol(handle, "XGetWindowAttributes", api.get_window_attributes) &&
        load_symbol(handle, "XSetInputFocus", api.set_input_focus) &&
        load_symbol(handle, "XSetClassHint", api.set_class_hint) &&
        load_symbol(handle, "XSetSelectionOwner", api.set_selection_owner) &&
        load_symbol(handle, "XGetSelectionOwner", api.get_selection_owner) &&
        load_symbol(handle, "XStoreName", api.store_name) &&
        load_symbol(handle, "XCreateGC", api.create_gc) &&
        load_symbol(handle, "XFreeGC", api.free_gc) &&
        load_symbol(handle, "XCreateImage", api.create_image) &&
        load_symbol(handle, "XDestroyImage", api.destroy_image) &&
        load_symbol(handle, "XPutImage", api.put_image) &&
        load_symbol(handle, "XSendEvent", api.send_event) &&
        load_symbol(handle, "XLookupString", api.lookup_string) &&
        load_symbol(handle, "XkbKeycodeToKeysym", api.keycode_to_keysym) &&
        load_symbol(handle, "XQLength", api.queue_length) &&
        load_symbol(handle, "XResourceManagerString", api.resource_manager_string) &&
        load_symbol(handle, "XSetErrorHandler", api.set_error_handler) &&
        load_symbol(handle, "XFree", api.free_memory);
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

[[nodiscard]] auto x11_available() -> bool { return x11_library().handle != nullptr; }

/// 探测并加载 libXext 的 MIT-SHM 符号。
///
/// 与 libX11 同一策略（一次取全、缺一即不可用）——它只是**可选加速**：
/// 拿不到就走 `XPutImage`，如实记原因（与「能力运行期探测、缺失如实回退」同姿态）。
[[nodiscard]] auto xext_library() -> const XextApi& {
  static const XextApi instance = [] {
    XextApi api;
    const std::string name = detail::library_available("libXext.so.6:libXext.so");
    if (name.empty()) return api;
    void* handle = ::dlopen(name.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (handle == nullptr) return api;
    const bool ok = load_symbol(handle, "XShmQueryExtension", api.query_extension) &&
                    load_symbol(handle, "XShmCreateImage", api.create_image) &&
                    load_symbol(handle, "XShmAttach", api.attach) &&
                    load_symbol(handle, "XShmDetach", api.detach) &&
                    load_symbol(handle, "XShmPutImage", api.put_image) &&
                    load_symbol(handle, "XShmGetEventBase", api.get_event_base);
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

// ————————————————————————————————————————————————————————————————————————————
// X11 协议常量（`X11/X.h` / `X11/keysymdef.h` 的数值）
//
// 为什么照抄数值而不是 `#include`：这些是 **X 协议**的一部分（不是某版本实现细节），
// 跨版本稳定；而 `CONVENTIONS §8 L3` 禁函数式宏（那些头文件里恰好都是宏）。
// 每个常量都注明出处，便于核对。
// ————————————————————————————————————————————————————————————————————————————

constexpr int kKeyPress{2};
constexpr int kKeyRelease{3};
constexpr int kButtonPress{4};
constexpr int kButtonRelease{5};
constexpr int kMotionNotify{6};
constexpr int kEnterNotify{7};
constexpr int kLeaveNotify{8};
constexpr int kFocusIn{9};
constexpr int kFocusOut{10};
constexpr int kExpose{12};
constexpr int kConfigureNotify{22};
constexpr int kPropertyNotify{28};
constexpr int kSelectionClear{29};
constexpr int kSelectionRequest{30};
constexpr int kClientMessage{33};
/// MIT-SHM 完成事件：`XShmGetEventBase(display) + ShmCompletion`（`shmstr.h` 里
/// `ShmCompletion = 0`）。基址是**运行期**取值（每个显示不同），不能当常量写死。
constexpr int kShmCompletionOffset{0};

constexpr long kKeyPressMask{1L << 0};
constexpr long kKeyReleaseMask{1L << 1};
constexpr long kButtonPressMask{1L << 2};
constexpr long kButtonReleaseMask{1L << 3};
constexpr long kEnterWindowMask{1L << 4};
constexpr long kLeaveWindowMask{1L << 5};
constexpr long kPointerMotionMask{1L << 6};
constexpr long kExposureMask{1L << 15};
constexpr long kStructureNotifyMask{1L << 17};
constexpr long kFocusChangeMask{1L << 21};
constexpr long kPropertyChangeMask{1L << 22};
/// `SubstructureRedirectMask`：WM 在 root 上选的掩码。发给 root 的 client message
/// 必须带它，否则窗口管理器收不到（`_NET_WM_MOVERESIZE` 就静默失效）。
constexpr long kSubstructureRedirectMask{1L << 20};

constexpr XId kAtomCardinal{6};
constexpr XId kAtomAtom{4};
constexpr XId kAtomString{31};
constexpr XId kAnyPropertyType{0L};
constexpr int kPropModeReplace{0};

constexpr unsigned int kShiftMask{1U << 0};
constexpr unsigned int kControlMask{1U << 2};
constexpr unsigned int kMod1Mask{1U << 3};  ///< 通常映射到 Alt
constexpr unsigned int kMod4Mask{1U << 6};  ///< 通常映射到 Super（"Win 键"）

/// `XSetWindowAttributes` 的 `valuemask` 位（`X11/X.h`）。
constexpr unsigned long kCWBackPixel{1UL << 1};
constexpr unsigned long kCWEventMask{1UL << 11};

/// `XImage` 的 `format`（`X11/X.h`）：`XYBitmap`=0 / `XYPixmap`=1 / `ZPixmap`=2。
constexpr int kZPixmap{2};

/// `dlsym` 取到的函数指针一旦为空就不该调用——统一走这个可达性检查。
[[nodiscard]] auto x11_ready(const XlibApi& x) -> bool {
  return x.handle != nullptr && x.open_display != nullptr;
}

}  // namespace
}  // namespace st::shell

#endif  // !defined(_WIN32)

#if !defined(_WIN32)

namespace st::shell {
namespace {

// ————————————————————————————————————————————————————————————————————————————
// 键盘：X11 键符号 → 框架键名
// ————————————————————————————————————————————————————————————————————————————

// `X11/keysymdef.h` 的数值（协议常量，跨版本稳定）
constexpr XId kXkBackSpace{0xFF08};
constexpr XId kXkTab{0xFF09};
constexpr XId kXkReturn{0xFF0D};
constexpr XId kXkEscape{0xFF1B};
constexpr XId kXkHome{0xFF50};
constexpr XId kXkLeft{0xFF51};
constexpr XId kXkUp{0xFF52};
constexpr XId kXkRight{0xFF53};
constexpr XId kXkDown{0xFF54};
constexpr XId kXkPrior{0xFF55};  ///< PageUp
constexpr XId kXkNext{0xFF56};   ///< PageDown
constexpr XId kXkEnd{0xFF57};
constexpr XId kXkInsert{0xFF63};
constexpr XId kXkKPEnter{0xFF8D};
constexpr XId kXkDelete{0xFFFF};
constexpr XId kXkFunction{0xFFBE};  ///< F1；Fn = kXkFunction + (n - 1)

/// 键符号 → 框架键名（空串 = "这个键没有名字"，事件仍照发）。
///
/// **必须与 Win32 的 `key_name` 逐项一致**：组件层拿 `event.key` 与字符串字面量比
/// （`"ArrowLeft"`/`"PageUp"`/`"Enter"`/`"Backspace"`…）。两个平台给不同的名字，
/// 就会出现"同一个界面在 Linux 上按键没反应"——而只造 `ui::Event` 的单元测试
/// **永远看不出来**（它不经过键名生成）。
[[nodiscard]] auto key_name_from_keysym(XId keysym) -> std::string {
  // 可打印 ASCII（含字母）与 keysym 数值重合：字母统一小写（与 Win32 同口径）
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

/// X11 的 `state` 位 → `ui::Event` 的修饰键（与 Win32 的 `GetKeyState` 口径一致）。
void apply_modifiers(ui::Event& event, unsigned int state) {
  event.ctrl = (state & kControlMask) != 0;
  event.shift = (state & kShiftMask) != 0;
  event.alt = (state & kMod1Mask) != 0;
  event.meta = (state & kMod4Mask) != 0;
}

/// `XEvent` 的联合体视图（L6：位级重解释只在本平台文件里发生）。
/// 调用方**必须先确认 `event.type`**（Xlib 的常规用法，本函数不额外校验）。
template <typename T>
[[nodiscard]] auto as(const XEvent& event) -> const T& {
  return *reinterpret_cast<const T*>(&event);  // lint-allow: L6 X11 联合体视图
}

template <typename T>
[[nodiscard]] auto as_mutable(XEvent& event) -> T& {
  return *reinterpret_cast<T*>(&event);  // lint-allow: L6 X11 联合体视图
}

// ————————————————————————————————————————————————————————————————————————————
// 后端
// ————————————————————————————————————————————————————————————————————————————

/// X11 窗口后端（与 `Win32Backend` 同一契约，见文件头的能力对照表）。
class X11Backend final : public Backend {
 public:
  /// 一块共享段槽：段 + 它的 `XImage` 视图（两者一起建、一起拆）。
  struct ShmSlot {
    XShmSegmentInfo segment{};
    XImage* image{nullptr};
  };

  X11Backend() = default;
  ~X11Backend() override { teardown(); }
  [[nodiscard]] auto name() const noexcept -> std::string_view override { return "x11"; }
  [[nodiscard]] auto headless() const noexcept -> bool override { return false; }
  [[nodiscard]] auto close_requested() const noexcept -> bool override { return close_requested_; }

  auto create_window(const WindowOptions& options) -> Status override {
    const XlibApi& x = x11_library();
    if (!x11_ready(x)) {
      return unexpected(ErrorCode::Unsupported, "未探测到 libX11（无 X 显示服务）");
    }
    const auto* display_env = std::getenv("DISPLAY");  // lint-allow: L14 平台边界读环境
    if (display_env == nullptr || *display_env == '\0') {
      return unexpected(ErrorCode::Unsupported, "无 DISPLAY 环境变量（无桌面会话）");
    }
    display_ = x.open_display(nullptr);  // nullptr = 用 DISPLAY
    if (display_ == nullptr) {
      return unexpected(ErrorCode::Unsupported,
                        std::format("XOpenDisplay 失败（DISPLAY={}）", display_env));
    }
    install_error_handler();

    decorations_ = options.decorations;
    window_resizable_ = options.resizable;
    logical_width_ = options.width > 0 ? options.width : 1280;
    logical_height_ = options.height > 0 ? options.height : 720;
    renderer_ = options.renderer.empty() ? "auto" : options.renderer;

    // DPI：显式 `--scale`/`ST_SCALE` 完全接管；否则读 Xft.dpi。
    // 语义与 Win32 侧同源（那边是 `options.scale > 0` 才算显式）——**不要在 app 层
    // 预先填 1.0**，否则每个窗口都被当成"显式 1x"（`app.cpp` 有这条教训的注释）。
    const bool explicit_scale = options.scale > 0.0f;
    scale_ = explicit_scale ? options.scale : query_display_scale();
    if (!(scale_ > 0.0f)) scale_ = 1.0f;

    const int screen = x.default_screen(display_);
    root_ = x.root_window(display_, screen);
    visual_ = x.default_visual(display_, screen);
    depth_ = x.default_depth(display_, screen);
    if (depth_ <= 0) depth_ = 24;

    const int physical_width = static_cast<int>(
        std::lround(static_cast<double>(logical_width_) * static_cast<double>(scale_)));
    const int physical_height = static_cast<int>(
        std::lround(static_cast<double>(logical_height_) * static_cast<double>(scale_)));

    // 建窗：用 `XCreateWindow`（而非 `XCreateSimpleWindow`）以便一次带上 event_mask 与
    // background_pixel——少一次往返，且避免"窗口已映射才选事件"的漏事件时序。
    const long event_mask = kKeyPressMask | kKeyReleaseMask | kButtonPressMask |
                            kButtonReleaseMask | kPointerMotionMask | kEnterWindowMask |
                            kLeaveWindowMask | kExposureMask | kStructureNotifyMask |
                            kFocusChangeMask | kPropertyChangeMask;
    XSetWindowAttributes attributes{};
    attributes.background_pixel = x.black_pixel(display_, screen);
    attributes.event_mask = event_mask;
    window_ = x.create_window(display_, root_, 0, 0, static_cast<unsigned int>(physical_width),
                              static_cast<unsigned int>(physical_height), 0, depth_, 0, visual_,
                              kCWBackPixel | kCWEventMask, &attributes);
    if (window_ == 0) {
      teardown();
      return unexpected(ErrorCode::Io, "XCreateWindow 失败");
    }
    gc_ = x.create_gc(display_, window_, 0, nullptr);
    if (gc_ == nullptr) {
      teardown();
      return unexpected(ErrorCode::Io, "XCreateGC 失败");
    }

    resolve_atoms();
    // 去装饰（自绘窗框的前提）：`_MOTIF_WM_HINTS` 是 X11 上**唯一被主流 WM 广泛支持**
    // 的"无边框"协商手段（GNOME/KDE/XFCE 均支持；纯 EWMH 无对应标准属性）。
    // 实测：GNOME（Mutter）尊重它——设置后 `_NET_FRAME_EXTENTS` 不再存在，
    // 即 WM 不为该窗口画任何边框。`decorations=true`（宿主显式要系统窗框）时不设。
    if (!decorations_) apply_no_decorations();
    x.set_class_hint(display_, window_, make_class_hint());
    x.store_name(display_, window_, options.title.empty() ? "霜天应用" : options.title.c_str());
    set_wm_protocols();
    if (!options.title.empty()) set_net_wm_name(options.title);
    set_net_wm_pid();

    // 绘制面：与 headless / win32 **同一套选型逻辑**（`auto` = 实测更快的那条）
    WindowOptions resolved = options;
    resolved.scale = scale_;
    resolved.width = logical_width_;
    resolved.height = logical_height_;
    detail::SurfaceChoice choice = detail::create_surface(resolved, renderer_);
    if (choice.surface == nullptr) {
      teardown();
      return unexpected(ErrorCode::Unsupported, "创建绘制面失败");
    }
    surface_ = std::move(choice.surface);
    renderer_name_ = std::move(choice.name);
    renderer_note_ = std::move(choice.note);
    surface_->clear(math::Color{0, 0, 0, 0});

    // 上屏通道探测（MIT-SHM）：**在首个缓冲建立之前**完成，使后续每次尺寸重建
    // 都直接知道该走哪条路径（而不是每帧重试 dlopen/query）。
    try_enable_shm();

    if (auto status = sync_buffers_to_client(); !status) {
      teardown();
      return status;
    }
    ST_LOG_INFO("x11 窗口已创建（逻辑 {}x{} · 缩放 {:.2f} · 物理 {}x{} · 库 {} · 渲染器 {}）",
                logical_width_, logical_height_, static_cast<double>(scale_),
                surface_->physical_width(), surface_->physical_height(), x.library, renderer_name_);
    return ok();
  }

  void show_when_ready() override {
    const XlibApi& x = x11_library();
    if (window_ == 0 || shown_) return;
    shown_ = true;
    // 与 Win32 同一条时序契约：**首帧画完再映射**。用户看到的第一眼就是完整界面，
    // 而不是 WM 给的白底/未初始化区域（`app.cpp` 的启动顺序注释解释了为什么）。
    x.map_window(display_, window_);
    x.flush(display_);
    blit();  // 映射后补一次上屏（映射前写的像素在某些 WM 下会被背景覆盖）
    // 让主循环下一帧再画一次：与 Win32 的 `size_dirty_ = true` 同姿态
    size_dirty_ = true;
  }

  void present() override {
    pump_events();
    if (window_ == 0 || surface_ == nullptr) return;
    // 尺寸重建发生在**取画布处**（见 `framebuffer()`），这里不再重复——
    // 与 Win32 同一时序结论：重建必须在"画"之前，否则本帧画旧画布。
    //
    // **局部上屏**：应用已告知本帧唯一被改动的区域（`set_present_region`），
    // 整屏重排 + 整屏上屏等于把没变的像素又送一遍——1080p 下那是 0.96 ms 的重排
    // 与 2.3 ms 的送显。局部上屏把两者都按区域缩小（400×300 实测 0.85 ms/帧）。
    //
    // 两种时候必须整屏：① 应用报了空矩形（本帧走了全量，或回调未装）；
    // ② 尚未建立“服务端已有完整画面”的前提（映射后首帧，见 `show_when_ready`）。
    if (present_region_.is_empty()) {
      blit();
    } else {
      blit(&present_region_);
    }
    ++frames_;
  }

  void set_present_region(math::IntRect region) override { present_region_ = region; }

  [[nodiscard]] auto poll_event() -> std::optional<ui::Event> override {
    pump_events();
    if (events_.empty()) return std::nullopt;
    ui::Event event = std::move(events_.front());
    events_.pop_front();
    return event;
  }

  void set_title(std::string_view title) override {
    const XlibApi& x = x11_library();
    if (display_ == nullptr) return;
    title_ = std::string(title);
    if (window_ != 0) {
      x.store_name(display_, window_, title_.c_str());
      set_net_wm_name(title_);  // UTF-8 标题（`XStoreName` 按 Latin-1 解释）
      x.flush(display_);
    }
  }

  // —— 自绘窗框所需的窗口控制（契约见 `shell.hpp`；不支持时如实报 `Unsupported`）——

  [[nodiscard]] auto supports_window_control() const noexcept -> bool override {
    return window_ != 0;
  }

  [[nodiscard]] auto minimize() -> Status override {
    if (window_ == 0) return unexpected(ErrorCode::Io, "窗口未创建");
    if (wm_change_state_ == 0) {
      return unexpected(ErrorCode::Unsupported, "窗口管理器不支持 WM_CHANGE_STATE（无法最小化）");
    }
    // ICCCM 标准的最小化请求（`XIconifyWindow` 走的是同一条路径，只是它会顺带回读状态）
    return send_to_root(wm_change_state_, 3 /* IconicState */, 0, 0, 0);
  }

  [[nodiscard]] auto toggle_maximize() -> Status override {
    if (window_ == 0) return unexpected(ErrorCode::Io, "窗口未创建");
    if (net_wm_state_ == 0 || net_wm_state_maximized_horz_ == 0) {
      return unexpected(ErrorCode::Unsupported, "窗口管理器不支持 EWMH 最大化（_NET_WM_STATE）");
    }
    const bool want = !maximized_;
    // action 用 1（add）/ 0（remove）而不是 2（toggle）：toggle 依赖 WM 自己的状态缓存，
    // 与我们的 `maximized_` 不同步时会"点两下才动一次"。
    return send_to_root(net_wm_state_, want ? 1 : 0,
                        static_cast<long>(net_wm_state_maximized_horz_),
                        static_cast<long>(net_wm_state_maximized_vert_), 1);
  }

  [[nodiscard]] auto request_close() -> Status override {
    if (window_ == 0) return unexpected(ErrorCode::Io, "窗口未创建");
    if (wm_delete_window_ == 0) {
      return unexpected(ErrorCode::Unsupported, "窗口管理器不支持 WM_DELETE_WINDOW");
    }
    // 与用户点关闭按钮**同一条路**：发 WM_DELETE_WINDOW → 事件泵置 `close_requested_`
    // → 应用主循环收尾（进程内还有控制通道/脚本宿主需要正常停止）。
    // ⚠ 这条消息发给**窗口自己**（不是 root）：它是"请求本窗口关闭"的语义。
    const XlibApi& x = x11_library();
    XEvent event{};
    event.type = kClientMessage;
    auto& message = as_mutable<XClientMessageEvent>(event);
    message.display = display_;
    message.window = window_;
    message.message_type = wm_protocols_;
    message.format = 32;
    message.data.l[0] = static_cast<long>(wm_delete_window_);
    message.data.l[1] = 0;
    if (x.send_event(display_, window_, 0, 0, &event) == 0) {
      return unexpected(ErrorCode::Io, "XSendEvent(WM_DELETE_WINDOW) 失败");
    }
    x.flush(display_);
    return ok();
  }

  [[nodiscard]] auto begin_move() -> Status override {
    if (window_ == 0) return unexpected(ErrorCode::Io, "窗口未创建");
    if (net_wm_moveresize_ == 0) {
      return unexpected(ErrorCode::Unsupported, "窗口管理器不支持 _NET_WM_MOVERESIZE（无法拖动）");
    }
    // 交给 WM 的模态手势：吸附、跨屏、贴边随之生效。自己搬窗口看起来简单，
    // 实则要重实现吸附/多屏 DPI/任务栏避让——与 Win32 侧同一结论。
    return send_moveresize(8 /* _NET_WM_MOVERESIZE_MOVE */);
  }

  [[nodiscard]] auto begin_resize(WindowEdge edge) -> Status override {
    if (window_ == 0) return unexpected(ErrorCode::Io, "窗口未创建");
    if (!window_resizable_) return unexpected(ErrorCode::Unsupported, "窗口声明为不可缩放");
    if (net_wm_moveresize_ == 0) {
      return unexpected(ErrorCode::Unsupported, "窗口管理器不支持 _NET_WM_MOVERESIZE（无法缩放）");
    }
    // EWMH 的方向码（**不是** Win32 的 HT* 码，且从 0 起）：
    //   0=SizeTopLeft 1=SizeTop 2=SizeTopRight 3=SizeRight 4=SizeBottomRight
    //   5=SizeBottom 6=SizeBottomLeft 7=SizeLeft 8=Move 9=SizeKeyboard 10=MoveKeyboard
    // ⚠ 这张表最容易抄错：Win32 里 TopLeft 是 HTTOPLEFT(13)、BottomRight 是 17，
    //   而这里 BottomRight 是 4。数值抄错的后果是"拖右下角结果在往左缩"。
    switch (edge) {
      case WindowEdge::TopLeft: return send_moveresize(0);
      case WindowEdge::Top: return send_moveresize(1);
      case WindowEdge::TopRight: return send_moveresize(2);
      case WindowEdge::Right: return send_moveresize(3);
      case WindowEdge::BottomRight: return send_moveresize(4);
      case WindowEdge::Bottom: return send_moveresize(5);
      case WindowEdge::BottomLeft: return send_moveresize(6);
      case WindowEdge::Left: return send_moveresize(7);
      case WindowEdge::None: break;
    }
    return unexpected(ErrorCode::Invalid, "非边缘（无可缩放方向）");
  }

  [[nodiscard]] auto maximized() const noexcept -> bool override { return maximized_; }

  // —— 剪贴板 ——
  //
  // X11 的剪贴板是**所有权模型**：数据留在所有者进程里，别人通过 SelectionRequest
  // 来要。完整实现（ICCCM 往返）在服务器上价值有限（没有别的桌面应用），
  // 因此本版实现**进程内闭环**（与 headless 后端同口径：无头下复制粘贴要能用），
  // 并如实说明跨进程路径的边界——不假装成功。

  [[nodiscard]] auto clipboard_text() -> Result<std::string> override {
    if (clipboard_cache_.empty()) {
      return unexpected(ErrorCode::Unsupported,
                        "X11 跨进程剪贴板读取尚未实现（本进程内复制粘贴可用）");
    }
    return clipboard_cache_;
  }

  [[nodiscard]] auto set_clipboard_text(std::string_view text) -> Status override {
    const XlibApi& x = x11_library();
    if (display_ == nullptr) return unexpected(ErrorCode::Io, "显示未连接");
    clipboard_cache_ = std::string(text);
    // 顺手争取 CLIPBOARD 所有权（能拿到就拿；后续要支持跨进程读取时，
    // SelectionRequest 的分发点已经在事件泵里留好了）。
    if (clipboard_atom_ != 0 && window_ != 0) {
      x.set_selection_owner(display_, window_, clipboard_atom_, last_input_time_);
      selection_owner_ok_ = x.get_selection_owner(display_, clipboard_atom_) == window_;
      x.flush(display_);
    }
    return ok();
  }

  // —— 尺寸 / 绘制面 ——

  void set_resize_repaint(std::function<void()> repaint) override { repaint_ = std::move(repaint); }

  [[nodiscard]] auto framebuffer() -> raster::Surface& override {
    // **尺寸重建发生在取画布时**（不在 `present()`）：`render_frame` 的顺序是
    // "取画布 → 画 → present"，重建若放 present，本帧画的是旧画布、present 时才换新的,
    // 而新画布是空的且"下一帧重画"没有必然触发 → 屏幕停在拉伸的旧内容上。
    // 这条时序结论与 Win32 侧完全一致（那边有实测记录）。
    if (size_dirty_) {
      size_dirty_ = false;
      (void)sync_buffers_to_client();
    }
    return *surface_;
  }

  [[nodiscard]] auto renderer_name() const noexcept -> std::string_view override {
    return renderer_name_;
  }
  [[nodiscard]] auto renderer_note() const -> std::string override { return renderer_note_; }
  /// 上屏路径 + **本帧实际送显的像素面积**。
  ///
  /// 面积是量出来的事实，不是声明：“局部上屏到底生效了没有”只能靠它回答
  /// （生效时该值远小于画布面积；等于画布面积就是走了整屏）。
  [[nodiscard]] auto present_note() const -> std::string override {
    const char* path = using_shm_ ? (shm_no_completion_ ? "MIT-SHM 单块段" : "MIT-SHM") : "XPutImage";
    const std::string slots = using_shm_ ? std::format(" · {}/{} 块", present_slots_.slot_count(),
                                                       kPresentSlotLimit)
                                        : std::string{};
    if (shm_status_.empty()) return std::format("{}{}（探测未进行）", path, slots);
    return std::format("{}{}（{}；上帧送显 {}x{}；完成事件 {} 个）", path, slots, shm_status_,
                       last_present_width_, last_present_height_, shm_completions_seen_);
  }
  [[nodiscard]] auto frame_count() const noexcept -> std::uint64_t override { return frames_; }
  [[nodiscard]] auto device_scale() const noexcept -> float override { return scale_; }
  [[nodiscard]] auto logical_size() const noexcept -> math::Size override {
    // 精确值（物理 / scale）而不是取整后的整数：布局按它排布，取整会在右侧/底部
    // 留下 1px 空隙或溢出，与画布实际可绘区域不符（Win32 侧同一理由）。
    return math::Size{logical_width_exact_, logical_height_exact_};
  }

  auto set_device_scale(float scale) -> Status override {
    if (scale <= 0.0f) return unexpected(ErrorCode::Invalid, "DPI 缩放必须为正数");
    const XlibApi& x = x11_library();
    scale_ = scale;
    // 与 Win32 同一口径：**客户区是唯一真源**。改 DPI 要先改窗口尺寸
    //（逻辑 × 新 scale），再以实际客户区重建缓冲——只改一半就会"画布与客户区不符"。
    if (window_ != 0) {
      const int physical_width = std::max(1, static_cast<int>(std::lround(
          static_cast<double>(logical_width_) * static_cast<double>(scale_))));
      const int physical_height = std::max(1, static_cast<int>(std::lround(
          static_cast<double>(logical_height_) * static_cast<double>(scale_))));
      x.resize_window(display_, window_, static_cast<unsigned int>(physical_width),
                      static_cast<unsigned int>(physical_height));
      x.flush(display_);
    }
    return sync_buffers_to_client();
  }

 private:
  // —— 装配辅助 ——

  /// 进程级 X 错误处理器：**不让协议错误杀掉应用**。
  ///
  /// Xlib 的默认处理器收到协议错误就 `exit(1)`——连控制通道都来不及收尾。
  /// 而"WM 不认识某个属性/某个请求"在真实桌面里是常态（不同 WM 的 EWMH 覆盖度不同），
  /// 绝不能让它变成进程退出。本处理器只记一条警告（含错误码与请求号），继续运行。
  static void install_error_handler() {
    const XlibApi& x = x11_library();
    if (x.set_error_handler == nullptr) return;
    (void)x.set_error_handler(&x11_error_handler);
  }

  static int x11_error_handler(void* display, XEvent* event) {
    (void)display;
    // `XErrorEvent` 的前若干字段（`Xlib.h`）：type/serial/error_code/request_code/...
    if (event == nullptr) return 0;
    const auto* raw = reinterpret_cast<const unsigned char*>(event);  // lint-allow: L6
    const int error_code = static_cast<int>(raw[8]);
    const int request_code = static_cast<int>(raw[9]);
    ST_LOG_WARN("X11 协议错误已忽略（error_code={} request_code={}）——"
                "通常表示窗口管理器不支持该请求",
                error_code, request_code);
    return 0;  // 0 = 已处理，不终止进程
  }

  static auto make_class_hint() -> XClassHint* {
    // `XSetClassHint` 要的是**可写** `char*`（传字面量是 UB）。静态缓冲：进程内唯一。
    static char res_name[] = "shuangtian";
    static char res_class[] = "Shuangtian";
    static XClassHint hint{res_name, res_class};
    return &hint;
  }

  void resolve_atoms() {
    const XlibApi& x = x11_library();
    const auto atom = [&](const char* name) { return x.intern_atom(display_, name, 0); };
    wm_protocols_ = atom("WM_PROTOCOLS");
    wm_delete_window_ = atom("WM_DELETE_WINDOW");
    wm_change_state_ = atom("WM_CHANGE_STATE");
    net_wm_name_ = atom("_NET_WM_NAME");
    net_wm_pid_ = atom("_NET_WM_PID");
    net_wm_state_ = atom("_NET_WM_STATE");
    net_wm_state_maximized_horz_ = atom("_NET_WM_STATE_MAXIMIZED_HORZ");
    net_wm_state_maximized_vert_ = atom("_NET_WM_STATE_MAXIMIZED_VERT");
    net_wm_moveresize_ = atom("_NET_WM_MOVERESIZE");
    motif_wm_hints_ = atom("_MOTIF_WM_HINTS");
    utf8_string_ = atom("UTF8_STRING");
    clipboard_atom_ = atom("CLIPBOARD");
    if (utf8_string_ == 0) utf8_string_ = kAtomString;  // 极老服务端：退化为 STRING
  }

  /// 去装饰：`_MOTIF_WM_HINTS` 的 `decorations` 位。
  ///
  /// 结构是**5 个 ulong**（flags / functions / decorations / input_mode / status），
  /// 按 `format=32` 写成数组——GNOME/KDE/XFCE 都按这个读。`flags` 里的
  /// `MWM_HINTS_DECORATIONS`(2) 表示"只看 decorations 字段"，值为 0 = 不要装饰。
  void apply_no_decorations() {
    const XlibApi& x = x11_library();
    if (motif_wm_hints_ == 0) return;
    unsigned long hints[5] = {2, 0, 0, 0, 0};
    x.change_property(display_, window_, motif_wm_hints_, motif_wm_hints_, 32, kPropModeReplace,
                      reinterpret_cast<const unsigned char*>(hints), 5);  // lint-allow: L6
  }

  void set_wm_protocols() {
    const XlibApi& x = x11_library();
    if (wm_delete_window_ == 0 || wm_protocols_ == 0) return;
    const unsigned long protocols[1] = {wm_delete_window_};
    x.change_property(display_, window_, wm_protocols_, kAtomAtom, 32, kPropModeReplace,
                      reinterpret_cast<const unsigned char*>(protocols), 1);  // lint-allow: L6
  }

  void set_net_wm_name(std::string_view title) {
    const XlibApi& x = x11_library();
    if (net_wm_name_ == 0 || utf8_string_ == 0) return;
    x.change_property(display_, window_, net_wm_name_, utf8_string_, 8, kPropModeReplace,
                      reinterpret_cast<const unsigned char*>(title.data()),  // lint-allow: L6
                      static_cast<int>(title.size()));
  }

  void set_net_wm_pid() {
    const XlibApi& x = x11_library();
    if (net_wm_pid_ == 0) return;
    const long pid = static_cast<long>(process::current_id());
    // `_NET_WM_PID` 的类型是 CARDINAL(32)；但 `ChangeProperty` 的 format=32 表示
    // "按 long 传输"（Xlib 会自己处理字节序与宽度），因此这里传 long 数组。
    x.change_property(display_, window_, net_wm_pid_, kAtomCardinal, 32, kPropModeReplace,
                      reinterpret_cast<const unsigned char*>(&pid), 1);  // lint-allow: L6
  }

  /// 发一条 EWMH client message 给 **root**（WM 在 root 上选 `SubstructureRedirectMask`）。
  ///
  /// 三个要点（**都是实测得来的**，错一个就静默失效、没有任何报错）：
  ///  ① 事件发给 **root**，不是窗口自己——发给别的窗口 WM 收不到；
  ///  ② 掩码必须含 `SubstructureRedirectMask(1<<20)`；
  ///  ③ `data.l[4]`（`send_moveresize` 用）是**事件时间戳**，必须取自最近一次真实输入，
  ///     且不能是 `CurrentTime`(0)：WM 拿它校验"这次请求是否对应一次真实按下"，
  ///     时间戳过旧会被静默丢弃。
  [[nodiscard]] auto send_to_root(XId message_type, long data0, long data1, long data2,
                                  long data3) -> Status {
    const XlibApi& x = x11_library();
    if (window_ == 0 || message_type == 0) {
      return unexpected(ErrorCode::Unsupported, "缺少窗口管理器协议支持");
    }
    XEvent event{};
    event.type = kClientMessage;
    auto& message = as_mutable<XClientMessageEvent>(event);
    message.display = display_;
    message.window = window_;
    message.message_type = message_type;
    message.format = 32;
    message.data.l[0] = data0;
    message.data.l[1] = data1;
    message.data.l[2] = data2;
    message.data.l[3] = data3;
    message.data.l[4] = 0;
    if (x.send_event(display_, root_, 0, kStructureNotifyMask | kSubstructureRedirectMask,
                     &event) == 0) {
      return unexpected(ErrorCode::Io, "XSendEvent 到 root 失败");
    }
    x.flush(display_);
    return ok();
  }

  [[nodiscard]] auto send_moveresize(long direction) -> Status {
    if (net_wm_moveresize_ == 0) {
      return unexpected(ErrorCode::Unsupported, "无 _NET_WM_MOVERESIZE");
    }
    const XlibApi& x = x11_library();
    const math::Point pointer = query_pointer_root();
    XEvent event{};
    event.type = kClientMessage;
    auto& message = as_mutable<XClientMessageEvent>(event);
    message.display = display_;
    message.window = window_;
    message.message_type = net_wm_moveresize_;
    message.format = 32;
    // data[0]/[1] 是**根窗口坐标**（不是窗口内坐标）；data[3] = 触发按钮
    message.data.l[0] = static_cast<long>(pointer.x);
    message.data.l[1] = static_cast<long>(pointer.y);
    message.data.l[2] = direction;
    message.data.l[3] = 1;  // Button1
    message.data.l[4] = static_cast<long>(last_input_time_);
    if (x.send_event(display_, root_, 0, kStructureNotifyMask | kSubstructureRedirectMask,
                     &event) == 0) {
      return unexpected(ErrorCode::Io, "XSendEvent(_NET_WM_MOVERESIZE) 失败");
    }
    x.flush(display_);
    return ok();
  }

  /// 取指针的**根窗口**坐标（`_NET_WM_MOVERESIZE` 的两个 data 字段要求它）。
  ///
  /// 实现取「最近一次真实输入事件的根坐标」——那正是 WM 用来校验 `data.l[4]` 时间戳
  /// 所对应的位置，语义上等价于现场 `XQueryPointer`，且省一次往返（符号表里也没有它）。
  [[nodiscard]] auto query_pointer_root() const -> math::Point { return last_input_root_; }

  /// 读 X resource 里的 `Xft.dpi` 得到显示缩放。
  ///
  /// X11 没有 EWMH 级的"显示器缩放"属性；桌面环境（GNOME/KDE）设置缩放时会把
  /// 实际 DPI 写进 `Xft.dpi`（本机实测为 `Xft.dpi: 192` → 2.0）。
  /// 读不到就 1.0——与 `cross_sections.md` §7「无 DPI 概念的平台返回 1.0」一致。
  [[nodiscard]] auto query_display_scale() const -> float {
    const XlibApi& x = x11_library();
    if (display_ == nullptr || x.resource_manager_string == nullptr) return 1.0f;
    const char* resources = x.resource_manager_string(display_);
    if (resources == nullptr) return 1.0f;
    const std::string_view text(resources);
    const std::size_t at = text.find("Xft.dpi");
    if (at == std::string_view::npos) return 1.0f;
    std::size_t index = at + 7;
    while (index < text.size() &&
           (text[index] == ':' || text[index] == ' ' || text[index] == '\t')) {
      ++index;
    }
    std::string digits;
    while (index < text.size() &&
           ((text[index] >= '0' && text[index] <= '9') || text[index] == '.')) {
      digits.push_back(text[index]);
      ++index;
    }
    if (digits.empty()) return 1.0f;
    const double dpi = std::strtod(digits.c_str(), nullptr);  // lint-allow: L14 平台边界解析
    if (!(dpi > 0.0)) return 1.0f;
    const auto scale = static_cast<float>(dpi / 96.0);
    return scale > 0.0f ? scale : 1.0f;
  }

  /// 以**窗口实际客户区**为准重算逻辑尺寸并重建绘制面。
  ///
  /// 为什么以窗口为准：`XResizeWindow` 只是**请求**，WM 可能给不同尺寸（贴边吸附、
  /// 最小尺寸限制、最大化到工作区）。反过来"以画布为准"会让两者不一致——
  /// 那是"画面被系统拉伸"的老病根（Win32 侧同一结论）。
  [[nodiscard]] auto sync_buffers_to_client() -> Status {
    const XlibApi& x = x11_library();
    if (display_ == nullptr || window_ == 0) return ok();
    XWindowAttributes attributes{};
    if (x.get_window_attributes(display_, window_, &attributes) == 0) {
      return unexpected(ErrorCode::Io, "XGetWindowAttributes 失败");
    }
    const int physical_width = std::max(1, attributes.width);
    const int physical_height = std::max(1, attributes.height);
    logical_width_exact_ = static_cast<float>(physical_width) / scale_;
    logical_height_exact_ = static_cast<float>(physical_height) / scale_;
    logical_width_ = std::max(1, static_cast<int>(std::lround(logical_width_exact_)));
    logical_height_ = std::max(1, static_cast<int>(std::lround(logical_height_exact_)));

    if (surface_ != nullptr && surface_->physical_width() == physical_width &&
        surface_->physical_height() == physical_height) {
      return ensure_backbuffer(physical_width, physical_height);
    }
    if (surface_ != nullptr) {
      if (auto status = surface_->resize(physical_width, physical_height); !status) {
        return status;
      }
    } else {
      WindowOptions options;
      options.width = logical_width_;
      options.height = logical_height_;
      options.scale = scale_;
      options.renderer = renderer_name_;
      detail::SurfaceChoice choice = detail::create_surface(options, renderer_name_);
      if (choice.surface == nullptr) return unexpected(ErrorCode::Unsupported, "重建绘制面失败");
      surface_ = std::move(choice.surface);
    }
    surface_->clear(math::Color{0, 0, 0, 0});
    if (auto status = ensure_backbuffer(physical_width, physical_height); !status) {
      return status;
    }
    return ok();
  }

  /// 准备上屏中转缓冲（`XImage`）。
  ///
  /// ## 中转区为什么必须存在
  ///
  /// 画布内部像素是 `0xRRGGBBAA`（预乘），而 X11 的 ZPixmap 在 24/32 位 TrueColor 上是
  /// **小端 `0x00RRGGBB`**（实测确认：直接送 `0xRRGGBBAA` 会得到红蓝互换 + alpha 当蓝）。
  /// 因此每个像素都要重排——与 Win32 侧把画布重排进 DIB 完全同构。
  ///
  /// ## 内存所有权（**最危险的一处，抄错就是越界崩溃**）
  ///
  /// 查 Xlib 实现（`ImUtil.c`）确认了两条：
  ///   ① `XCreateImage` **不分配** `data`（`image->data = data`，原样存）；传 `nullptr`
  ///      得到的是空指针，之后往 `image->data` 写就是**空指针写入**（段错误，
  ///      而且看起来像"X11 后端一启动就崩"）；
  ///   ② `XDestroyImage` → `_XDestroyImage` 会 `Xfree(ximage->data)`，
  ///      而 `Xfree` 走 Xlib 自己的分配器（`libX11` 未导出 `Xmalloc`/`Xfree`，
  ///      但导出了 `XFree`——两者是同一分配器）。
  ///
  /// 所以：**自己分配中转缓冲**（`std::vector<uint32_t>`，内存归本对象所有），
  /// 交给 `XCreateImage` 打包成一个"只读视图"，并向 Xlib 登记一个**自定义
  /// `destroy_image`**（置空 `data` 再交给原销毁函数）——这样 `XDestroyImage`
  /// 不会去释放我们的 vector，也不会有跨分配器释放。
  /// （`f.destroy_image` 是 Xlib 的公开扩展点：`XImage` 的函数表按设计可由调用方替换，
  ///   这正是"自定义后备存储"的官方用法。）
  /// `XDestroyImage` 在 Xlib 里是宏：`(*((ximage)->f.destroy_image))((ximage))`——
  /// 默认的 `_XDestroyImage` 会依次 `Xfree(data); Xfree(obdata); Xfree(image)`。
  ///
  /// `data` 是**借用**给它的（我们分配的 `std::vector`），销毁前必须摘掉指针：
  /// 否则 Xlib 会去释放 vector 的缓冲，而两者分配器不同（跨分配器释放是
  /// **未定义行为**，典型表现是进程直接崩）。
  /// 结构的本体则是 Xlib 用 `Xmalloc` 分配的，交给 `XFree` 释放才对——
  /// 因此这里两件事都做，既不越界也不泄漏。
  static auto destroy_image_shim(XImage* image) -> int {
    if (image == nullptr) return 0;
    image->data = nullptr;    // 归还借用（缓冲归本对象所有）
    image->obdata = nullptr;  // 我们从未设置过它（XCreateImage 里为 NULL）
    const XlibApi& x = x11_library();
    if (x.free_memory != nullptr) x.free_memory(image);  // XFree：释放结构本体
    return 1;
  }

  void destroy_backbuffer() {
    const XlibApi& x = x11_library();
    if (image_ != nullptr) {
      // `XDestroyImage` 是宏，等价于按 `f.destroy_image` 调用（已被我们替换为 shim）
      (void)x.destroy_image(image_);
      image_ = nullptr;
    }
    destroy_shm_slots();
    pixels_.clear();
    image_width_ = 0;
    image_height_ = 0;
    present_slots_.reset(0);
  }

  /// 拆掉一个共享段槽（映射 + System V 段 + `XImage` 视图）。
  ///
  /// 顺序：先 `XShmDetach`（让**服务端**放下该段），再 `shmdt`（本地取消映射），
  /// 最后 `IPC_RMID`（内核回收）。三段错一步就是泄漏或下一帧 `BadShmSeg`。
  void destroy_slot(ShmSlot& slot) {
    const XlibApi& x = x11_library();
    const XextApi& ext = xext_library();
    if (slot.segment.shmaddr != nullptr) {
      if (slot.segment.shmseg != 0 && ext.detach != nullptr && display_ != nullptr) {
        (void)ext.detach(display_, &slot.segment);  // lint-allow: L9 XShmDetach 是 Xlib C API
      }
      (void)::shmdt(slot.segment.shmaddr);
      if (slot.segment.shmid >= 0) (void)::shmctl(slot.segment.shmid, IPC_RMID, nullptr);
    }
    if (slot.image != nullptr) {
      // `image->data` 指向共享段，**不能**交给 Xlib 释放（那是映射内存，
      // 不是 `Xmalloc` 的堆块）。置空后原销毁函数只释放结构本体。
      slot.image->data = nullptr;
      if (x.destroy_image != nullptr) (void)x.destroy_image(slot.image);
    }
    slot = ShmSlot{};
    slot.segment.shmid = -1;
  }

  void destroy_shm_slots() {
    for (auto& slot : shm_slots_) destroy_slot(slot);
    shm_slots_.fill(ShmSlot{});
    for (auto& slot : shm_slots_) slot.segment.shmid = -1;
  }

  [[nodiscard]] auto shm_slot_count() const noexcept -> int {
    int count = 0;
    for (const auto& slot : shm_slots_) {
      if (slot.image != nullptr) ++count;
    }
    return count;
  }

  /// 建一个共享段槽：`shmget` → `shmat` → `XShmCreateImage` → `XShmAttach`。
  ///
  /// 必须**就地**建在目标槽里（不能先建到局部变量再拷进去）：`XShmCreateImage` 把
  /// `XShmSegmentInfo*` 存进 `XImage::obdata`，之后每次 `XShmPutImage` 都**跟着那个指针
  /// 读段 ID**。拷一份结构体后指针仍指向已销毁的局部对象 → 读到垃圾段 ID →
  /// 服务端报 `BadShmSeg`（实测：上屏全部失败，日志里一帧两条协议错误，而画面“看起来没崩”）。
  ///
  /// 失败时把已建的部分**完整拆掉**（一段都不能漏，否则是进程存活期的泄漏）
  /// 并返回 false——调用方据此降级（双块退单块，或整条 SHM 回 `XPutImage`）。
  [[nodiscard]] auto create_slot(int width, int height) -> bool {
    const XextApi& ext = xext_library();
    int index = -1;
    for (int candidate = 0; candidate < kPresentSlotLimit; ++candidate) {
      if (shm_slots_[static_cast<std::size_t>(candidate)].image == nullptr) {
        index = candidate;
        break;
      }
    }
    if (index < 0) return false;  // 槽位已满（不应发生：调用方先查过容量）
    ShmSlot& slot = shm_slots_[static_cast<std::size_t>(index)];
    const std::size_t bytes = static_cast<std::size_t>(width) *
                              static_cast<std::size_t>(height) * sizeof(std::uint32_t);
    const int shmid = ::shmget(IPC_PRIVATE, bytes, IPC_CREAT | 0600);
    if (shmid < 0) return false;
    slot.segment = XShmSegmentInfo{};
    slot.segment.shmid = shmid;
    slot.segment.shmaddr = static_cast<char*>(::shmat(shmid, nullptr, 0));
    slot.segment.read_only = 0;
    if (slot.segment.shmaddr == nullptr) {
      (void)::shmctl(shmid, IPC_RMID, nullptr);
      slot.segment.shmid = -1;
      return false;
    }
    slot.image = ext.create_image(display_, visual_, static_cast<unsigned int>(depth_), kZPixmap,
                                  slot.segment.shmaddr, &slot.segment,
                                  static_cast<unsigned int>(width),
                                  static_cast<unsigned int>(height));
    if (slot.image == nullptr || ext.attach(display_, &slot.segment) == 0) {
      destroy_slot(slot);
      return false;
    }
    return true;
  }

  /// 槽 `slot` 的像素首地址（共享段时指向段内，否则指向自己的 vector）。
  ///
  /// 为什么共享段优先：X11 的“上屏”是客户端把像素**交出去**。私有 vector 只能
  /// 经 socket 逐字节拷给服务端（`XPutImage`）；共享段则让服务端直接读。
  [[nodiscard]] auto backbuffer_pixels(int slot) -> std::uint32_t* {
    if (slot >= 0 && slot < kPresentSlotLimit) {
      const auto& shm = shm_slots_[static_cast<std::size_t>(slot)];
      if (shm.segment.shmaddr != nullptr) {
        return reinterpret_cast<std::uint32_t*>(shm.segment.shmaddr);  // lint-allow: L6 像素视图
      }
    }
    return pixels_.empty() ? nullptr : pixels_.data();
  }

  /// 槽 `slot` 对应的 `XImage`（共享段时是它自己那块；私有缓冲时恒为 `image_`）。
  [[nodiscard]] auto slot_image(int slot) -> XImage* {
    if (slot >= 0 && slot < kPresentSlotLimit) {
      if (XImage* image = shm_slots_[static_cast<std::size_t>(slot)].image; image != nullptr) {
        return image;
      }
    }
    return image_;
  }

  /// 尝试建 MIT-SHM 上屏通道（失败**不报错**：如实记原因并按 `XPutImage` 走）。
  ///
  /// 三道门都得过：① `libXext` 可加载且符号齐（dlopen 探测）；
  /// ② 服务端在**当前 DISPLAY 上**支持 MIT-SHM（`XShmQueryExtension`）——远程 X
  /// （SSH 转发）通常不支持，那时共享段建出来也不能用；
  /// ③ 能取到完成事件基址（`XShmGetEventBase`）——双缓冲靠它判“哪块能写”。
  /// 前两道不过走 `XPutImage`；第三道不过而前两道过，仍可用 SHM，只是走单段 + 每帧排空。
  void try_enable_shm() {
    if (shm_slot_count() > 0 || shm_attempted_) return;
    shm_attempted_ = true;
    const XextApi& ext = xext_library();
    if (ext.handle == nullptr) {
      shm_status_ = "libXext 不可加载（未装/未探测到）——上屏走 XPutImage";
      return;
    }
    int major = 0;
    int minor = 0;
    int pixmaps = 0;
    if (ext.query_extension(display_, &major, &minor, &pixmaps) == 0) {
      shm_status_ = std::format("服务端未启用 MIT-SHM（DISPLAY={}）——上屏走 XPutImage",
                                std::getenv("DISPLAY") != nullptr ? std::getenv("DISPLAY") : "");
      return;
    }
    shm_completion_base_ = ext.get_event_base(display_) + kShmCompletionOffset;
    shm_extension_ready_ = true;
    shm_status_ = std::format("MIT-SHM 已启用（{}.{}）", major, minor);
  }

  /// 为 `width`×`height` 准备上屏缓冲：先试共享段，不成则回退私有 vector。
  ///
  /// 两件事一次做完（而不是“先建 SHM 再建私有”）的理由：像素重排的目标地址
  /// 必须**只有一个**，否则每帧都要问“这次写哪里”，而写错的那一边会静默地
  /// 上屏旧内容（那类缺陷在截图上只表现为“画面不跟手”）。
  [[nodiscard]] auto prepare_backbuffer(int width, int height) -> Status {
    const XlibApi& x = x11_library();
    if (display_ == nullptr || window_ == 0) return ok();
    // 已就绪的判据**两条路径都要看**：只判 `image_` 会让 SHM 路径每帧重走一次
    // “shmget + shmat + XShmAttach + XSync + 拆段”（实测症状：日志刷屏、
    // 送显 6.2 ms/帧——建段比上屏还贵，因为每帧都在建）。
    const bool ready = (shm_slot_count() > 0 || image_ != nullptr) &&
                       image_width_ == width && image_height_ == height;
    if (ready) return ok();
    destroy_backbuffer();
    const std::size_t bytes = static_cast<std::size_t>(width) *
                              static_cast<std::size_t>(height) * sizeof(std::uint32_t);
    // 用显式标志而不是 `shm_status_` 的字符串前缀：两者一旦不同步，"探测到了但没建成"
    // 这条分支就永远不进去（而它只能靠日志发现）。
    if (shm_extension_ready_) {
      // 先试两块（双缓冲）：交替写入两块段，靠完成事件判“哪块已被服务端读完”。
      // 第二块建不出来时**如实降级到单块**（而不是整条路径回退）——单块 + 每帧
      // 排空仍然比 `XPutImage` 快一个量级（见 `DESIGN.md` §4.6.2 的对照表）。
      for (int index = 0; index < shm_slot_goal_; ++index) {
        if (!create_slot(width, height)) break;
      }
      const int built = shm_slot_count();
      if (built > 0) {
        // `XShmAttach` 的段 ID 由服务端异步回填。不排空就上屏，首个请求可能带
        // 未定的段 ID（`BadShmSeg`，服务端 xerror）。
        x.sync(display_, 0);
        image_width_ = width;
        image_height_ = height;
        using_shm_ = true;
        if (built < kPresentSlotLimit) {
          shm_status_ = std::format("MIT-SHM 已启用，但只建成 {} 块段（另一块建不出来或已降级）——"
                                    "降为单块 + 每帧排空",
                                    built);
        }
        present_slots_.reset(built);
        ST_LOG_INFO("x11 上屏走 MIT-SHM（{}x{} · {} 块段 · 每块 {} 字节 · 完成事件类型 {}）", width,
                    height, built, bytes, shm_completion_base_);
        return ok();
      }
      ST_LOG_WARN("MIT-SHM 共享段建立失败，本次回退 XPutImage");
      shm_status_ =
          "MIT-SHM 已探测到，但共享段建立失败（shmget/shmat/XShmAttach；可能受 kernel.shmmax / "
          "shmall 限制）——上屏走 XPutImage";
    }
    // 回退：私有中转缓冲（内存归本对象所有，Xlib 只拿一个“只读视图”）。
    pixels_.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0U);
    image_ = x.create_image(display_, visual_, static_cast<unsigned int>(depth_), kZPixmap, 0,
                            reinterpret_cast<char*>(pixels_.data()),  // lint-allow: L6 像素视图
                            static_cast<unsigned int>(width), static_cast<unsigned int>(height),
                            32, 0);
    if (image_ == nullptr) {
      pixels_.clear();
      return unexpected(ErrorCode::Io, std::format("XCreateImage 失败（{}x{}）", width, height));
    }
    image_width_ = width;
    image_height_ = height;
    using_shm_ = false;
    present_slots_.reset(1);
    // 接管销毁：shim 会先归还 `data`（我们的 vector）再释放 XImage 结构本体。
    // 取 `&X11Backend::destroy_image_shim` 得到一个**函数指针**（静态成员函数即普通函数），
    // 再按 `XImage` 函数表的 C 签名存进去。
    image_->f.destroy_image = reinterpret_cast<void*>(&destroy_image_shim);  // lint-allow: L6
    return ok();
  }

  /// `ensure_backbuffer` 的旧名保留给调用点（语义已合并到 `prepare_backbuffer`）。
  [[nodiscard]] auto ensure_backbuffer(int width, int height) -> Status {
    return prepare_backbuffer(width, height);
  }

  /// 画布像素 → X11 像素并上屏。
  ///
  /// ## 为什么要分「交付路径」与「局部区域」两件事
  ///
  /// 上屏是 X11 后端**单帧最大的一段固定开销**（1080p 实测：整屏 `XPutImage`
  /// 17.9 ms/帧，而绘制本身才 17.9 ms——两者同量级）。两条改进分离病因：
  ///   ① **传输介质**：`XPutImage` 走 socket 逐字节拷给服务端；`XShmPutImage`
  ///      走 System V 共享内存（服务端直接读）。1080p 实测 17.9 → 2.4 ms/帧（7.5×）；
  ///   ② **传输范围**：局部上屏（只转换+送损坏区）把转换从 0.96 ms 降到 0.05 ms
  ///      （400×300 区），局部 SHM 上屏 0.85 ms/帧（整屏 2.27 ms）。
  ///
  /// `region` 为空 = 整屏（曝光事件、首次映射、尺寸变化都必须走这条）。
  void blit(const math::IntRect* region = nullptr) {
    const XlibApi& x = x11_library();
    if (surface_ == nullptr || window_ == 0 || gc_ == nullptr) return;
    const int width = surface_->physical_width();
    const int height = surface_->physical_height();
    if (auto status = ensure_backbuffer(width, height); !status) return;

    // 把逻辑像素的损坏区换算成物理像素并夹到画布内。
    // 换算规则是**共用纯函数**（`resolve_present_area`）：两端对“区域边界”的解释
    // 必须逐位一致，否则同一个应用在两个后端的局部重绘边缘会差一列像素。
    math::IntRect area{0, 0, width, height};
    if (region != nullptr) {
      area = resolve_present_area(*region, scale_, width, height);
      if (area.is_empty()) return;  // 区域在画布外：本帧无事可做
    }

    // 选槽：所有槽都在途时必须先让服务端追上（或者这个环境根本不回报完成事件 →
    // 降级成单段 + 每帧排空，与改造前完全一致，只是如实记下原因）。
    ensure_slot_available();
    const int slot = present_slots_.pick();
    if (slot < 0) return;
    std::uint32_t* target = backbuffer_pixels(slot);
    XImage* image = slot_image(slot);
    if (target == nullptr || image == nullptr) return;

    const std::span<const std::uint32_t> source = surface_->pixels();
    const std::size_t stride = static_cast<std::size_t>(width);
    for (int row = 0; row < area.height; ++row) {
      const std::size_t offset =
          static_cast<std::size_t>(area.y + row) * stride + static_cast<std::size_t>(area.x);
      if (offset + static_cast<std::size_t>(area.width) > source.size()) break;
      // 画布：`R<<24 | G<<16 | B<<8 | A`；X11 小端 ZPixmap 的像素值是 0xFFRRGGBB。
      // 这段重排下沉在 `st::raster::simd`（两端共用同一语义，三路逐像素一致有测试钉住）。
      raster::simd::convert_rgba_to_xrgb_row(target + offset, source.data() + offset,
                                             static_cast<std::size_t>(area.width));
    }

    const auto src_x = static_cast<unsigned int>(area.x);
    const auto src_y = static_cast<unsigned int>(area.y);
    const auto put_width = static_cast<unsigned int>(area.width);
    const auto put_height = static_cast<unsigned int>(area.height);
    last_present_width_ = area.width;
    last_present_height_ = area.height;
    present_slots_.commit(slot);
    if (using_shm_) {
      const XextApi& ext = xext_library();
      // `send_event` = 1：要求服务端读完后回一枚 `XShmCompletionEvent`（带 `shmseg`）。
      // 之后**只 flush 不 sync**：请求必须现在发出去（否则下一帧写另一块段时
      // 服务端还没看到上一帧），但不必等它读完——那是双缓冲省下的那一次往返。
      ext.put_image(display_, window_, gc_, image, static_cast<int>(src_x), static_cast<int>(src_y),
                    static_cast<int>(src_x), static_cast<int>(src_y), put_width, put_height, 1);
      present_slots_.mark_in_flight(slot);
      x.flush(display_);
      return;
    }
    x.put_image(display_, window_, gc_, image, static_cast<int>(src_x), static_cast<int>(src_y),
                static_cast<int>(src_x), static_cast<int>(src_y), put_width, put_height);
    x.flush(display_);
  }

  /// 确保**当前至少有一块可用槽**（所有槽都在途时先让服务端追上）。
  ///
  /// 三级退让，每级都比前一级贵，但只在真需要时才走到：
  ///  ① **快路径**：已有一块空闲（上一帧/上上帧的完成事件已在事件泵里收过了）——
  ///     不 sync、不等待，这就是双缓冲的全部收益；
  ///  ② **补齐路径**：先收一遍已到达的完成事件（别的进 `held_events_`，不能丢）；
  ///  ③ **兜底路径**：跑一次 `XSync`。它回来就**证明**服务端已处理完全部请求（含读完段），
  ///     所以“段现在安全”这个事实不依赖完成事件是否到达。据此：若仍认领不到而
  ///     累计从未见过完成事件 → 这环境不回报它 → 降为单块段 + 每帧排空（与改造前同路）；
  ///     若只是这次没到（之前见过）→ 段已安全，直接放开在途标记继续用双缓冲。
  void ensure_slot_available() {
    if (present_slots_.in_flight_count() <= 0) return;
    if (!using_shm_ || shm_slots_[0].image == nullptr) {
      present_slots_.mark_all_idle();  // 私有缓冲：像素是我们的副本，无"服务端在读"可言
      return;
    }
    const XlibApi& x = x11_library();
    bool got_now = false;
    collect_pending_completions(got_now);
    if (present_slots_.in_flight_count() <= 0) return;
    x.sync(display_, 0);
    collect_pending_completions(got_now);
    // `XSync` 已回 ⇒ 服务端处理完了全部 PutImage ⇒ 没有任何段还在被读。
    // 放开标记是**安全**的（与是否收到完成事件无关）。
    present_slots_.mark_all_idle();
    if (decide_after_sync_wait(static_cast<int>(shm_completions_seen_), got_now) ==
        SyncVerdict::Degrade) {
      degrade_to_single();
    }
  }

  /// 把已到达的完成事件收掉（其余事件进 `held_events_`，不能丢）。
  void collect_pending_completions(bool& got_any) {
    const XlibApi& x = x11_library();
    int guard = 0;
    while (x.pending(display_) > 0) {
      if (++guard > 4096) break;
      XEvent event{};
      x.next_event(display_, &event);
      if (event.type == shm_completion_base_) {
        if (collect_completion(event)) got_any = true;
        continue;
      }
      held_events_.push_back(event);
    }
  }

  /// 降为单块段：拆掉多余的段。
  void degrade_to_single() {
    if (shm_no_completion_) return;
    shm_no_completion_ = true;
    shm_slot_goal_ = 1;
    for (int index = 1; index < kPresentSlotLimit; ++index) {
      destroy_slot(shm_slots_[static_cast<std::size_t>(index)]);
    }
    present_slots_.degrade_to_single();
    shm_status_ = "MIT-SHM 已启用，但收不到 XShmCompletionEvent（服务端不回报）——"
                  "降为单块段 + 每帧 XSync 排空（与单缓冲路径同代价）";
    ST_LOG_WARN("x11 上屏：收不到 XShmCompletionEvent → 降为单共享段 + 每帧排空");
  }

  /// 处理一枚完成事件：按 `shmseg` 认领它对应哪块槽（**不能**按下标猜——
  /// 完成事件的到达顺序与提交顺序在乱序实现下不保证一致）。
  [[nodiscard]] auto collect_completion(const XEvent& event) -> bool {
    const auto& completion = as<XShmCompletionEvent>(event);
    ++shm_completions_seen_;
    for (int index = 0; index < kPresentSlotLimit; ++index) {
      const auto& slot = shm_slots_[static_cast<std::size_t>(index)];
      if (slot.image == nullptr || slot.segment.shmseg != completion.shmseg) continue;
      if (!present_slots_.in_flight(index)) return true;
      present_slots_.mark_idle(index);
      return true;
    }
    return false;
  }

  // —— 事件泵 ——

  void pump_events() {
    const XlibApi& x = x11_library();
    if (display_ == nullptr) return;
    // 先倒出等待完成事件期间“挟”住的其它事件：它们比队列里现有的更早，
    // 顺序错了会让“按键→松开”这类成对事件乱序（拖选、双击计数都会错）。
    while (!held_events_.empty()) {
      XEvent event = held_events_.front();
      held_events_.pop_front();
      handle_event(event);
    }
    int guard = 0;
    while (x.pending(display_) > 0) {
      if (++guard > 4096) break;  // 病态事件风暴时不死循环（如实丢弃剩余）
      XEvent event{};
      x.next_event(display_, &event);
      // 完成事件不对外派发：它是上屏状态机的内部消息（与输入/窗口事件无关）。
      if (using_shm_ && event.type == shm_completion_base_) {
        (void)collect_completion(event);
        continue;
      }
      handle_event(event);
    }
  }

  void handle_event(const XEvent& event) {
    switch (event.type) {
      case kExpose:
        // 曝光：把离屏帧缓冲重新贴上去（不能只依赖 present 的节奏，与 Win32 的
        // `WM_PAINT` 同一理由）
        blit();
        return;
      case kConfigureNotify: {
        const auto& configure = as<XConfigureEvent>(event);
        if (configure.window != window_) return;
        if (configure.width <= 0 || configure.height <= 0) return;
        const int width = configure.width;
        const int height = configure.height;
        // **立即**更新 `logical_size()` 的数据源——这是打破"等待输入才清晰"死锁的关键：
        // 若仍报旧值，`tick` 会认为视口没变 → 不重绘 → 重建永不发生。
        // 与 Win32 的 `WM_SIZE` 逐字同源。
        logical_width_exact_ = static_cast<float>(width) / scale_;
        logical_height_exact_ = static_cast<float>(height) / scale_;
        logical_width_ = std::max(1, static_cast<int>(std::lround(logical_width_exact_)));
        logical_height_ = std::max(1, static_cast<int>(std::lround(logical_height_exact_)));
        size_dirty_ = true;
        repaint_for_resize();
        return;
      }
      case kClientMessage: {
        const auto& message = as<XClientMessageEvent>(event);
        if (message.message_type == wm_protocols_ &&
            static_cast<XId>(message.data.l[0]) == wm_delete_window_) {
          close_requested_ = true;
        } else if (message.message_type == net_wm_state_) {
          update_maximized_from_property();
        }
        return;
      }
      case kPropertyNotify: {
        const auto& property = as<XPropertyEvent>(event);
        if (property.window == window_ && property.atom == net_wm_state_) {
          update_maximized_from_property();
        }
        return;
      }
      case kFocusIn:
        events_.push_back(ui::Event{.kind = ui::EventKind::FocusIn});
        return;
      case kFocusOut:
        events_.push_back(ui::Event{.kind = ui::EventKind::FocusOut});
        return;
      case kEnterNotify: {
        const auto& crossing = as<XInputEvent>(event);
        last_input_root_ = math::Point{static_cast<float>(crossing.x_root),
                                       static_cast<float>(crossing.y_root)};
        ui::Event entered = make_event(crossing);
        entered.kind = ui::EventKind::HoverIn;
        events_.push_back(std::move(entered));
        return;
      }
      case kLeaveNotify: {
        const auto& crossing = as<XInputEvent>(event);
        ui::Event left = make_event(crossing);
        left.kind = ui::EventKind::HoverOut;
        events_.push_back(std::move(left));
        return;
      }
      case kMotionNotify: {
        const auto& motion = as<XInputEvent>(event);
        last_input_root_ = math::Point{static_cast<float>(motion.x_root),
                                       static_cast<float>(motion.y_root)};
        ui::Event moved = make_event(motion);
        moved.kind = ui::EventKind::MouseMove;
        events_.push_back(std::move(moved));
        return;
      }
      case kButtonPress: {
        const auto& button = as<XInputEvent>(event);
        last_input_time_ = button.time;
        last_input_root_ = math::Point{static_cast<float>(button.x_root),
                                       static_cast<float>(button.y_root)};
        if (button.detail == 4 || button.detail == 5) {
          // 滚轮走 button 4/5（X11 传统；6/7 是水平滚轮）
          ui::Event wheel;
          wheel.kind = ui::EventKind::Wheel;
          wheel.position = to_logical(button);
          apply_modifiers(wheel, button.state);
          // 方向约定：`Event::wheel_delta` **向上为正 / 向下为负**（见 `scroll.cpp` 与
          // `terminal.cpp` 的注释）；X11 的 button 4 = 上、5 = 下。
          wheel.wheel_delta = button.detail == 4 ? 1.0f : -1.0f;
          events_.push_back(std::move(wheel));
          return;
        }
        // 双击/三击：X11 **没有** Win32 的 `CS_DBLCLKS`，必须自己按时间 + 位移算。
        // 阈值取桌面通行值（400ms / 4px，与 GTK 同量级）。
        const auto now = std::chrono::steady_clock::now();
        const bool near = std::abs(button.x - last_press_x_) <= 4 &&
                          std::abs(button.y - last_press_y_) <= 4;
        const bool quick = (now - last_press_at_) < std::chrono::milliseconds(400);
        if (button.detail == 1 && near && quick) {
          ++click_streak_;
        } else {
          click_streak_ = 1;
        }
        last_press_at_ = now;
        last_press_x_ = button.x;
        last_press_y_ = button.y;
        push_button(ui::EventKind::MouseDown, button);
        return;
      }
      case kButtonRelease: {
        const auto& button = as<XInputEvent>(event);
        last_input_time_ = button.time;
        if (button.detail == 4 || button.detail == 5) return;  // 滚轮的"松开"不派发
        push_button(ui::EventKind::MouseUp, button);
        // 与 Win32 同一份事件序列（Down → Up → Click）：`Element::activate()` 只在
        // `UiRoot::dispatch` 的 `Click` 分支里被调用——只发 Down/Up 会让按钮
        // "有按压效果但点了没反应"（那边有实测记录）。双击/三击按 `click_streak_` 升级。
        if (button.detail == 1) {
          ui::Event click = make_event(button);
          click.kind = click_streak_ >= 3 ? ui::EventKind::TripleClick
                      : click_streak_ == 2 ? ui::EventKind::DoubleClick
                                           : ui::EventKind::Click;
          click.button = 1;
          click.click_count = click_streak_;
          events_.push_back(std::move(click));
        }
        return;
      }
      case kKeyPress: {
        const auto& key = as<XInputEvent>(event);
        last_input_time_ = key.time;
        push_key(ui::EventKind::KeyDown, key);
        // 文本输入走 `XLookupString`（Xlib 自带的输入法回归）：比自己按 keysym 拼
        // 正确得多——后者在重音组合键、非拉丁键盘（中文输入法）上直接出错。
        char buffer[64] = {};
        XId keysym = 0;
        void* compose = nullptr;
        const int length = x11_library().lookup_string(const_cast<XEvent*>(&event), buffer,
                                                       static_cast<int>(sizeof(buffer)), &keysym,
                                                       &compose);
        if (length > 0) {
          ui::Event text;
          text.kind = ui::EventKind::TextInput;
          text.text.assign(buffer, static_cast<std::size_t>(length));
          // 控制字符由键事件表达（与 Win32 的 `WM_CHAR` 口径一致：那边也跳过 <32）
          if (!text.text.empty() && static_cast<unsigned char>(text.text[0]) >= 32) {
            apply_modifiers(text, key.state);
            events_.push_back(std::move(text));
          }
        }
        return;
      }
      case kKeyRelease: {
        const auto& key = as<XInputEvent>(event);
        last_input_time_ = key.time;
        push_key(ui::EventKind::KeyUp, key);
        return;
      }
      case kSelectionClear:
        // 别人抢走 CLIPBOARD 所有权：如实更新状态（本进程缓存保留，
        // 便于"读回自己刚写的"这一最常见用途）
        selection_owner_ok_ = false;
        return;
      default:
        return;
    }
  }

  /// 读 `_NET_WM_STATE` 判断是否最大化（自绘窗框据此切换按钮形态）。
  void update_maximized_from_property() {
    const XlibApi& x = x11_library();
    if (net_wm_state_ == 0 || window_ == 0 || x.get_window_property == nullptr) return;
    XId actual_type = 0;
    int actual_format = 0;
    unsigned long count = 0;
    unsigned long remaining = 0;
    unsigned char* data = nullptr;
    // `delete=0`：只读不删。属性长度按 64 个元素取（状态原子通常 < 10 个）。
    // 第 7 个实参是 `req_type`（`AnyPropertyType` = 0）
    if (x.get_window_property(display_, window_, net_wm_state_, 0, 64, 0, kAnyPropertyType,
                              &actual_type, &actual_format, &count, &remaining, &data) != 0 ||
        data == nullptr) {
      return;
    }
    bool maximized = false;
    if (actual_format == 32) {
      const auto* atoms = reinterpret_cast<const long*>(data);  // lint-allow: L6 属性视图
      for (unsigned long index = 0; index < count; ++index) {
        const auto atom = static_cast<XId>(atoms[index]);
        if (atom == net_wm_state_maximized_horz_ || atom == net_wm_state_maximized_vert_) {
          maximized = true;
          break;
        }
      }
    }
    // `XGetWindowProperty` 的返回缓冲必须用 `XFree` 释放（Xlib 自己的分配器）。
    // 符号表里已收 `XFree`——不释放就是每次 WM 改状态泄漏几十字节（长期跑会攒）。
    x.free_memory(data);
    maximized_ = maximized;
  }

  [[nodiscard]] auto make_event(const XInputEvent& source) const -> ui::Event {
    ui::Event event;
    event.position = to_logical(source);
    apply_modifiers(event, source.state);
    return event;
  }

  [[nodiscard]] auto to_logical(const XInputEvent& source) const -> math::Point {
    return math::Point{
        static_cast<float>(static_cast<double>(source.x) / static_cast<double>(scale_)),
        static_cast<float>(static_cast<double>(source.y) / static_cast<double>(scale_))};
  }

  void push_button(ui::EventKind kind, const XInputEvent& source) {
    ui::Event event = make_event(source);
    event.kind = kind;
    // 按钮号与 Win32 对齐：1=左 2=右 3=中。**注意 X11 与 Win32 的定义不同**：
    // X11 是 1=左 2=中 3=右，Win32 是 1=左 2=右 3=中——不换算的话
    // 右键会变中键、中键变右键（`TimelineView`/`Viewport` 的中键平移会失效）。
    switch (source.detail) {
      case 1: event.button = 1; break;
      case 2: event.button = 3; break;  // X11 中键 → 框架（Win32 口径）中键
      case 3: event.button = 2; break;  // X11 右键 → 框架右键
      default: event.button = static_cast<int>(source.detail); break;
    }
    event.click_count = click_streak_;
    events_.push_back(std::move(event));
  }

  void push_key(ui::EventKind kind, const XInputEvent& source) {
    const XlibApi& x = x11_library();
    ui::Event event;
    event.kind = kind;
    if (x.keycode_to_keysym != nullptr && display_ != nullptr) {
      // group 0 / level 0：默认键盘组与未按 Shift 的层。
      // 为什么不用 `XLookupKeysym`：它假定"事件里的键码与当前映射一致"，而
      // WM 改键盘布局（切换输入法/语言）后事件可能带旧键码——`XkbKeycodeToKeysym`
      // 每次都按当前映射解，是 XKB 时代的正确做法。
      const XId keysym = x.keycode_to_keysym(display_, source.detail, 0);
      event.key = key_name_from_keysym(keysym);
    }
    apply_modifiers(event, source.state);
    events_.push_back(std::move(event));
  }

  /// 拖动/缩放期间同步重画一帧（由 `ConfigureNotify` 驱动）。
  ///
  /// 为什么要**重入保护**：`repaint_` 内部会取画布并送显，可能再次触发尺寸变化 →
  /// 再次进 `ConfigureNotify` → 无限递归（Win32 侧实测过：拖动时直接栈溢出崩掉）。
  void repaint_for_resize() {
    if (resizing_ || repaint_ == nullptr) return;
    resizing_ = true;
    repaint_();
    resizing_ = false;
  }

  void teardown() {
    const XlibApi& x = x11_library();
    // 拆段前先让服务端把在途的读完（否则 `XShmDetach` 与仍在处理的上屏请求竞争：
    // 服务端报 `BadShmSeg`，而错误处理器只会记一笔日志）。
    if (display_ != nullptr && present_slots_.in_flight_count() > 0) x.sync(display_, 0);
    present_slots_.mark_all_idle();
    held_events_.clear();
    destroy_backbuffer();
    if (display_ != nullptr) {
      if (gc_ != nullptr) {
        x.free_gc(display_, gc_);
        gc_ = nullptr;
      }
      if (window_ != 0) {
        x.destroy_window(display_, window_);
        window_ = 0;
      }
      x.close_display(display_);
      display_ = nullptr;
    }
    surface_.reset();
  }

  XDisplay* display_{nullptr};
  XId window_{0};
  XId root_{0};
  void* visual_{nullptr};
  int depth_{24};
  void* gc_{nullptr};

  XId wm_protocols_{0};
  XId wm_delete_window_{0};
  XId wm_change_state_{0};
  XId net_wm_name_{0};
  XId net_wm_pid_{0};
  XId net_wm_state_{0};
  XId net_wm_state_maximized_horz_{0};
  XId net_wm_state_maximized_vert_{0};
  XId net_wm_moveresize_{0};
  XId motif_wm_hints_{0};
  XId utf8_string_{0};
  XId clipboard_atom_{0};

  std::unique_ptr<raster::Surface> surface_{};
  XImage* image_{nullptr};
  std::vector<std::uint32_t> pixels_{};
  /// 共享段槽（可用时上屏走它；否则都不为真）。
  /// 两块 = 双缓冲（交替写，靠完成事件判哪块已被服务端读完）。
  std::array<ShmSlot, kPresentSlotLimit> shm_slots_{};
  /// 槽环状态（哪块在途 / 复用前要补写哪里）。纯逻辑，有单测。
  PresentSlotRing present_slots_{0};
  /// 想建几块段：2 = 双缓冲；降级/尺寸重建保留它的当前值。
  int shm_slot_goal_{kPresentSlotLimit};
  /// `XShmGetEventBase + ShmCompletion`：完成事件的 `type` 值（运行期取，不是常量）。
  int shm_completion_base_{0};
  /// 收到过的完成事件总数（判“这个环境到底回报不回报”的依据，见 `ensure_slot_available`）。
  std::uint64_t shm_completions_seen_{0};
  /// 环境不回报完成事件（已降级为单块段 + 每帧排空）。
  bool shm_no_completion_{false};
  /// 等待完成事件期间“挟”到的事件（不能丢：它们先于我们自己的事件到达）。
  std::deque<XEvent> held_events_{};
  std::string shm_status_{};
  bool shm_attempted_{false};
  /// `XShmQueryExtension` 已通过（= 探测到扩展，可以尝试建段）。
  /// 与 `shm_status_` 的文案分开：文案是给人看的，不能兼任控制流判据。
  bool shm_extension_ready_{false};
  bool using_shm_{false};
  int image_width_{0};
  int image_height_{0};
  /// 上一帧实际上屏的区域尺寸（诊断用；`present_note` 报它）。
  int last_present_width_{0};
  int last_present_height_{0};

  std::deque<ui::Event> events_{};
  std::function<void()> repaint_{};

  std::string title_{};
  std::string renderer_{"auto"};
  std::string renderer_name_{"software"};
  std::string renderer_note_{};
  std::string clipboard_cache_{};

  bool selection_owner_ok_{false};
  bool decorations_{false};
  bool window_resizable_{true};
  bool shown_{false};
  bool close_requested_{false};
  bool maximized_{false};
  bool size_dirty_{false};
  bool resizing_{false};
  /// 本帧被改动的区域（逻辑像素；空 = 整帧）——由 `set_present_region` 写入。
  math::IntRect present_region_{};

  int logical_width_{1280};
  int logical_height_{720};
  float logical_width_exact_{1280.0f};
  float logical_height_exact_{720.0f};
  float scale_{1.0f};
  std::uint64_t frames_{0};

  /// 最近一次真实输入事件的**根坐标**与时间戳：
  /// `_NET_WM_MOVERESIZE` 两者都要（WM 拿时间戳校验"是否对应一次真实按下"）。
  XTime last_input_time_{0};
  math::Point last_input_root_{0.0f, 0.0f};
  std::chrono::steady_clock::time_point last_press_at_{};
  int last_press_x_{0};
  int last_press_y_{0};
  int click_streak_{0};
};

}  // namespace

[[nodiscard]] auto create_x11_backend() -> Result<std::unique_ptr<Backend>> {
  if (!x11_available()) {
    return unexpected(ErrorCode::Unsupported, "未探测到 libX11（无 X 显示服务）");
  }
  return std::unique_ptr<Backend>(std::make_unique<X11Backend>());
}

}  // namespace st::shell

#endif  // !defined(_WIN32)
