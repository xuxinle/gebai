/// @file
/// @brief 可编程嵌入浏览器的 **Windows / WebView2** 实现（`st::shell::webview`）。
///
/// ## 平台差异的全部落点
///
/// 本文件是 WebView2 唯一的接触面（`CONVENTIONS.md` §10 第 1 条）：`WebView2.h` 的
/// 包含、`LoadLibrary`、COM 接口、`HWND` 全在这里，别处只认 `webview.hpp` 的中立形状。
///
/// ## 三层分离（为什么这么切）
///
/// ```
///   platform_win32_webview.cpp   ← 本文件：COM/Runtime/窗口（平台细节）
///         ↑
///   include/st/shell/webview.hpp ← 中立接口（无 HWND / 无 COM）
///         ↑
///   ui::WebView ← 组件（复用 NativeHost 的几何机械）
/// ```
///
/// ## 三条实测坑（都在这里被处理，别处不必知道）
///
/// 1. **DPI 会被导航重置**：`put_RasterizationScale(1.25)` 之后页面一导航，读回
///    变成 `1.000`（实测）。所以 `RasterizationScaleChanged` 事件里必须把目标缩放
///    **重新下发**，否则高 DPI 下网页缩成一半，而所有状态读数看起来都正常。
/// 2. **`ExecuteScript` 不解析 Promise**：`async` 函数返回 `{}`；再发一次同步读取
///    也拿不到（两段之间没有时序保证，实测拿到 `"pending"`）。异步结果由页面经
///    `postMessage` 回推——这条路顺带就是"页面 → 宿主"那条能力。
/// 3. **Loader 只能运行时加载**：`WebView2LoaderStatic.lib` 是 MSVC 产物，MinGW
///    链不上（`__security_cookie` / `IID_*` 重定义）。见 `third_party/SOURCES.md`。
///
/// ## 线程契约
///
/// 全部在 UI 线程（WebView2 要求 STA）。`webview_execute_script` 名义上是异步的
///（COM 回调回来才知道结果），但这里的实现**泵消息等回**——它与界面同线程，
/// 而"执行一段脚本"的耗时由页面决定（通常亚毫秒）。**不泵会死锁**（结果是经消息队列
/// 回来的），**无限泵会挂住界面**，所以两者都设了上限。

#include "st/shell/webview.hpp"

// 本文件只在 Windows 目标编译：`src/shell/*.cpp` 的 glob 是所有平台共用的，
// 若不加守卫，Linux/macOS 构建会去包含 `<windows.h>` 直接失败
//（与 `platform_win32.cpp` / `platform_x11.cpp` 同一姿态）。
#if defined(_WIN32)

// `windows.h` 默认把 `min`/`max` 定义成宏：一旦带上，`std::min`/`std::max` 的调用点
// 会以「C2589 非法标记」报错，与真正原因相隔很远。
#define NOMINMAX 1
#include <windows.h>
// `CoInitializeEx`（套间初始化）与 `CoTaskMemFree` 在 objbase.h。
// WebView2 的头只带 objidl/oaidl，不保证带出这一个。
#include <objbase.h>

// `SHCreateMemStream`（把字节包成 COM `IStream`）：头在 shlwapi.h，库是 shlwapi
//（核对 link-libs.txt 的 windows 节，**新增系统库要同步那份单一数据源**）。
#include <shlwapi.h>

// ## 第三方头的告警收敛（不是改上游，见 `third_party/SOURCES.md`）
//
// `WebView2.h` 是上游单文件分发的 IDL 产物，带有三类在本工程 `-Werror` 下会红的特征：
// - `#pragma warning(...)`：GCC 不认，`-Wunknown-pragmas` 报错；
// - 每个 COM 接口都是「有虚函数但无虚析构」：`-Wnon-virtual-dtor` 报错；
// - `(TYPE*)` 形式的 C 转换：`-Wold-style-cast` 报错。
//
// 三条都只在**包含这个头**时收敛，本文件自己的代码仍受全量严格集约束
//（`CONVENTIONS.md` §8 第三方头纪律：我们不为上游的写法放宽自家代码）。
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#include "WebView2.h"
#pragma GCC diagnostic pop

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "battery/embed.hpp"
#include "st/core/fs.hpp"
#include "st/core/log.hpp"

#include "child_view_geometry.hpp"

namespace st::shell::webview {
namespace {

// ─────────────────────────────────────────────────────────────────────────────
// 小工具：宽窄串、COM 引用计数
// ─────────────────────────────────────────────────────────────────────────────

auto widen(std::string_view text) -> std::wstring {
  if (text.empty()) return {};
  const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
  if (size <= 0) return {};
  std::wstring out(static_cast<std::size_t>(size), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
  return out;
}

auto narrow(const wchar_t* text) -> std::string {
  if (text == nullptr) return {};
  const int size = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1) return {};
  // `size` 含结尾 NUL，故减 1；多出来的那一格不参与构造。
  std::string out(static_cast<std::size_t>(size - 1), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
  return out;
}

/// 本线程的 COM 套间初始化（**WebView2 的硬前提**）。
///
/// ## 为什么必须自己调（实测代价）
///
/// WebView2 的每个接口都是 COM 对象，而 `CreateCoreWebView2EnvironmentWithOptions`
/// 要求**当前线程已调入套间**。没调时它的真实返回值是
/// `0x800401F0`（`CO_E_NOTINITIALIZED`，“CoInitialize has not been called”），
/// 而本层只看到“建环境失败”这一句。
///
/// 症状很阴：**界面一切正常、属性面 `supported=true`**（Loader 能加载、Runtime 版本
/// 查得到），只有导航与页面永远不出现。gallery 之所以没暴露，是因为它进页面时
/// 进程里已另有路径调过；换一个“窗口一建就导般”的宿主（桌面端就是这样）立刻现形。
///
/// ## 为何是 STA + 只调一次
///
/// WebView2 要求 `COINIT_APARTMENTTHREADED`（同 `docs/PITFALLS.md` 里那条）。
/// `RPC_E_CHANGED_MODE`（套间已被别人按 MTA 调过）**不算失败**：那时进程已在
/// 一个套间里，COM 可用，只是模式不是我们想要的——如实记一笔，继续走。
///
/// 返回值仅用于日志：真失败时后续 `create_environment` 会给出更具体的原因。
void ensure_com_apartment() {
  static const bool initialized = [] {
    const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr)) {
      ST_LOG_INFO("webview：COM 套间已初始化（STA）");
      return true;
    }
    if (hr == RPC_E_CHANGED_MODE) {
      ST_LOG_INFO("webview：COM 套间已是其它模式（RPC_E_CHANGED_MODE），继续");
      return true;
    }
    ST_LOG_INFO("webview：CoInitializeEx 失败（HRESULT=0x{:08X}）", static_cast<unsigned>(hr));
    return false;
  }();
  (void)initialized;
}

/// COM 接口的 RAII 包装（只做 AddRef/Release，不做别的）。
///
/// ## 为什么不用 `Microsoft::WRL::ComPtr`
///
/// MinGW 的 `<wrl.h>` **不提供** `wrl/client.h` 之外的东西（实测：
/// `Microsoft::WRL::Callback` 不存在），而 `ComPtr` 与 `Callback` 是配套使用的。
/// 只用 `ComPtr` 而自己写回调会得到"一半 WRL"，反而更难读。这里用一个 30 行的
/// 最小包装，行为清楚、跨工具链一致。
template <typename T>
class Com {
 public:
  Com() = default;
  explicit Com(T* raw) : ptr_(raw) {}  // 接管一个已持有引用的指针
  ~Com() { reset(); }

  Com(const Com& other) : ptr_(other.ptr_) {
    if (ptr_ != nullptr) ptr_->AddRef();
  }
  auto operator=(const Com& other) -> Com& {
    if (this == &other) return *this;
    reset();
    ptr_ = other.ptr_;
    if (ptr_ != nullptr) ptr_->AddRef();
    return *this;
  }
  Com(Com&& other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }
  auto operator=(Com&& other) noexcept -> Com& {
    if (this == &other) return *this;
    reset();
    ptr_ = other.ptr_;
    other.ptr_ = nullptr;
    return *this;
  }

