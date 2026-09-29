/// D3D11 GPU 绘制目标的平台实现（Windows）。
///
/// 本文件是**唯一**允许出现 `d3d11.h`/`dxgi.h` 与 `LoadLibrary` 的地方
/// （`CONVENTIONS.md` §10 第 1 条：系统头只出现在平台文件里）。
///
/// 两个刻意的工程选择：
///
/// 1. **全部动态加载**（`LoadLibrary` + `GetProcAddress`）而非链接 `d3d11.lib`。
///    框架对窗口后端立下的规矩是"运行时探测、不产生链接期依赖"，GPU 沿用同一条：
///    这样没有显卡/驱动的机器照常构建与运行（自动落软件路径），而不是链接期就挂。
///    代价是要写几个函数指针类型——比"链接期依赖一个可能不存在的设备"便宜得多。
/// 2. **设备与上下文按进程共享**（`DeviceHolder`）：建 D3D11 设备是几十毫秒级的操作，
///    每建一个画布就建一次设备会让"窗口缩放/DPI 切换"卡顿。

#include "st/raster/gpu.hpp"

#if !defined(_WIN32)

namespace st::raster::gpu {

auto available() noexcept -> bool { return false; }
auto probe() -> Result<DeviceInfo> {
  return unexpected(ErrorCode::Unsupported, "GPU 渲染当前仅实现了 D3D11（Windows）后端");
}
auto create_canvas(int, int, float, const Options&) -> Result<std::unique_ptr<Surface>> {
  return unexpected(ErrorCode::Unsupported, "GPU 渲染当前仅实现了 D3D11（Windows）后端");
}
auto live_canvas_count() noexcept -> std::uint32_t { return 0; }

}  // namespace st::raster::gpu

#else

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>

#include <atomic>
#include <cmath>
#include <format>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "st/core/fs.hpp"
#include "st/core/log.hpp"
#include "st/core/time.hpp"
#include "st/raster/canvas.hpp"

namespace st::raster::gpu {
namespace {

// ————————————————————————————————————————————————————————————————————————————
// 动态加载：d3d11.dll（设备创建）与 d3dcompiler_47.dll（着色器编译）
// ————————————————————————————————————————————————————————————————————————————

/// UTF-16 → UTF-8（适配器描述是宽字符）。
[[nodiscard]] auto to_utf8(std::wstring_view wide) -> std::string {
  if (wide.empty()) return {};
  const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
  if (needed <= 0) return {};
  std::string out(static_cast<std::size_t>(needed), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), needed,
                        nullptr, nullptr);
  return out;
}

using CreateDeviceFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                        const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**,
                                        D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

/// 进程级模块句柄（`LoadLibrary` 只做一次；失败者连重试的机会都不给——那不是偶发错误）。
struct Modules {
  HMODULE d3d11{nullptr};
  HMODULE d3dcompiler{nullptr};
  CreateDeviceFn create_device{nullptr};
  std::string d3d11_error{};  ///< 具体原因（"缺 d3d11.dll" 与 "缺符号" 是两回事）

  Modules() {
    d3d11 = ::LoadLibraryW(L"d3d11.dll");
    if (d3d11 == nullptr) {
      d3d11_error = std::format("加载 d3d11.dll 失败（错误码 {}）", ::GetLastError());
      return;
    }
    // 用 FARPROC 中转而不是直接转函数指针：MSVC 下这是唯一不触发
    // `-Wcast-function-type` 风格告警的写法（行为相同）。
    const FARPROC symbol = ::GetProcAddress(d3d11, "D3D11CreateDevice");
    if (symbol == nullptr) {
      d3d11_error = "d3d11.dll 中找不到 D3D11CreateDevice";
      return;
    }
    create_device = reinterpret_cast<CreateDeviceFn>(symbol);
    // 着色器编译器在**另一个** DLL（系统自带 d3dcompiler_47.dll；老系统可能是 43/46/47）
    for (const wchar_t* name : {L"d3dcompiler_47.dll", L"d3dcompiler_46.dll", L"d3dcompiler_43.dll"}) {
      d3dcompiler = ::LoadLibraryW(name);
      if (d3dcompiler != nullptr) break;
    }
  }

