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

/// 非 Windows 平台：一切能力均为空（应用自动回软件）。

auto capabilities() -> Capabilities { return Capabilities{}; }
auto probe() -> Result<DeviceInfo> {
  return unexpected(ErrorCode::Unsupported, "GPU 渲染当前仅实现了 D3D11（Windows）后端");
}
auto create_canvas(int, int, float, const Options&) -> Result<std::unique_ptr<Surface>> {
  return unexpected(ErrorCode::Unsupported, "GPU 渲染当前仅实现了 D3D11（Windows）后端");
}
auto live_canvas_count() noexcept -> std::uint32_t { return 0; }

auto create_presenter(void*, int, int) -> Result<std::unique_ptr<Presenter>> {
  // 其他平台没有 DXGI：如实报不支持，由窗口后端落回软件呈现
  return unexpected(ErrorCode::Unsupported, "GPU 呈现当前仅实现了 DXGI（Windows）后端");
}

}  // namespace st::raster::gpu

// 非 Windows：没有 GPU 三维腿（`Scene3D::create` 会自动落软件腿）。
// 这两个符号在 `software_scene.cpp` 里声明、这里定义，保证“平台文件里没有系统头”
// 这条约束不对二分叉开。
namespace st::raster {
[[nodiscard]] auto gpu_scene3d_available() -> bool { return false; }
[[nodiscard]] auto create_gpu_scene3d(int, int) -> Result<std::unique_ptr<Scene3D>> {
  return unexpected(ErrorCode::Unsupported, "GPU 三维当前仅实现了 D3D11（Windows）后端");
}
}  // namespace st::raster

#else

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
// `<cstring>`：`std::memcpy`（下方映射常量缓冲时用）。**不能靠 <windows.h> 间接带入**——
// 那是实现细节，换编译器/换 SDK 就断（本文件在 MinGW 上曾因缺它而编译不过）。
#include <cstring>
#include <format>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "rasterize_internal.hpp"
#include "st/core/fs.hpp"
#include "st/core/log.hpp"
#include "st/core/time.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/glyph_atlas_content.hpp"
#include "st/raster/scene3d.hpp"

namespace st::raster::gpu {
/// 进程级上下文互斥锁：**所有**使用立即上下文的地方共用一把。
///
/// 为何是进程级：锁要保护的资源是立即上下文本身（`DeviceHolder` 里的进程级单例）。
/// 按对象分锁等于没锁（两个对象各持不同锁、却写同一个上下文）。
/// 使用者：`GpuCanvas` 的 `composite_native_texture`（三维纹理→二维画布的零拷贝合成）
/// 与 GPU 三维场景（清屏/绘网格/回读/flush）。
///
/// 为何函数内静态：避开全局构造顺序问题，且锁与 `holder()` 同生命周期。
/// 定义位置在匿名命名空间**之外**（`GpuCanvas` 与三维场景分居文件前后，都要看见它）。
///
/// lint-allow: L8 进程级互斥锁（保护进程级 D3D11 上下文，无注入点）
[[nodiscard]] auto g_context_mutex() -> std::mutex& {
  static std::mutex instance;
  return instance;
}

namespace {

/// 字形图集是否启用（`ST_GPU_GLYPH_ATLAS=0` 可关，供对照与出问题时的逃生门）。
[[nodiscard]] auto glyph_atlas_enabled() noexcept -> bool {
  static const bool enabled = [] {
    const char* value = std::getenv("ST_GPU_GLYPH_ATLAS");
    if (value == nullptr || *value == '\0') return true;
    return std::string_view{value} != "0";
  }();
  return enabled;
}

/// 字形合批是否启用。
///
/// **默认关闭**：合批本身已实现且逐像素正确（结构差 0.000%、彩边 289 vs 289），
/// 但实测**无收益**，且 p95 变差：
///
/// | | p50 | p95 | 最大 |
/// |---|---|---|---|
/// | 合批开 | 1.47 ms | **5.31 ms** | **10.20 ms** |
/// | 合批关 | 1.56 ms | 2.31 ms | 2.78 ms |
///
/// 根因是**粒度太细**：实测每帧 **244 次 flush**、平均每批只有 **4.7 个字形**。
/// 而原路径是每字形 2 次 draw（305 字形 = 610 次），合批后 244×2 = 488 次
/// ——只减了 20%，却多了顶点缓冲上传与常量重设。
///
/// 为何粒度这么细：`submit()` 里要 flush（保证绘制顺序），而 UI 树是
/// 「边框 → 背景 → 文字 → 下个元素」顺序绘制的，字形天然被其它原语打散。
///
/// **要拿到收益必须先换掉“顺序绘制”这个前提**（例如把整帧的字形先收集起来、
/// 最后统一画一次）——那是渲染流程改动，不是这个开关能解决的。
/// 在那之前保留实现（已正确），用 `ST_GPU_GLYPH_BATCH=1` 可开启验证。
[[nodiscard]] auto glyph_batch_enabled() noexcept -> bool {
  static const bool enabled = [] {
    const char* value = std::getenv("ST_GPU_GLYPH_BATCH");
    if (value == nullptr || *value == '\0') return false;   // 默认关闭
    return std::string_view{value} != "0";
  }();
  return enabled;
}

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
  HMODULE dxgi{nullptr};   ///< 呈现（DXGI swapchain）用；缺失只是"不能零拷贝上屏"
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
    create_device = reinterpret_cast<CreateDeviceFn>(reinterpret_cast<void*>(symbol));
    // 着色器编译器在**另一个** DLL（系统自带 d3dcompiler_47.dll；老系统可能是 43/46/47）
    for (const wchar_t* name : {L"d3dcompiler_47.dll", L"d3dcompiler_46.dll", L"d3dcompiler_43.dll"}) {
      d3dcompiler = ::LoadLibraryW(name);
  // DXGI：呈现器需要；缺了不影响渲染，只影响"零拷贝上屏"（会落回 GDI 路径）
  dxgi = ::LoadLibraryW(L"dxgi.dll");
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

// 画布存活计数（诊断/降级用）。真进程级计数器：跨所有设备与窗口、由多个线程增减，
// 不存在“注入 Context”的落点（它记录的正是**进程全局**的 GPU 画布数）。已用 atomic。
// lint-allow: L8 进程级诊断计数器（跨设备/跨线程），已 atomic
std::atomic<std::uint32_t> g_live_canvases{0};

// ————————————————————————————————————————————————————————————————————————————
// 着色器管线（M3）
// ————————————————————————————————————————————————————————————————————————————

/// HLSL 源码（运行期编译）。
///
/// 为什么用一个「über-shader + 模式常量」而不是每个原语一个着色器：
/// 这些分支的**代价极低**（都是一个分支 + 几行算术，且同一 draw 内取值恒定、
/// 分支完全可预测），而独立着色器会带来多套状态对象、多套常量布局与更多的
/// 绑定切换。在自绘 2D 这个场景里，**状态切换比着色器分支贵**。
///
/// 顶点阶段不需要顶点缓冲：用 `SV_VertexID` 生成单位四边形（`Draw(4, 0)` 三角带）——
/// 少一套输入布局、少一条上传路径。
constexpr const char* kShaderSource = R"hlsl(
cbuffer Params : register(b0) {
  float4 g_rect;        // x, y, w, h（物理像素）
  float4 g_color;       // 预乘 RGBA（0..1）
  float4 g_radii;       // 四角半径 TL/TR/BR/BL（物理像素）
  float4 g_mode;        // x=模式 y=opacity z=裁剪模式 w=备用
  float4 g_clip;        // 圆角裁剪矩形（物理像素）
  float4 g_clip_radii;
  float4 g_axis;        // 渐变几何：线性=起点/终点，径向=圆心+…，扫掠=圆心
  float4 g_axis_extra;  // x=渐变种类(0线性/1径向/2扫掠) y=半径 或 模糊半径
  float4 g_viewport;    // 视口（物理像素）
  // **纹理子矩形**（归一化 `{u0, v0, 宽, 高}`）：缺省 (0,0,1,1) = 整张纹理（主路径）；
  // 图集里是字形所在的子矩形。
  float4 g_uv;
};

Texture2D g_texture : register(t0);
Texture2D g_clip_texture : register(t1);
SamplerState g_sampler : register(s0);

struct VsOut {
  float4 position : SV_Position;
  float2 local : TEXCOORD0;   // 相对矩形左上角
  float2 pixel : TEXCOORD1;   // 物理像素坐标
};

// ————————————————————————————————————————————————————————————————
// **字形合批的顶点路径**
//
// 为何需要：实测每帧 305 个字形、每个 2 次 `submit`（LCD 两遍）= **610 次 draw**，
// GPU 侧 `text` 因此占整帧 **84%**（0.67 / 1.40 ms）。而 `submit` 的成本几乎全在
// **状态设置**（渲染目标/视口/scissor/混合/常量缓冲 Map+memcpy/资源绑定）——
// 把 N 个字形合成一次 draw，那些设置只做一次。
//
// 为何用**顶点缓冲**而不是实例化：
// 每个字形需要自己的矩形与图集子矩形，而 PS 的采样靠 `g_uv`。
// 实例化下 `g_uv` 在常量缓冲里逐实例无法变；顶点路径把 uv 作为顶点属性插值，
// **PS 只读 `input.uv`**，更直接。
//
// 代价：一个字形 6 个顶点（两个三角形），每顶点 8 个 float（位置 2 + uv01 2 + sub 4）= 32 字节；
// 305 字形 ≈ 58 KB——比实例缓冲大，但换来了 PS 不用分支。
struct BatchVertex {
  float2 position : POSITION;   // 物理像素坐标
  float2 local : TEXCOORD0;     // 相对字形左上角（**像素**，SDF 用）
  float2 local01 : TEXCOORD1;   // 字形内归一化坐标 0..1（采样用）
  float4 uv : TEXCOORD2;        // 图集子矩形 {u0, v0, w, h}（归一化）
  float2 half_size : TEXCOORD3; // 字形尺寸的一半（像素；SDF 用）
};

struct BatchOut {
  float4 position : SV_Position;
  float2 local : TEXCOORD0;
  float2 local01 : TEXCOORD1;
  float2 pixel : TEXCOORD2;
  float4 uv : TEXCOORD3;
  float2 half_size : TEXCOORD4;
};

BatchOut vs_batch(BatchVertex vertex) {
  BatchOut o;
  const float2 ndc = vertex.position / g_viewport.xy * 2.0 - 1.0;
  o.position = float4(ndc.x, -ndc.y, 0.0, 1.0);
  o.local = vertex.local;
  o.local01 = vertex.local01;
  o.pixel = vertex.position;
  o.uv = vertex.uv;
  o.half_size = vertex.half_size;
  return o;
}

VsOut vs_main(uint vertex_id : SV_VertexID) {
  // 三角带四个角：(0,0) (1,0) (0,1) (1,1)
  float2 uv = float2(vertex_id & 1u, (vertex_id >> 1u) & 1u);
  // ⚠ 四边形必须**向外扩 1 像素**。
  //
  // 光栅化按**像素中心**判定图元内外，而矩形边界一般不在像素中心上：
  // 例如 Input 的底边在 y=124.3，像素行 124 的中心是 124.5 —— 落在四边形外，
  // 那一整行**根本不会产生片元**，SDF 再准也来不及（它本应给出 70% 的覆盖率）。
  // 软件光栅器对像素做面积积分，不会丢这一行；于是两边差**一整行**
  // （实测 `gpu_matches_software_for_layout_of_real_widgets`：350/120000 = 0.29% 结构性差异，
  //   全部集中在元素边界那一行——正是本测试 0.307% 超阈的真因）。
  // 扩 1 像素后，被部分覆盖的边界像素落在图元内，覆盖率完全由 SDF/纹理给出。
  //
  // 对 `local`/`pixel` 无副作用：二者都是 `uv` 的仿射函数，扩张只是加了余量，
  // 同一片元处的取值不变（梯度与位图采样因此不受影响）。
  const float2 pad = float2(1.0, 1.0);
  float2 p = g_rect.xy - pad + uv * (g_rect.zw + pad * 2.0);
  VsOut o;
  float2 ndc = p / g_viewport.xy * 2.0 - 1.0;
  o.position = float4(ndc.x, -ndc.y, 0.0, 1.0);  // 画布 Y 向下，D3D NDC Y 向上
  o.local = p - g_rect.xy;
  o.pixel = p;
  return o;
}

/// 圆角矩形**有符号距离**：内部为负、边界为 0、外部为正。
/// 用 SDF 而不是三角化 + MSAA 的理由：距离场天然给出边缘的精确覆盖率，
/// 1px 发丝边框在任何 DPI 下都平滑（与软件光栅器的目标一致），
/// 且不需要额外的多重采样目标与解析步骤。
float rounded_sdf(float2 local, float2 half_size, float4 radii) {
  float r = (local.y < half_size.y) ? ((local.x < half_size.x) ? radii.x : radii.y)
                                    : ((local.x < half_size.x) ? radii.w : radii.z);
  r = min(r, min(half_size.x, half_size.y));
  float2 q = abs(local - half_size) - (half_size - r);
  return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

/// **按子矩形采样**（图集支持）。
///
/// `uv01` 是字形内的归一化坐标（0..1，由 `local / g_rect.zw` 得到）；
/// `rect_uv` 是子矩形 `{u0, v0, w, h}`。
///
/// 为何这样改而不是“把字形坐标映射进图集”：
/// 主路径的 `local = p - g_rect.xy`（**相对字形左上角**），`g_rect.zw` 是字形尺寸，
/// 所以 `local / g_rect.zw` 天然就是字形内的归一化坐标——**这部分完全不用改**。
/// 图集只需要把那个 0..1 的坐标**重映射到纹理里的子矩形**。
/// 缺省 `rect_uv = (0,0,1,1)` 时 `uv01 * 1 + 0 == uv01`，与改动前逐位等价。
float4 sample_subrect(float2 uv01, float4 rect_uv) {
  return g_texture.Sample(g_sampler, rect_uv.xy + uv01 * rect_uv.zw);
}

/// 像素着色的**共用实现**。
///
/// 两条顶点路径（`vs_main` / `vs_batch`）的产物不同（后者多带逐字形子矩形），
/// 但它们要算的东西完全一样——抽成这个函数，两边各自取到参数后调它。
/// 这样**只有一份着色逻辑**，不会出现“合批路径与主路径行为漂移”那类难查的偏差。
///
/// 参数：
///   · `local_px`：字形内的**像素**偏移（SDF 用）
///   · `local01`：字形内的**归一化**坐标 0..1（采样用）
///   · `half_size`：字形尺寸的一半（像素；SDF 用）
///
/// ⚠ `local01` **必须由调用方算好**（而不是在这里除 `g_rect.zw`）：
/// 合批时每个字形尺寸不同，而 `g_rect.zw` 是常量缓冲里的单一值。
/// 早期版本在 `shade` 里写 `local / g_rect.zw`，合批时传 `g_rect.zw = (1,1)`
/// 于是 `local01` 一直是**像素值**（没被归一化）——采样坐标直接飞到图集外，
/// 全被夹到边缘，实测“彩边像素 289 → 23”。
float4 shade(float2 local_px, float2 local01, float2 pixel, float4 rect_uv, float2 half_size) {
  const int mode = (int)g_mode.x;
  float4 source = g_color;
  float coverage = 1.0;
  // 亚像素（LCD）覆盖率：每像素独立的 R/G/B 三个覆盖度（模式 5）
  float3 coverage_rgb = float3(1.0, 1.0, 1.0);

  if (mode == 4) {
    // 可分离盒式模糊（阴影遮罩）：与 `blur_mask` 同一口径（3 遍 [水平+垂直]）。
    // 循环上界必须是编译期常量，而半径是运行期参数：取足够大的常量 + 条件跳过。
    const int radius = (int)g_axis_extra.x;
    const float2 step = g_axis.xy / g_viewport.xy;
    float sum = 0.0;
    float count = 0.0;
    for (int i = -64; i <= 64; ++i) {
      if (i < -radius || i > radius) continue;
      sum += g_texture.SampleLevel(g_sampler, local01 + step * (float)i, 0).r;
      count += 1.0;
    }
    return float4(count > 0.0 ? sum / count : 0.0, 0.0, 0.0, 1.0);
  }

  if (mode == 0 || mode == 1) {
    // 实心 / 渐变圆角矩形：SDF 给出解析覆盖率（1px 过渡带）
    coverage = saturate(0.5 - rounded_sdf(local_px, half_size, g_radii));
  }
  if (mode == 1) {
    // 渐变：直接从**渐变纹理**采样（CPU 侧按 stops 生成 256×1 的预乘色带）——
    // 这样 linear/radial/sweep 与任意数量的色标都是精确的，
    // 而不需要在着色器里重建多色标插值。
    float t = 0.0;
    if (g_axis_extra.x < 0.5) {
      const float2 axis = g_axis.zw - g_axis.xy;
      t = saturate(dot(pixel - g_axis.xy, axis) / max(dot(axis, axis), 1e-6));
    } else if (g_axis_extra.x < 1.5) {
      t = saturate(length(pixel - g_axis.xy) / max(g_axis_extra.y, 1e-6));
    } else {
      const float2 d = pixel - g_axis.xy;
      t = frac(atan2(d.y, d.x) / 6.28318530718 + 1.0);
    }
    source = g_texture.Sample(g_sampler, float2(t, 0.5));
  } else if (mode == 2) {
    // 覆盖率遮罩（R8）× 纯色：字形与路径遮罩走这条
    coverage = sample_subrect(local01, rect_uv).r;
  } else if (mode == 5) {
    // 亚像素覆盖率（R8G8B8）× 纯色：桌面字形走这条（两遍混合，见 g_mode.w）
    coverage_rgb = sample_subrect(local01, rect_uv).rgb;
  } else if (mode == 3) {
    // 预乘 RGBA 位图（离屏画布合成）
    source = sample_subrect(local01, rect_uv);
  }

  // 覆盖率**逐通道**：灰度模式下三通道相同（factor3 = coverage.xxx），
  // 亚像素模式下三通道各自独立——彩边就是从这里的差异来的。
  float3 factor3 = (mode == 5) ? coverage_rgb : float3(coverage, coverage, coverage);
  factor3 *= g_mode.y;
  float clip_factor = 1.0;
  if (g_mode.z > 2.5) {
    // 路径裁剪：用遮罩纹理采样（t1）。遮罩在区域内的相对位置 = (像素 - 区域左上) / 区域尺寸
    const float2 uv = (pixel - g_clip.xy) / max(g_clip.zw, float2(1.0, 1.0));
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) discard;
    clip_factor = g_clip_texture.SampleLevel(g_sampler, uv, 0).r;
  } else if (g_mode.z > 1.5) {
    // 圆角裁剪：用同一个 SDF 求覆盖率（不需要额外的遮罩纹理）
    const float2 clip_half = g_clip.zw * 0.5;
    clip_factor = saturate(0.5 - rounded_sdf(pixel - g_clip.xy, clip_half, g_clip_radii));
  }
  // 裁剪是**标量**（几何遮罩），乘进三个通道
  factor3 *= clip_factor;
  if (max(factor3.r, max(factor3.g, factor3.b)) <= 0.0) discard;
  if (mode == 5 && g_mode.w < 0.5) {
    // 亚像素第一遍：把**目标**按 (1 - α_c) 逐通道衰减（DestBlend = INV_SRC_COLOR），
    // 源项由第二遍加性加回。为什么要两遍：硬件混合的 α 是标量，而彩边恰恰长在
    // “逐通道的目标衰减”上——黑字压白底时源项为 0，用标量衰减就等于什么都没做。
    return float4(factor3, 1.0);
  }
  // 预乘输出（与软件画布同一混合空间）
  // α 通道是**标量**：亚像素取三通道均值（不透明画布上与逐通道等价），
  // 与软件侧 `over_premul_lcd` 同一口径。
  const float alpha = (mode == 5)
                          ? source.a * (factor3.r + factor3.g + factor3.b) / 3.0
                          : source.a * factor3.r;
  return float4(source.rgb * factor3, alpha);
}

// —— 两个入口 ——

// 主路径：单一子矩形（`g_uv`，通常是整张纹理）；字形尺寸来自 `g_rect.zw`。
float4 ps_main(VsOut input) : SV_Target {
  const float2 half_size = g_rect.zw * 0.5;
  return shade(input.local, input.local / g_rect.zw, input.pixel, g_uv, half_size);
}

// 合批路径：子矩形、字形尺寸都由顶点带下来（逐字形不同）。
float4 ps_batch(BatchOut input) : SV_Target {
  return shade(input.local, input.local01, input.pixel, input.uv, input.half_size);
}
)hlsl";

/// 与 HLSL 的 cbuffer 逐字节对应（4 字节对齐、总长 16 字节倍数）。
/// 用 `std::array` 而非 `float[4]`：数组型成员不可赋值，会让“构造参数”变成逐元素拷贝。
struct ShaderParams {
  std::array<float, 4> rect{};
  std::array<float, 4> color{};
  std::array<float, 4> radii{};
  std::array<float, 4> mode{};
  std::array<float, 4> clip{};
  std::array<float, 4> clip_radii{};
  std::array<float, 4> axis{};
  std::array<float, 4> axis_extra{};
  std::array<float, 4> viewport{};
  /// 纹理子矩形（归一化）：`{u0, v0, w, h}`。缺省整张纹理——主路径都是这个。
  std::array<float, 4> uv{0.0f, 0.0f, 1.0f, 1.0f};
};
static_assert(sizeof(ShaderParams) % 16 == 0, "常量缓冲必须是 16 字节的倍数");

/// 合批顶点（与 HLSL 的 `BatchVertex` 逐字节对应）。
///
/// 一个字形 = **6 个顶点**（两个三角形，不依赖 `SV_VertexID` 也不依赖实例化）。
/// 为何不用实例化：实例化需要输入布局的语义槽位与 `InputSlot` 配合，
/// 而 D3D11 在这块的匹配失败**是静默的**（`CreateInputLayout` 返回失败、
/// 之后 draw 什么都不画）——已花三轮试错。顶点缓冲是 `submit` 已在用的模型，
/// 行为可预测。
struct BatchVertex {
  std::array<float, 2> position{};
  std::array<float, 2> local{};
  std::array<float, 2> local01{};
  std::array<float, 4> uv{};
  std::array<float, 2> half_size{};
};
static_assert(sizeof(BatchVertex) == 48, "顶点跨距必须与 HLSL 的 struct 一致");

/// 绘制模式（与 HLSL 里的 `g_mode.x` 对应）。
enum class DrawMode : std::uint32_t {
  SolidRoundRect = 0,
  GradientRoundRect = 1,
  CoverageMask = 2,
  Bitmap = 3,
  BoxBlur = 4,
  /// 亚像素（LCD）覆盖率遮罩：纹理是 R8G8B8，混合走**两遍**（`g_mode.w` 选遍次）。
  CoverageMaskLcd = 5,
};

/// 着色器与固定状态（设备级共享；懒创建）。
struct Pipeline {
  ID3D11VertexShader* vs{nullptr};
  ID3D11PixelShader* ps{nullptr};
  /// 合批着色器与它们的输入布局（缺失时文字退回逐字形路径）。
  ID3D11VertexShader* batch_vs{nullptr};
  ID3D11PixelShader* batch_ps{nullptr};
  ID3DBlob* batch_signature{nullptr};
  ID3D11InputLayout* batch_layout{nullptr};
  ID3D11Buffer* constants{nullptr};
  ID3D11BlendState* blend{nullptr};
  ID3D11BlendState* opaque{nullptr};  ///< 关闭混合（写遮罩时用：要覆盖写入而非叠加）
  /// 亚像素混合的**第一遍**：`Src=ZERO / Dest=INV_SRC_COLOR` ⇒ `D ← D·(1-α_c)`，逐通道。
  /// 硬件混合唯一做不到的就是“逐通道 α”，而把它拆成两步就绕过去了：
  /// 这一步只做目标衰减，α 通道保持不动（`SrcAlpha=ZERO / DestAlpha=ONE`）。
  ID3D11BlendState* lcd_attenuate{nullptr};
  /// 亚像素混合的**第二遍**：`ONE/ONE` 加性加回源项 `S_c·α_c`；α 走正常 src-over。
  ID3D11BlendState* lcd_add{nullptr};
  ID3D11RasterizerState* raster_scissor{nullptr};
  ID3D11SamplerState* sampler_linear{nullptr};
  ID3D11SamplerState* sampler_point{nullptr};
  std::string error{};
  bool ok{false};