  void reset() {
    if (ptr_ != nullptr) {
      ptr_->Release();
      ptr_ = nullptr;
    }
  }
  [[nodiscard]] auto get() const noexcept -> T* { return ptr_; }
  [[nodiscard]] auto operator->() const noexcept -> T* { return ptr_; }
  [[nodiscard]] auto valid() const noexcept -> bool { return ptr_ != nullptr; }
  /// 取 `T**` 以便交给"出参式"的 COM 调用；调用前必须为空。
  auto put() -> T** {
    reset();
    return &ptr_;
  }

 private:
  T* ptr_{nullptr};
};

/// 单方法 COM 回调的通用实现。
///
/// ## 为什么不用 `Microsoft::WRL::Callback`（MinGW 下不存在）
///
/// 而且即便存在，它在 MinGW 下还依赖 `__uuidof`——`WebView2.h` 的接口**没有** uuid 属性
///（只有 `EXTERN_C const IID IID_Xxx` 常量），所以 `__uuidof(IFace)` 直接链接失败
///（实测：`undefined reference to __mingw_uuidof<…>`）。因此 IID 必须由模板参数显式给出。
///
/// ## `Invoke` 必须吞异常
///
/// 这是 C++ 对象穿过 COM ABI 边界的地方：让它抛出就是 UB。本仓禁 `throw`（L5），
/// 但 `std::string` 分配失败仍会抛 `std::bad_alloc`——所以这里 `catch` 一下，
/// 转成 `E_FAIL` 让平台的错误处理接手（比消失在一个未定义行为里好）。
/// ## 为何 `-Wnon-virtual-dtor` 要在这里单独收敛
///
/// 上面包 `WebView2.h` 的 pragma 只盖住**包头**，盖不住「我们继承它的接口」这个
/// 实例化点——告警是在**模板体**里报的（基类 `IFace` 有可访问的非虚析构）。
/// 而那个基类是**第三方的 COM 接口**：COM 的释放靠 `Release()` 引用计数，根本不走析构，
/// 所以这条告警在本场景是形态误报。收敛范围限于这一个类定义。
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
template <typename IFace, const IID* kIid, typename Signature>
class Handler;

template <typename IFace, const IID* kIid, typename... Args>
class Handler<IFace, kIid, HRESULT(Args...)> final : public IFace {
 public:
  explicit Handler(std::function<HRESULT(Args...)> forward) : forward_(std::move(forward)) {}
  Handler(const Handler&) = delete;
  auto operator=(const Handler&) -> Handler& = delete;

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
    if (out == nullptr) return E_POINTER;
    if (::IsEqualIID(riid, *kIid) || ::IsEqualIID(riid, IID_IUnknown)) {
      *out = static_cast<IFace*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return static_cast<ULONG>(::InterlockedIncrement(&refs_));
  }
  ULONG STDMETHODCALLTYPE Release() override {
    const long now = ::InterlockedDecrement(&refs_);
    // COM 的释放语义就是“最后一个引用者删自己”：对象是 COM 回调，生命周期完全由
    // 引用计数决定（没有外部 owner 能替它做这件事）。这是 COM 的唯一正确写法。
    if (now == 0) delete this;   // lint-allow: L1 COM 引用计数归零时自我销毁（无法改成 unique_ptr）
    return static_cast<ULONG>(now);
  }
  HRESULT STDMETHODCALLTYPE Invoke(Args... args) override {
    try {
      return forward_(args...);
    } catch (...) {
      return E_FAIL;
    }
  }

 private:
  ~Handler() = default;
  std::function<HRESULT(Args...)> forward_;
  long refs_{1};
};
#pragma GCC diagnostic pop

/// 造一个回调（初始引用计数 1；交给 COM 之后由它负责 Release）。
template <typename IFace, const IID* kIid, typename Signature>
[[nodiscard]] auto make_handler(std::function<Signature> forward) -> IFace* {
  // 返回裸指针是**COM 的 ABI 要求**：调用方要把 `IFace*` 传给 `AddXxxEventHandler`，
  // 之后引用计数归零时由 `Release` 自我销毁（见上）。用 `unique_ptr` 包反而会在
  // 函数返回时就把对象删掉。
  return new Handler<IFace, kIid, Signature>(std::move(forward));  // lint-allow: L1 COM 回调的 ABI 产出（所有权转交引用计数）
}

// ─────────────────────────────────────────────────────────────────────────────
// Loader：运行时加载（不产生链接期依赖）
// ─────────────────────────────────────────────────────────────────────────────

/// `WebView2Loader.dll` 的符号表。
///
/// ## 为何是"进程级"而不是每实例一份
///
/// `LoadLibrary` 对同一模块是**引用计数**语义，重复加载返回同一句柄；符号地址也就一样。
/// 进程内做成一份，避免"两处各自 LoadLibrary，一处释放另一处还在用"那类顺序事故。
///
/// ⚠ 这里的 `static` 局部量是**惰性初始化**（C++11 起线程安全），不是可变的
/// 进程级共享状态：它由一个函数独占管理、外部拿不到句柄去做别的（L8 的口径是
/// "可变的进程级共享状态"，本类不共享可变数据——`loaded_` 只从 false 变 true 一次）。
class Loader {
 public:
  /// 找并加载 Loader DLL。找不到返回 `nullptr` 并在 `error_` 里放人话原因。
  ///
  /// 查找顺序（**每一档都记日志**，因为"到底从哪加载的"是这类动态加载最常见的疑问）：
  /// 1. exe 同目录（分发形态：单 exe + 邻居 DLL）；
  /// 2. 用户在环境变量 `ST_WEBVIEW2_LOADER` 指定的路径；
  /// 3. 本进程内存里嵌的那一份（`b::embed`；落盘到临时目录后加载）——见 `load_embedded`。
  static auto instance() -> Loader& {
    static Loader loader;
    return loader;
  }

  [[nodiscard]] auto loaded() const noexcept -> bool { return create_environment != nullptr; }
  [[nodiscard]] auto error() const -> const std::string& { return error_; }

  decltype(&CreateCoreWebView2EnvironmentWithOptions) create_environment{nullptr};
  decltype(&GetAvailableCoreWebView2BrowserVersionString) browser_version{nullptr};

  /// Runtime 版本串（未安装时为空）。
  [[nodiscard]] auto runtime_version() const -> std::string {
    if (browser_version == nullptr) return {};
    LPWSTR version = nullptr;
    if (SUCCEEDED(browser_version(nullptr, &version)) && version != nullptr) {
      std::string out = narrow(version);
      ::CoTaskMemFree(version);
      return out;
    }
    return {};
  }

 private:
  Loader() { load(); }

  void load();
  auto load_from(const std::wstring& path) -> bool;
  /// 把嵌入的 Loader DLL 落到一个临时文件再加载。
  auto load_embedded() -> bool;

  HMODULE module_{nullptr};
  std::string error_{};
};

}  // namespace
}  // namespace st::shell::webview

// ─────────────────────────────────────────────────────────────────────────────
// Loader 实现
// ─────────────────────────────────────────────────────────────────────────────