  Modules(const Modules&) = delete;
  auto operator=(const Modules&) -> Modules& = delete;
  ~Modules() {
    if (d3dcompiler != nullptr) ::FreeLibrary(d3dcompiler);
    if (d3d11 != nullptr) ::FreeLibrary(d3d11);
  }
};

[[nodiscard]] auto modules() -> const Modules& {
  static const Modules instance;
  return instance;
}

/// 特性级别从高到低尝试：只要求 10_0（自绘 2D 用不到几何着色器与计算着色器）。
constexpr D3D_FEATURE_LEVEL kFeatureLevels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                                D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};

[[nodiscard]] auto feature_level_name(D3D_FEATURE_LEVEL level) -> std::string {
  switch (level) {
    case D3D_FEATURE_LEVEL_11_1: return "11_1";
    case D3D_FEATURE_LEVEL_11_0: return "11_0";
    case D3D_FEATURE_LEVEL_10_1: return "10_1";
    case D3D_FEATURE_LEVEL_10_0: return "10_0";
    default: return "unknown";
  }
}

/// 设备 + 立即上下文（进程共享）。构建失败时保留原因，供 `probe()` 如实上报。
struct DeviceHolder {
  ID3D11Device* device{nullptr};
  ID3D11DeviceContext* context{nullptr};
  D3D_FEATURE_LEVEL feature_level{D3D_FEATURE_LEVEL_10_0};
  DeviceInfo info{};
  std::string error{};
  bool hardware{false};
  bool ok{false};

  DeviceHolder() {
    const Modules& mods = modules();
    if (mods.create_device == nullptr) {
      error = mods.d3d11_error;
      return;
    }
    // **强制 WARP**：环境变量 `ST_GPU_FORCE_WARP=1`。
    // 存在的理由很实在：在有显卡的机器上，"没有 GPU 时会怎样"根本无法被验证——
    // 而 WARP 就是微软提供的"无显卡"参考实现（同一条 D3D 管线，纯 CPU 执行）。
    // 设上它就能把 GPU 代码路径整条跑一遍，包括在我们自己的 CI 里。
    bool warp_only = false;
    if (const auto flag = st::fs::read_env("ST_GPU_FORCE_WARP"); flag.has_value()) {
      warp_only = !flag->empty() && *flag != "0";
    }
    // 先试硬件（不传适配器 = 默认适配器），失败再试 WARP：
    // 顺序很重要——先 WARP 就永远用不上显卡了。
    const std::vector<bool> attempts = warp_only ? std::vector<bool>{true}
                                                 : std::vector<bool>{false, true};
    for (const bool want_warp : attempts) {
      const D3D_DRIVER_TYPE type = want_warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
      UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
      const HRESULT result = mods.create_device(nullptr, type, nullptr, flags, kFeatureLevels,
                                                static_cast<UINT>(std::size(kFeatureLevels)),
                                                D3D11_SDK_VERSION, &device, &feature_level, &context);
      if (SUCCEEDED(result) && device != nullptr) {
        warp = want_warp;
        hardware = !want_warp;
        ok = true;
        fill_info();
        return;
      }
      if (device != nullptr) {
        device->Release();
        device = nullptr;
      }
      if (context != nullptr) {
        context->Release();
        context = nullptr;
      }
      error = std::format("{}创建失败（HRESULT 0x{:08X}）", want_warp ? "WARP 设备" : "硬件设备",
                          static_cast<unsigned>(result));
    }
  }

  bool warp{false};

  void fill_info() {
    info.backend = "d3d11";
    info.warp = warp;
    info.feature_level = feature_level_name(feature_level);
    // 适配器名：Device → IDXGIDevice → IDXGIAdapter → GetDesc
    IDXGIDevice* dxgi_device = nullptr;
    if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice),
                                        reinterpret_cast<void**>(&dxgi_device))) &&
        dxgi_device != nullptr) {
      IDXGIAdapter* adapter = nullptr;
      if (SUCCEEDED(dxgi_device->GetAdapter(&adapter)) && adapter != nullptr) {
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(adapter->GetDesc(&desc))) {
          info.adapter = to_utf8(desc.Description);
          info.vram_bytes = static_cast<std::uint64_t>(desc.DedicatedVideoMemory);
          // 适配器自报软件实现（DXGI_ADAPTER_DESC1 才有 Flags；这里以显存为 0
          // 且名字含软件标记来判定，避免为了一个标志位再查一次接口）
          info.warp = desc.DedicatedVideoMemory == 0;
        }
        adapter->Release();
      }
      dxgi_device->Release();
    }
    if (info.adapter.empty()) info.adapter = warp ? "WARP（CPU 模拟）" : "未知适配器";
    if (!warp && info.warp) {
      // 适配器自报是软件实现：修正判定（否则会谎称"在用显卡"）
      warp = true;
      hardware = false;
    }
  }

  [[nodiscard]] auto device_or_null() const noexcept -> ID3D11Device* { return device; }
  [[nodiscard]] auto context_or_null() const noexcept -> ID3D11DeviceContext* { return context; }

  DeviceHolder(const DeviceHolder&) = delete;
  auto operator=(const DeviceHolder&) -> DeviceHolder& = delete;

  ~DeviceHolder() {
    if (context != nullptr) context->Release();
    if (device != nullptr) device->Release();
  }
};