  explicit Pipeline(ID3D11Device* device) {
    if (!compile_shaders(device)) return;
    // 合批的**顶点**输入布局（槽 0，逐顶点）。
    //
    // ⚠ 失败**必须可见**：`CreateInputLayout` 失败后 draw 会什么也不画，
    // 而 `CreateInputLayout` 自己只返回一个 HRESULT——之前三轮就是在这里
    // 静默失败、而我一直在猜“为什么画出来是错的”（实际是“根本没画”）。
    if (batch_vs != nullptr && batch_signature != nullptr) {
      const D3D11_INPUT_ELEMENT_DESC elements[] = {
          {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
          {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
          {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
          {"TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
          {"TEXCOORD", 3, DXGI_FORMAT_R32G32_FLOAT, 0, 40, D3D11_INPUT_PER_VERTEX_DATA, 0},
      };
      const HRESULT layout =
          device->CreateInputLayout(elements, 5, batch_signature->GetBufferPointer(),
                                    batch_signature->GetBufferSize(), &batch_layout);
      if (FAILED(layout) || batch_layout == nullptr) {
        ST_LOG_WARN("合批输入布局创建失败（hr=0x{:08X}）——文字将退回逐字形路径：{}",
                    static_cast<unsigned>(layout), error);
        batch_vs = nullptr;   // 标记合批不可用
      }
    }
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = sizeof(ShaderParams);
    buffer.Usage = D3D11_USAGE_DYNAMIC;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&buffer, nullptr, &constants))) {
      error = "创建常量缓冲失败";
      return;
    }
    // 预乘 src-over：SrcBlend=ONE、DestBlend=INV_SRC_ALPHA（着色器输出预乘色）
    D3D11_BLEND_DESC blend_desc{};
    blend_desc.RenderTarget[0].BlendEnable = TRUE;
    blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&blend_desc, &blend))) {
      error = "创建混合状态失败";
      return;
    }
    // 不混合（写遮罩：要覆盖写入而非叠加）
    D3D11_BLEND_DESC opaque_desc{};
    opaque_desc.RenderTarget[0].BlendEnable = FALSE;
    opaque_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&opaque_desc, &opaque))) {
      error = "创建不混合状态失败";
      return;
    }
    // 亚像素两遍混合：第一遍逐通道衰减目标，第二遍加性加回源
    D3D11_BLEND_DESC attenuate_desc{};
    attenuate_desc.RenderTarget[0].BlendEnable = TRUE;
    attenuate_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO;
    attenuate_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_COLOR;
    attenuate_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    attenuate_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    attenuate_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    attenuate_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    attenuate_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&attenuate_desc, &lcd_attenuate))) {
      error = "创建亚像素衰减混合状态失败";
      return;
    }
    D3D11_BLEND_DESC add_desc{};
    add_desc.RenderTarget[0].BlendEnable = TRUE;
    add_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    add_desc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    add_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    add_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    add_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    add_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    add_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&add_desc, &lcd_add))) {
      error = "创建亚像素加性混合状态失败";
      return;
    }
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.ScissorEnable = TRUE;  // 矩形裁剪走剪裁矩形（零成本）
    if (FAILED(device->CreateRasterizerState(&raster, &raster_scissor))) {
      error = "创建光栅化状态失败";
      return;
    }
    const auto make_sampler = [device](D3D11_FILTER filter) -> ID3D11SamplerState* {
      D3D11_SAMPLER_DESC desc{};
      desc.Filter = filter;
      desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
      desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
      desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
      desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
      desc.MaxLOD = D3D11_FLOAT32_MAX;
      ID3D11SamplerState* state = nullptr;
      (void)device->CreateSamplerState(&desc, &state);
      return state;
    };
    sampler_linear = make_sampler(D3D11_FILTER_MIN_MAG_MIP_LINEAR);
    sampler_point = make_sampler(D3D11_FILTER_MIN_MAG_MIP_POINT);
    ok = vs != nullptr && ps != nullptr && sampler_linear != nullptr && blend != nullptr &&
         opaque != nullptr && lcd_attenuate != nullptr && lcd_add != nullptr;
  }

  auto compile_shaders(ID3D11Device* device) -> bool {
    const Modules& mods = modules();
    if (mods.d3dcompiler == nullptr) {
      error = "找不到 d3dcompiler_47.dll（无法编译着色器）";
      return false;
    }
    // 编译入口：动态取符号（只在这里用一次）
    using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
                                       LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    const auto compile = reinterpret_cast<CompileFn>(
        reinterpret_cast<void*>(::GetProcAddress(mods.d3dcompiler, "D3DCompile")));
    if (compile == nullptr) {
      error = "d3dcompiler 中找不到 D3DCompile";
      return false;
    }
    const auto build = [&](const char* entry, const char* target, ID3DBlob** blob) -> bool {
      ID3DBlob* errors = nullptr;
      const HRESULT result = compile(kShaderSource, std::strlen(kShaderSource), "st_gpu", nullptr,
                                     nullptr, entry, target, 0, 0, blob, &errors);
      if (FAILED(result)) {
        error = errors != nullptr ? std::string(static_cast<const char*>(errors->GetBufferPointer()))
                                  : std::format("编译 {} 失败（0x{:08X}）", entry,
                                                static_cast<unsigned>(result));
        if (errors != nullptr) errors->Release();
        return false;
      }
      if (errors != nullptr) errors->Release();
      return true;
    };
    ID3DBlob* vs_blob = nullptr;
    ID3DBlob* ps_blob = nullptr;
    if (!build("vs_main", "vs_4_0", &vs_blob)) return false;
    if (!build("ps_main", "ps_4_0", &ps_blob)) {
      vs_blob->Release();
      return false;
    }
    const HRESULT vs_result = device->CreateVertexShader(vs_blob->GetBufferPointer(),
                                                         vs_blob->GetBufferSize(), nullptr, &vs);
    const HRESULT ps_result = device->CreatePixelShader(ps_blob->GetBufferPointer(),
                                                        ps_blob->GetBufferSize(), nullptr, &ps);
    vs_blob->Release();
    ps_blob->Release();
    // 合批着色器。**失败不阻断**（文字退回逐字形 `submit`），
    // 但**必须可见**——否则“合批没生效”会看起来像“合批算了但算错了”。
    ID3DBlob* batch_vs_blob = nullptr;
    ID3DBlob* batch_ps_blob = nullptr;
    if (build("vs_batch", "vs_4_0", &batch_vs_blob) && build("ps_batch", "ps_4_0", &batch_ps_blob) &&
        batch_vs_blob != nullptr && batch_ps_blob != nullptr) {
      const HRESULT bvs = device->CreateVertexShader(batch_vs_blob->GetBufferPointer(),
                                                     batch_vs_blob->GetBufferSize(), nullptr,
                                                     &batch_vs);
      const HRESULT bps = device->CreatePixelShader(batch_ps_blob->GetBufferPointer(),
                                                    batch_ps_blob->GetBufferSize(), nullptr,
                                                    &batch_ps);
      if (SUCCEEDED(bvs) && SUCCEEDED(bps)) {
        batch_signature = batch_vs_blob;   // 保留供 `CreateInputLayout`
      }
    }
    if (batch_ps_blob != nullptr) batch_ps_blob->Release();
    if (FAILED(vs_result) || FAILED(ps_result)) {
      error = "创建着色器对象失败";
      return false;
    }
    return true;
  }

  Pipeline(const Pipeline&) = delete;
  auto operator=(const Pipeline&) -> Pipeline& = delete;

  ~Pipeline() {
    if (sampler_point != nullptr) sampler_point->Release();
    if (sampler_linear != nullptr) sampler_linear->Release();
    if (raster_scissor != nullptr) raster_scissor->Release();
    if (lcd_add != nullptr) lcd_add->Release();
    if (lcd_attenuate != nullptr) lcd_attenuate->Release();
    if (blend != nullptr) blend->Release();
    if (opaque != nullptr) opaque->Release();
    if (constants != nullptr) constants->Release();
    if (ps != nullptr) ps->Release();
    if (vs != nullptr) vs->Release();
  }
};