namespace st::shell::webview {
namespace {

/// exe 所在目录（不含结尾分隔符；拿不到时为空）。
auto executable_directory() -> std::string {
  std::wstring buffer(MAX_PATH, L'\0');
  for (;;) {
    const DWORD written = ::GetModuleFileNameW(nullptr, buffer.data(),
                                               static_cast<DWORD>(buffer.size()));
    if (written == 0) return {};
    if (written < buffer.size()) {
      buffer.resize(written);
      break;
    }
    // 缓冲区不够：按返回值扩（长路径/深层目录）。
    buffer.resize(buffer.size() * 2);
  }
  const std::size_t slash = buffer.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return {};
  return narrow(buffer.substr(0, slash).c_str());
}

/// 把一段字节写成文件（已存在且大小相同则跳过——避免每次启动重写）。
///
/// ## 为什么要落盘而不能"从内存加载 DLL"
///
/// Windows 的 `LoadLibrary` 只吃文件路径（没有 `dlopen` 那种 `fmemopen` 等价物）。
/// 从内存加载需要自写 PE 映射器——那是另一个量级的复杂度与风险面，为一个 194KB 的
/// 官方 DLL 不值得。落盘到临时目录 + 加载，是这一层的标准做法。
auto materialize(const std::wstring& path, std::span<const std::uint8_t> bytes) -> bool {
  if (bytes.empty()) return false;
  const std::string narrow_path = narrow(path.c_str());
  if (fs::is_regular_file(narrow_path)) {
    // 大小相同就认为已经是同一份（Loader 是官方二进制，版本变则大小变——
    // 这条判据的失效面是"同大小不同版本"，而那种情形的后果只是白加载一次旧 Loader，
    // 下一轮版本比较会把 Runtime 不匹配报出来，不会静默错）。
    auto existing = fs::read_bytes(narrow_path);
    if (existing && existing->size() == bytes.size()) return true;
  }
  if (auto status = fs::write_bytes(narrow_path, bytes); !status) return false;
  return true;
}

auto Loader::load_from(const std::wstring& path) -> bool {
  module_ = ::LoadLibraryW(path.c_str());
  if (module_ == nullptr) {
    ST_LOG_INFO("webview：LoadLibrary 失败（{}）：GetLastError={}", narrow(path.c_str()),
                ::GetLastError());
    return false;
  }
  create_environment = reinterpret_cast<decltype(create_environment)>(
      reinterpret_cast<void*>(::GetProcAddress(module_, "CreateCoreWebView2EnvironmentWithOptions")));
  browser_version = reinterpret_cast<decltype(browser_version)>(
      reinterpret_cast<void*>(
          ::GetProcAddress(module_, "GetAvailableCoreWebView2BrowserVersionString")));
  if (create_environment == nullptr) {
    ST_LOG_INFO("webview：{} 里没有 CreateCoreWebView2EnvironmentWithOptions（不是 Loader？）",
                narrow(path.c_str()));
    ::FreeLibrary(module_);
    module_ = nullptr;
    return false;
  }
  ST_LOG_INFO("webview：Loader 已加载（{}）", narrow(path.c_str()));
  return true;
}

auto Loader::load_embedded() -> bool {
  // Loader 随库编译期嵌入（`st.pkg` 的 `embed`），所以产物是单 exe：
  // 用户不需要在 exe 旁边放 DLL。**只有真的用到浏览器时**才落盘。
  const auto file = b::embed<"third_party/webview2/WebView2Loader.dll">();
  const auto bytes = std::span<const std::uint8_t>(
      reinterpret_cast<const std::uint8_t*>(file.data()), file.length());
  if (bytes.empty()) {
    ST_LOG_INFO("webview：嵌入的 Loader 为空（构建期未打包？）");
    return false;
  }
  wchar_t temp_root[MAX_PATH]{};
  const DWORD written = ::GetTempPathW(MAX_PATH, temp_root);
  if (written == 0) return false;
  std::wstring directory(temp_root);
  directory.append(L"gebai-webview2");
  (void)::CreateDirectoryW(directory.c_str(), nullptr);   // 已存在不是错误
  std::wstring path = directory;
  path.append(L"\\WebView2Loader.dll");
  if (!materialize(path, bytes)) {
    ST_LOG_INFO("webview：嵌入的 Loader 落盘失败");
    return false;
  }
  return load_from(path);
}

void Loader::load() {
  const std::string exe_dir = executable_directory();
  // ① exe 同目录（分发形态：想用更新的 Loader 就直接覆盖它）
  if (!exe_dir.empty()) {
    if (load_from(widen(exe_dir + "\\WebView2Loader.dll"))) return;
  }
  // ② 环境变量显式指定（部署/排障用）
  if (const auto explicit_path = fs::read_env("ST_WEBVIEW2_LOADER");
      explicit_path.has_value() && !explicit_path->empty()) {
    if (load_from(widen(*explicit_path))) return;
  }
  // ③ 编译期嵌入的那一份（单 exe 形态的默认路径）
  if (load_embedded()) return;

  error_ = "找不到 WebView2Loader.dll（exe 同目录、ST_WEBVIEW2_LOADER、嵌入副本都不可用）";
}

// ─────────────────────────────────────────────────────────────────────────────
// 页面对象
// ─────────────────────────────────────────────────────────────────────────────

/// 虚拟 origin 的主机名。
///
/// ## 为什么需要一个"看起来像域名"的 origin
///
/// 内嵌资源要享受到 Web 的**完整**语义（同源策略、相对路径解析、ES module、
/// `fetch('./x.json')`），页面就必须有一个**真 origin**——`about:blank` / `data:` /
/// `file:` 都不行（前两者没有 origin，`file:` 受严格限制且路径泄漏）。
///
/// 用一个**不可能真实存在**的域名（`.invalid` 是 RFC 2606 保留的顶级域，
/// 保证不会与真实站点撞名），其下所有请求都被 `WebResourceRequested` 拦下，
/// 永远不出网。这也是"资源全在 exe 里、离线可用"的结构保证。
inline constexpr const wchar_t* kVirtualOriginHost = L"https://app.st.invalid";

/// 把页面内路径拼成完整虚拟地址。
auto virtual_url(std::string_view path) -> std::string {
  std::string out = narrow(kVirtualOriginHost);
  const std::string normalized = WebAssets::normalize(path);
  out.append(normalized);
  return out;
}

/// 从完整地址里取出页面内路径（剥 scheme/host，去查询串与锚点）。
///
/// 与 `WebAssets::normalize` 分开写：那个吃"页面内路径"，这个吃"完整 URL"。
/// 合并成一个函数会让调用方经常拿错输入类型（而错法的症状是整站 404）。
auto path_from_url(const std::wstring& url) -> std::string {
  const std::string text = narrow(url.c_str());
  const std::size_t scheme_end = text.find("://");
  std::string rest = scheme_end == std::string::npos ? text : text.substr(scheme_end + 3);
  const std::size_t slash = rest.find('/');
  rest = slash == std::string::npos ? "/" : rest.substr(slash);
  return WebAssets::normalize(rest);
}

/// `ICoreWebView2Environment` 等 COM 对象的构造/释放顺序在这里统一。
///
/// ## 为何销毁顺序是"先页面、再 controller、最后 environment"
///
/// WebView2 的引用是链式的：controller 持有页面、environment 支撑 controller。
/// 反序释放会留下一个"页面还活着但宿主没了"的窗口——实测表现为退出时进程挂住
/// （浏览器进程还在等一条永不到来的消息）。
class Win32WebView final : public WebViewHost {
 public:
  Win32WebView(HWND parent, float scale);
  ~Win32WebView() override;

  Win32WebView(const Win32WebView&) = delete;
  auto operator=(const Win32WebView&) -> Win32WebView& = delete;

  [[nodiscard]] auto webview_supported() const noexcept -> bool override;
  [[nodiscard]] auto webview_unsupported_reason() const -> std::string override;
  [[nodiscard]] auto webview_create(math::Rect bounds) -> Status override;
  void webview_destroy() override;
  void webview_sync(math::Rect bounds, math::Rect clip) override;

  [[nodiscard]] auto webview_navigate(std::string_view url) -> Status override;
  [[nodiscard]] auto webview_load_html(std::string_view html) -> Status override;
  [[nodiscard]] auto webview_load_embedded(std::string_view entry, const WebAssets& assets)
      -> Status override;

  [[nodiscard]] auto webview_reload() -> Status override;
  [[nodiscard]] auto webview_stop() -> Status override;
  [[nodiscard]] auto webview_go_back() -> Status override;
  [[nodiscard]] auto webview_go_forward() -> Status override;

  [[nodiscard]] auto webview_add_init_script(std::string_view script) -> Status override;
  [[nodiscard]] auto webview_execute_script(std::string_view script, std::string& result)
      -> Status override;
  [[nodiscard]] auto webview_post_message(std::string_view text) -> Status override;
  [[nodiscard]] auto webview_set_zoom(double factor) -> Status override;
  [[nodiscard]] auto webview_set_script_enabled(bool enabled) -> Status override;