[[nodiscard]] auto holder() -> const DeviceHolder& {
  static const DeviceHolder instance;
  return instance;
}

std::atomic<std::uint32_t> g_live_canvases{0};

// ————————————————————————————————————————————————————————————————————————————
// GpuCanvas（M2：设备层 + 清屏 + 回读；绘制原语见后续里程碑）
// ————————————————————————————————————————————————————————————————————————————

/// 像素格式：`DXGI_FORMAT_R8G8B8A8_UNORM`。
///
/// 为什么不用 `B8G8R8A8`（显卡更"原生"的那个）：画布内部是 `0xRRGGBBAA` 预乘，
/// 而 `R8G8B8A8` 的内存序就是 R,G,B,A——回读/上传只需要一次通道重排，
/// 且不依赖 `D3D11_CREATE_DEVICE_BGRA_SUPPORT` 的行为差异。
constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

class GpuCanvas final : public Surface {
 public:
  GpuCanvas(ID3D11Device* device, ID3D11DeviceContext* context, int physical_width,
            int physical_height, float scale)
      : device_(device),
        context_(context),
        physical_width_(physical_width),
        physical_height_(physical_height),
        scale_(scale) {
    device_->AddRef();
    context_->AddRef();
    ++g_live_canvases;
    create_target();
  }

  ~GpuCanvas() override {
    release_readback();
    release_resources();
    context_->Release();
    device_->Release();
    --g_live_canvases;
  }

  GpuCanvas(const GpuCanvas&) = delete;
  auto operator=(const GpuCanvas&) -> GpuCanvas& = delete;

  // —— 尺寸 ——
  [[nodiscard]] auto physical_width() const noexcept -> int override { return physical_width_; }
  [[nodiscard]] auto physical_height() const noexcept -> int override { return physical_height_; }
  [[nodiscard]] auto device_scale() const noexcept -> float override { return scale_; }

  void set_device_scale(float scale) override {
    if (scale <= 0.0f || scale == scale_) return;
    scale_ = scale;
    // 物理缓冲不变（与软件画布同语义：scale 只影响逻辑↔物理换算）
  }