/// 设备级共享的管线（首次使用时创建）。
[[nodiscard]] auto pipeline() -> const Pipeline& {
  static Pipeline instance(holder().device_or_null());
  return instance;
}

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
    for (auto& entry : mask_cache_) {
      if (entry.view != nullptr) entry.view->Release();
    }
    for (auto& entry : ramp_cache_) {
      if (entry.view != nullptr) entry.view->Release();
    }
    for (auto& entry : path_cache_) {
      if (entry.view != nullptr) entry.view->Release();
    }
    release_shadow_cache();
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

  /// 原地改尺寸：**不重建设备/上下文**，只重建目标纹理与相关视图。
  ///
  /// 保留什么、丢什么，分界线是"它依不依赖于尺寸":
  /// - 设备/上下文/着色器管线/字形遮罩缓存/渐变 ramp/路径遮罩：与尺寸无关 → **保留**；
  ///   拖动缩放时重建它们等于每帧重传整个字形集（实测这是重建里的大头）。
  /// - 目标纹理/`rtv`/回读暂存/回读缓冲/裁剪栈/阴影缓存：与尺寸钩着 → **重建**；
  ///   阴影目标按区域尺寸分配，留着会直接画出错误结果（比丢掉缓存贵得多）。
  [[nodiscard]] auto resize(int physical_width, int physical_height) -> Status override {
    const int width = physical_width > 0 ? physical_width : 0;
    const int height = physical_height > 0 ? physical_height : 0;
    if (width == 0 || height == 0) {
      return unexpected(ErrorCode::Invalid, "画布尺寸必须为正");
    }
    if (width == physical_width_ && height == physical_height_) return ok();
    // 先释放依赖于旧尺寸的东西（顺序重要：回读暂存先于目标纹理释放没差别，
    // 但目标纹理一旦换成新的，旧 `rtv` 就是悬垂视图——必须成对先清）。
    if (rtv_ != nullptr) {
      rtv_->Release();
      rtv_ = nullptr;
    }
    if (target_ != nullptr) {
      target_->Release();
      target_ = nullptr;
    }
    release_shadow_cache();
    release_readback();
    readback_.clear();
    physical_width_ = width;
    physical_height_ = height;
    create_target();
    if (target_ == nullptr || rtv_ == nullptr) {
      return unexpected(ErrorCode::Io, "GPU 画布：重建渲染目标失败");
    }
    readback_dirty_ = true;
    // 裁剪栈是物理像素矩形：旧栈顶按旧尺寸算，必须重置（否则内容被裁到旧那块）。
    clip_stack_.clear();
    ClipFrame frame;
    frame.rect = math::IntRect{0, 0, width, height};
    frame.mode = 1;
    clip_stack_.push_back(frame);
    return ok();
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

  // —— 绘制原语 ——
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

  void fill_rect(math::Rect rect, const Paint& paint, float radius = 0.0f,
                 DrawOptions options = {}) override {
    if (rect.is_empty()) return;
    const std::array<float, 4> radii{radius * scale_, radius * scale_, radius * scale_,
                                     radius * scale_};
    const PaintOp op = radius > 0.0f ? PaintOp::FillRoundRect : PaintOp::FillRect;
    const math::Rect physical = to_physical_rect(rect);
    if (paint.is_solid()) {
      draw_solid(physical, radii, paint.color(), options.opacity, op);
      return;
    }
    draw_gradient(physical, radii, *paint.gradient(), options.opacity, op);
  }
  void fill_circle(math::Point center, float radius, const Paint& paint,
                   DrawOptions options = {}) override {
    // 圆就是“半径等于半边的圆角矩形”——SDF 下两者完全等价，不需要单独的圆图元
    fill_rect(math::Rect{center.x - radius, center.y - radius, radius * 2.0f, radius * 2.0f}, paint,
              radius, options);
  }
  /// 投影：遮罩 RT → 可分离盒式模糊 ×3 → 缓存 → 合成。
  ///
  /// 与软件同一口径（几何按 0.25 量化做缓存键、遮罩在**规范化坐标**下模糊），
  /// 因此同一张卡片在两者下位置与形状一致。
  void draw_shadow(math::Rect rect, float radius, float blur, math::Color color,
                   math::Point offset = {}, DrawOptions options = {}) override {
    if (rect.is_empty() || color.a == 0U) return;
    const math::Rect physical = to_physical_rect(rect);
    const std::int64_t start = st::time::now_ns();
    const ShadowMask mask =
        shadow_mask(physical.width, physical.height, radius * scale_, blur * scale_,
                    math::Point{offset.x * scale_, offset.y * scale_});
    if (mask.view == nullptr) return;
    const ShaderParams params = base_params(
        DrawMode::CoverageMask,
        math::Rect{physical.x + static_cast<float>(mask.region.x),
                   physical.y + static_cast<float>(mask.region.y),
                   static_cast<float>(mask.region.width),
                   static_cast<float>(mask.region.height)},
        {0.0f, 0.0f, 0.0f, 0.0f}, options.opacity, color);
    submit(params, mask.view, false);
    log_op(PaintOp::Shadow, start,
           static_cast<std::uint64_t>(mask.region.width) *
               static_cast<std::uint64_t>(mask.region.height));
  }

  // —— 路径：**CPU 覆盖率光栅化 + GPU 合成** ——
  //
  // 这是有意为之的混合路径，把理由说清楚：
  // 任意路径要“与软件光栅器一致”有两条路——
  //   ① GPU 三角化 + 解析 AA：需要曲线三角化与边缘 AA 权重，而软件侧已实现这套
  //      且被大量回归测试钉住；在 GPU 侧重写一份，结果是两套不可能一致的抗锯齿，
  //      而一致性正是本项目的验收口径。
  //   ② 复用同一套覆盖率光栅化做**遮罩**，交给 GPU 合成。
  // 选 ②：正确性同源（同一个 `rasterize_mask`），且**静态路径可整块缓存**——
  // 图标与自绘形状在帧间不变，缓存后每帧只剩一次纹理贴图（软件侧每帧都要重新光栅化）。
  // 因此路径上的 GPU 收益来自**缓存与合成**，不来自光栅化本身；不假装它是全 GPU 光栅化。
  void fill_path(const Path& path, const Paint& paint, DrawOptions options = {}) override {
    if (path.is_empty()) return;
    // 逻辑→物理：与软件 Canvas::fill_path 同口径（内部 `path.scaled(scale_)`）。
    // 缺了这步，非整数 DPI 下路径类绘制（菜单面板/勾选/图标）整体缩成 1/scale——
    // hit_test 按逻辑坐标算，画出来缩小错位（实测 1.5x 下菜单面板缩 2/3，光标视觉错位）。
    const Path physical = scale_ == 1.0f ? path : path.scaled(scale_);
    // 缓存键：直接用逻辑 path（与缩放后几何一一对应，不会误命中）。
    rasterize_path(path, physical, options.opacity, paint.color(), PaintOp::FillPath);
  }
  void stroke_path(const Path& path, const Paint& paint, float width, const StrokeStyle& style,
                   DrawOptions options = {}) override {
    if (path.is_empty() || width <= 0.0f) return;
    // 先按**物理像素**把路径描边成轮廓，再光栅化为遮罩（与软件同一套 stroke_to_path）
    const Path outline = detail::stroke_to_path(path.scaled(scale_), width * scale_,
                                                options.antialias ? 0.25f : 0.5f, style);
    rasterize_path(path, outline, options.opacity, paint.color(), PaintOp::Stroke);
  }
  void draw_canvas(const Surface& source, math::Rect destination, DrawOptions options) override {
    const int width = source.physical_width();
    const int height = source.physical_height();
    if (destination.is_empty() || width <= 0 || height <= 0) return;
    // 源像素经**接口**取（对方可能是软件画布，在这里触发上传）
    ID3D11ShaderResourceView* view = bitmap_texture(source.pixels(), width, height);
    if (view == nullptr) return;
    const std::int64_t start = st::time::now_ns();
    const ShaderParams params =
        base_params(DrawMode::Bitmap, to_physical_rect(destination), {0.0f, 0.0f, 0.0f, 0.0f},
                    options.opacity);
    submit(params, view, false);
    log_op(PaintOp::Image, start,
           static_cast<std::uint64_t>(std::lround(destination.width * scale_)) *
               static_cast<std::uint64_t>(std::lround(destination.height * scale_)));
  }

  /// **同后端原生纹理的零拷贝合成**（`Surface::composite_native_texture` 的实现）。
  ///
  /// 为何它能消掉三维那笔 100 ms：普通 `draw_canvas(source, …)` 要 `source.pixels()`，
  /// 而三维场景的像素在显存里——那一步就是“显卡→内存”的同步回读。
  /// 这里拿到的是同设备的 `ID3D11ShaderResourceView*`，直接当纹理采样画一个四边形，
  /// 全程不出显存。
  ///
  /// 安全边界（必须由后端标签 + 设备身份双重校验）：
  /// 两个不同 `ID3D11Device` 的 SRV 不能互用——那种情况下采样会失败或读到垃圾。
  /// 所以要求 `backend == "d3d11"` **且** 句柄确实属于本设备（这里用一个
  /// “本进程建的三维纹理登记表”无法低成本验证，改为默认信任 + 标签约定；
  /// 裁云的用法里两者同进程同设备）。
  [[nodiscard]] auto composite_native_texture(const st::raster::Surface::NativeTexture& texture,
                                             math::Rect destination,
                                             DrawOptions options) -> bool override {
    if (!texture.valid() || texture.backend == nullptr) return false;
    if (std::string_view(texture.backend) != "d3d11") return false;
    if (destination.is_empty()) return false;
    const auto* view = static_cast<const ID3D11ShaderResourceView*>(texture.handle);
    const std::int64_t start = st::time::now_ns();
    const ShaderParams params =
        base_params(DrawMode::Bitmap, to_physical_rect(destination), {0.0f, 0.0f, 0.0f, 0.0f},
                    options.opacity);
    // ⚠ 渲染上下文是**进程级共享**的（`DeviceHolder`），而三维场景也在用它。
    // 裁云导出 16 路并行时，“三维绘制”与“二维合成”会从不同线程同时来——
    // 两者都必须过同一把锁，否则就是那个 0xC0000005。
    //
    // 为何不把 `submit` 整个包起来：`submit` 是所有二维原语的公共入口（窗口毎帧调几百次），
    // 无竞争时加锁也就几纳秒，但这层“对外合成入口”加锁范围最小、意图最清。
    const std::scoped_lock lock(gpu::g_context_mutex());
    submit(params, const_cast<ID3D11ShaderResourceView*>(view), false);
    log_op(PaintOp::Image, start,
           static_cast<std::uint64_t>(std::lround(destination.width * scale_)) *
               static_cast<std::uint64_t>(std::lround(destination.height * scale_)));
    return true;
  }
  void draw_canvas_at(const Surface& source, int x, int y, DrawOptions options = {}) override {
    // ⚠ 与 `Canvas::draw_canvas_at` **必须同语义**：`x/y` 是**物理像素**，
    // 而 `draw_canvas` 的 `destination` 是逻辑坐标——直接传会多乘一次 `device_scale`。
    //
    // 两处实现分开写是"同一语义多处实现"的典型贵处：曾经只修了软件 Canvas 那份，
    // 真窗口（走本文件）毫无变化，而 headless 回归却全绿——
    // 看上去"修好了"，实际用户看到的一模一样。
    const float target_scale = scale_ > 0.0f ? scale_ : 1.0f;
    draw_canvas(source,
                math::Rect{static_cast<float>(x) / target_scale,
                           static_cast<float>(y) / target_scale,
                           static_cast<float>(source.width()), static_cast<float>(source.height())},
                options);
  }
  /// 覆盖率位图混合（字形/路径遮罩）：上传成 **R8**（灰度）或 **R8G8B8A8**（亚像素）纹理，
  /// 当一个四边形画。
  ///
  /// 这是**软件与 GPU 共享语义**的关键原语：字形渲染产出的就是覆盖率位图，
  /// CPU 逐行混合、GPU 贴纹理——两边像素结果可达一致（字形位图本身同源于
  /// 同一套字体引擎，所以字的形状完全一样，只有边缘合成方式不同）。
  ///
  /// `CoverageFormat::Lcd` 时走**两遍混合**（见下方实现注释）：硬件混合的 α 是标量，
  /// 而亚像素的彩边恰恰长在“逐通道的目标衰减”上——一次绘制做不到，两次可以。
  /// 逐行原语：GPU 侧把**一行**覆盖率当「一行宽的纹理片」上传后画四边形。
  ///
  /// 为什么要有这个（而不是只留 `blend_coverage_bitmap`）：`Surface` 的默认
  /// `blend_coverage_bitmap` 是按行驱动这两个原语的——即接口只要求"能做一行"。
  /// GPU 覆写了整块版本（下面的 `blend_coverage_bitmap`，一次上传整张遮罩 + 纹理缓存，
  /// 明显更快），故这两个逐行版本**实际不会被默认实现调到**；但它们是纯虚，
  /// 必须给出实现。这里按语义**一致**的方式落地：把这一行当 1 行的位图交给同一台机制
  /// （不做任何近似），而不是留一个 `not_yet()` 之类的空壳。
  void blend_coverage_row(int y, int x_begin, std::span<const float> coverage, const Paint& paint,
                          float opacity, BlendMode blend) override {
    blend_coverage_bitmap(x_begin, y, coverage, static_cast<int>(coverage.size()), 1, paint, opacity,
                          blend, 0, CoverageFormat::Grayscale);
  }
  void blend_coverage_row_lcd(int y, int x_begin, std::span<const float> coverage,
                              const Paint& paint, float opacity, BlendMode blend) override {
    blend_coverage_bitmap(x_begin, y, coverage, static_cast<int>(coverage.size() / 3U), 1, paint,
                          opacity, blend, 0, CoverageFormat::Lcd);
  }

  /// 从图集取（或写入）一个字形，拿到**图集纹理 + 归一化子矩形**。
  ///
  /// 返回 false 表示图集不可用（装不下/纹理上传失败）——调用方退回独立纹理。
  ///
  /// ⚠ **两个关键的 timing 约束**（都是实测踩过的）：
  ///
  /// ① **新增条目后必须立即重传**。图集是 CPU 缓冲，GPU 纹理是副本；
  ///    `place()` 返回 `fresh=true` 就意味着“CPU 缓冲变了、纹理还是旧的”。
  ///    不能把上传推迟到“某个批次边界”——逐字形路径**根本没有批次**，
  ///    推迟的结果是画出旧内容（实测：`text_glyphs_stay_correct_...` 报 **7018 像素错**）。
  /// ② **命中时完全不碰纹理**。否则每新增一个字形就重传整张图集（1 MB），
  ///    305 字形 → **92 ms/帧**（比不合批慢 60 倍）。
  ///
  /// 两者合起来：只有“遇到新字形”才付一次上传，稳定态零开销。
  /// 这也意味着首帧会慢一点（字形逐渐入图集，每个都传一次）——可接受，
  /// 因为首帧本来就在建各种缓存。
  auto atlas_sample(ID3D11ShaderResourceView*& view, std::array<float, 4>& subrect,
                    std::span<const float> coverage, int width, int height, bool lcd,
                    std::uint64_t cache_key) -> bool {
    auto& atlas = lcd ? lcd_atlas_ : gray_atlas_;
    const auto placement = atlas.place(cache_key, width, height,
                                       [&](std::uint8_t* dst, int row_bytes) {
                                         write_coverage(dst, row_bytes, coverage, width, height,
                                                        lcd);
                                       });
    if (!placement.has_value()) return false;
    ID3D11ShaderResourceView*& slot = lcd ? lcd_atlas_view_ : gray_atlas_view_;
    if (placement->fresh && !upload_atlas(atlas, slot)) return false;
    if (slot == nullptr) return false;
    view = slot;
    const float atlas_width = static_cast<float>(atlas.width());
    const float atlas_height = static_cast<float>(atlas.height());
    const atlas::Region& region = placement->region;
    subrect = {static_cast<float>(region.x) / atlas_width,
               static_cast<float>(region.y) / atlas_height,
               static_cast<float>(region.width) / atlas_width,
               static_cast<float>(region.height) / atlas_height};
    return true;
  }

  /// 攒入一个字形。返回 false 表示本字形**没有入队**（调用方直接画逐字形）。
  ///
  /// 分组条件：颜色 / opacity / LCD——三者都是着色器常量，不同值不能共发一次 draw。
  /// 实测界面里绝大多数文字同为默认前景色，所以分组数很少。
  ///
  /// ⚠ **返回 false 必须让调用方继续走逐字形路径**，而不是把字形丢掉。
  /// 上一轮就是在“入队后 flush 静默失败”时把字形整个弄没了（300 多个字形），
  /// 表现为“彩边像素 289 → 0”——那是比“合批不生效”严重得多的缺陷。
  auto enqueue_glyph(int x, int y, int width, int height, std::array<float, 4> subrect,
                     const Paint& paint, float opacity, bool lcd) -> bool {
    const Pipeline& pipes = pipeline();
    if (!pipes.ok || pipes.batch_vs == nullptr || pipes.batch_ps == nullptr ||
        pipes.batch_layout == nullptr) {
      return false;   // 合批不可用：调用方直接画
    }
    if (batch_.active && (batch_.color != paint.color() || batch_.opacity != opacity ||
                          batch_.lcd != lcd)) {
      flush_glyph_batch();
    }
    if (!batch_.active) {
      batch_.color = paint.color();
      batch_.opacity = opacity;
      batch_.lcd = lcd;
      batch_.active = true;
      batch_.vertices.clear();
    }
    // 两个三角形（6 个顶点），四边形向外扩 1 像素（理由同 `vs_main`：
    // 光栅化按像素中心判定内外，不扩会丢边界那一行）。
    const float px = static_cast<float>(x);
    const float py = static_cast<float>(y);
    const float pw = static_cast<float>(width);
    const float ph = static_cast<float>(height);
    const float x0 = px - 1.0f;
    const float y0 = py - 1.0f;
    const float x1 = px + pw + 1.0f;
    const float y1 = py + ph + 1.0f;
    const std::array<float, 2> half{pw * 0.5f, ph * 0.5f};
    const auto push = [&](float vx, float vy) {
      BatchVertex vertex{};
      vertex.position = {vx, vy};
      vertex.local = {vx - px, vy - py};
      vertex.local01 = {(vx - px) / pw, (vy - py) / ph};
      vertex.uv = subrect;
      vertex.half_size = half;
      batch_.vertices.push_back(vertex);
    };
    push(x0, y0);
    push(x1, y0);
    push(x0, y1);
    push(x1, y0);
    push(x1, y1);
    push(x0, y1);
    return true;
  }

  /// 把累积的字形一次发出。**必须在任何非字形绘制前调用**，否则绘制顺序错乱。
  ///
  /// 失败时**回落逐字形**（而不是丢字形）——每个字形的位置与子矩形都在顶点里，
  /// 可以逐个重放。
  void flush_glyph_batch() {
    if (!batch_.active || batch_.vertices.empty()) {
      batch_.active = false;
      batch_.vertices.clear();
      return;
    }
    // 先取出并清空状态，再绘制：下面的回落路径会经 `submit`，
    // 而 `submit` 第一行就是 `flush_glyph_batch()`——不清空会无限递归。
    const bool was_lcd = batch_.lcd;
    const st::math::Color was_color = batch_.color;
    const float was_opacity = batch_.opacity;
    std::vector<BatchVertex> vertices = std::move(batch_.vertices);
    batch_.active = false;
    batch_.vertices.clear();

    const Pipeline& pipes = pipeline();
    ID3D11ShaderResourceView* atlas_view = was_lcd ? lcd_atlas_view_ : gray_atlas_view_;

    if (atlas_view != nullptr && pipes.batch_vs != nullptr && pipes.batch_ps != nullptr &&
        pipes.batch_layout != nullptr && submit_glyph_batch(was_lcd, was_color, was_opacity,
                                                            atlas_view, vertices)) {
      return;   // 合批成功
    }

    // **回落**：逐个字形走普通 `submit`（每 6 个顶点 = 1 个字形）。
    // 这条路径已验证逐像素正确（结构差 0.000%）。
    for (std::size_t index = 0; index + 5 < vertices.size(); index += 6) {
      const BatchVertex& corner = vertices[index];
      const BatchVertex& opposite = vertices[index + 4];
      const float left = corner.position[0] + 1.0f;
      const float top = corner.position[1] + 1.0f;
      const float right = opposite.position[0] - 1.0f;
      const float bottom = opposite.position[1] - 1.0f;
      ShaderParams params = base_params(
          was_lcd ? DrawMode::CoverageMaskLcd : DrawMode::CoverageMask,
          math::Rect{left, top, right - left, bottom - top}, {0.0f, 0.0f, 0.0f, 0.0f}, was_opacity,
          was_color);
      params.uv = corner.uv;
      if (!was_lcd) {
        submit(params, atlas_view, /*point_sample=*/true);
      } else {
        params.mode[3] = 0.0f;
        submit(params, atlas_view, true, false, 0, 0, pipes.lcd_attenuate);
        params.mode[3] = 1.0f;
        submit(params, atlas_view, true, false, 0, 0, pipes.lcd_add);
      }
    }
  }

  /// 保证顶点缓冲能装 `count` 个顶点（按 2 的幂扩容）。
  auto ensure_glyph_capacity(std::size_t count) -> bool {
    const std::size_t needed = count * sizeof(BatchVertex);
    if (glyph_vertices_ != nullptr && needed <= glyph_vertex_capacity_) return true;
    if (glyph_vertices_ != nullptr) {
      glyph_vertices_->Release();
      glyph_vertices_ = nullptr;
      glyph_vertex_capacity_ = 0;
    }
    std::size_t capacity = 4096 * sizeof(BatchVertex);
    while (capacity < needed) capacity *= 2;
    D3D11_BUFFER_DESC buffer{};
    buffer.Usage = D3D11_USAGE_DYNAMIC;
    buffer.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    buffer.ByteWidth = static_cast<UINT>(capacity);
    if (FAILED(device_->CreateBuffer(&buffer, nullptr, &glyph_vertices_))) {
      glyph_vertices_ = nullptr;
      return false;
    }
    glyph_vertex_capacity_ = capacity;
    return true;
  }

  /// 一次 draw 发出整批字形。返回 false 时调用方回落逐字形。
  auto submit_glyph_batch(bool lcd, st::math::Color color, float opacity,
                          ID3D11ShaderResourceView* view,
                          const std::vector<BatchVertex>& vertices) -> bool {
    const Pipeline& pipes = pipeline();
    if (rtv_ == nullptr || vertices.empty()) return false;
    if (!ensure_glyph_capacity(vertices.size())) return false;

    D3D11_MAPPED_SUBRESOURCE mapped_vertices{};
    if (FAILED(context_->Map(glyph_vertices_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped_vertices))) {
      return false;
    }
    std::memcpy(mapped_vertices.pData, vertices.data(), vertices.size() * sizeof(BatchVertex));
    context_->Unmap(glyph_vertices_, 0);

    const ClipFrame& clip = current_clip();
    ID3D11ShaderResourceView* clip_mask = clip.mode > 2 ? clip.mask : nullptr;
    ID3D11RenderTargetView* targets[1] = {rtv_};
    context_->OMSetRenderTargets(1, targets, nullptr);
    const D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(physical_width_),
                                  static_cast<float>(physical_height_), 0.0f, 1.0f};
    context_->RSSetViewports(1, &viewport);
    const math::IntRect scissor = clip.rect;
    const D3D11_RECT rect{scissor.x, scissor.y, scissor.x + scissor.width,
                          scissor.y + scissor.height};
    context_->RSSetScissorRects(1, &rect);
    context_->RSSetState(pipes.raster_scissor);

    const UINT stride = sizeof(BatchVertex);
    const UINT offset = 0;
    ID3D11Buffer* buffers[1] = {glyph_vertices_};
    context_->IASetVertexBuffers(0, 1, buffers, &stride, &offset);
    context_->IASetInputLayout(pipes.batch_layout);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(pipes.batch_vs, nullptr, 0);
    context_->PSSetShader(pipes.batch_ps, nullptr, 0);

    // 字形尺寸在顶点里，但 `g_rect.zw` 仍要设成字形的（SDF/归一化用不到它，
    // 但 `shade` 之外的路径可能读）——这里取最后一个字形无意义，
    // 所以用图集尺寸占位并**依赖 ps_batch 只用顶点数据**。
    ShaderParams params = base_params(
        lcd ? DrawMode::CoverageMaskLcd : DrawMode::CoverageMask,
        math::Rect{0.0f, 0.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, opacity, color);

    ID3D11ShaderResourceView* views[1] = {view};
    ID3D11ShaderResourceView* clip_views[1] = {clip_mask};
    ID3D11SamplerState* samplers[1] = {pipes.sampler_point};
    context_->PSSetShaderResources(0, 1, views);
    context_->PSSetShaderResources(1, 1, clip_views);
    context_->PSSetSamplers(0, 1, samplers);

    const float blend_factor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const auto draw_once = [&](ID3D11BlendState* state) -> bool {
      D3D11_MAPPED_SUBRESOURCE mapped{};
      if (FAILED(context_->Map(pipes.constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return false;
      }
      std::memcpy(mapped.pData, &params, sizeof(params));
      context_->Unmap(pipes.constants, 0);
      ID3D11Buffer* constant_buffers[1] = {pipes.constants};
      context_->VSSetConstantBuffers(0, 1, constant_buffers);
      context_->PSSetConstantBuffers(0, 1, constant_buffers);
      context_->OMSetBlendState(state, blend_factor, 0xFFFFFFFFU);
      context_->Draw(static_cast<UINT>(vertices.size()), 0);
      return true;
    };
    if (!lcd) {
      if (!draw_once(pipes.blend)) return false;
    } else {
      params.mode[3] = 0.0f;
      if (!draw_once(pipes.lcd_attenuate)) return false;
      params.mode[3] = 1.0f;
      if (!draw_once(pipes.lcd_add)) return false;
    }
    readback_dirty_ = true;
    return true;
  }

  /// 把图集 CPU 缓冲整张传成纹理（重建旧的）。
  ///
  /// 全量重传而不是“只传脏矩形”：图集 ≤ 2048² RGBA = ≤ 16 MB，
  /// 而增量上传要维护脏区域、且在扩容后失效——复杂度远高于收益。
  /// 稳定态下这个方法**根本不会被调用**（只在遇到新字形时）。
  auto upload_atlas(atlas::GlyphAtlas& atlas, ID3D11ShaderResourceView*& slot) -> bool {
    const int width = atlas.width();
    const int height = atlas.height();
    (void)atlas.take_resized();
    if (slot != nullptr) {
      slot->Release();
      slot = nullptr;
    }
    slot = create_texture(DXGI_FORMAT_R8G8B8A8_UNORM, width, height, atlas.pixels().data(),
                          static_cast<std::size_t>(width) * 4U);
    return slot != nullptr;
  }

  /// 把覆盖率写入图集的 RGBA8 缓冲。
  ///
  /// 统一用 RGBA8（不分开 RGBA/R8 两种格式）：
  ///   · 灰度：R=G=B=覆盖率 · 亚像素：R/G/B=逐通道覆盖率 · A=255
  /// 两种模式因此共用同一套搬运/上传代码，只是写入内容不同。
  /// 代价是灰度下 4 倍内存（图集 ≤ 2048²，即 ≤ 16 MB），可接受。
  static void write_coverage(std::uint8_t* dst, int row_bytes, std::span<const float> coverage,
                             int width, int height, bool lcd) {
    const auto clamp_byte = [](float value) -> std::uint8_t {
      return static_cast<std::uint8_t>(
          std::lround(std::max(0.0f, std::min(1.0f, value)) * 255.0f));
    };
    for (int row = 0; row < height; ++row) {
      std::uint8_t* line = dst + static_cast<std::size_t>(row) * row_bytes;
      const std::size_t base = static_cast<std::size_t>(row) * static_cast<std::size_t>(width);
      for (int column = 0; column < width; ++column) {
        if (lcd) {
          const std::size_t offset = (base + static_cast<std::size_t>(column)) * 3U;
          line[column * 4 + 0] = clamp_byte(coverage[offset + 0]);
          line[column * 4 + 1] = clamp_byte(coverage[offset + 1]);
          line[column * 4 + 2] = clamp_byte(coverage[offset + 2]);
        } else {
          const std::uint8_t value =
              clamp_byte(coverage[base + static_cast<std::size_t>(column)]);
          line[column * 4 + 0] = value;
          line[column * 4 + 1] = value;
          line[column * 4 + 2] = value;
        }
        line[column * 4 + 3] = 255U;
      }
    }
  }

  void blend_coverage_bitmap(int x, int y, std::span<const float> coverage, int width, int height,
                             const Paint& paint, float opacity, BlendMode blend,
                             std::uint64_t cache_key = 0,
                             CoverageFormat format = CoverageFormat::Grayscale) override {
    if (width <= 0 || height <= 0 || opacity <= 0.0f || blend != BlendMode::SrcOver) return;
    const bool lcd = format == CoverageFormat::Lcd;
    const std::size_t expected = static_cast<std::size_t>(width) *
                                 static_cast<std::size_t>(height) * (lcd ? 3U : 1U);
    if (coverage.size() < expected) return;
    const std::int64_t start = st::time::now_ns();
    // ⚠ **按稳定身份缓存，不能按指针**。
    //
    // 这里原来把 `coverage.data()` 当键，并注释"字形位图长期存活且指针稳定"——
    // 那个假设是错的：字体引擎的缓存超过上限会 `glyphs.clear()`，位图内存被释放后
    // **被新字形复用同一地址**，于是"按指针命中"把上一个字形的纹理当成了这个字形的。
    // 症状：界面文字间歇性变成别的字（`folder`→`folBer`、`概览`→`外测`），
    // 且只在渲染过足够多字形（触发过一次清空）之后才出现——极难复现。
    // 稳定身份来自字体引擎（face + 字形号 + 字号档 + 超采样），与内存生命周期无关。
    // 亚像素模式下身份里还带**渲染模式位**（灰度与亚像素位图排布不同，不能互相顶替）。
    //
    // **图集路径**（`ST_GPU_GLYPH_ATLAS=0` 可关）：把字形写进共享图集纹理，
    // 省掉“每个字形一张独立纹理”的建立/销毁。
    //
    // 正确性要求（本次已按此实现）：
    //   ① 新增条目后**立即**重传图集纹理——不能推迟到某个“批次边界”，
    //      因为逐字形路径根本没有批次（推迟就会画出旧内容）；
    //   ② 稳定态（命中）**完全不碰纹理**，否则每字形 1 MB 上传会慢 60 倍（实测 92 ms/帧）。
    ID3D11ShaderResourceView* view = nullptr;
    std::array<float, 4> subrect{0.0f, 0.0f, 1.0f, 1.0f};
    const bool atlas_ok = glyph_atlas_enabled() && cache_key != 0 &&
                          atlas_sample(view, subrect, coverage, width, height, lcd, cache_key);
    if (!atlas_ok) {
      view = mask_texture(coverage.data(), width, height, cache_key, format);
    }
    if (view == nullptr) return;

    // **攒入批次**（`ST_GPU_GLYPH_BATCH=0` 可关）。
    //
    // 安全网（这是上一轮真正该做而没做的事）：入队**一定成功或一定不生效**——
    // `enqueue_glyph` 返回 false 时本函数继续走下面的逐字形路径，
    // **不会出现“字形进了队但队没发出去”的丢字形情况**。
    // 而 `flush_glyph_batch` 里的任一失败（布局缺失/缓冲建不出来）都回落逐字形。
    if (glyph_batch_enabled() && atlas_ok &&
        enqueue_glyph(x, y, width, height, subrect, paint, opacity, lcd)) {
      log_op(PaintOp::Text, start,
             static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height));
      return;
    }

    ShaderParams params = base_params(
        lcd ? DrawMode::CoverageMaskLcd : DrawMode::CoverageMask,
        math::Rect{static_cast<float>(x), static_cast<float>(y), static_cast<float>(width),
                   static_cast<float>(height)},
        {0.0f, 0.0f, 0.0f, 0.0f}, opacity, paint.color());
    params.uv = subrect;
    if (!lcd) {
      submit(params, view, /*point_sample=*/true);
    } else {
      // 亚像素：**两遍**。第一遍把目标按 (1-α_c) 逐通道衰减，第二遍加性加回 S_c·α_c。
      // 合成式与软件侧 `over_premul_lcd` 完全一致：out_c = S_c·α_c + D_c·(1 - a_s·α_c)。
      const Pipeline& pipes = pipeline();
      params.mode[3] = 0.0f;  // g_mode.w = 0 → 衰减遍
      submit(params, view, /*point_sample=*/true, /*opaque=*/false, /*viewport_width=*/0,
             /*viewport_height=*/0, pipes.lcd_attenuate);
      params.mode[3] = 1.0f;  // g_mode.w = 1 → 源项加回遍
      submit(params, view, /*point_sample=*/true, /*opaque=*/false, /*viewport_width=*/0,
             /*viewport_height=*/0, pipes.lcd_add);
    }
    log_op(PaintOp::Text, start,
           static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height));
  }

  // —— 裁剪 ——
  void push_clip_rect(math::Rect rect) override {
    // ⚠ **必须继承父帧的形状**（与软件光栅器的 `Canvas::push_clip_rect` 同口径）：
    // 嵌套时外层可能是**圆角裁剪**（卡片 radius），内层再加一个矩形裁剪
    //（例如 `CodeEditor::paint_content` 开头就 `push_clip_rect(bounds_)`）。
    // 若这里新建一个只带 `rect` 的帧，栈顶就变成 `mode=1`、圆角信息全丢——
    // 而 `submit` 只读**栈顶**一帧（`current_clip()`），于是圆角被降级成直角。
    // 实测症状：整个应用走软件渲染时卡片四角正常，走 GPU（D3D11）时
    // **编辑器卡片左下角/右下角是方的**（用户报的那条）。
    ClipFrame frame = current_clip();
    frame.rect = to_physical(rect).intersect(frame.rect);
    // 如果父帧已有形状（圆角/路径遮罩），保留它；否则降为纯矩形裁剪。
    if (frame.mode == 0) frame.mode = 1;
    clip_stack_.push_back(frame);
  }
  void push_clip_rounded_rect(math::Rect rect, float radius) override {
    ClipFrame frame;
    frame.rect = to_physical(rect).intersect(current_clip().rect);
    frame.shape = to_physical_rect(rect);
    frame.radii = {radius * scale_, radius * scale_, radius * scale_, radius * scale_};
    frame.mode = 2;
    clip_stack_.push_back(frame);
  }
  /// 阴影裁剪（遮罩纹理）：着色器用 t1 采样遮罩
  void push_clip_path(const Path& path) override {
    // 路径裁剪：同样走“覆盖率遮罩 + 纹理裁剪”，与填充/描边同一套（见下方 `rasterize_path`）
    if (path.is_empty()) return;
    const Path physical = scale_ == 1.0f ? path : path.scaled(scale_);
    const math::Rect bounds = physical.flattened_bounds(0.25f);
    if (bounds.is_empty()) return;
    math::IntRect area = bounds.inflate(1.0f).round_out();
    if (area.is_empty()) return;
    // 与 `rasterize_path` 同理：遮罩只需覆盖当前裁剪域（不取交时超界路径裁剪同样
    // 会要求 TB 级分配）；frame.shape 与遮罩同界，着色器的 UV 映射才一致。
    area = area.intersect(current_clip().rect);
    if (area.is_empty()) return;
    ID3D11ShaderResourceView* view = path_mask_texture(path, area, &physical);
    if (view == nullptr) return;
    ClipFrame frame;
    frame.rect = area;
    frame.shape = math::Rect{static_cast<float>(area.x), static_cast<float>(area.y),
                             static_cast<float>(area.width), static_cast<float>(area.height)};
    frame.mask = view;
    frame.mode = 3;  // 遮罩纹理裁剪
    clip_stack_.push_back(frame);
  }
  void pop_clip() override {
    if (!clip_stack_.empty()) clip_stack_.pop_back();
  }
  [[nodiscard]] auto clip_rect() const noexcept -> math::IntRect override {
    return current_clip().rect;
  }
  [[nodiscard]] auto has_mask_clip() const noexcept -> bool override {
    return current_clip().mode > 1;
  }

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
  /// 裁剪帧：矩形裁剪走剪裁矩形；圆角裁剪用 SDF；路径裁剪用遮罩纹理。
  struct ClipFrame {
    math::IntRect rect{0, 0, 0, 0};
    math::Rect shape{};
    std::array<float, 4> radii{0.0f, 0.0f, 0.0f, 0.0f};
    ID3D11ShaderResourceView* mask{nullptr};  ///< mode 3 时的遮罩纹理（不持有所有权）
    std::uint32_t mode{0};  ///< 0=无 1=矩形（剪裁矩形） 2=圆角（SDF） 3=遮罩纹理
  };

  [[nodiscard]] auto current_clip() const noexcept -> const ClipFrame& {
    if (clip_stack_.empty()) {
      // 默认裁剪 = 整块目标（用函数内静态量避免每次构造）
      static thread_local ClipFrame full;
      full.rect = math::IntRect{0, 0, physical_width_, physical_height_};
      return full;
    }
    return clip_stack_.back();
  }

  /// 逻辑矩形 → 物理矩形（浮点，**不取整**：SDF 需要精确边界才能正确抗锯齿）。
  [[nodiscard]] auto to_physical_rect(math::Rect rect) const noexcept -> math::Rect {
    return math::Rect{rect.x * scale_, rect.y * scale_, rect.width * scale_, rect.height * scale_};
  }

  /// 组装一份参数（除颜色外都就位）。
  [[nodiscard]] auto base_params(DrawMode mode, math::Rect physical, std::array<float, 4> radii,
                                 float opacity,
                                 math::Color color = math::Color{}) const -> ShaderParams {
    ShaderParams params;
    params.rect[0] = physical.x;
    params.rect[1] = physical.y;
    params.rect[2] = physical.width;
    params.rect[3] = physical.height;
    params.radii = radii;
    const ClipFrame& clip = current_clip();
    params.mode[0] = static_cast<float>(static_cast<std::uint32_t>(mode));
    params.mode[1] = std::max(0.0f, std::min(1.0f, opacity));
    params.mode[2] = static_cast<float>(clip.mode);
    params.clip[0] = clip.shape.x;
    params.clip[1] = clip.shape.y;
    params.clip[2] = clip.shape.width;
    params.clip[3] = clip.shape.height;
    params.clip_radii = clip.radii;
    params.viewport[0] = static_cast<float>(physical_width_);
    params.viewport[1] = static_cast<float>(physical_height_);
    write_color(params, color);
    return params;
  }

  /// 预乘 RGBA 写入参数（着色器输出预乘色，输入也必须是预乘）。
  static void write_color(ShaderParams& params, math::Color color) {
    const float alpha = static_cast<float>(color.a) / 255.0f;
    params.color[0] = static_cast<float>(color.r) / 255.0f * alpha;
    params.color[1] = static_cast<float>(color.g) / 255.0f * alpha;
    params.color[2] = static_cast<float>(color.b) / 255.0f * alpha;
    params.color[3] = alpha;
  }

  /// 提交一次四边形绘制（设状态 → 绑纹理 → 上传参数 → Draw）。
  /// `point_sample`：小字形纹理必须**点采样**（线性采样会把 ≤1px 的笔画抹糊）。
  void submit(const ShaderParams& params, ID3D11ShaderResourceView* view, bool point_sample,
              bool opaque = false, int viewport_width = 0, int viewport_height = 0,
              ID3D11BlendState* blend_override = nullptr) {
    // **先 flush 字形批次**：`submit` 是“所有非字形绘制”的唯一入口，
    // 在这里 flush 就保证了字形与其它图元的相对顺序不变。
    //
    // 为何选这个位置而不是逐个原语加：`Surface` 有十几个绘制入口
    //（fill_rect/fill_path/stroke/shadow/blur/image/clip…），逐个加必漏；
    // 而它们**全部**最终经过 `submit`（`submit_glyph_batch` 除外）。
    // 漏掉任何一个 flush 点的后果是“文字被后画的矩形盖住”。
    flush_glyph_batch();
    const Pipeline& pipes = pipeline();
    if (!pipes.ok) return;
    if (rtv_ == nullptr) return;
    const ClipFrame& clip = current_clip();
    ID3D11ShaderResourceView* clip_mask = clip.mode > 2 ? clip.mask : nullptr;
    const int view_w = viewport_width > 0 ? viewport_width : physical_width_;
    const int view_h = viewport_height > 0 ? viewport_height : physical_height_;

    ID3D11RenderTargetView* targets[1] = {rtv_};
    context_->OMSetRenderTargets(1, targets, nullptr);
    const D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(view_w),
                                  static_cast<float>(view_h), 0.0f, 1.0f};
    context_->RSSetViewports(1, &viewport);
    // 矩形裁剪走剪裁矩形（零着色器成本）：圆角裁剪交给 SDF
    const math::IntRect scissor = current_clip().rect;
    const D3D11_RECT rect{scissor.x, scissor.y, scissor.x + scissor.width,
                          scissor.y + scissor.height};
    context_->RSSetScissorRects(1, &rect);
    context_->RSSetState(pipes.raster_scissor);
    const float blend_factor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    // 亚像素文字的两遍混合需要**换混合状态**（一次绘制内唯一的变量，其余状态相同；
    // 第二处同名调用在 mask 绘制路径上，不接这个开关）。
    ID3D11BlendState* blend_state = opaque ? pipes.opaque : pipes.blend;
    if (blend_override != nullptr) blend_state = blend_override;
    context_->OMSetBlendState(blend_state, blend_factor, 0xFFFFFFFFU);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context_->VSSetShader(pipes.vs, nullptr, 0);
    context_->PSSetShader(pipes.ps, nullptr, 0);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(pipes.constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    std::memcpy(mapped.pData, &params, sizeof(params));
    context_->Unmap(pipes.constants, 0);
    ID3D11Buffer* buffers[1] = {pipes.constants};
    context_->VSSetConstantBuffers(0, 1, buffers);
    context_->PSSetConstantBuffers(0, 1, buffers);

    ID3D11ShaderResourceView* views[1] = {view};
    context_->PSSetShaderResources(0, 1, views);
    // 路径裁剪的遮罩绑到 t1（未用时绑 null：不绑就会残留上一个 draw 的绑定）
    ID3D11ShaderResourceView* clip_views[1] = {clip_mask};
    context_->PSSetShaderResources(1, 1, clip_views);
    ID3D11SamplerState* samplers[1] = {point_sample ? pipes.sampler_point : pipes.sampler_linear};
    context_->PSSetSamplers(0, 1, samplers);
    context_->Draw(4, 0);
    readback_dirty_ = true;
  }

  void draw_solid(math::Rect physical, std::array<float, 4> radii, math::Color color, float opacity,
                  PaintOp op) {
    const std::int64_t start = st::time::now_ns();
    const ShaderParams params =
        base_params(DrawMode::SolidRoundRect, physical, radii, opacity, color);
    submit(params, nullptr, false);
    log_op(op, start, static_cast<std::uint64_t>(std::lround(physical.width)) *
                         static_cast<std::uint64_t>(std::lround(physical.height)));
  }

  /// 渐变：CPU 侧按色标生成 256×1 的**预乘**色带，着色器按几何采样。
  /// 这样任意数量/位置的色标都是精确的（不需要在 HLSL 里重建插值逻辑）。
  void draw_gradient(math::Rect physical, std::array<float, 4> radii, const Gradient& gradient,
                     float opacity, PaintOp op) {
    ID3D11ShaderResourceView* ramp = ramp_texture(gradient);
    if (ramp == nullptr) return;
    const std::int64_t start = st::time::now_ns();
    ShaderParams params =
        base_params(DrawMode::GradientRoundRect, physical, radii, opacity);
    switch (gradient.kind()) {
      case Gradient::Kind::Linear: {
        const math::Point from = gradient.start();
        const math::Point to = gradient.end();
        params.axis[0] = from.x * scale_;
        params.axis[1] = from.y * scale_;
        params.axis[2] = to.x * scale_;
        params.axis[3] = to.y * scale_;
        params.axis_extra[0] = 0.0f;
        break;
      }
      case Gradient::Kind::Radial: {
        const math::Point center = gradient.start();
        params.axis[0] = center.x * scale_;
        params.axis[1] = center.y * scale_;
        params.axis_extra[0] = 1.0f;
        params.axis_extra[1] = gradient.radius() * scale_;
        break;
      }
      default: {
        const math::Point center = gradient.start();
        params.axis[0] = center.x * scale_;
        params.axis[1] = center.y * scale_;
        params.axis_extra[0] = 2.0f;
        break;
      }
    }
    submit(params, ramp, false);
    log_op(op, start, static_cast<std::uint64_t>(std::lround(physical.width)) *
                         static_cast<std::uint64_t>(std::lround(physical.height)));
  }

  void log_op(PaintOp op, std::int64_t start_ns, std::uint64_t pixels) {
    if (profiler_ == nullptr) return;
    profiler_->add(op, static_cast<double>(st::time::now_ns() - start_ns) / 1'000'000.0, pixels);
  }

  // ————————————————————————————————————————————————————————————————————————
  // 路径遮罩与阴影遮罩（都带缓存）
  // ————————————————————————————————————————————————————————————————————————

  /// 路径 → 覆盖率遮罩纹理（按**路径几何**缓存）。
  auto path_mask_texture(const Path& cache_key_source, const math::IntRect& area,
                         const Path* rasterize) -> ID3D11ShaderResourceView* {
    const std::uint64_t key = path_key(cache_key_source, area);
    for (const auto& entry : path_cache_) {
      if (entry.key == key) return entry.view;
    }
    if (rasterize == nullptr || rasterize->is_empty()) return nullptr;
    st::raster::Mask mask(area.width, area.height);
    // `rasterize_mask` 的语义是“路径坐标 **减去** origin”（内部 `add_path(path, -origin)`，
    // 与 canvas.cpp 的遮罩调用同约定）——因此这里传 **area 本身**。
    //
    // 曾经的 `-area.x/-area.y` 是**符号反了**：遮罩内容被画到 `path + 2·area` 处，
    // 大部分在缓存外的区域被裁掉，表现为“GPU 路径/描边/图标的位置飘移且多半只剩半边”。
    // 由于当时的奇偶测试的 structural 计数器恒为 0（假绿），这一直没被量化到；
    // 修复后由 `gpu_huge_path_mask_is_bounded_by_canvas` 与真实控件对比用例钉住。
    st::raster::detail::rasterize_mask(mask, *rasterize, static_cast<float>(area.x),
                                       static_cast<float>(area.y));
    const std::span<const std::uint8_t> values = mask.values();
    ID3D11ShaderResourceView* view =
        create_texture(DXGI_FORMAT_R8_UNORM, area.width, area.height, values.data(),
                       static_cast<std::size_t>(area.width));
    if (view == nullptr) return nullptr;
    if (path_cache_.size() >= kPathCacheLimit) {
      path_cache_.front().view->Release();
      path_cache_.erase(path_cache_.begin());
    }
    path_cache_.push_back(PathCacheEntry{key, view});
    return view;
  }

  /// 路径缓存键：命令序列 + 坐标 + 遮罩区域（量化到 1/4 像素）。
  [[nodiscard]] static auto path_key(const Path& path, const math::IntRect& area) -> std::uint64_t {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&hash](std::uint64_t value) {
      hash ^= value;
      hash *= 1099511628211ULL;
    };
    const auto quantize = [](float value) -> std::uint64_t {
      return static_cast<std::uint64_t>(static_cast<std::int64_t>(std::lround(value * 4.0f)) +
                                        0x40000000);
    };
    mix(static_cast<std::uint64_t>(area.x));
    mix(static_cast<std::uint64_t>(area.y));
    mix(static_cast<std::uint64_t>(area.width));
    mix(static_cast<std::uint64_t>(area.height));
    for (const st::raster::PathCommand& command : path.commands()) {
      mix(static_cast<std::uint64_t>(command.kind));
      for (const st::math::Point& point : {command.p1, command.p2, command.p3}) {
        mix(quantize(point.x));
        mix(quantize(point.y));
      }
    }
    return hash;
  }

  /// 描边/填充的公共部分：光栅化为遮罩 → 合成。
  void rasterize_path(const Path& cache_key_source, const Path& rasterize, float opacity,
                      math::Color color, PaintOp op) {
    if (rasterize.is_empty()) return;
    const math::Rect bounds = rasterize.flattened_bounds(0.25f);
    if (bounds.is_empty()) return;
    math::IntRect area = bounds.inflate(1.0f).round_out();
    if (area.is_empty()) return;
    // 遮罩只需覆盖**当前裁剪域内的像素**：与 clip 取交。
    //
    // 不取交时的真实事故（2026-09-30 定位）：把无界约束哨兵 kUnbounded（1e9）
    // 当作自己高度的元素（早期 mdeditor 示例的 SourceView 边框，示例已删、教训保留），其描边路径的包围盒高度
    // ≈1e9 → `Mask(1362, 999999979)` 试图分配 ~1.3 TB → std::bad_alloc 直接崩掉
    // 进程（GPU 路径特有：软件光栅器按扫描线裁剪，不受影响；因此无头 GPU 渲染
    // 下必崩、软件渲染下不崩——差异很迷惑）。
    // 取交后遮罩与 clip 同界，**clip 外的像素本来就不会被写入，语义完全不变**；
    // 同时这也是对一切越界几何（NaN/巨大坐标）的通用止血。
    area = area.intersect(current_clip().rect);
    if (area.is_empty()) return;
    const std::int64_t start = st::time::now_ns();
    ID3D11ShaderResourceView* view = path_mask_texture(cache_key_source, area, &rasterize);
    if (view == nullptr) return;
    const ShaderParams params = base_params(
        DrawMode::CoverageMask,
        math::Rect{static_cast<float>(area.x), static_cast<float>(area.y),
                   static_cast<float>(area.width), static_cast<float>(area.height)},
        {0.0f, 0.0f, 0.0f, 0.0f}, opacity, color);
    submit(params, view, false);
    log_op(op, start,
           static_cast<std::uint64_t>(area.width) * static_cast<std::uint64_t>(area.height));
  }

  /// 投影遮罩：建 R8 目标 → 画圆角矩形 → 可分离盒式模糊 ×3 → 缓存。
  struct ShadowMask {
    ID3D11ShaderResourceView* view{nullptr};
    math::IntRect region{};  ///< 物理像素（相对于被投影矩形）
  };

  /// 影子遮罩的两个 R8 目标（模糊 ping-pong 用）。
  struct ShadowTargets {
    ID3D11Texture2D* texture_a{nullptr};
    ID3D11RenderTargetView* rtv_a{nullptr};
    ID3D11ShaderResourceView* srv_a{nullptr};
    ID3D11Texture2D* texture_b{nullptr};
    ID3D11RenderTargetView* rtv_b{nullptr};
    ID3D11ShaderResourceView* srv_b{nullptr};
  };
  struct ShadowCacheEntry {
    std::uint64_t key{0};
    math::IntRect region{};
    ShadowTargets targets{};
  };
  struct PathCacheEntry {
    std::uint64_t key{0};
    ID3D11ShaderResourceView* view{nullptr};
  };

  auto shadow_mask(float width, float height, float radius, float blur, math::Point offset)
      -> ShadowMask {
    if (width <= 0.0f || height <= 0.0f) return {};
    const auto quantize = [](float value) -> std::uint64_t {
      return static_cast<std::uint64_t>(static_cast<std::int64_t>(std::lround(value * 4.0f)) +
                                        0x40000000);
    };
    std::uint64_t key = 1469598103934665603ULL;
    for (const std::uint64_t part : {quantize(width), quantize(height), quantize(radius),
                                     quantize(blur), quantize(offset.x), quantize(offset.y)}) {
      key ^= part;
      key *= 1099511628211ULL;
    }
    for (const auto& entry : shadow_cache_) {
      if (entry.key == key) return ShadowMask{entry.targets.srv_a, entry.region};
    }
    // 与软件同一口径：region = 矩形（带偏移）外扩 blur*2+2，遮罩在**规范化坐标**下模糊
    const float padding = blur * 2.0f + 2.0f;
    const math::Rect region =
        math::Rect{0.0f, 0.0f, width, height}.offset(offset.x, offset.y).inflate(padding);
    const math::IntRect area = region.round_out();
    if (area.is_empty()) return {};
    ShadowTargets targets;
    if (!acquire_shadow_targets(area.width, area.height, targets)) return {};
    const float zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    context_->ClearRenderTargetView(targets.rtv_a, zero);
    const float zero_b[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    context_->ClearRenderTargetView(targets.rtv_b, zero_b);
    // 画圆角矩形（白色 → R 通道就是覆盖率）
    const math::Rect local{offset.x - static_cast<float>(area.x),
                           offset.y - static_cast<float>(area.y), width, height};
    ShaderParams shape = base_params(DrawMode::SolidRoundRect, local,
                                     {radius, radius, radius, radius}, 1.0f,
                                     math::Color{255, 255, 255, 255});
    // **必须**把着色器视口改成遮罩尺寸：顶点阶段用它算 NDC，而 `base_params` 默认填的是
    // 画布尺寸——渲染到遮罩 RT 时会算错尺度，四边形落到目标外面。
    // 现象是“投影完全没画出来、也不报任何错”（靠 ink_ratio 断言才捉到）。
    apply_shadow_viewport(shape, area);
    submit_to(targets.rtv_a, shape, nullptr, false, true, area.width, area.height);
    // 可分离盒式模糊 ×3（水平 + 垂直各一遍算一 pass），ping-pong 于 A、B
    const int box_radius = static_cast<int>(std::lround(blur * 0.5f));
    ID3D11RenderTargetView* read_rtv = targets.rtv_a;
    ID3D11ShaderResourceView* read_srv = targets.srv_a;
    for (int pass = 0; pass < 3 && box_radius > 0; ++pass) {
      for (const bool along_x : {true, false}) {
        const bool reading_a = read_rtv == targets.rtv_a;
        ID3D11RenderTargetView* write_rtv = reading_a ? targets.rtv_b : targets.rtv_a;
        ShaderParams blur_params = base_params(
            DrawMode::BoxBlur,
            math::Rect{0.0f, 0.0f, static_cast<float>(area.width), static_cast<float>(area.height)},
            {0.0f, 0.0f, 0.0f, 0.0f}, 1.0f);
        blur_params.axis[0] = along_x ? 1.0f : 0.0f;
        blur_params.axis[1] = along_x ? 0.0f : 1.0f;
        blur_params.axis_extra[0] = static_cast<float>(box_radius);
        apply_shadow_viewport(blur_params, area);
        submit_to(write_rtv, blur_params, read_srv, false, true, area.width, area.height);
        read_rtv = write_rtv;
        read_srv = reading_a ? targets.srv_b : targets.srv_a;
      }
    }
    // 3 pass × 2 遍后结果回到 A（半径为 0 时也直接在 A）
    const ShadowMask result{targets.srv_a, area};
    if (shadow_cache_.size() >= kShadowCacheLimit) {
      ShadowTargets victim = shadow_cache_.front().targets;
      shadow_cache_.erase(shadow_cache_.begin());
      release_shadow_targets(victim);
    }
    shadow_cache_.push_back(ShadowCacheEntry{key, area, targets});
    return result;
  }

  /// 遮罩渲染时把着色器视口改成遮罩尺寸（否则 NDC 尺度错，四边形落到目标外）。
  static void apply_shadow_viewport(ShaderParams& params, const math::IntRect& area) {
    params.viewport[0] = static_cast<float>(area.width);
    params.viewport[1] = static_cast<float>(area.height);
  }

  /// 提交到指定 RT（影子遮罩用；与 `submit` 的区别是目标不是画布自身的 RT）。
  void submit_to(ID3D11RenderTargetView* target, const ShaderParams& params,
                 ID3D11ShaderResourceView* view, bool point_sample, bool opaque, int viewport_width,
                 int viewport_height) {
    const Pipeline& pipes = pipeline();
    if (!pipes.ok || target == nullptr) return;
    ID3D11RenderTargetView* targets[1] = {target};
    context_->OMSetRenderTargets(1, targets, nullptr);
    const D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(viewport_width),
                                  static_cast<float>(viewport_height), 0.0f, 1.0f};
    context_->RSSetViewports(1, &viewport);
    const D3D11_RECT scissor{0, 0, viewport_width, viewport_height};
    context_->RSSetScissorRects(1, &scissor);
    context_->RSSetState(pipes.raster_scissor);
    const float blend_factor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    context_->OMSetBlendState(opaque ? pipes.opaque : pipes.blend, blend_factor, 0xFFFFFFFFU);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context_->VSSetShader(pipes.vs, nullptr, 0);
    context_->PSSetShader(pipes.ps, nullptr, 0);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(pipes.constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    std::memcpy(mapped.pData, &params, sizeof(params));
    context_->Unmap(pipes.constants, 0);
    ID3D11Buffer* buffers[1] = {pipes.constants};
    context_->VSSetConstantBuffers(0, 1, buffers);
    context_->PSSetConstantBuffers(0, 1, buffers);
    ID3D11ShaderResourceView* views[1] = {view};
    context_->PSSetShaderResources(0, 1, views);
    ID3D11SamplerState* samplers[1] = {point_sample ? pipes.sampler_point : pipes.sampler_linear};
    context_->PSSetSamplers(0, 1, samplers);
    context_->Draw(4, 0);
  }

  auto acquire_shadow_targets(int width, int height, ShadowTargets& targets) -> bool {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    const auto build = [&](ID3D11Texture2D** texture, ID3D11RenderTargetView** rtv,
                           ID3D11ShaderResourceView** srv) -> bool {
      if (FAILED(device_->CreateTexture2D(&desc, nullptr, texture))) return false;
      if (FAILED(device_->CreateRenderTargetView(*texture, nullptr, rtv))) return false;
      if (FAILED(device_->CreateShaderResourceView(*texture, nullptr, srv))) return false;
      return true;
    };
    if (!build(&targets.texture_a, &targets.rtv_a, &targets.srv_a)) return false;
    return build(&targets.texture_b, &targets.rtv_b, &targets.srv_b);
  }

  static void release_shadow_targets(ShadowTargets& targets) {
    if (targets.srv_b != nullptr) targets.srv_b->Release();
    if (targets.rtv_b != nullptr) targets.rtv_b->Release();
    if (targets.texture_b != nullptr) targets.texture_b->Release();
    if (targets.srv_a != nullptr) targets.srv_a->Release();
    if (targets.rtv_a != nullptr) targets.rtv_a->Release();
    if (targets.texture_a != nullptr) targets.texture_a->Release();
    targets = ShadowTargets{};
  }

  // —— 纹理创建与缓存 ——

  /// R8 覆盖率纹理（字形/路径遮罩）。`cache_by_pointer` 适合指针稳定的源（字形位图）。
  /// `cache_key` 为**稳定身份**（0 = 没有身份，不做跨帧缓存——正确性优先）。
  /// 键里同时保留宽高：虽字号已进身份，但宽高是"内容是否真的一样"的直接判据。
  ///
  /// `CoverageFormat::Lcd` 时上传成 **R8G8B8A8** 纹理（A 写满）：一张纹理同时带 R/G/B
  /// 三个覆盖度，着色器采样一次拿全——这是亚像素在 GPU 上唯一划算的做法
  /// （三条平面纹理要多两次采样与三张缓存项）。
  auto mask_texture(const float* coverage, int width, int height, std::uint64_t cache_key,
                    CoverageFormat format) -> ID3D11ShaderResourceView* {
    const bool lcd = format == CoverageFormat::Lcd;
    if (cache_key != 0) {
      for (const auto& entry : mask_cache_) {
        if (entry.key == cache_key && entry.width == width && entry.height == height &&
            entry.lcd == lcd) {
          return entry.view;
        }
      }
    }
    const auto clamp_byte = [](float value) -> std::uint8_t {
      return static_cast<std::uint8_t>(
          std::lround(std::max(0.0f, std::min(1.0f, value)) * 255.0f));
    };
    const std::size_t pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::vector<std::uint8_t> bytes(pixels * (lcd ? 4U : 1U));
    if (lcd) {
      for (std::size_t index = 0; index < pixels; ++index) {
        bytes[index * 4U + 0U] = clamp_byte(coverage[index * 3U + 0U]);
        bytes[index * 4U + 1U] = clamp_byte(coverage[index * 3U + 1U]);
        bytes[index * 4U + 2U] = clamp_byte(coverage[index * 3U + 2U]);
        bytes[index * 4U + 3U] = 255U;  // α 不参与混合（着色器只读 .rgb）
      }
    } else {
      for (std::size_t index = 0; index < pixels; ++index) {
        bytes[index] = clamp_byte(coverage[index]);
      }
    }
    const DXGI_FORMAT texture_format = lcd ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R8_UNORM;
    const std::size_t row_pitch = lcd ? static_cast<std::size_t>(width) * 4U
                                      : static_cast<std::size_t>(width);
    ID3D11ShaderResourceView* view =
        create_texture(texture_format, width, height, bytes.data(), row_pitch);
    if (view != nullptr && cache_key != 0) {
      if (mask_cache_.size() >= kMaskCacheLimit) {
        // 简单淘汰：丢最早一项（字形集在工作集稳定后很少再换）
        mask_cache_.front().view->Release();
        mask_cache_.erase(mask_cache_.begin());
      }
      mask_cache_.push_back(MaskCacheEntry{cache_key, width, height, lcd, view});
    }
    return view;
  }

  /// 预乘 RGBA8 位图纹理（`draw_canvas` 的源）。
  auto bitmap_texture(std::span<const std::uint32_t> pixels, int width, int height)
      -> ID3D11ShaderResourceView* {
    if (pixels.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height)) {
      return nullptr;
    }
    std::vector<std::uint8_t> bytes(pixels.size() * 4U);
    for (std::size_t index = 0; index < pixels.size(); ++index) {
      const std::uint32_t pixel = pixels[index];
      bytes[index * 4U + 0U] = static_cast<std::uint8_t>((pixel >> 24U) & 0xFFU);
      bytes[index * 4U + 1U] = static_cast<std::uint8_t>((pixel >> 16U) & 0xFFU);
      bytes[index * 4U + 2U] = static_cast<std::uint8_t>((pixel >> 8U) & 0xFFU);
      bytes[index * 4U + 3U] = static_cast<std::uint8_t>(pixel & 0xFFU);
    }
    return create_texture(kFormat, width, height, bytes.data(),
                          static_cast<std::size_t>(width) * 4U);
  }

  /// 渐变采样色带（256×1，**预乘**）。
  ///
  /// 缓存键是色标序列的哈希：同一套主题里的渐变在帧间完全一致，
  /// 不缓存就是每帧重建 + 上传同样几条色带。
  auto ramp_texture(const Gradient& gradient) -> ID3D11ShaderResourceView* {
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&hash](std::uint64_t value) {
      hash ^= value;
      hash *= 1099511628211ULL;
    };
    mix(static_cast<std::uint64_t>(gradient.kind()));
    for (const GradientStop& stop : gradient.stops()) {
      mix(static_cast<std::uint64_t>(stop.offset * 10000.0f));
      mix((static_cast<std::uint64_t>(stop.color.r) << 24U) |
          (static_cast<std::uint64_t>(stop.color.g) << 16U) |
          (static_cast<std::uint64_t>(stop.color.b) << 8U) | stop.color.a);
    }
    for (const auto& entry : ramp_cache_) {
      if (entry.key == hash) return entry.view;
    }
    // 按色标插值出 256 级色带（与软件光栅器的渐变采样同一口径：线性插值 + 预乘）
    std::vector<std::uint8_t> bytes(256U * 4U);
    const std::span<const GradientStop> stops = gradient.stops();
    for (int index = 0; index < 256; ++index) {
      const float t = static_cast<float>(index) / 255.0f;
      math::Color color{0, 0, 0, 0};
      if (stops.empty()) {
        color = math::Color{0, 0, 0, 0};
      } else if (t <= stops.front().offset) {
        color = stops.front().color;
      } else if (t >= stops.back().offset) {
        color = stops.back().color;
      } else {
        for (std::size_t each = 1; each < stops.size(); ++each) {
          if (t > stops[each].offset) continue;
          const GradientStop& a = stops[each - 1U];
          const GradientStop& b = stops[each];
          const float span = b.offset - a.offset;
          const float local = span > 0.0f ? (t - a.offset) / span : 0.0f;
          color = a.color.mix(b.color, local);
          break;
        }
      }
      const auto premultiplied = math::premultiply(color);
      bytes[static_cast<std::size_t>(index) * 4U + 0U] =
          static_cast<std::uint8_t>((premultiplied >> 24U) & 0xFFU);
      bytes[static_cast<std::size_t>(index) * 4U + 1U] =
          static_cast<std::uint8_t>((premultiplied >> 16U) & 0xFFU);
      bytes[static_cast<std::size_t>(index) * 4U + 2U] =
          static_cast<std::uint8_t>((premultiplied >> 8U) & 0xFFU);
      bytes[static_cast<std::size_t>(index) * 4U + 3U] =
          static_cast<std::uint8_t>(premultiplied & 0xFFU);
    }
    ID3D11ShaderResourceView* view = create_texture(kFormat, 256, 1, bytes.data(), 256U * 4U);
    if (view != nullptr) {
      if (ramp_cache_.size() >= kRampCacheLimit) {
        ramp_cache_.front().view->Release();
        ramp_cache_.erase(ramp_cache_.begin());
      }
      ramp_cache_.push_back(RampCacheEntry{hash, view});
    }
    return view;
  }

  /// 上传一张纹理并返回视图（不需要 mipmap：自绘 2D 都是 1:1 贴图）。
  auto create_texture(DXGI_FORMAT format, int width, int height, const void* data,
                      std::size_t row_pitch) -> ID3D11ShaderResourceView* {
    if (width <= 0 || height <= 0 || data == nullptr) return nullptr;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = data;
    initial.SysMemPitch = static_cast<UINT>(row_pitch);
    ID3D11Texture2D* texture = nullptr;
    const HRESULT create = device_->CreateTexture2D(&desc, &initial, &texture);
    if (FAILED(create)) {
      // **失败必须可见**：纹理建不出来时上层会拿 nullptr 走去一条“什么都不画”的
      // 静默分支（例如字形就直接消失）。而那个现象（“内容凭空少了”）
      // 与“算错了”完全不同，没有这条日志就只能盲猜。
      if (std::getenv("ST_GPU_TEXTURE_DIAG") != nullptr) {
        ST_LOG_WARN("创建纹理失败：{}×{} fmt={} hr=0x{:08X} pitch={}", width, height,
                    static_cast<int>(format), static_cast<unsigned>(create), row_pitch);
      }
      return nullptr;
    }
    ID3D11ShaderResourceView* view = nullptr;
    const HRESULT result = device_->CreateShaderResourceView(texture, nullptr, &view);
    texture->Release();
    if (FAILED(result)) {
      if (std::getenv("ST_GPU_TEXTURE_DIAG") != nullptr) {
        ST_LOG_WARN("创建纹理视图失败：{}×{} fmt={} hr=0x{:08X}", width, height,
                    static_cast<int>(format), static_cast<unsigned>(result));
      }
      return nullptr;
    }
    return view;
  }

  /// 遮罩纹理缓存项。键是调用方给的**稳定身份**（见 `blend_coverage_bitmap` 的说明：
  /// 按指针缓存会被内存复用骗到），宽高作为内容判据一并保留。
  struct MaskCacheEntry {
    std::uint64_t key{0};
    int width{0};
    int height{0};
    /// 纹理是三通道（亚像素）还是单通道（灰度）：两种排布**绝不能互相顶替**。
    bool lcd{false};
    ID3D11ShaderResourceView* view{nullptr};
  };
  struct RampCacheEntry {
    std::uint64_t key{0};
    ID3D11ShaderResourceView* view{nullptr};
  };
  static constexpr std::size_t kMaskCacheLimit{512};
  static constexpr std::size_t kRampCacheLimit{64};
  static constexpr std::size_t kPathCacheLimit{256};
  static constexpr std::size_t kShadowCacheLimit{48};
  std::vector<PathCacheEntry> path_cache_{};
  std::vector<ShadowCacheEntry> shadow_cache_{};
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
    ST_LOG_WARN("GPU 渲染：{} 尚未实现（当前里程碑只落地设备层与清屏）", what);
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
      ST_LOG_ERROR("GPU 渲染：创建离屏渲染目标失败");
      return;
    }
    if (FAILED(device_->CreateRenderTargetView(target_, nullptr, &rtv_))) {
      ST_LOG_ERROR("GPU 渲染：创建渲染目标视图失败");
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

  /// 丢掉阴影缓存（目标纹理按尺寸分配，改尺寸后旧目标尺寸不对）。
  void release_shadow_cache() {
    for (auto& entry : shadow_cache_) release_shadow_targets(entry.targets);
    shadow_cache_.clear();
  }

  /// 从 GPU 拉回像素（`0xRRGGBBAA` 预乘，与软件画布同一布局）。
  auto ensure_readback() -> bool {
    if (target_ == nullptr) return false;
    // **回读前必须 flush 字形批次**：否则“本轮攒着还没发的字形”不会被画进目标，
    // 截图/像素断言看到的就会是“文字消失了”（而屏幕上其实是好的）。
    flush_glyph_batch();
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
        ST_LOG_ERROR("GPU 渲染：创建回读用暂存纹理失败");
        return false;
      }
    }
    context_->CopyResource(staging_, target_);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(staging_, 0, D3D11_MAP_READ, 0, &mapped))) {
      ST_LOG_ERROR("GPU 渲染：回读映射失败");
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
  std::vector<ClipFrame> clip_stack_{};
  std::vector<MaskCacheEntry> mask_cache_{};
  /// 两张图集（灰度/亚像素各自一张纹理，不能混用）与它们的纹理视图。
  atlas::GlyphAtlas gray_atlas_{512, 2048};
  atlas::GlyphAtlas lcd_atlas_{512, 2048};
  ID3D11ShaderResourceView* gray_atlas_view_{nullptr};
  ID3D11ShaderResourceView* lcd_atlas_view_{nullptr};
  /// 合批的顶点缓冲与容量。
  ID3D11Buffer* glyph_vertices_{nullptr};
  std::size_t glyph_vertex_capacity_{0};
  struct GlyphBatch {
    st::math::Color color{};
    float opacity{0.0f};
    bool lcd{false};
    bool active{false};
    std::vector<BatchVertex> vertices;
  };
  GlyphBatch batch_{};
  std::vector<RampCacheEntry> ramp_cache_{};
  bool readback_dirty_{true};
  int physical_width_{0};
  int physical_height_{0};
  float scale_{1.0f};
  PaintProfiler* profiler_{nullptr};
};

}  // namespace