  [[nodiscard]] auto webview_state() const -> State override;
  [[nodiscard]] auto webview_take_events() -> std::vector<Event> override;
  [[nodiscard]] auto webview_native_handle() -> void* override;

 private:
  /// 建宿主容器子窗口（`bounds` 是**逻辑像素**，这里换算成客户区像素）。
  auto ensure_host_window(math::Rect bounds) -> bool;
  /// 真正发起"建 environment → 建 controller"（异步；成功后继续装事件与内容）。
  void begin_create();
  /// controller 就绪后的装配：DPI、事件、过滤器、内容。
  void on_controller_ready();
  /// 装虚拟 origin 的请求拦截（清空旧表再装新的）。
  void install_asset_filter(const WebAssets& assets);
  /// 拦截器对单条请求的答复。
  void serve_request(ICoreWebView2WebResourceRequestedEventArgs* args);
  /// 下发目标 DPI（并记下它——导航后会被重置，见文件头第 1 条）。
  void apply_rasterization_scale();
  /// 泵消息直到 `done()` 为真或超时（COM 回调要走消息队列，不泵就永远等不到）。
  [[nodiscard]] auto pump_until(const std::function<bool()>& done, int timeout_ms) -> bool;
  /// 当前逻辑→物理换算（与 `NativeHost` 同一口径：乘 device scale）。
  [[nodiscard]] auto scale() const noexcept -> float;
  /// 期望的 rasterization scale（= 逻辑到物理的倍数）。
  [[nodiscard]] auto target_rasterization_scale() const noexcept -> double;

  void push_event(Event event);
  void sync_bounds_to_host();
  /// 把一段脚本注册为“每次新文档创建时执行”（`AddScriptToExecuteOnDocumentCreated`）。
  auto apply_init_script(const std::string& script) -> HRESULT;

  HWND parent_{nullptr};
  HWND host_{nullptr};
  float device_scale_{1.0F};
  math::Rect last_bounds_{};
  math::Rect last_clip_{};
  bool has_synced_{false};
  bool hidden_by_clip_{false};

  // COM 对象（顺序即声明顺序；析构时按 life_ 状态显式释放）
  Com<ICoreWebView2Environment> environment_{};
  Com<ICoreWebView2Controller> controller_{};
  Com<ICoreWebView2> webview_{};
  Com<ICoreWebView2Settings> settings_{};
  Com<ICoreWebView2Controller3> controller3_{};

  bool creating_{false};   ///< 创建请求已发出、还没回调
  bool ready_{false};      ///< 第一次导航成功完成过
  double zoom_{1.0};
  double target_scale_{1.0};
  SourceKind source_{SourceKind::None};
  std::string current_url_{};
  std::string title_{};
  bool loading_{false};
  bool script_enabled_{true};
  /// 文档初始化脚本（注册顺序即执行顺序；页面就绪时逐条下发）。
  std::vector<std::string> init_scripts_{};
  std::size_t asset_count_{0};
  /// 虚拟 origin 的资源表副本（拦截器在 COM 回调里跑，不能引用调用方的临时对象）。
  WebAssets assets_{};
  bool assets_active_{false};

  mutable std::mutex events_mutex_{};
  std::vector<Event> events_{};

  std::string failure_{};   ///< 创建失败的人话原因
};

}  // namespace
}  // namespace st::shell::webview

// ─────────────────────────────────────────────────────────────────────────────
// 组件实现
// ─────────────────────────────────────────────────────────────────────────────

namespace st::shell::webview {
namespace {

/// 宿主容器窗口类（与 `NativeHost` 的 `StChildViewHost` 分开注册：那个类带
/// `WS_EX_TRANSPARENT`（不抢鼠标，因为外部窗口自己吃事件），而浏览器**必须**吃鼠标）。
[[nodiscard]] auto host_class_name() -> const wchar_t* {
  static const wchar_t* const kName = [] {
    constexpr const wchar_t* kClassName = L"StWebViewHost";
    WNDCLASSEXW cls{};
    cls.cbSize = sizeof(cls);
    cls.lpfnWndProc = ::DefWindowProcW;
    cls.hInstance = ::GetModuleHandleW(nullptr);
    cls.hCursor = ::LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    cls.lpszClassName = kClassName;
    // 已注册（第二个实例）时返回 0 / ERROR_CLASS_ALREADY_EXISTS，不是错误。
    ::RegisterClassExW(&cls);
    return kClassName;
  }();
  return kName;
}

constexpr int kCreateTimeoutMs = 15000;
constexpr int kScriptTimeoutMs = 5000;

}  // namespace

Win32WebView::Win32WebView(HWND parent, float scale)
    : parent_(parent), device_scale_(scale > 0.0F ? scale : 1.0F) {}

Win32WebView::~Win32WebView() { webview_destroy(); }

auto Win32WebView::scale() const noexcept -> float {
  return device_scale_ > 0.0F ? device_scale_ : 1.0F;
}

auto Win32WebView::target_rasterization_scale() const noexcept -> double {
  // WebView2 的 rasterization scale 就是"物理 / 逻辑"，与后端的 `device_scale` 同义。
  return static_cast<double>(scale()) * zoom_;
}

auto Win32WebView::webview_supported() const noexcept -> bool {
  if (parent_ == nullptr) return false;
  return Loader::instance().loaded();
}

auto Win32WebView::webview_unsupported_reason() const -> std::string {
  if (parent_ == nullptr) return "窗口未创建";
  const Loader& loader = Loader::instance();
  if (!loader.loaded()) return loader.error();
  if (loader.runtime_version().empty()) {
    return "未安装 WebView2 Runtime（请安装 Microsoft Edge WebView2 Runtime）";
  }
  return {};
}

auto Win32WebView::pump_until(const std::function<bool()>& done, int timeout_ms) -> bool {
  // ## 为什么必须泵消息
  //
  // WebView2 的所有"完成通知"都经 STA 的消息队列回来。不泵 ⇒ 回调永远不来 ⇒
  // 这里的等待直接超时（实测：不泵时 controller 回调一次都没进，而 HRESULT 全是 S_OK，
  // 看上去像"创建成功但没反应"）。
  //
  // ## 为什么要 `Sleep(0)` 而不是等事件
  //
  // 我们等的是**同一个线程**的消息，没有可等的句柄。`PeekMessage` 空了就短暂让出
  // CPU（`Sleep(1)`：既不让这一核空转，也不拖慢回调到达）。
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
  MSG message{};
  for (;;) {
    while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
      ::TranslateMessage(&message);
      ::DispatchMessageW(&message);
    }
    if (done()) return true;
    if (std::chrono::steady_clock::now() >= deadline) return false;
    ::Sleep(1);
  }
}

auto Win32WebView::ensure_host_window(math::Rect bounds) -> bool {
  if (host_ != nullptr && ::IsWindow(host_) != 0) return true;
  if (parent_ == nullptr) return false;
  // 裁剪：父窗口必须带 `WS_CLIPCHILDREN`，否则浏览器会画到自绘窗框外面去
  //（与 `NativeHost` 的 `adopt_child_view` 同一个理由与同一处处理）。
  const LONG_PTR parent_style = ::GetWindowLongPtrW(parent_, GWL_STYLE);
  if ((parent_style & WS_CLIPCHILDREN) == 0) {
    ::SetWindowLongPtrW(parent_, GWL_STYLE, parent_style | WS_CLIPCHILDREN);
  }
  const float safe = scale();
  const int x = static_cast<int>(bounds.x * safe);
  const int y = static_cast<int>(bounds.y * safe);
  const int width = std::max(1, static_cast<int>(bounds.width * safe));
  const int height = std::max(1, static_cast<int>(bounds.height * safe));
  // `WS_CLIPCHILDREN` 在这里防的是"浏览器的子窗口溢出到我们界面上"。
  host_ = ::CreateWindowExW(0, host_class_name(), L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                            x, y, width, height, parent_, nullptr, ::GetModuleHandleW(nullptr),
                            nullptr);
  if (host_ == nullptr) {
    failure_ = std::format("建宿主窗口失败（GetLastError={}）", ::GetLastError());
    return false;
  }
  ::ShowWindow(host_, SW_SHOWNA);
  return true;
}