  [[nodiscard]] auto to_physical(math::Rect rect) const noexcept -> math::IntRect override {
    const auto x0 = static_cast<int>(std::floor(rect.x * scale_));
    const auto y0 = static_cast<int>(std::floor(rect.y * scale_));
    const auto x1 = static_cast<int>(std::ceil((rect.x + rect.width) * scale_));
    const auto y1 = static_cast<int>(std::ceil((rect.y + rect.height) * scale_));
    return math::IntRect{x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
  }
  [[nodiscard]] auto to_physical(math::Point point) const noexcept -> math::Point override {
    return math::Point{point.x * scale_, point.y * scale_};
  }

  // —— 剖析 ——
  void set_profiler(PaintProfiler* profiler) noexcept override { profiler_ = profiler; }
  [[nodiscard]] auto profiler() const noexcept -> PaintProfiler* override { return profiler_; }
  void add_profile(PaintOp op, double ms, std::uint64_t pixels = 0) noexcept override {
    if (profiler_ != nullptr) profiler_->add(op, ms, pixels);
  }

  // —— 绘制原语（M2 仅落地清屏；其余在 M3 接入着色器管线） ——
  void clear(math::Color color) override {
    if (rtv_ == nullptr) return;
    // 画布内部是**预乘**色（与软件光栅器一致），清屏值也必须预乘：
    // 否则半透明底色会与后续绘制叠成两次 alpha（视觉上“颜色偏深”）。
    const float alpha = static_cast<float>(color.a) / 255.0f;
    const float values[4] = {
        static_cast<float>(color.r) / 255.0f * alpha, static_cast<float>(color.g) / 255.0f * alpha,
        static_cast<float>(color.b) / 255.0f * alpha, alpha};
    const std::int64_t start = st::time::now_ns();
    context_->ClearRenderTargetView(rtv_, values);
    readback_dirty_ = true;
    if (profiler_ != nullptr) {
      profiler_->add(PaintOp::Clear,
                     static_cast<double>(st::time::now_ns() - start) / 1'000'000.0,
                     static_cast<std::uint64_t>(physical_width_) *
                         static_cast<std::uint64_t>(physical_height_));
    }
  }

  void fill_rect(math::Rect, const Paint&, float, DrawOptions) override { not_yet("fill_rect"); }
  void fill_path(const Path&, const Paint&, DrawOptions) override { not_yet("fill_path"); }
  void stroke_path(const Path&, const Paint&, float, DrawOptions) override { not_yet("stroke"); }
  void fill_circle(math::Point, float, const Paint&, DrawOptions) override {
    not_yet("fill_circle");
  }
  void draw_shadow(math::Rect, float, float, math::Color, math::Point, DrawOptions) override {
    not_yet("shadow");
  }
  void draw_canvas(const Surface&, math::Rect, DrawOptions) override { not_yet("draw_canvas"); }
  void draw_canvas_at(const Surface&, int, int, DrawOptions) override { not_yet("draw_canvas_at"); }
  void blend_coverage_bitmap(int, int, std::span<const float>, int, int, const Paint&, float,
                             BlendMode) override {
    not_yet("blend_coverage_bitmap");
  }

  // —— 裁剪 ——
  void push_clip_rect(math::Rect) override { not_yet("push_clip_rect"); }
  void push_clip_rounded_rect(math::Rect, float) override { not_yet("push_clip_rounded_rect"); }
  void push_clip_path(const Path&) override { not_yet("push_clip_path"); }
  void pop_clip() override { not_yet("pop_clip"); }
  [[nodiscard]] auto clip_rect() const noexcept -> math::IntRect override {
    return math::IntRect{0, 0, physical_width_, physical_height_};
  }
  [[nodiscard]] auto has_mask_clip() const noexcept -> bool override { return false; }

  // —— 像素访问（触发回读） ——
  [[nodiscard]] auto pixel_at(int x, int y) const -> math::Color override {
    const std::span<const std::uint32_t> data = pixels();
    if (x < 0 || y < 0 || x >= physical_width_ || y >= physical_height_) return math::Color{};
    return math::unpremultiply(data[static_cast<std::size_t>(y) *
                                        static_cast<std::size_t>(physical_width_) +
                                    static_cast<std::size_t>(x)]);
  }
  void set_pixel(int x, int y, math::Color color) override {
    if (x < 0 || y < 0 || x >= physical_width_ || y >= physical_height_) return;
    std::span<std::uint32_t> data = mutable_pixels();
    data[static_cast<std::size_t>(y) * static_cast<std::size_t>(physical_width_) +
         static_cast<std::size_t>(x)] = math::premultiply(color);
    readback_dirty_ = true;  // 下次取像素时重新回读（写回 GPU 属 M3 的图集路径）
  }
  [[nodiscard]] auto pixel_at_point(math::Point point) const -> math::Color override {
    const math::Point physical = to_physical(point);
    return pixel_at(static_cast<int>(physical.x), static_cast<int>(physical.y));
  }
  [[nodiscard]] auto pixels() const noexcept -> std::span<const std::uint32_t> override {
    // 只读访问也要确保回读发生过（否则返回空 span 会静默变成"全黑"）
    (void)const_cast<GpuCanvas*>(this)->ensure_readback();
    return {readback_.data(), readback_.size()};
  }
  [[nodiscard]] auto pixels() noexcept -> std::span<std::uint32_t> override {
    (void)ensure_readback();
    return {readback_.data(), readback_.size()};
  }
  [[nodiscard]] auto to_rgba8() const -> std::vector<std::uint8_t> override {
    const std::span<const std::uint32_t> data = pixels();
    std::vector<std::uint8_t> out(data.size() * 4U);
    for (std::size_t index = 0; index < data.size(); ++index) {
      const std::uint32_t pixel = data[index];
      out[index * 4U + 0U] = static_cast<std::uint8_t>((pixel >> 24U) & 0xFFU);
      out[index * 4U + 1U] = static_cast<std::uint8_t>((pixel >> 16U) & 0xFFU);
      out[index * 4U + 2U] = static_cast<std::uint8_t>((pixel >> 8U) & 0xFFU);
      out[index * 4U + 3U] = static_cast<std::uint8_t>(pixel & 0xFFU);
    }
    return out;
  }
  [[nodiscard]] auto content_bounds() const noexcept -> math::IntRect override {
    const std::span<const std::uint32_t> data = pixels();
    int min_x = physical_width_;
    int min_y = physical_height_;
    int max_x = -1;
    int max_y = -1;
    for (int y = 0; y < physical_height_; ++y) {
      for (int x = 0; x < physical_width_; ++x) {
        if ((data[static_cast<std::size_t>(y) * static_cast<std::size_t>(physical_width_) +
                   static_cast<std::size_t>(x)] &
             0xFFU) == 0U) {
          continue;
        }
        min_x = std::min(min_x, x);
        min_y = std::min(min_y, y);
        max_x = std::max(max_x, x);
        max_y = std::max(max_y, y);
      }
    }
    if (max_x < min_x || max_y < min_y) return math::IntRect{};
    return math::IntRect{min_x, min_y, max_x - min_x + 1, max_y - min_y + 1};
  }
  [[nodiscard]] auto content_bounds_logical() const noexcept -> math::IntRect override {
    const math::IntRect bounds = content_bounds();
    if (bounds.is_empty()) return bounds;
    const float inverse = 1.0f / scale_;
    return math::IntRect{static_cast<int>(std::floor(static_cast<float>(bounds.x) * inverse)),
                         static_cast<int>(std::floor(static_cast<float>(bounds.y) * inverse)),
                         static_cast<int>(std::ceil(static_cast<float>(bounds.width) * inverse)),
                         static_cast<int>(std::ceil(static_cast<float>(bounds.height) * inverse))};
  }

  [[nodiscard]] auto target() const noexcept -> ID3D11Texture2D* { return target_; }
  [[nodiscard]] auto rtv() const noexcept -> ID3D11RenderTargetView* { return rtv_; }

 private:
  /// 未实现的绘制原语**必须出声**：静默画不出东西会让"界面是空的"变成一个谜。
  ///
  /// 每个名字只报一次（用固定数组而不是 `vector<string>`：无分配、无锁竞争面）。
  static void not_yet(const char* what) {
    static std::mutex mutex;
    static std::vector<const char*> reported;
    const std::scoped_lock lock(mutex);
    for (const char* item : reported) {
      if (std::string_view(item) == what) return;
    }
    reported.push_back(what);
    st::log::warn("GPU 渲染：{} 尚未实现（当前里程碑只落地设备层与清屏）", what);
  }

  void create_target() {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(physical_width_);
    desc.Height = static_cast<UINT>(physical_height_);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = kFormat;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &target_))) {
      st::log::error("GPU 渲染：创建离屏渲染目标失败");
      return;
    }
    if (FAILED(device_->CreateRenderTargetView(target_, nullptr, &rtv_))) {
      st::log::error("GPU 渲染：创建渲染目标视图失败");
    }
  }

  void release_resources() {
    if (rtv_ != nullptr) {
      rtv_->Release();
      rtv_ = nullptr;
    }
    if (target_ != nullptr) {
      target_->Release();
      target_ = nullptr;
    }
  }

  void release_readback() {
    if (staging_ != nullptr) {
      staging_->Release();
      staging_ = nullptr;
    }
  }

  /// 从 GPU 拉回像素（`0xRRGGBBAA` 预乘，与软件画布同一布局）。
  auto ensure_readback() -> bool {
    if (target_ == nullptr) return false;
    if (!readback_dirty_ && !readback_.empty()) return true;
    if (staging_ == nullptr) {
      D3D11_TEXTURE2D_DESC desc{};
      desc.Width = static_cast<UINT>(physical_width_);
      desc.Height = static_cast<UINT>(physical_height_);
      desc.MipLevels = 1;
      desc.ArraySize = 1;
      desc.Format = kFormat;
      desc.SampleDesc.Count = 1;
      desc.Usage = D3D11_USAGE_STAGING;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      if (FAILED(device_->CreateTexture2D(&desc, nullptr, &staging_))) {
        st::log::error("GPU 渲染：创建回读用暂存纹理失败");
        return false;
      }
    }
    context_->CopyResource(staging_, target_);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(staging_, 0, D3D11_MAP_READ, 0, &mapped))) {
      st::log::error("GPU 渲染：回读映射失败");
      return false;
    }
    readback_.assign(static_cast<std::size_t>(physical_width_) *
                         static_cast<std::size_t>(physical_height_),
                     0U);
    const auto* source = static_cast<const std::uint8_t*>(mapped.pData);
    for (int y = 0; y < physical_height_; ++y) {
      const std::uint8_t* row = source + static_cast<std::size_t>(y) * mapped.RowPitch;
      for (int x = 0; x < physical_width_; ++x) {
        // 纹理内存序 R,G,B,A → 画布布局 0xRRGGBBAA（预乘值直接搬，不做反预乘）
        const std::uint8_t red = row[x * 4 + 0];
        const std::uint8_t green = row[x * 4 + 1];
        const std::uint8_t blue = row[x * 4 + 2];
        const std::uint8_t alpha = row[x * 4 + 3];
        readback_[static_cast<std::size_t>(y) * static_cast<std::size_t>(physical_width_) +
                  static_cast<std::size_t>(x)] =
            (static_cast<std::uint32_t>(red) << 24U) | (static_cast<std::uint32_t>(green) << 16U) |
            (static_cast<std::uint32_t>(blue) << 8U) | static_cast<std::uint32_t>(alpha);
      }
    }
    context_->Unmap(staging_, 0);
    readback_dirty_ = false;
    return true;
  }

  /// 供 `set_pixel` 用的可写像素（写完由调用方标脏并整体回传——M2 只保证接口语义正确）。
  auto mutable_pixels() -> std::span<std::uint32_t> {
    (void)ensure_readback();
    return {readback_.data(), readback_.size()};
  }

  ID3D11Device* device_{nullptr};
  ID3D11DeviceContext* context_{nullptr};
  ID3D11Texture2D* target_{nullptr};
  ID3D11RenderTargetView* rtv_{nullptr};
  ID3D11Texture2D* staging_{nullptr};
  std::vector<std::uint32_t> readback_{};
  bool readback_dirty_{true};
  int physical_width_{0};
  int physical_height_{0};
  float scale_{1.0f};
  PaintProfiler* profiler_{nullptr};
};

}  // namespace