auto available() noexcept -> bool { return holder().ok; }

/// D3D11 后端当前已落地的能力（随里程碑推进逐项翻真）。
///
/// 每一项都必须对应**已验证过**的实现：宁可报缺失（应用会回软件路径）
/// 也不能报“有”而画出错东西——后者会静默地毁掉一整帧。
// ————————————————————————————————————————————————————————————————————————————
// DXGI 呈现器：把画布纹理**零拷贝**送上屏
// ————————————————————————————————————————————————————————————————————————————

/// DXGI 工厂创建函数（同样动态取，不产生链接期依赖）。
using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);

/// 实现 `gpu::Presenter`：持有 swapchain，`present()` 用 `CopyResource` 把画布纹理
/// 拷进后备缓冲再 `Present`——**不经过 CPU**。
///
/// 为什么 `CopyResource` 而不是直接让画布画进后备缓冲：画布尺寸与窗口尺寸可能不同
/// （DPI 变化、窗口 resize 的瞬间），且画布还要能被控制通道截图与测试读回；
/// 保持"画布是权威副本"这条不变量，代价只是一次 GPU 内拷贝（微秒级）。
class D3dPresenter final : public Presenter {
 public:
  D3dPresenter() = default;

  ~D3dPresenter() override {
    if (backbuffer_ != nullptr) backbuffer_->Release();
    if (swapchain_ != nullptr) swapchain_->Release();
    if (factory_ != nullptr) factory_->Release();
  }