auto Win32WebView::webview_create(math::Rect bounds) -> Status {
  if (!webview_supported()) {
    return unexpected(ErrorCode::Unsupported, webview_unsupported_reason());
  }
  if (webview_.valid()) {
    // 已创建：当作"重新落位"（幂等，与 `adopt_child_view` 同一姿态）。
    webview_sync(bounds, bounds);
    return ok();
  }
  if (!ensure_host_window(bounds)) {
    return unexpected(ErrorCode::Io, failure_);
  }
  last_bounds_ = bounds;
  begin_create();
  return ok();
}

void Win32WebView::begin_create() {
  if (creating_) return;
  creating_ = true;
  ready_ = false;
  failure_.clear();
  // COM 套间必须是调入状态，否则建环境直接回 `CO_E_NOTINITIALIZED`（见 ensure_com_apartment）。
  ensure_com_apartment();

  ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* env_handler =
      make_handler<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
                   &IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
                   HRESULT(HRESULT, ICoreWebView2Environment*)>(
          [this](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(result) || env == nullptr) {
              failure_ = std::format("建 WebView2 环境失败（HRESULT=0x{:08X}）",
                                     static_cast<unsigned>(result));
              ST_LOG_INFO("webview：{}", failure_);
              creating_ = false;
              return S_OK;
            }
            env->AddRef();
            environment_ = Com<ICoreWebView2Environment>(env);
            ICoreWebView2CreateCoreWebView2ControllerCompletedHandler* controller_handler =
                make_handler<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
                             &IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
                             HRESULT(HRESULT, ICoreWebView2Controller*)>(
                    [this](HRESULT controller_result,
                           ICoreWebView2Controller* controller) -> HRESULT {
                      if (FAILED(controller_result) || controller == nullptr) {
                        failure_ = std::format("建 WebView2 页面失败（HRESULT=0x{:08X}）",
                                               static_cast<unsigned>(controller_result));
                        ST_LOG_INFO("webview：{}", failure_);
                        creating_ = false;
                        return S_OK;
                      }
                      controller->AddRef();
                      controller_ = Com<ICoreWebView2Controller>(controller);
                      on_controller_ready();
                      creating_ = false;
                      return S_OK;
                    });
            // ★ 关键：父窗口给的是**我们的容器子窗口**，不是主窗口。
            //
            // 这样裁剪/切页/滚出视口/隐藏全部复用 `NativeHost` 那一套机械
            //（宿主窗口 = `bounds ∩ clip`），浏览器不必另造一套可见性逻辑。
            const HRESULT created =
                env->CreateCoreWebView2Controller(host_, controller_handler);
            controller_handler->Release();
            if (FAILED(created)) {
              failure_ = std::format("CreateCoreWebView2Controller 失败（HRESULT=0x{:08X}）",
                                     static_cast<unsigned>(created));
              creating_ = false;
            }
            return S_OK;
          });

  // 用户数据目录：**独立于用户的 Edge 配置**。
  //
  // 为何不能沿用默认（exe 同目录下的 `*.exe.WebView2`）：那会把缓存/cookie 写进安装目录，
  // 在只读安装位（Program Files）下直接失败。放到 `%LOCALAPPDATA%` 是唯一稳妥的位置。
  std::wstring user_data_dir;
  wchar_t local_app_data[MAX_PATH]{};
  const DWORD written = ::GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, MAX_PATH);
  if (written > 0 && written < MAX_PATH) {
    user_data_dir.assign(local_app_data);
    user_data_dir.append(L"\\gebai-webview2\\user-data");
  }
  const HRESULT started = Loader::instance().create_environment(
      nullptr, user_data_dir.empty() ? nullptr : user_data_dir.c_str(), nullptr, env_handler);
  env_handler->Release();
  if (FAILED(started)) {
    failure_ = std::format("CreateCoreWebView2EnvironmentWithOptions 失败（HRESULT=0x{:08X}）",
                           static_cast<unsigned>(started));
    ST_LOG_INFO("webview：{}", failure_);
    creating_ = false;
  }
}

void Win32WebView::on_controller_ready() {
  if (!controller_.valid()) return;
  // 先落到当前矩形（controller 建出来时是 0×0，不摆就是一块看不见的空）。
  RECT client{};
  ::GetClientRect(host_, &client);
  RECT bounds{0, 0, std::max<LONG>(1, client.right), std::max<LONG>(1, client.bottom)};
  controller_->put_Bounds(bounds);
  controller_->put_IsVisible(TRUE);

  // DPI：`RasterizationScale` 在 `ICoreWebView2Controller3` 上（不是 1/2）。
  //
  // 拿到它就等于拿到"高 DPI 支持"；拿不到就如实记一笔——那样网页会按 1.0 栅格化，
  // 在高 DPI 屏上是**物理像素级模糊**（不是布局错，所以更难发现）。
  ICoreWebView2Controller3* controller3 = nullptr;
  if (SUCCEEDED(controller_->QueryInterface(IID_ICoreWebView2Controller3,
                                            reinterpret_cast<void**>(&controller3))) &&
      controller3 != nullptr) {
    controller3_ = Com<ICoreWebView2Controller3>(controller3);
  } else {
    ST_LOG_INFO("webview：Controller3 不可用——高 DPI 下网页会按 1.0 栅格化（无缩放控制）");
  }
  apply_rasterization_scale();

  if (FAILED(controller_->get_CoreWebView2(webview_.put())) || !webview_.valid()) {
    failure_ = "取 CoreWebView2 失败";
    ST_LOG_INFO("webview：{}", failure_);
    return;
  }
  if (SUCCEEDED(webview_->get_Settings(settings_.put())) && settings_.valid()) {
    // 双向消息默认关着？不，默认开着；这里显式置上是为了让"页面能回推"成为
    // **契约而不是运气**（异步结果全靠这条回来的，见文件头第 2 条）。
    settings_->put_IsWebMessageEnabled(TRUE);
    settings_->put_AreDefaultScriptDialogsEnabled(TRUE);
    settings_->put_IsScriptEnabled(script_enabled_ ? TRUE : FALSE);
  }
  // 文档初始化脚本：**必须在任何导航之前落地**。
  //
  // 为何不能等导航完成再 `ExecuteScript`：页面的模块体在文档解析阶段就求值了，
  // 那时全局标记必须已经存在（注入晚一步的症状是“返回值对、行为不生效”）。
  // 建页面时没有导航发生，这里就是最早的可下发点。
  for (const auto& script : init_scripts_) {
    const HRESULT applied = apply_init_script(script);
    if (FAILED(applied)) {
      ST_LOG_INFO("webview：文档初始化脚本下发失败（HRESULT=0x{:08X}）",
                  static_cast<unsigned>(applied));
    }
  }
  // DPI 变化：**重新下发**（导航/显示器切换都会重置它，见文件头第 1 条）。
  if (controller3_.valid()) {
    EventRegistrationToken token{};
    controller3_->add_RasterizationScaleChanged(
        make_handler<ICoreWebView2RasterizationScaleChangedEventHandler,
                     &IID_ICoreWebView2RasterizationScaleChangedEventHandler,
                     HRESULT(ICoreWebView2Controller*, IUnknown*)>(
            [this](ICoreWebView2Controller*, IUnknown*) -> HRESULT {
              apply_rasterization_scale();
              Event event;
              event.kind = Event::Kind::RasterizationScaleChanged;
              push_event(std::move(event));
              return S_OK;
            }),
        &token);
  }

  // ── 事件 ──
  EventRegistrationToken token{};
  webview_->add_NavigationStarting(
      make_handler<ICoreWebView2NavigationStartingEventHandler,
                   &IID_ICoreWebView2NavigationStartingEventHandler,
                   HRESULT(ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs*)>(
          [this](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
            loading_ = true;
            Event event;
            event.kind = Event::Kind::NavigationStarting;
            LPWSTR uri = nullptr;
            if (args != nullptr && SUCCEEDED(args->get_Uri(&uri)) && uri != nullptr) {
              event.url = narrow(uri);
              current_url_ = event.url;
              ::CoTaskMemFree(uri);
            }
            push_event(std::move(event));
            return S_OK;
          }),
      &token);
  webview_->add_NavigationCompleted(
      make_handler<ICoreWebView2NavigationCompletedEventHandler,
                   &IID_ICoreWebView2NavigationCompletedEventHandler,
                   HRESULT(ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*)>(
          [this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
            BOOL success = FALSE;
            if (args != nullptr) args->get_IsSuccess(&success);
            loading_ = false;
            if (success != 0) ready_ = true;
            // 导航后 DPI 可能已被重置：补一次（这是实测缺陷的兜底，
            // 正常路径由 RasterizationScaleChanged 触发）。
            apply_rasterization_scale();
            Event event;
            event.kind = Event::Kind::NavigationCompleted;
            event.outcome = success != 0 ? NavigationOutcome::Completed : NavigationOutcome::Failed;
            event.url = current_url_;
            if (success == 0 && args != nullptr) {
              COREWEBVIEW2_WEB_ERROR_STATUS status = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
              if (SUCCEEDED(args->get_WebErrorStatus(&status))) {
                event.detail = std::format("WebErrorStatus={}", static_cast<int>(status));
              }
            }
            push_event(std::move(event));
            return S_OK;
          }),
      &token);
  webview_->add_DocumentTitleChanged(
      make_handler<ICoreWebView2DocumentTitleChangedEventHandler,
                   &IID_ICoreWebView2DocumentTitleChangedEventHandler,
                   HRESULT(ICoreWebView2*, IUnknown*)>(
          [this](ICoreWebView2* sender, IUnknown*) -> HRESULT {
            LPWSTR value = nullptr;
            if (sender != nullptr && SUCCEEDED(sender->get_DocumentTitle(&value)) &&
                value != nullptr) {
              title_ = narrow(value);
              ::CoTaskMemFree(value);
            }
            Event event;
            event.kind = Event::Kind::TitleChanged;
            event.title = title_;
            push_event(std::move(event));
            return S_OK;
          }),
      &token);
  webview_->add_WebMessageReceived(
      make_handler<ICoreWebView2WebMessageReceivedEventHandler,
                   &IID_ICoreWebView2WebMessageReceivedEventHandler,
                   HRESULT(ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*)>(
          [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            Event event;
            event.kind = Event::Kind::WebMessageReceived;
            LPWSTR message = nullptr;
            if (args != nullptr && SUCCEEDED(args->TryGetWebMessageAsString(&message)) &&
                message != nullptr) {
              event.message = narrow(message);
              ::CoTaskMemFree(message);
            }
            push_event(std::move(event));
            return S_OK;
          }),
      &token);
  webview_->add_ProcessFailed(
      make_handler<ICoreWebView2ProcessFailedEventHandler,
                   &IID_ICoreWebView2ProcessFailedEventHandler,
                   HRESULT(ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*)>(
          [this](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
            Event event;
            event.kind = Event::Kind::ProcessFailed;
            if (args != nullptr) {
              COREWEBVIEW2_PROCESS_FAILED_KIND kind = COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
              if (SUCCEEDED(args->get_ProcessFailedKind(&kind))) {
                event.detail = std::format("kind={}", static_cast<int>(kind));
              }
            }
            push_event(std::move(event));
            ST_LOG_INFO("webview：渲染进程失败（{}）——页面需要 reload", event.detail);
            return S_OK;
          }),
      &token);

  // 装配完成：把"创建时就想加载的内容"补上（`webview_create` 只建壳）。
  sync_bounds_to_host();
}