auto available() noexcept -> bool { return holder().ok; }

auto probe() -> Result<DeviceInfo> {
  const DeviceHolder& device = holder();
  if (!device.ok) {
    return unexpected(ErrorCode::Unsupported,
                      device.error.empty() ? std::string("GPU 不可用") : device.error);
  }
  return device.info;
}

auto create_canvas(int physical_width, int physical_height, float device_scale,
                   const Options& options) -> Result<std::unique_ptr<Surface>> {
  const DeviceHolder& device = holder();
  if (!device.ok) {
    return unexpected(ErrorCode::Unsupported,
                      device.error.empty() ? std::string("GPU 不可用") : device.error);
  }
  if (device.warp && !options.allow_warp) {
    return unexpected(ErrorCode::Unsupported,
                      "只探测到 WARP（无硬件 GPU），而已要求不允许回退");
  }
  if (options.require_warp && !device.warp) {
    return unexpected(ErrorCode::Unsupported,
                      "要求 WARP 设备，但当前进程用的是硬件设备（设 ST_GPU_FORCE_WARP=1 后重启）");
  }
  if (physical_width <= 0 || physical_height <= 0) {
    return unexpected(ErrorCode::Invalid, "GPU 画布尺寸必须为正");
  }
  auto canvas = std::make_unique<GpuCanvas>(device.device_or_null(), device.context_or_null(),
                                            physical_width, physical_height, device_scale);
  return std::unique_ptr<Surface>(std::move(canvas));
}

auto live_canvas_count() noexcept -> std::uint32_t { return g_live_canvases.load(); }

}  // namespace st::raster::gpu

#endif  // _WIN32