  auto initialize(ID3D11Device* device, HWND window, int width, int height) -> Status {
    device_ = device;
    window_ = window;
    const Modules& mods = modules();
    if (mods.dxgi == nullptr) {
      return unexpected(ErrorCode::Unsupported, "找不到 dxgi.dll");
    }
    const auto create_factory = reinterpret_cast<CreateFactoryFn>(
        reinterpret_cast<void*>(::GetProcAddress(mods.dxgi, "CreateDXGIFactory1")));
    if (create_factory == nullptr) {
      return unexpected(ErrorCode::Unsupported, "dxgi 中找不到 CreateDXGIFactory1");
    }
    if (FAILED(create_factory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory_))) ||
        factory_ == nullptr) {
      return unexpected(ErrorCode::Io, "CreateDXGIFactory1 失败");
    }
    return resize(width, height);
  }

  auto resize(int width, int height) -> Status override {
    if (factory_ == nullptr || window_ == nullptr) {
      return unexpected(ErrorCode::Invalid, "呈现器未初始化");
    }
    if (width <= 0 || height <= 0) return unexpected(ErrorCode::Invalid, "呈现尺寸非法");
    if (swapchain_ != nullptr && width == width_ && height == height_) return ok();

    if (backbuffer_ != nullptr) {
      backbuffer_->Release();
      backbuffer_ = nullptr;
    }
    // 已有 swapchain：只 resize 后备缓冲（重建 swapchain 会让窗口闪一下）
    if (swapchain_ != nullptr) {
      const HRESULT result = swapchain_->ResizeBuffers(0, static_cast<UINT>(width),
                                                       static_cast<UINT>(height),
                                                       DXGI_FORMAT_UNKNOWN, 0);
      if (SUCCEEDED(result)) {
        width_ = width;
        height_ = height;
        return acquire_backbuffer();
      }
      // resize 失败（例如窗口最小化）：丢掉重建
      swapchain_->Release();
      swapchain_ = nullptr;
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 2;
    desc.BufferDesc.Width = static_cast<UINT>(width);
    desc.BufferDesc.Height = static_cast<UINT>(height);
    desc.BufferDesc.Format = kFormat;
    desc.BufferDesc.RefreshRate.Numerator = 0;  // 0 = 用窗口自带的刷新率
    desc.BufferDesc.RefreshRate.Denominator = 0;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window_;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;  // 最兼容；不需要保留后备内容
    desc.Flags = 0;
    const HRESULT result = factory_->CreateSwapChain(
        device_, &desc, reinterpret_cast<IDXGISwapChain**>(&swapchain_));
    if (FAILED(result) || swapchain_ == nullptr) {
      swapchain_ = nullptr;
      return unexpected(ErrorCode::Io, std::format("CreateSwapChain 失败（0x{:08X}）",
                                                   static_cast<unsigned>(result)));
    }
    // 别让 DXGI 截 Alt+Enter（窗口行为由我们自己管）
    (void)factory_->MakeWindowAssociation(window_, DXGI_MWA_NO_ALT_ENTER);
    width_ = width;
    height_ = height;
    return acquire_backbuffer();
  }

  auto present(Surface& canvas, int width, int height) -> Status override {
    if (swapchain_ == nullptr) return unexpected(ErrorCode::Invalid, "呈现器未就绪");
    // 尺寸不匹配时**不再让 DXGI 自己拉**（那块内容会被非等比拉伸 → 文字/边框全变形）。
    //
    // 正常路径上画布与后备缓冲是同步的（画布 `resize` 之后紧接着 `ResizeBuffers`），
    // 走不到这里。能走到只有两种情况：
    // - 画布 `resize` 失败（内存不够/设备丢失）→ 后备缓冲仍是旧尺寸；
    // - DPI 变化那一帧的时序缝隙。
    // 这两种情况下"拉伸一帧"比"黑一帧"更难排查（用户看到的是整个界面变形，
    // 而不是一块空白），所以这里失败就**如实报错**，由上层落回 GDI blit。
    if (width != width_ || height != height_) {
      if (auto resized = resize(width, height); !resized) return resized;
      if (backbuffer_ == nullptr) {
        return unexpected(ErrorCode::Invalid, "后备缓冲缺失");
      }
    }
    auto* canvas_at = dynamic_cast<GpuCanvas*>(&canvas);
    if (canvas_at == nullptr || canvas_at->target() == nullptr) {
      return unexpected(ErrorCode::Invalid, "present 失败：画布与后备缓冲尺寸不一致");
    }
    if (canvas_at->physical_width() != width_ || canvas_at->physical_height() != height_) {
      return unexpected(ErrorCode::Invalid,
                        std::format("present 失败：画布 {}x{} 与后备缓冲 {}x{} 不一致",
                                    canvas_at->physical_width(), canvas_at->physical_height(),
                                    width_, height_));
    }
    // 画布必须是本进程的 GPU 画布：不同设备之间无法直接拷贝纹理
    auto* gpu_canvas = canvas_at;
    if (backbuffer_ == nullptr) {
      if (auto acquired = acquire_backbuffer(); !acquired) return acquired;
    }
    ID3D11DeviceContext* context = holder().context_or_null();
    if (context == nullptr) return unexpected(ErrorCode::Io, "没有 D3D11 上下文");
    // 关键一步：GPU → GPU 拷贝，**不经过 CPU**（这就是 M5 的全部价值）
    context->CopyResource(backbuffer_, gpu_canvas->target());
    const HRESULT result = swapchain_->Present(0, 0);
    if (FAILED(result)) {
      return unexpected(ErrorCode::Io, std::format("Present 失败（0x{:08X}）",
                                                   static_cast<unsigned>(result)));
    }
    return ok();
  }

  [[nodiscard]] auto note() const -> std::string override {
    return std::format("DXGI swapchain {}×{} · 双缓冲 · {}",
                       width_, height_, present_count_ > 0 ? "已上屏" : "待上屏");
  }

 private:
  auto acquire_backbuffer() -> Status {
    const HRESULT result = swapchain_->GetBuffer(
        0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backbuffer_));
    if (FAILED(result) || backbuffer_ == nullptr) {
      backbuffer_ = nullptr;
      return unexpected(ErrorCode::Io, "取 swapchain 后备缓冲失败");
    }
    return ok();
  }

  ID3D11Device* device_{nullptr};  ///< 非拥有（来自进程级 holder）
  IDXGIFactory1* factory_{nullptr};
  IDXGISwapChain* swapchain_{nullptr};
  ID3D11Texture2D* backbuffer_{nullptr};
  HWND window_{nullptr};
  int width_{0};
  int height_{0};
  std::uint32_t present_count_{0};
};