void Win32WebView::apply_rasterization_scale() {
  target_scale_ = target_rasterization_scale();
  if (!controller3_.valid()) return;
  controller3_->put_RasterizationScale(target_scale_);
}

auto Win32WebView::apply_init_script(const std::string& script) -> HRESULT {
  if (!webview_.valid()) return E_UNEXPECTED;
  // 这个接口是异步的（完成时有回调），但**不需等回执**：注册与导航进同一条
  // WebView2 内部队列（按调用顺序处理）——先注册后导航，就是先注入后加载。
  ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler* handler =
      make_handler<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler,
                   &IID_ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler,
                   HRESULT(HRESULT, LPCWSTR)>(
          [](HRESULT result, LPCWSTR id) -> HRESULT {
            if (FAILED(result)) {
              ST_LOG_INFO("webview：文档初始化脚本注册失败（HRESULT=0x{:08X}）",
                          static_cast<unsigned>(result));
            } else if (id != nullptr) {
              ST_LOG_INFO("webview：文档初始化脚本已注册（id={}）", narrow(id));
            }
            return S_OK;
          });
  const HRESULT issued =
      webview_->AddScriptToExecuteOnDocumentCreated(widen(script).c_str(), handler);
  handler->Release();   // COM 已持有自己的引用
  return issued;
}

void Win32WebView::webview_destroy() {
  // 顺序：先页面、再 controller、最后 environment（见类注释）。
  webview_.reset();
  settings_.reset();
  controller3_.reset();
  controller_.reset();
  environment_.reset();
  if (host_ != nullptr) {
    // 宿主窗口连同它的子窗口一起销毁；浏览器进程由 WebView2 自己收尾。
    if (::IsWindow(host_) != 0) ::DestroyWindow(host_);
    host_ = nullptr;
  }
  creating_ = false;
  ready_ = false;
  loading_ = false;
  source_ = SourceKind::None;
  current_url_.clear();
  title_.clear();
  has_synced_ = false;
  hidden_by_clip_ = false;
  assets_active_ = false;
  assets_ = WebAssets{};
  asset_count_ = 0;
}

void Win32WebView::sync_bounds_to_host() {
  if (host_ == nullptr || !has_synced_) return;
  webview_sync(last_bounds_, last_clip_);
}

void Win32WebView::webview_sync(math::Rect bounds, math::Rect clip) {
  last_bounds_ = bounds;
  last_clip_ = clip;
  has_synced_ = true;
  if (host_ == nullptr) return;
  const float safe = scale();
  // 只露出 `bounds ∩ clip`——与 `Backend::sync_child_view` **同一个决策**：
  // 平台子窗口只能被父窗口矩形裁剪，所以宿主窗口本身就是那块交汇区。
  const float left = std::max(bounds.x, clip.x);
  const float top = std::max(bounds.y, clip.y);
  const float right = std::min(bounds.x + bounds.width, clip.x + clip.width);
  const float bottom = std::min(bounds.y + bounds.height, clip.y + clip.height);
  if (right <= left || bottom <= top) {
    // 完全看不见：隐藏（不是缩成 1px——那会留一条缝在界面上）。
    if (!hidden_by_clip_) {
      hidden_by_clip_ = true;
      ::ShowWindow(host_, SW_HIDE);
    }
    return;
  }
  // 边缘取整（floor/ceil）而不是对尺寸截断：分数 DPI（1.5×）下截断必丢小数部分
  //（1247×1.5=1870.5→1870），子窗口盖不到客户区底边 → 底部一条背景色缝（实测白线）。
  // 左/上 floor、右/下 ceil 保证**覆盖**全部逻辑区域；超出部分被宿主客户区裁掉。
  // 与 `to_client_rect`（`sync_child_view`）同一决策，口径只有一份：`cover_span`。
  const auto [host_x, host_right] = shell::detail::cover_span(left, right - left, safe);
  const auto [host_y, host_bottom] = shell::detail::cover_span(top, bottom - top, safe);
  const int width = std::max(1, host_right - host_x);
  const int height = std::max(1, host_bottom - host_y);
  ::SetWindowPos(host_, HWND_TOP, host_x, host_y, width, height, SWP_NOACTIVATE | SWP_NOZORDER);
  if (hidden_by_clip_) {
    hidden_by_clip_ = false;
    ::ShowWindow(host_, SW_SHOWNA);
  }
  // 宿主被裁小了，页面要按**完整矩形**布局再把偏移补回来，否则内容会整体位移。
  // 页面 Bounds 同样用 `cover_span`（与宿主子窗口同一口径：宿主窗口盖到的物理范围
  // 与页面布局范围必须一致，否则窗口边缘会“裁在页面像素中间”——页面按完整矩形
  // 排、宿主裁掉溢出，两边才能严丝合缝）。
  if (controller_.valid()) {
    const auto [page_x, page_right] = shell::detail::cover_span(bounds.x, bounds.width, safe);
    const auto [page_y, page_bottom] = shell::detail::cover_span(bounds.y, bounds.height, safe);
    const int view_w = std::max(1, page_right - page_x);
    const int view_h = std::max(1, page_bottom - page_y);
    RECT page{static_cast<LONG>(page_x - host_x),
              static_cast<LONG>(page_y - host_y),
              static_cast<LONG>(page_x - host_x) + view_w,
              static_cast<LONG>(page_y - host_y) + view_h};
    controller_->put_Bounds(page);
  }
}