auto create_presenter(void* native_window, int width, int height)
    -> Result<std::unique_ptr<Presenter>> {
  const DeviceHolder& holder_ref = holder();
  if (!holder_ref.ok || holder_ref.device_or_null() == nullptr) {
    return unexpected(ErrorCode::Unsupported, "没有可用的 D3D11 设备");
  }
  if (native_window == nullptr) return unexpected(ErrorCode::Invalid, "窗口句柄为空");
  auto presenter = std::make_unique<D3dPresenter>();
  if (auto status = presenter->initialize(holder_ref.device_or_null(),
                                          static_cast<HWND>(native_window), width, height);
      !status) {
    return unexpected(status.error().code, status.error().message);
  }
  return std::unique_ptr<Presenter>(std::move(presenter));
}

auto capabilities() -> Capabilities {
  Capabilities caps;
  // **能力声明以管线真的建起来为前提**：着色器编译失败（HLSL 写错是运行期才发现的那种）
  // 却声称一切可用，上层会选中 GPU 然后画出一片空白——而“什么都不画”看起来还特别快。
  const Pipeline& pipes = pipeline();
  if (!pipes.ok) return caps;
  caps.solid_shapes = true;
  caps.gradients = true;
  caps.coverage_masks = true;
  caps.bitmaps = true;
  caps.clips = true;
  caps.shadows = true;  // M3b：多遍可分离盒式模糊 + 遮罩缓存
  caps.paths = true;    // M4：CPU 覆盖率光栅化（同一套 rasterize_mask）+ 遮罩缓存 + GPU 合成
  caps.lcd_text = true;  // 亚像素文字：RGB 覆盖率纹理 + 两遍混合（见 blend_coverage_bitmap）
  return caps;
}

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
  // 管线建不起来（着色器编译失败/固定状态创建失败）就在这里担下来：
  // 建一个“能建但画不出东西”的画布，会把问题变成“界面一片空白”的谜案。
  const Pipeline& pipes = pipeline();
  if (!pipes.ok) {
    return unexpected(ErrorCode::Unsupported,
                      pipes.error.empty() ? std::string("GPU 渲染管线不可用")
                                          : std::format("GPU 渲染管线不可用：{}", pipes.error));
  }
  auto canvas = std::make_unique<GpuCanvas>(device.device_or_null(), device.context_or_null(),
                                            physical_width, physical_height, device_scale);
  return std::unique_ptr<Surface>(std::move(canvas));
}

auto live_canvas_count() noexcept -> std::uint32_t { return g_live_canvases.load(); }

// ————————————————————————————————————————————————————————————————————————————
// 三维的 **GPU 腿**（`Scene3D` 的 D3D11 实现）
// ————————————————————————————————————————————————————————————————————————————
//
// 写在同一编译单元的理由：D3D11 的设备与上下文由本文件的 `DeviceHolder` 持有
// （进程级共享），着色器编译入口、`Modules` 动态加载、`ST_GPU_FORCE_WARP`
// 降级也都在这。另开一个文件就得把那些全部暴露成对外接口或再写一遍——
// 前者泄漏实现，后者必然漂移（本仓已为"同一件事两条路径"付过多次代价）。
// CONVENTIONS §10 的限制是「系统头只能出现在 `platform_*` 文件」，本文件满足。
//
// ## 为何之前只有软件腿、现在才加 GPU 腿
//
// `DESIGN.md` §8.4.2 把这条腿的触发条件定在**测量**上：三维此前只用于界面里一块
// 960×200 的卡片（软件腿个位数毫秒），写 GPU 实现属投机。现在裁云要把三维当
// **画面主体**（半屏到整屏、1080p 逐帧导出）——前提变了，结论就要重算。
//
// ## 两条腿怎么选，不靠"有显卡就用"
//
// GPU 优先，但**不静默假设它更快**：小尺寸下每帧一次纹理合成可能比软件腿逐像素算
// 更贵。所以走运行时实测（`raster::create_scene3d`），与二维 `create_surface`
// 同一套立场；不可用则如实回退并记原因（不静默）。

/// 三维着色器：顶点仅位置+法线+颜色，PS 与 `raster::shade_terms` **逐字对应**。
///
/// 为何不把光照做成可插拔：现在只有「Lambert + 环境 + 边缘光」一种模型，参数
/// （`Lighting`）已覆盖打光变化。多一套 BRDF 就要多一套两腿一致性测试——
/// 那属于"先有真实需求再写"。
constexpr const char* kScene3dShader = R"hlsl(
cbuffer Scene : register(b0) {
  float4x4 g_mvp;
  float4x4 g_model;
  float4 g_light;      // xyz = 主光方向（指向光源），w = 环境项
  float4 g_tune;       // x = 漫反射权重, y = 边缘光, z = 曝光, w = 未用
};

struct VsIn {
  float3 position : POSITION;
  float3 normal : NORMAL;
  float3 color : COLOR;
  float2 uv : TEXCOORD0;
};

struct VsOut {
  float4 position : SV_Position;
  float3 normal : TEXCOORD0;
  float3 color : TEXCOORD1;
  float2 uv : TEXCOORD2;
};

// 纹理（RGBA8，非预乘）：无 UV 的网格不采它，行为与以前完全一致
Texture2D g_scene_texture : register(t0);
SamplerState g_scene_sampler : register(s0);

VsOut vs_scene3d(VsIn input) {
  VsOut o;
  // 矩阵两边约定**恰好一致**：`math::Mat4` 是列主序（m[column*4+row]），
  // D3D 常数缓冲默认按列读——**不需要转置**。转置一次就对、两次也"看着对"
  // （整个场景绕中心镜像），只有拿参照图比才看得出。
  o.position = mul(g_mvp, float4(input.position, 1.0));
  // 法线用模型矩阵的 3x3（与软件腿一致：不取逆转置——本仓的模型矩阵只有
  // 旋转/等比缩放/平移，逆转置与直接变换只差一个标量因子，归一化后相同）
  o.normal = mul((float3x3)g_model, input.normal);
  o.color = input.color;
  o.uv = input.uv;
  return o;
}

float4 ps_scene3d(VsOut input) : SV_Target {
  // ⚠ 本段是 `st::raster::shade_terms` 的**逐字翻译**。改一边必须改另一边
  //   （回归：`tests/raster_scene3d_two_legs_test.cpp` 比两条腿的同像素输出）。
  float3 n = normalize(input.normal);
  float3 light = normalize(g_light.xyz);
  float diffuse = max(dot(n, light), 0.0);
  float rim_base = 1.0 - max(n.z, 0.0);
  float rim = rim_base * rim_base * g_tune.y;
  // 边缘光乘在固有色上（不是加到最终色上）——详见 C++ 侧 `shade_terms` 的说明：
  // 加法版本会把侧壁抬到比顶面还亮，看着像“盒子贴了一片白边”。
  float scale = (g_light.w + diffuse * g_tune.x + rim) * g_tune.z;
  // g_tune.w > 0.5 = 本网格带 UV 且已设纹理 → 采样并与顶点色相乘
  // （与软件腿同一口径：顶点色当基础色/染色，纹理提供图案）
  float3 base = input.color;
  if (g_tune.w > 0.5) {
    base = input.color * g_scene_texture.Sample(g_scene_sampler, input.uv).rgb;
  }
  float3 lit = saturate(base * scale);
  return float4(lit, 1.0);   // 预乘 alpha=1（不透明），与画布内部布局一致
}
)hlsl";

/// 与 HLSL 的 cbuffer 逐字节对应。
struct Scene3dParams {
  std::array<float, 16> mvp{};
  std::array<float, 16> model{};
  std::array<float, 4> light{};
  std::array<float, 4> tune{};
};
static_assert(sizeof(Scene3dParams) % 16 == 0, "常量缓冲必须是 16 字节的倍数");

/// 三维管线（**每个渲染线程一套**；懒创建）。
///
/// 为何是线程局部而不是设备级单例：D3D11 的**立即上下文不是线程安全的**，
/// 而裁云导出按多路并行渲染（每线程一块画布）。共享一份管线 = 多线程同时
/// `Map`/`Draw` 同一个上下文 = 未定义行为（症状是偶发画面错乱或驱动崩溃，
/// 而不是稳定报错）。设备（ID3D11Device）本身线程安全，可共享。
/// 代价是每线程几百字节状态对象，相对每线程已有的画布/读回缓冲可忽略。
struct Scene3dPipeline {
  ID3D11VertexShader* vs{nullptr};
  ID3D11PixelShader* ps{nullptr};
  ID3D11InputLayout* layout{nullptr};
  ID3D11Buffer* constants{nullptr};
  ID3D11DepthStencilState* depth{nullptr};
  ID3D11RasterizerState* raster{nullptr};
  ID3D11SamplerState* sampler{nullptr};
  std::string error{};
  bool ok{false};

  explicit Scene3dPipeline(ID3D11Device* device) {
    if (device == nullptr) {
      error = "没有 D3D11 设备";
      return;
    }
    const Modules& mods = modules();
    if (mods.d3dcompiler == nullptr) {
      error = "找不到 d3dcompiler（无法编译三维着色器）";
      return;
    }
    using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*,
                                       ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**,
                                       ID3DBlob**);
    const auto compile = reinterpret_cast<CompileFn>(
        reinterpret_cast<void*>(::GetProcAddress(mods.d3dcompiler, "D3DCompile")));
    if (compile == nullptr) {
      error = "d3dcompiler 中找不到 D3DCompile";
      return;
    }
    ID3DBlob* vs_blob = nullptr;
    ID3DBlob* ps_blob = nullptr;
    ID3DBlob* errors = nullptr;
    const auto build = [&](const char* entry, const char* target, ID3DBlob** out) -> bool {
      const HRESULT result =
          compile(kScene3dShader, std::strlen(kScene3dShader), "st_scene3d", nullptr, nullptr,
                  entry, target, 0, 0, out, &errors);
      if (FAILED(result)) {
        error = errors != nullptr
                    ? std::string(static_cast<const char*>(errors->GetBufferPointer()))
                    : std::format("编译 {} 失败（0x{:08X}）", entry, static_cast<unsigned>(result));
        if (errors != nullptr) {
          errors->Release();
          errors = nullptr;
        }
        return false;
      }
      return true;
    };
    if (!build("vs_scene3d", "vs_4_0", &vs_blob) || !build("ps_scene3d", "ps_4_0", &ps_blob)) {
      if (vs_blob != nullptr) vs_blob->Release();
      if (ps_blob != nullptr) ps_blob->Release();
      return;
    }
    const HRESULT vs_result = device->CreateVertexShader(vs_blob->GetBufferPointer(),
                                                        vs_blob->GetBufferSize(), nullptr, &vs);
    const HRESULT ps_result = device->CreatePixelShader(ps_blob->GetBufferPointer(),
                                                       ps_blob->GetBufferSize(), nullptr, &ps);
    // 输入布局：位置/法线/颜色 + **可选 UV**。
    //
    // UV 作为第 4 个元素，而顶点缓冲的 stride 仍然按 `Mesh::kVertexFloats`（9）算：
    // UV 存在**第二张顶点缓冲**（单独一个 stream）而不是塞进交错布局。
    // 为何这样做：`kVertexFloats = 9` 是生成器与后端的契约，改 stride 要动
    // 所有生成器与 OBJ 加载；用独立 stream 则“有 UV 才传第二个流、没有就不传”。
    const D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 1, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    const HRESULT layout_result =
        device->CreateInputLayout(elements, 4, vs_blob->GetBufferPointer(),
                                 vs_blob->GetBufferSize(), &layout);
    vs_blob->Release();
    ps_blob->Release();
    if (FAILED(vs_result) || FAILED(ps_result) || FAILED(layout_result)) {
      error = std::format("创建三维着色器/输入布局失败（vs=0x{:08X} ps=0x{:08X} layout=0x{:08X}）",
                          static_cast<unsigned>(vs_result), static_cast<unsigned>(ps_result),
                          static_cast<unsigned>(layout_result));
      return;
    }
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = sizeof(Scene3dParams);
    buffer.Usage = D3D11_USAGE_DYNAMIC;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&buffer, nullptr, &constants))) {
      error = "创建三维常量缓冲失败";
      return;
    }
    // 深度：**开深度测试 + 写深度**，比较函数 LESS。
    //
    // 三维与二维的分界线就在这里：没有深度测试，后画的三角形会盖住前面的，
    // 模型会"从内部翻出来"（`raster_software_scene_test` 里用两个前后错开的
    // 立方体测的正是这条）。
    D3D11_DEPTH_STENCIL_DESC depth_desc{};
    depth_desc.DepthEnable = TRUE;
    depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth_desc.DepthFunc = D3D11_COMPARISON_LESS;
    depth_desc.StencilEnable = FALSE;
    if (FAILED(device->CreateDepthStencilState(&depth_desc, &depth))) {
      error = "创建深度状态失败";
      return;
    }
    D3D11_RASTERIZER_DESC raster_desc{};
    raster_desc.FillMode = D3D11_FILL_SOLID;
    raster_desc.CullMode = D3D11_CULL_NONE;   // 与软件腿一致：模型可能未闭合/法线朝内
    raster_desc.DepthClipEnable = TRUE;
    raster_desc.ScissorEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&raster_desc, &raster))) {
      error = "创建光栅化状态失败";
      return;
    }
    // 纹理采样器：**线性**（与软件腿的双线性同口径），clamp 寻址
    //（边缘 clamp 而不是 wrap：图标边缘不应该循环到对面颜色）。
    D3D11_SAMPLER_DESC sampler_desc{};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&sampler_desc, &sampler))) {
      error = "创建三维纹理采样器失败";
      return;
    }
    ok = true;
  }

  Scene3dPipeline(const Scene3dPipeline&) = delete;
  auto operator=(const Scene3dPipeline&) -> Scene3dPipeline& = delete;

  ~Scene3dPipeline() {
    if (sampler != nullptr) sampler->Release();
    if (raster != nullptr) raster->Release();
    if (depth != nullptr) depth->Release();
    if (constants != nullptr) constants->Release();
    if (layout != nullptr) layout->Release();
    if (ps != nullptr) ps->Release();
    if (vs != nullptr) vs->Release();
  }
};

[[nodiscard]] auto scene3d_pipeline() -> const Scene3dPipeline& {
  static thread_local Scene3dPipeline instance(holder().device_or_null());
  return instance;
}

/// 一个网格在 GPU 上的缓冲（顶点 + 索引 + **可选 UV**）。
struct MeshBuffers {
  ID3D11Buffer* vertices{nullptr};
  ID3D11Buffer* indices{nullptr};
  /// UV 顶点缓冲（独立 stream 1）：网格无 UV 时为 null。
  ID3D11Buffer* uvs{nullptr};
  UINT index_count{0};

  MeshBuffers() = default;
  MeshBuffers(const MeshBuffers&) = delete;
  auto operator=(const MeshBuffers&) -> MeshBuffers& = delete;
  MeshBuffers(MeshBuffers&& other) noexcept
      : vertices(other.vertices), indices(other.indices), uvs(other.uvs),
        index_count(other.index_count) {
    other.vertices = nullptr;
    other.indices = nullptr;
    other.uvs = nullptr;
    other.index_count = 0;
  }
  auto operator=(MeshBuffers&& other) noexcept -> MeshBuffers& {
    if (this == &other) return *this;
    release();
    vertices = other.vertices;
    indices = other.indices;
    uvs = other.uvs;
    index_count = other.index_count;
    other.vertices = nullptr;
    other.indices = nullptr;
    other.uvs = nullptr;
    other.index_count = 0;
    return *this;
  }
  ~MeshBuffers() { release(); }

  void release() {
    if (vertices != nullptr) {
      vertices->Release();
      vertices = nullptr;
    }
    if (indices != nullptr) {
      indices->Release();
      indices = nullptr;
    }
    if (uvs != nullptr) {
      uvs->Release();
      uvs = nullptr;
    }
    index_count = 0;
  }
};

/// 网格内容哈希：同一份顶点/索引数据只上传一次。
///
/// 为何不按 `Mesh*` 指针做键：裁云的用法是"每帧按脚本重建 Mesh"（数据驱动、
/// 无状态），指针每帧都变、缓存永不命中——那等于每帧重传全部顶点。
/// 内容哈希让"同一几何每帧重算"也命中；键含长度，不会错拿另一份数据。
[[nodiscard]] auto mesh_key(const st::raster::Mesh& mesh) noexcept -> std::uint64_t {
  std::uint64_t hash = 1469598103934665603ULL;   // FNV-1a 64
  const auto mix = [&hash](const void* data, std::size_t bytes) {
    const auto* bytes_ptr = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < bytes; ++i) {
      hash ^= static_cast<std::uint64_t>(bytes_ptr[i]);
      hash *= 1099511628211ULL;
    }
  };
  mix(mesh.vertices.data(), mesh.vertices.size() * sizeof(float));
  mix(mesh.indices.data(), mesh.indices.size() * sizeof(std::uint32_t));
  return hash;
}

/// GPU 三维场景：颜色 + 深度目标、网格缓冲缓存、结果回读。
///
/// ## 为何每个碰上下文的调用都要拿一把**进程级互斥锁**
///
/// 实测崩溃（0xC0000005）：裁云导出开多路并行，每线程一个本类实例，
/// 而它们**共用同一个立即上下文**（`ID3D11DeviceContext` 是 `DeviceHolder` 里
/// 进程级单例：设备本身线程安全、**上下文不是**）。多线程同时 `Map`/`DrawIndexed`
/// 同一上下文 = 未定义行为。
///
/// ## 为何是**逐调用**加锁，而不是“整帧持有”（踩过，必看）
///
/// 第一版在 `begin_frame` 拿锁、在 `end_frame` 放锁。看起来很自然（一帧是一个
/// 原子单元），但它有一个致命的失效模式：**调用方没调 `end_frame`（或提前 return、
/// 抛错、被跳过）→ 锁永不释放 → 下一个线程永久阻塞。**
/// 实测表现是“测试/导出卡住不动”，而不是报错——比崩溃难查得多。
/// （各人写渲染循环时忘了配对再正常不过：本仓自己的测试就有“只 begin 不 end”的用例。）
/// ，
/// 逐调用加锁的代价：每次多一次 lock/unlock（无竞争时几个纳秒），而每个帧只有
/// 3~4 次带上下文的调用（清屏、设状态一遍、DrawIndexed、回读），成本可忽略。
/// 换来的保证是：**任何单一调用都不会把别人卡死**，即使调用方完全不配对。
///
/// 真正的原子性需求（渲染目标/深度视图/清屏不能交错）由**接口的用法**保证：
/// 每个场景实例有自己的颜色+深度目标，`begin_frame` 只能清自己的；
/// 不同实例之间唯一共享的是着色器管线与上下文，而那几个调用各自原子即可。
/// 若将来真需要跨实例的帧级隔离，正解是每线程延迟上下文（`CreateDeferredContext`），
/// 而不是在这里抱一把锁。
class GpuScene3d final : public st::raster::Scene3D {
 public:
  GpuScene3d(ID3D11Device* device, ID3D11DeviceContext* context, int width, int height)
      : device_(device), context_(context), width_(std::max(1, width)),
        height_(std::max(1, height)) {
    ok_ = create_targets();
  }

  ~GpuScene3d() override { release_targets(); }

  GpuScene3d(const GpuScene3d&) = delete;
  auto operator=(const GpuScene3d&) -> GpuScene3d& = delete;

  void begin_frame(math::Color clear_color) override {
    if (rtv_ == nullptr || dsv_ == nullptr || !ok_) return;
    // 逐调用锁（见类注释：帧级持有会因“没调 `end_frame`”而永久阻塞）。
    const std::scoped_lock lock(g_context_mutex());
    // 清屏**按预乘色**下传（与画布内部布局一致）：透明背景是三维层的真实需求——
    // 把模型叠在视频背景上时，三维区域的空白必须透出下层，而不是一块黑。
    const float alpha = static_cast<float>(clear_color.a) / 255.0f;
    const float values[4] = {static_cast<float>(clear_color.r) / 255.0f * alpha,
                             static_cast<float>(clear_color.g) / 255.0f * alpha,
                             static_cast<float>(clear_color.b) / 255.0f * alpha, alpha};
    context_->ClearRenderTargetView(rtv_, values);
    context_->ClearDepthStencilView(dsv_, D3D11_CLEAR_DEPTH, 1.0f, 0);
    mvp_ready_ = false;
    lighting_ = st::raster::Lighting{};
    draw_calls_ = 0;
  }

  void set_camera(const st::raster::Camera& camera) override {
    const float aspect = height_ > 0 ? static_cast<float>(width_) / static_cast<float>(height_)
                                     : 1.0f;
    const float fov = camera.fov_y_degrees * 3.14159265358979f / 180.0f;
    view_ = math::Mat4::look_at(camera.eye, camera.target, camera.up);
    projection_ = math::Mat4::perspective(fov, aspect, camera.near_z, camera.far_z);
    mvp_ready_ = true;
  }

  void set_lighting(const st::raster::Lighting& lighting) override { lighting_ = lighting; }

  void set_texture(std::span<const std::uint8_t> pixels, int width, int height) override {
    const std::scoped_lock lock(g_context_mutex());
    upload_texture(pixels, width, height);
  }