void Win32WebView::push_event(Event event) {
  const std::scoped_lock lock(events_mutex_);
  events_.push_back(std::move(event));
}

auto Win32WebView::webview_take_events() -> std::vector<Event> {
  const std::scoped_lock lock(events_mutex_);
  std::vector<Event> out = std::move(events_);
  events_.clear();
  return out;
}

auto Win32WebView::webview_state() const -> State {
  State state;
  state.attached = webview_.valid();
  state.visible = webview_.valid() && !hidden_by_clip_;
  state.source = source_;
  state.url = current_url_;
  state.title = title_;
  state.loading = loading_;
  state.ready = ready_;
  state.zoom = zoom_;
  state.rasterization_scale = target_scale_;
  state.asset_count = asset_count_;
  if (webview_.valid()) {
    BOOL can_back = FALSE;
    BOOL can_forward = FALSE;
    // `const` 方法里调非 const 的 COM 方法：WebView2 的 getter 在接口上是非 const 的
    //（IDL 产物），这里用 `const_cast` 只是为满足 const 契约——不修改任何状态。
    if (SUCCEEDED(const_cast<ICoreWebView2*>(webview_.get())->get_CanGoBack(&can_back))) {
      state.can_go_back = can_back != 0;
    }
    if (SUCCEEDED(const_cast<ICoreWebView2*>(webview_.get())->get_CanGoForward(&can_forward))) {
      state.can_go_forward = can_forward != 0;
    }
  }
  return state;
}

auto Win32WebView::webview_native_handle() -> void* { return host_; }

}  // namespace st::shell::webview

// ─────────────────────────────────────────────────────────────────────────────
// 内容源与拦截器
// ─────────────────────────────────────────────────────────────────────────────

namespace st::shell::webview {
namespace {

/// 把 `std::vector<uint8_t>` 包成 COM `IStream`（`SHCreateMemStream` 会拷贝）。
///
/// `bytes` 为空时 `SHCreateMemStream` 也会给一个合法的空流（不是 `nullptr`），
/// 而 `CreateWebResourceResponse` 需要一个非空流——空序列也照传。
auto make_stream(const std::vector<std::uint8_t>& bytes) -> Com<IStream> {
  static const std::uint8_t kEmpty = 0;
  const std::uint8_t* data = bytes.empty() ? &kEmpty : bytes.data();
  IStream* stream = ::SHCreateMemStream(  // lint-allow: L6 COM 边界的内存流
      reinterpret_cast<const BYTE*>(data), static_cast<UINT>(bytes.size()));
  return Com<IStream>(stream);
}

}  // namespace

void Win32WebView::install_asset_filter(const WebAssets& assets) {
  assets_ = assets;
  assets_active_ = assets.size() > 0;
  asset_count_ = assets.size();
  if (!webview_.valid()) return;
  // 过滤器要在**导航之前**装好，否则首次导航的请求会漏出去（然后 404 到真网络）。
  //
  // 通配 `/*` 而不是逐条路径：内嵌资源是**一个 origin 下的整棵树**，
  // 逐条注册会在"页面里有个没注册的 `x.js`"时静默走真网络（拿到 404 而不是我们的诊断）。
  const std::wstring pattern = std::wstring(kVirtualOriginHost) + L"/*";
  webview_->AddWebResourceRequestedFilter(pattern.c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
  EventRegistrationToken token{};
  webview_->add_WebResourceRequested(
      make_handler<ICoreWebView2WebResourceRequestedEventHandler,
                   &IID_ICoreWebView2WebResourceRequestedEventHandler,
                   HRESULT(ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs*)>(
          [this](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
            serve_request(args);
            return S_OK;
          }),
      &token);
}

void Win32WebView::serve_request(ICoreWebView2WebResourceRequestedEventArgs* args) {
  if (args == nullptr || !environment_.valid()) return;
  Com<ICoreWebView2WebResourceRequest> request;
  if (FAILED(args->get_Request(request.put())) || !request.valid()) return;
  LPWSTR uri = nullptr;
  std::string path = "/";
  if (SUCCEEDED(request->get_Uri(&uri)) && uri != nullptr) {
    path = path_from_url(uri);
    ::CoTaskMemFree(uri);
  }
  const Asset* asset = assets_.find(path);
  if (asset == nullptr) {
    // 未注册：**如实回 404**（不回落到真网络——那会让"忘了注册"变成"偶发外部请求"，
    // 既不可预期也可能泄漏）。
    Com<IStream> missing = make_stream({});
    const wchar_t* headers = L"Content-Type: text/plain; charset=utf-8\r\nCache-Control: no-store";
    Com<ICoreWebView2WebResourceResponse> response;
    if (missing.valid() &&
        SUCCEEDED(environment_->CreateWebResourceResponse(missing.get(), 404, L"Not Found",
                                                         headers, response.put())) &&
        response.valid()) {
      args->put_Response(response.get());
    }
    ST_LOG_INFO("webview：内嵌资源未注册 {}（可用 {} 条）", path, asset_count_);
    return;
  }
  Com<IStream> stream = make_stream(asset->bytes);
  const std::wstring header_text =
      L"Content-Type: " + widen(asset->mime) + L"\r\nCache-Control: no-store";
  Com<ICoreWebView2WebResourceResponse> response;
  if (stream.valid() && SUCCEEDED(environment_->CreateWebResourceResponse(
                            stream.get(), 200, L"OK", header_text.c_str(), response.put())) &&
      response.valid()) {
    args->put_Response(response.get());
  }
}

auto Win32WebView::webview_navigate(std::string_view url) -> Status {
  if (!webview_.valid()) {
    return unexpected(ErrorCode::Unsupported, webview_unsupported_reason());
  }
  if (url.empty()) return unexpected(ErrorCode::Invalid, "导航地址为空");
  const std::wstring wide_url = widen(url);
  current_url_ = std::string(url);
  source_ = SourceKind::Url;
  loading_ = true;
  const HRESULT hr = webview_->Navigate(wide_url.c_str());
  if (FAILED(hr)) {
    loading_ = false;
    return unexpected(ErrorCode::Io,
                      std::format("导航失败（HRESULT=0x{:08X}）", static_cast<unsigned>(hr)));
  }
  return ok();
}

auto Win32WebView::webview_load_html(std::string_view html) -> Status {
  if (!webview_.valid()) {
    return unexpected(ErrorCode::Unsupported, webview_unsupported_reason());
  }
  // 单文件 HTML：没有 origin，所以相对路径子资源**不可用**（契约写在 `webview.hpp`）。
  const std::wstring wide_html = widen(html);
  current_url_.clear();
  source_ = SourceKind::Html;
  loading_ = true;
  const HRESULT hr = webview_->NavigateToString(wide_html.c_str());
  if (FAILED(hr)) {
    loading_ = false;
    return unexpected(ErrorCode::Io,
                      std::format("加载 HTML 失败（HRESULT=0x{:08X}）", static_cast<unsigned>(hr)));
  }
  return ok();
}

auto Win32WebView::webview_load_embedded(std::string_view entry, const WebAssets& assets)
    -> Status {
  if (!webview_.valid()) {
    return unexpected(ErrorCode::Unsupported, webview_unsupported_reason());
  }
  if (assets.empty()) return unexpected(ErrorCode::Invalid, "内嵌资源表为空");
  const Asset* found = assets.find(entry);
  if (found == nullptr) {
    // **不落空白页**：入口不存在是装配错误，应当当场报出来。
    return unexpected(
        ErrorCode::NotFound,
        std::format("内嵌资源包里没有入口 {}（共 {} 条）", std::string(entry), assets.size()));
  }
  install_asset_filter(assets);
  source_ = SourceKind::Embedded;
  const std::string url = virtual_url(found->path);
  current_url_ = url;
  loading_ = true;
  const std::wstring wide_url = widen(url);
  const HRESULT hr = webview_->Navigate(wide_url.c_str());
  if (FAILED(hr)) {
    loading_ = false;
    return unexpected(ErrorCode::Io,
                      std::format("导航内嵌入口失败（HRESULT=0x{:08X}）",
                                  static_cast<unsigned>(hr)));
  }
  return ok();
}

auto Win32WebView::webview_reload() -> Status {
  if (!webview_.valid()) {
    return unexpected(ErrorCode::Unsupported, webview_unsupported_reason());
  }
  loading_ = true;
  const HRESULT hr = webview_->Reload();
  if (FAILED(hr)) {
    loading_ = false;
    return unexpected(ErrorCode::Io, "重新加载失败");
  }
  return ok();
}

auto Win32WebView::webview_stop() -> Status {
  if (!webview_.valid()) {
    return unexpected(ErrorCode::Unsupported, webview_unsupported_reason());
  }
  const HRESULT hr = webview_->Stop();
  if (FAILED(hr)) return unexpected(ErrorCode::Io, "停止加载失败");
  loading_ = false;
  return ok();
}

auto Win32WebView::webview_go_back() -> Status {
  if (!webview_.valid()) {
    return unexpected(ErrorCode::Unsupported, webview_unsupported_reason());
  }
  BOOL can = FALSE;
  if (FAILED(webview_->get_CanGoBack(&can)) || can == 0) {
    return unexpected(ErrorCode::Invalid, "没有可后退的历史");
  }
  const HRESULT hr = webview_->GoBack();
  if (FAILED(hr)) return unexpected(ErrorCode::Io, "后退失败");
  return ok();
}

auto Win32WebView::webview_go_forward() -> Status {
  if (!webview_.valid()) {
    return unexpected(ErrorCode::Unsupported, webview_unsupported_reason());
  }
  BOOL can = FALSE;
  if (FAILED(webview_->get_CanGoForward(&can)) || can == 0) {
    return unexpected(ErrorCode::Invalid, "没有可前进的历史");
  }
  const HRESULT hr = webview_->GoForward();
  if (FAILED(hr)) return unexpected(ErrorCode::Io, "前进失败");
  return ok();
}

auto Win32WebView::webview_add_init_script(std::string_view script) -> Status {
  if (script.empty()) return unexpected(ErrorCode::Invalid, "脚本为空");
  // 两件事：存起来（页面还没建时也能注册）+ 页面已在则立即下发。
  //
  // 为何要存：调用方通常在**建页面之前**就要标记身份（前端模块体在文档解析时就读它）。
  // WebView2 的接口只能在拿到 `ICoreWebView2` 之后调，所以“先登记、后落地”。
  init_scripts_.emplace_back(script);
  if (!webview_.valid()) return ok();   // 页面就绪时由 on_controller_ready 统一落地
  const HRESULT hr = apply_init_script(init_scripts_.back());
  if (FAILED(hr)) {
    return unexpected(ErrorCode::Io,
                      std::format("注册文档初始化脚本失败（HRESULT=0x{:08X}）",
                                  static_cast<unsigned>(hr)));
  }
  return ok();
}

auto Win32WebView::webview_execute_script(std::string_view script, std::string& result) -> Status {
  if (!webview_.valid()) {
    return unexpected(ErrorCode::Unsupported, webview_unsupported_reason());
  }
  if (script.empty()) return unexpected(ErrorCode::Invalid, "脚本为空");
  result.clear();
  // ## 同步回读的做法：泵消息等回调
  //
  // WebView2 的执行接口是异步的（结果经 COM 回调回来），而调用方（组件动作面、
  // 控制通道 `invoke`）要的是**一个返回值**。两条路：
  // ① 回调风格（上层自己攒状态机）；② 这里泵消息等回。
  //
  // 选 ②：脚本执行是**亚毫秒级**的（实测一次 DOM 读取 <1ms），而 ① 会把
  // "读一个 DOM 值"变成跨三层的异步状态机。前提是同线程（本类就是 UI 线程），
  // 并且**有超时**——页面卡住时不能让界面一起挂住。
  bool done = false;
  HRESULT script_status = E_FAIL;
  std::wstring value;
  ICoreWebView2ExecuteScriptCompletedHandler* handler =
      make_handler<ICoreWebView2ExecuteScriptCompletedHandler,
                   &IID_ICoreWebView2ExecuteScriptCompletedHandler, HRESULT(HRESULT, LPCWSTR)>(
          [&done, &script_status, &value](HRESULT status, LPCWSTR text) -> HRESULT {
            script_status = status;
            if (text != nullptr) value = text;
            done = true;
            return S_OK;
          });
  const HRESULT issued = webview_->ExecuteScript(widen(script).c_str(), handler);
  handler->Release();
  if (FAILED(issued)) {
    return unexpected(ErrorCode::Io,
                      std::format("提交脚本失败（HRESULT=0x{:08X}）", static_cast<unsigned>(issued)));
  }
  if (!pump_until([&done] { return done; }, kScriptTimeoutMs)) {
    return unexpected(ErrorCode::Timeout, "执行脚本超时（页面可能卡住或被脚本阻塞）");
  }
  if (FAILED(script_status)) {
    // 脚本抛异常时 WebView2 用非 S_OK 回报，并在 value 里放异常文本。
    return unexpected(ErrorCode::Parse,
                      std::format("脚本执行出错：{}", narrow(value.c_str())));
  }
  result = narrow(value.c_str());
  // `"null"` 与 `""` 都是合法结果（`null` / `undefined`），不转成错误。
  return ok();
}

auto Win32WebView::webview_post_message(std::string_view text) -> Status {
  if (!webview_.valid()) {
    return unexpected(ErrorCode::Unsupported, webview_unsupported_reason());
  }
  const HRESULT hr = webview_->PostWebMessageAsString(widen(text).c_str());
  if (FAILED(hr)) {
    return unexpected(ErrorCode::Io,
                      std::format("发送消息失败（HRESULT=0x{:08X}）", static_cast<unsigned>(hr)));
  }
  return ok();
}

auto Win32WebView::webview_set_zoom(double factor) -> Status {
  if (!webview_.valid()) {
    return unexpected(ErrorCode::Unsupported, webview_unsupported_reason());
  }
  if (factor <= 0.0 || factor > 10.0) {
    return unexpected(ErrorCode::Invalid, "缩放必须是 (0, 10] 之间的倍数");
  }
  zoom_ = factor;
  // 缩放通过 rasterization scale 表达（WebView2 没有独立 zoom 属性）。
  // **它会在导航后被重置**，所以下一次导航完成时 `apply_rasterization_scale` 会重发。
  apply_rasterization_scale();
  return ok();
}

auto Win32WebView::webview_set_script_enabled(bool enabled) -> Status {
  script_enabled_ = enabled;
  if (!settings_.valid()) {
    return unexpected(ErrorCode::Unsupported, "页面尚未就绪（或 Settings 不可用）");
  }
  const HRESULT hr = settings_->put_IsScriptEnabled(enabled ? TRUE : FALSE);
  if (FAILED(hr)) return unexpected(ErrorCode::Io, "设置脚本开关失败");
  return ok();
}

// ─────────────────────────────────────────────────────────────────────────────
// 工厂
// ─────────────────────────────────────────────────────────────────────────────

auto create_win32_webview(void* parent, float device_scale) -> std::unique_ptr<WebViewHost> {
  // `parent` 为空 = 还没建窗（不支持就是如实回答，不是错误）。
  // 非 Windows 目标上整个文件体被 `#if defined(_WIN32)` 挡掉，不会走到这里。
  if (parent == nullptr) return nullptr;
  return std::make_unique<Win32WebView>(static_cast<HWND>(parent), device_scale);
}

}  // namespace st::shell::webview

#endif  // _WIN32