  void draw_mesh(const st::raster::Mesh& mesh, const math::Mat4& model) override {
    if (!ok_ || !mvp_ready_ || mesh.is_empty()) return;
    const Scene3dPipeline& pipes = scene3d_pipeline();
    if (!pipes.ok) return;
    const MeshBuffers* buffers = buffers_for(mesh);
    if (buffers == nullptr) return;

    Scene3dParams params{};
    const math::Mat4 mvp = projection_ * view_ * model;
    for (int column = 0; column < 4; ++column) {
      for (int row = 0; row < 4; ++row) {
        const auto index =
            static_cast<std::size_t>(column) * 4U + static_cast<std::size_t>(row);
        params.mvp[index] = mvp.value(row, column);
        params.model[index] = model.value(row, column);
      }
    }
    const math::Vec3 light = lighting_.key_direction.normalized();
    params.light = {light.x, light.y, light.z, lighting_.ambient};
    // g_tune.w：本网格是否采样纹理（1 = 采）
    const bool use_texture = texture_view_ != nullptr && buffers->uvs != nullptr;
    params.tune = {lighting_.diffuse, lighting_.rim, lighting_.exposure,
                   use_texture ? 1.0f : 0.0f};

    // 从这一行到 DrawIndexed 全部在锁内：常景缓冲 Map、状态设置、绘制
    // 都是上下文操作，不能与别的线程交错。
    const std::scoped_lock lock(g_context_mutex());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(pipes.constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    std::memcpy(mapped.pData, &params, sizeof(params));
    context_->Unmap(pipes.constants, 0);

    ID3D11RenderTargetView* targets[1] = {rtv_};
    context_->OMSetRenderTargets(1, targets, dsv_);
    const D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width_),
                                  static_cast<float>(height_), 0.0f, 1.0f};
    context_->RSSetViewports(1, &viewport);
    const D3D11_RECT scissor{0, 0, width_, height_};
    context_->RSSetScissorRects(1, &scissor);
    context_->RSSetState(pipes.raster);
    context_->OMSetDepthStencilState(pipes.depth, 0);
    // 三维写不透明值（自身已着色），不走 src-over；与深度一起保证遮挡正确
    context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFU);
    context_->IASetInputLayout(pipes.layout);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const UINT stride = static_cast<UINT>(st::raster::Mesh::kVertexFloats * sizeof(float));
    const UINT offset = 0;
    ID3D11Buffer* vertex_buffer = buffers->vertices;
    context_->IASetVertexBuffers(0, 1, &vertex_buffer, &stride, &offset);
    // UV：独立 stream 1（无 UV 时 stride 仍要非零——D3D 不接 stride=0，
    // 否则整个 draw 静默失败；给个 8 字节占位但不清缓冲也不会被读，因为
    // 着色器只靠 `g_tune.w` 判断是否采样，而无 UV 时 buffe 为 null → 不绑）。
    if (buffers->uvs != nullptr) {
      const UINT uv_stride = static_cast<UINT>(2U * sizeof(float));
      ID3D11Buffer* uv_buffer = buffers->uvs;
      context_->IASetVertexBuffers(1, 1, &uv_buffer, &uv_stride, &offset);
      // 纹理 + 采样器
      ID3D11ShaderResourceView* views[1] = {texture_view_};
      context_->PSSetShaderResources(0, 1, views);
      ID3D11SamplerState* samplers[1] = {pipes.sampler};
      context_->PSSetSamplers(0, 1, samplers);
    }
    context_->IASetIndexBuffer(buffers->indices, DXGI_FORMAT_R32_UINT, 0);
    context_->VSSetShader(pipes.vs, nullptr, 0);
    context_->PSSetShader(pipes.ps, nullptr, 0);
    ID3D11Buffer* constants[1] = {pipes.constants};
    context_->VSSetConstantBuffers(0, 1, constants);
    context_->PSSetConstantBuffers(0, 1, constants);
    context_->DrawIndexed(buffers->index_count, 0, 0);
    ++draw_calls_;
  }

  void end_frame(st::raster::Surface& target, math::Rect destination,
                 float opacity = 1.0f) override {
    if (srv_ == nullptr || !ok_) return;
    // MSAA 必须先 resolve（MSAA 纹理不能直接当 shader 资源采样）。
    // 这一步同时修掉了“轮廓锯启”：resolve 就是硬件做的面积平均。
    resolve_if_needed();
    // ① **零拷贝快路**：目标是同后端 GPU 画布 → 纹理直接采样合成，不离开显存。
    //
    // 为何这一条是整个设计的成败点（实测）：三维光栅只要 0.06 ms，而回读 + 上传
    // 要 100 ms（1080p）——不想办法消掉它，“用显卡画三维”就是个负优化。
    //
    // `composite_native_texture` 内部会拿同一把锁（它的 `submit` 也碰上下文），
    // 所以这里**不能再持锁调它**（`std::mutex` 不可重入）。
    st::raster::DrawOptions options{};
    options.opacity = opacity;
    const st::raster::SceneTexture native = texture();
    st::raster::Surface::NativeTexture as_surface{native.handle, native.backend, native.width,
                                                  native.height};
    if (target.composite_native_texture(as_surface, destination, options)) return;
    // ② **回退路**：目标不是同后端（软件画布）→ 只能回读成像素。
    // 这条路上显卡的收益会被传输吃掉，但它保证任何目标都能拿到画面。
    ensure_readback();
    if (readback_.empty()) return;
    st::raster::Canvas view{width_, height_, 1.0f};
    const std::span<std::uint32_t> pixels = view.pixels();
    std::copy(readback_.begin(), readback_.end(), pixels.begin());
    target.draw_canvas(view, destination, options);
  }

  [[nodiscard]] auto width() const noexcept -> int override { return width_; }
  [[nodiscard]] auto height() const noexcept -> int override { return height_; }
  [[nodiscard]] auto draw_calls() const noexcept -> std::uint64_t override { return draw_calls_; }
  [[nodiscard]] auto backend() const -> std::string override { return "gpu"; }

  /// 把三维结果以**原生纹理**交出去（不读回）。调用方（`Surface`）若是同后端
  /// 就能零拷贝采样它；否则它会自己回退到 `draw_canvas`（即走 `end_frame`）。
  [[nodiscard]] auto texture() const noexcept -> st::raster::SceneTexture override {
    return st::raster::SceneTexture{srv_, "d3d11", width_, height_};
  }

  /// 阻塞到 GPU 把已提交的绘制跑完。
  ///
  /// 为何接口上需要它：GPU 腿的 `draw_mesh` 只是把命令塞进缓冲就返回，真光栅
  /// 异步在显卡上发生——不等待就计时，量到的是 CPU 侧 API 开销（实测会得出
  /// “GPU 光栅 0.03 ms”这种假数据）。
  ///
  /// 它同样碰上下文，所以整段在锁内（与其它调用同一把）。
  void flush() override {
    if (context_ == nullptr) return;
    const std::scoped_lock lock(g_context_mutex());
    if (query_ == nullptr) {
      D3D11_QUERY_DESC desc{};
      desc.Query = D3D11_QUERY_EVENT;
      if (FAILED(device_->CreateQuery(&desc, &query_))) {
        context_->Flush();
        return;
      }
    }
    context_->End(query_);
    BOOL done = FALSE;
    // 有界等待：驱动异常时不死等（截图/导出卡死比量错更难查）。
    for (int spin = 0; spin < 100000; ++spin) {
      if (context_->GetData(query_, &done, sizeof(done), 0) == S_OK) return;
      ::Sleep(0);
    }
    context_->Flush();
  }

 private:
  /// 建颜色/深度目标（带 MSAA）。
  ///
  /// ## 为何要 MSAA（实测）
  ///
  /// 单采样下三维模型的斜边是一排排像素阶梯，成片上一眼就能看到；
  /// 而二维那边有扫描线覆盖率抗锯齿，两者放在一起反差更刺眼（文字平滑、模型崩齿）。
  ///
  /// ## 为何要两套纹理（MSAA 纹理 + 可采样纹理）
  ///
  /// D3D11 的多重采样纹理**不能建 SRV**（不能直接当 shader 资源采样）。
  /// 所以标准做法是：渲染进 MSAA 纹理 → `ResolveSubresource` 到一张同尺寸普通纹理
  /// → 那张普通纹理才建 SRV。零拷贝合成用的是 resolve 后的那张。
  [[nodiscard]] auto create_targets() -> bool {
    release_targets();
    // 先查设备对 `kFormat` 支持哪些采样数（不支持就退 1，并在日志里说明）
    UINT quality = 0;
    for (const UINT candidate : {8U, 4U, 2U}) {
      quality = 0;
      if (SUCCEEDED(device_->CheckMultisampleQualityLevels(kFormat, candidate, &quality)) &&
          quality > 0) {
        sample_count_ = candidate;
        break;
      }
    }
    D3D11_TEXTURE2D_DESC color_desc{};
    color_desc.Width = static_cast<UINT>(width_);
    color_desc.Height = static_cast<UINT>(height_);
    color_desc.MipLevels = 1;
    color_desc.ArraySize = 1;
    color_desc.Format = kFormat;
    color_desc.SampleDesc.Count = sample_count_;
    color_desc.Usage = D3D11_USAGE_DEFAULT;
    // ⚠ MSAA 纹理**不能**绑 SHADER_RESOURCE（只能 RENDER_TARGET），否则创建失败
    color_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(device_->CreateTexture2D(&color_desc, nullptr, &color_))) {
      // MSAA 建不出来（罕见）：退单采样，宁可锯一点也不能没画面
      sample_count_ = 1;
      color_desc.SampleDesc.Count = 1;
      color_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
      if (FAILED(device_->CreateTexture2D(&color_desc, nullptr, &color_))) return false;
    }
    if (FAILED(device_->CreateRenderTargetView(color_, nullptr, &rtv_))) return false;

    if (sample_count_ > 1) {
      // resolve 目标：单采样、可采样（零拷贝合成用它的 SRV）
      D3D11_TEXTURE2D_DESC resolved_desc = color_desc;
      resolved_desc.SampleDesc.Count = 1;
      resolved_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
      if (FAILED(device_->CreateTexture2D(&resolved_desc, nullptr, &resolved_))) return false;
    }
    // SRV 建在 resolve 后的纹理上（或单采样时就是 color_ 本身）
    ID3D11Texture2D* sampleable = resolved_ != nullptr ? resolved_ : color_;
    if (FAILED(device_->CreateShaderResourceView(sampleable, nullptr, &srv_))) return false;

    D3D11_TEXTURE2D_DESC depth_desc{};
    depth_desc.Width = static_cast<UINT>(width_);
    depth_desc.Height = static_cast<UINT>(height_);
    depth_desc.MipLevels = 1;
    depth_desc.ArraySize = 1;
    depth_desc.Format = DXGI_FORMAT_D32_FLOAT;
    // 深度采样数必须与颜色一致，否则 `CreateDepthStencilView` 与 OMSetRenderTargets 失败
    depth_desc.SampleDesc.Count = sample_count_;
    depth_desc.Usage = D3D11_USAGE_DEFAULT;
    depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(device_->CreateTexture2D(&depth_desc, nullptr, &depth_))) return false;
    if (FAILED(device_->CreateDepthStencilView(depth_, nullptr, &dsv_))) return false;
    return true;
  }

  /// 把 MSAA 结果 resolve 到可采样纹理（单采样时是空操作）。
  void resolve_if_needed() {
    if (sample_count_ > 1 && resolved_ != nullptr && color_ != nullptr) {
      context_->ResolveSubresource(resolved_, 0, color_, 0, kFormat);
    }
  }

  void release_targets() {
    if (resolved_ != nullptr) {
      resolved_->Release();
      resolved_ = nullptr;
    }
    if (dsv_ != nullptr) {
      dsv_->Release();
      dsv_ = nullptr;
    }
    if (depth_ != nullptr) {
      depth_->Release();
      depth_ = nullptr;
    }
    if (srv_ != nullptr) {
      srv_->Release();
      srv_ = nullptr;
    }
    if (rtv_ != nullptr) {
      rtv_->Release();
      rtv_ = nullptr;
    }
    if (color_ != nullptr) {
      color_->Release();
      color_ = nullptr;
    }
    if (staging_ != nullptr) {
      staging_->Release();
      staging_ = nullptr;
    }
    if (query_ != nullptr) {
      query_->Release();
      query_ = nullptr;
    }
  }

  /// 取（或上传）一个网格的缓冲。键是**内容哈希**：数据驱动的每帧重建也能命中。
  [[nodiscard]] auto buffers_for(const st::raster::Mesh& mesh) -> const MeshBuffers* {
    const std::uint64_t key = mesh_key(mesh);
    for (const auto& entry : cache_) {
      if (entry.key == key) return &entry.buffers;
    }
    MeshBuffers buffers{};
    D3D11_BUFFER_DESC vertex_desc{};
    vertex_desc.ByteWidth = static_cast<UINT>(mesh.vertices.size() * sizeof(float));
    vertex_desc.Usage = D3D11_USAGE_IMMUTABLE;
    vertex_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vertex_data{};
    vertex_data.pSysMem = mesh.vertices.data();
    if (FAILED(device_->CreateBuffer(&vertex_desc, &vertex_data, &buffers.vertices))) {
      return nullptr;
    }
    D3D11_BUFFER_DESC index_desc{};
    index_desc.ByteWidth = static_cast<UINT>(mesh.indices.size() * sizeof(std::uint32_t));
    index_desc.Usage = D3D11_USAGE_IMMUTABLE;
    index_desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA index_data{};
    index_data.pSysMem = mesh.indices.data();
    if (FAILED(device_->CreateBuffer(&index_desc, &index_data, &buffers.indices))) {
      return nullptr;
    }
    buffers.index_count = static_cast<UINT>(mesh.indices.size());
    // UV 独立 stream：有 UV 才建。
    //
    // 为何不用交错布局：`kVertexFloats = 9` 是生成器与后端的契约，改 stride
    // 要动所有生成器与 OBJ 加载；而绝大多数网格不需要贴图。独立 stream 让
    // “有 UV 才绑、没 UV 就不绑”成为一次判空。
    if (mesh.has_uvs()) {
      D3D11_BUFFER_DESC uv_desc{};
      uv_desc.ByteWidth = static_cast<UINT>(mesh.uvs.size() * sizeof(float));
      uv_desc.Usage = D3D11_USAGE_IMMUTABLE;
      uv_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
      D3D11_SUBRESOURCE_DATA uv_data{};
      uv_data.pSysMem = mesh.uvs.data();
      if (FAILED(device_->CreateBuffer(&uv_desc, &uv_data, &buffers.uvs))) {
        buffers.release();
        return nullptr;
      }
    }
    // 缓存上限：脚本可以让每帧都换网格，所以要有上限而不是无限长。
    // 满了丢最早一项（FIFO）——与画布几何贴图缓存同一套策略。
    if (cache_.size() >= kMeshCacheLimit) cache_.erase(cache_.begin());
    cache_.push_back(CacheEntry{key, std::move(buffers)});
    return &cache_.back().buffers;
  }

  /// 上传（或释放）纹理。空 span = 释放。
  void upload_texture(std::span<const std::uint8_t> pixels, int width, int height) {
    if (texture_view_ != nullptr) {
      texture_view_->Release();
      texture_view_ = nullptr;
    }
    if (texture_ != nullptr) {
      texture_->Release();
      texture_ = nullptr;
    }
    if (pixels.empty() || width <= 0 || height <= 0) return;
    const std::size_t needed =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
    if (pixels.size() < needed) return;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = kFormat;   // R8G8B8A8_UNORM：与画布/纹理数据同一通道序
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = pixels.data();
    data.SysMemPitch = static_cast<UINT>(width) * 4U;
    if (FAILED(device_->CreateTexture2D(&desc, &data, &texture_))) {
      texture_ = nullptr;
      return;
    }
    if (FAILED(device_->CreateShaderResourceView(texture_, nullptr, &texture_view_))) {
      texture_view_ = nullptr;
    }
  }

  /// 回读成画布布局（`0xRRGGBBAA`）。每帧 `end_frame` 调一次。
  void ensure_readback() {
    // 回读必须取 **resolve 后**的纹理：MSAA 纹理不能 CopyResource 到单采样 staging。
    resolve_if_needed();
    ID3D11Texture2D* source_texture = resolved_ != nullptr ? resolved_ : color_;
    if (staging_ == nullptr) {
      D3D11_TEXTURE2D_DESC desc{};
      desc.Width = static_cast<UINT>(width_);
      desc.Height = static_cast<UINT>(height_);
      desc.MipLevels = 1;
      desc.ArraySize = 1;
      desc.Format = kFormat;
      desc.SampleDesc.Count = 1;
      desc.Usage = D3D11_USAGE_STAGING;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      if (FAILED(device_->CreateTexture2D(&desc, nullptr, &staging_))) return;
    }
    context_->CopyResource(staging_, source_texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(staging_, 0, D3D11_MAP_READ, 0, &mapped))) return;
    readback_.resize(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_));
    const auto* source = static_cast<const std::uint8_t*>(mapped.pData);
    for (int y = 0; y < height_; ++y) {
      const std::uint8_t* row = source + static_cast<std::size_t>(y) * mapped.RowPitch;
      for (int x = 0; x < width_; ++x) {
        const std::uint8_t red = row[x * 4 + 0];
        const std::uint8_t green = row[x * 4 + 1];
        const std::uint8_t blue = row[x * 4 + 2];
        const std::uint8_t alpha = row[x * 4 + 3];
        readback_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
                  static_cast<std::size_t>(x)] =
            (static_cast<std::uint32_t>(red) << 24U) |
            (static_cast<std::uint32_t>(green) << 16U) |
            (static_cast<std::uint32_t>(blue) << 8U) | static_cast<std::uint32_t>(alpha);
      }
    }
    context_->Unmap(staging_, 0);
  }

  struct CacheEntry {
    std::uint64_t key{0};
    MeshBuffers buffers{};
  };
  static constexpr std::size_t kMeshCacheLimit{32};

  ID3D11Device* device_{nullptr};
  ID3D11DeviceContext* context_{nullptr};
  int width_{1};
  int height_{1};
  bool ok_{false};
  ID3D11Texture2D* color_{nullptr};
  ID3D11RenderTargetView* rtv_{nullptr};
  /// resolve 目标（MSAA 时非空）：单采样且可建 SRV，零拷贝合成与回读都用它。
  ID3D11Texture2D* resolved_{nullptr};
  /// 实际生效的 MSAA 采样数（1 = 无抗锯齿，设备不支持时自动退到 1）。
  UINT sample_count_{1};
  ID3D11ShaderResourceView* srv_{nullptr};
  ID3D11Texture2D* depth_{nullptr};
  ID3D11DepthStencilView* dsv_{nullptr};
  ID3D11Texture2D* staging_{nullptr};
  ID3D11Query* query_{nullptr};   ///< `flush()` 用的完成查询（复用以避免每帧建对象）
  std::vector<std::uint32_t> readback_{};
  std::vector<CacheEntry> cache_{};
  /// 本网格是否正在采样纹理。
  bool use_texture_{false};
  ID3D11Texture2D* texture_{nullptr};
  ID3D11ShaderResourceView* texture_view_{nullptr};
  st::raster::Lighting lighting_{};
  math::Mat4 view_{math::Mat4::identity()};
  math::Mat4 projection_{math::Mat4::identity()};
  bool mvp_ready_{false};
  std::uint64_t draw_calls_{0};
};

}  // namespace st::raster::gpu

namespace st::raster {

/// GPU 三维腿是否可用（着色器与状态**真建起来**才算，不只看"有设备"）。
[[nodiscard]] auto gpu_scene3d_available() -> bool {
  return st::raster::gpu::scene3d_pipeline().ok;
}

/// 建 GPU 三维场景；不可用时给出**具体原因**（上层据此回退软件腿并记日志）。
[[nodiscard]] auto create_gpu_scene3d(int width, int height)
    -> Result<std::unique_ptr<Scene3D>> {
  using namespace st::raster::gpu;
  const DeviceHolder& device = holder();
  if (!device.ok) {
    return unexpected(ErrorCode::Unsupported,
                      device.error.empty() ? std::string("没有可用的 D3D11 设备")
                                           : device.error);
  }
  if (width <= 0 || height <= 0) {
    return unexpected(ErrorCode::Invalid, "渲染尺寸必须为正");
  }
  const Scene3dPipeline& pipes = scene3d_pipeline();
  if (!pipes.ok) {
    return unexpected(ErrorCode::Unsupported,
                      pipes.error.empty() ? std::string("三维渲染管线不可用") : pipes.error);
  }
  auto scene = std::make_unique<GpuScene3d>(device.device_or_null(), device.context_or_null(),
                                            width, height);
  return std::unique_ptr<Scene3D>(std::move(scene));
}

}  // namespace st::raster

#endif  // _WIN32
