#include <cstdio>
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
///
/// ## 上下文策略：三个类，两套规则（**改代码前先看这张表**）
///
/// D3D11 的**立即上下文不是线程安全的**，而本文件让它进程级共享。于是每个多线程
/// 入口都必须自己解决并发。目前有三个类，各自选了不同办法：
///
/// | 类 | 用途 | 上下文策略 | 代价 |
/// |---|---|---|---|
/// | `GpuCanvas` | 2D 绘制（界面/卡片/文字） | **每画布一个 deferred context** | 多 6 处必须提交的不变量 |
/// | `GpuScene3d` | 3D 场景（裁云三维图层） | **逐调用加锁**（立即上下文） | 每次调一次 lock/unlock |
/// | `D3dPresenter` | 上屏（DXGI swapchain） | 整段持锁，不自己画 | — |
///
/// ## 判据：为什么是两套而不是一套
///
/// 分水岭是**每帧碰上下文的次数**：
///
/// - `GpuCanvas`：绘制入口十几条、每帧几十上百次调用。给 `submit` 加全局锁等于把
///   并行渲染整个串行化（**实测比软件还慢**，注释原话“那条路是负优化”）。
///   所以选 D3D11 官方的 deferred 模型：指令录进各自命令列表，需要落地时串行回放。
/// - `GpuScene3d`：每帧**只有 3~4 次**碰上下文的调用（清屏 / 设状态 / `DrawIndexed` /
///   回读）。逐次加锁在无竞争时几纳秒，不值得为它背上一整套提交义务。
///   另有一个实际教训：曾试过“整帧持锁”（`begin_frame` 拿、`end_frame` 放），
///   但调用方**漏了 `end_frame`** 就永久阻塞——而症状是“卡住不报错”，极难查。
///
/// 一句话：**次数多到值得并行 → deferred；只有几次 → 加锁。**
///
/// ## 两套规则各自的义务（混淆就会出隐形缺陷）
///
/// ### `GpuCanvas`：凡读 `target_` 必先提交
///
/// deferred 下绘制是**录制态**，不提交就不执行。所以：
///
/// | 读 target_ 的路径 | 提交点 |
/// |---|---|
/// | `present()`（上屏） | 拷贝前 |
/// | `ensure_readback()`（截图/导出） | 攒批完成后 |
/// | `shadow_mask()` 的模糊多 pass | 每个 pass 前（下一 pass 要采样它） |
/// | `flush()` | — |
///
/// **发绘制的唯一入口是 `draw_scope()`**：构造即记账，所以“漏标记”在类型系统上
/// 不可能发生（`active_context()` 已是 private）。新增绘制路径时**不要**绕过它。
///
/// ### `GpuScene3d`：不要套用 deferred 的纪律
///
/// 它全程在立即上下文上直接发绘制，命令即时执行——**没有“提交”这一步**。
/// 2026-10-25 排查时这里曾误加过 `submit_deferred_locked()`（从 `GpuCanvas` 的
/// 修复里复制过来的注释与代码），编译不过才发现套错了对象。
///
/// ## 为什么会有人搞错（真实代价）
///
/// 2026-10-25：`present` 漏提交 → 窗口**全黑**，而应用内截图正常（回读路径自己会提交），
/// 于是“截图对、屏幕黑”这条错位把排查引向显示器/DWM/驱动，绕了一圈才回到代码。
/// 随后又查出 `submit_glyph_batch` / `submit_to` / 离屏 clear 共 4 处同类遗漏。
/// 这就是现在把记账做成 RAII、把策略写在这里的原因：**靠记忆不行，靠编译器。**

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
[[nodiscard]] auto gpu_scene3d_unavailable_reason() -> std::string {
  return "GPU 三维当前仅实现了 D3D11（Windows）后端";
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
// `<map>`：AO 的世界位置量化缓存用 `std::map`。
//
// 与上面 `<cstring>` 同一道理：**不能靠别的头间接带入**。
// 实测过：本地全量构建能过，而 `independent_project_builds_links_and_runs`
// 编不过（那个测试从零构建一个独立工程）——依赖间接包含就会这样分成两派。
#include <map>
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
#include "st/raster/color_pipeline.hpp"
#include "st/raster/agx_lut.hpp"
#include "st/raster/pbr.hpp"
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
    // 可分离盒式模糊：与 `blur_mask` 同一口径（3 遍 [水平+垂直]）。
    // 循环上界必须是编译期常量，而半径是运行期参数：取足够大的常量 + 条件跳过。
    //
    // ## `g_mode.w` 决定模糊**几个通道**
    //
    //   0（缺省）= **单通道**（只模糊 R，输出 `(r,0,0,1)`）—— 阴影遮罩用，
    //             遮罩是灰度覆盖率，只要一个通道。
    //   1       = **全色**（RGBA 都模糊）—— 毛玻璃用。
    //
    // ⚠ 毛玻璃**必须**走全色：直接复用单通道会把 G/B 写成 0、A 写成 1，
    // 贴回画布后整块区域只剩红色分量——实测暗色弹窗右侧出现一条
    // “红绿色块”竖带（R 在 44~62 波动、G 恒为 30、B 恒为 38），
    // 就是色通道被丢弃后与底色混合的结果。
    const int radius = (int)g_axis_extra.x;
    const float2 step = g_axis.xy / g_viewport.xy;
    const bool full_color = g_mode.w > 0.5;
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    float count = 0.0;
    for (int i = -64; i <= 64; ++i) {
      if (i < -radius || i > radius) continue;
      sum += g_texture.SampleLevel(g_sampler, local01 + step * (float)i, 0);
      count += 1.0;
    }
    if (count <= 0.0) return float4(0.0, 0.0, 0.0, full_color ? 0.0 : 1.0);
    if (full_color) return sum / count;
    // 单通道（阴影）：R 通道取均值，alpha 固定 1（遮罩语义）。
    return float4(sum.r / count, 0.0, 0.0, 1.0);
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
  const int mode = (int)g_mode.x;

  // ## 位图模式（mode 3）必须**裁掉外扩那一圈**（2026-11-04 实测定位）
  //
  // `vs_main` 把四边形向外扩了 1 像素——那是为了 SDF 圆角/描边能拿到边界像素的
  // 部分覆盖率（见那里的说明）。但对**预乘 RGBA 位图**，外扩没有任何好处，
  // 反而把纹理“拉大”：
  //
  // | | 软件 `draw_canvas` | GPU 原实现 |
  // |---|---|---|
  // | 写入范围 | `area = round_out(dest)`，正好 30 像素 | quad 扩到 32 像素 |
  // | 纹理映射 | 1 texel = 1 像素 | 30 texel 拉到 32 像素（放大 6.7%） |
  // | 图标起点 | x=18 | **x=17** |
  // | 图标尺寸 | 30×30 | **32×32** |
  //
  // 实测取证（TEMP-PROBE 把 mode==3 改成输出纯红）：红块是 **32×32 @ (17,14)**，
  // 而传入的 rect 是 `(18,15,30,30)`——完全对上了外扩。
  // 用户看到的：相期标题栏图标在 `defer_gpu` 升级那一帧（~780ms）从 (18,15)
  // 跳到 (17,14)、且边缘发糊。这就是“软件与 GPU 渲染效果不一致”的具体形态。
  //
  // 修法：位图模式把 `local` 超出 `[0, g_rect.zw]` 的片元直接 discard。
  // 这比“UV 内缩半纹素”更贴合软件语义——软件就是**不写**那些像素，
  // 而不是“写一个边缘颜色”。
  if (mode == 3) {
    if (input.local.x < 0.0 || input.local.y < 0.0 || input.local.x >= g_rect.z ||
        input.local.y >= g_rect.w) {
      discard;
    }
  }
  const float2 uv01 = input.local / g_rect.zw;
  return shade(input.local, uv01, input.pixel, g_uv, half_size);
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

/// 三维渲染目标格式（sRGB 视图）。
///
/// 让硬件在做"着色器输出 → 存储"时自动线性→sRGB 编码，于是**半透明混合
/// 发生在编码之前的线性空间**——与软件腿的"解码目标 → 线性混合 → 编码回去"等价。
constexpr DXGI_FORMAT kSceneFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;

/// 2D 绘制画布。
///
/// **上下文策略：每画布一个 deferred context**——完整判据与义务见文件头
/// 「上下文策略：三个类，两套规则」。要点：发绘制只走 draw_scope()（构造即记账），
/// 且凡读 target_ 之前必须 submit_deferred_now() / submit_deferred_locked()。
class GpuCanvas final : public Surface {
 public:
  // ——————————————————————————————————————————————————————————————————————————
  // **每画布一个 deferred context**（2026-10-25）
  //
  // ## 为什么需要
  //
  // D3D11 的**立即上下文不是线程安全的**，而 `DeviceHolder` 让整个进程共用一份。
  // 裁云导出按多路并行渲染、每线程一块画布，于是多个线程同时 `Draw` 同一个
  // 立即上下文 = 未定义行为（实测症状：堆损坏 0xC0000374 / 访问违例 0xC0000005，
  // 且**只在多路时出现**，单路 100% 正常，极易被误判成"GPU 画布不能用"）。
  //
  // 给 `submit` 加全局锁能消除崩溃，但那是所有二维原语的公共入口，
  // 等于把并行渲染整个串行化（实测：比软件还慢）。**那条路是负优化。**
  //
  // ## deferred context 是 D3D11 官方的多线程模型
  //
  // 每线程/每画布一个 `CreateDeferredContext`：绘制指令录进各自的命令列表
  // （**不碰**共享的立即上下文），需要落地时才在立即上下文上
  // `ExecuteCommandList` 串行回放。
  //
  // 实测（`tools/deferred_ctx_probe`）：
  //   · `CreateDeferredContext` 成功；空命令列表往返 **0.053ms**
  //   · 16 路并行录制 3.10ms + 串行回放 1.27ms ≈ **4.4ms**（一帧是 200ms 量级）
  //   · 回放顺序生效、像素正确（PASS）
  //
  // ## 何时提交
  //
  // **不每条指令都提交**（那样等于没并行）。只在**要读 `target_` 的那一刻**之前：
  //
  // | 读 target_ 的路径 | 提交点 |
  // |---|---|
  // | `present()`（上屏） | ✅ 在 `CopyResource` 之前 |
  // | `ensure_readback()`（截图/导出） | ✅ 函数开头 |
  // | `flush()` | ✅ |
  //
  // ⚠ **这条清单曾漏了 `present()`**：录制的命令永不执行，而上屏拷到的是空纹理 →
  // **窗口全黑**。更阴的是截图/导出正常（它们自己会补提交），于是症状变成
  // “截图对、屏幕黑”，排查时那条“截图正常”反而成了误导证据。
  //
  // ## 不变量（写代码时照这个查）
  //
  // **凡在 `active_context()` 上发绘制的地方，必须同一函数内调 `note_deferred_draw()`。**
  // 现有 6 处（改动时用 grep `active_context()->(Draw|ClearRenderTargetView)` 全覆盖）：
  //
  //   1. `clear()` —— 清屏幕
  //   2. `submit()` —— 所有逐图元绘制的唯一入口
  //   3. `submit_glyph_batch()` —— 字形合批（**唯一绕过 `submit` 的批量绘制**）
  //   4. `submit_to()` —— 离屏绘制（阴影遮罩 / 盒式模糊 / 图像处理）
  //   5. `shadow_mask()` 里的两个离屏 `ClearRenderTargetView`
  //
  // 漏了它的症状各不相同，但都难查：
  //   · `present` 漏 → 全黑（本条）
  //   · `submit_glyph_batch` 漏 → **文字整体消失**，矩形/背景都在
  //   · `submit_to` 漏 → 阴影/模糊“画了但看不见”（离屏结果没产出）
  //   · 离屏 clear 漏 → 阴影出现**上一帧的残影**（RT 是池化复用的）
  //
  // 注：`GpuScene3d` 是另一套——它全程用立即上下文（不做 deferred 录制），
  // 所以不在本不变量范围内。
  ID3D11DeviceContext* deferred_{nullptr};
  /// 已录制但尚未提交的绘制数（0 = 无待提交内容）。
  std::size_t deferred_pending_{0};

  /// 取（按需创建）本画布的 deferred context。失败返回 nullptr（调用方退回立即上下文）。
  [[nodiscard]] auto deferred_or_create() -> ID3D11DeviceContext* {
    if (deferred_ != nullptr) return deferred_;
    if (device_ == nullptr) return nullptr;
    ID3D11DeviceContext* created = nullptr;
    if (FAILED(device_->CreateDeferredContext(0, &created)) || created == nullptr) {
      return nullptr;   // 建不出来就退回立即上下文（正确性优先于并行）
    }
    deferred_ = created;
    return deferred_;
  }

  void release_deferred() {
    if (deferred_ != nullptr) {
      deferred_->Release();
      deferred_ = nullptr;
    }
    deferred_pending_ = 0;
  }


  /// 发绘制的**唯一入口**：构造即记账，`->` 取上下文发命令。
  ///
  /// ## 为何要有它（2026-10-25 全黑 bug 的根治）
  ///
  /// 原来每个绘制函数都得自己记得先 `(void)active_context(); note_deferred_draw();`，
  /// 共 5 个入口 6 处（`clear` / `submit` / `submit_glyph_batch` / `submit_to` /
  /// 离屏 clear ×2）。**靠注释提醒记不住**——同一天就漏了 4 处：
  ///
  ///   · `present()` 漏 → 全黑
  ///   · `submit_glyph_batch()` 漏 → 文字整体消失
  ///   · `submit_to()` 漏 → 阴影/模糊画了看不见
  ///   · 离屏 clear 漏 → 阴影上一帧残影
  ///
  /// 改成 RAII 后**结构上不可能漏**：没有其它办法拿到上下文发绘制，而构造
  /// `DrawScope` 就必然记了账。这是把纪律从"人的注意力"换成"类型系统"。
  ///
  /// 用法：
  /// ```cpp
  /// const DrawScope gfx = draw_scope();
  /// gfx->Draw(...);
  /// ```
  class DrawScope {
   public:
    explicit DrawScope(GpuCanvas& owner) : context_(owner.active_context()) {
      owner.note_deferred_draw();
    }
    [[nodiscard]] auto operator->() const noexcept -> ID3D11DeviceContext* { return context_; }
    [[nodiscard]] auto get() const noexcept -> ID3D11DeviceContext* { return context_; }

   private:
    ID3D11DeviceContext* context_{nullptr};
  };

  [[nodiscard]] auto draw_scope() -> DrawScope { return DrawScope{*this}; }


  /// 是否还有**录了但未提交**的绘制命令。
  ///
  /// 给上屏/回读路径做前置断言用：为真却去读 `target_`，读到的是“上一轮的内容”。
  /// 正常路径下调用方总先 `submit_deferred_locked()`，所以它应为假；
  /// 为真即意味着“有人新增了绘制入口但没经 `draw_scope()`”——那是需要立刻暴露的缺陷，
  /// 而不是默默上一张旧画面。
  [[nodiscard]] auto has_uncommitted_draws() const noexcept -> bool {
    return deferred_ != nullptr && deferred_pending_ != 0;
  }

 private:
  // ——————————————————————————————————————————————————————————————————————————
  // 以下两个是 `draw_scope()` 的内部实现，**不对外**。
  //
  // 为何改成 private：2026-10-25 的教训是“靠注释提醒记不住”——只要上下文能
  // 随便拿到，就总有人直接拿它发绘制、然后忘了记账。提到 private 后，唯一的
  // 绘制通道就是 `draw_scope()`，而它构造即记账——**漏标记在类型系统上就不可能**。
  //
  // `DrawScope` 是嵌套类（能访外部类私有），`draw_scope()` 是唯一的口子。
  // ——————————————————————————————————————————————————————————————————————————

  /// 当前应当把绘制指令录进哪个上下文。
  ///
  /// 默认走 deferred（并行安全）；建不出来时退回立即上下文（单线程也对，
  /// 只是多线程下会回到"未定义行为"—— 但那种机器本来就跑不动 GPU 导出）。
  [[nodiscard]] auto active_context() -> ID3D11DeviceContext* {
    if (ID3D11DeviceContext* d = deferred_or_create(); d != nullptr) return d;
    return context_;
  }

  /// 记一笔"有内容待提交"（仅在走 deferred 时）。
  void note_deferred_draw() {
    if (deferred_ != nullptr) ++deferred_pending_;
  }

 public:

  /// 把已录制的命令在**立即上下文**上执行掉。必须在持有 `g_context_mutex()` 时调用。
  ///
  /// 这是整个并行模型里唯一碰共享上下文的地方 —— 也是串行点。
  void submit_deferred_locked() {
    if (deferred_ == nullptr || deferred_pending_ == 0) return;
    ID3D11CommandList* list = nullptr;
    const HRESULT finish = deferred_->FinishCommandList(FALSE, &list);
    if (SUCCEEDED(finish) && list != nullptr) {
      // **必须在立即上下文上执行**（`context_`），不能走 `active_context()`
      // —— 后者返回的是 deferred 自己，等于"把命令列表交给录制者自己执行"，
      // 什么都不会发生（实测症状：pending 清成 0，但画面永远是空的/旧的，
      // 而 `Map(D3D11_MAP_READ)` 会一直等一个永远不会到来的完成信号 → 卡死）。
      context_->ExecuteCommandList(list, FALSE);
      list->Release();
      // ## 必须显式 Flush（2026-10-25 死等的真因）
      //
      // `ExecuteCommandList` 只是把命令**排进**立即上下文的队列；D3D11 的驱动
      // 有它自己的批处理节奏，不保证立刻提交给 GPU。紧接着的
      // `CopyResource` + `Map(D3D11_MAP_READ)` 会**阻塞等 GPU**，
      // 而 GPU 还在等一个没到的 flush 信号 —— 死锁。
      //
      // 实测症状：进程 126 秒只消耗 2 秒 CPU（纯等），日志停在"准备 Map"。
      // 这一步的代价很小（本就要同步），但少了它整条 GPU 路径不可用。
      context_->Flush();
    } else {
      // ## 失败必须报（2026-10-27 补）
      //
      // 旧实现把这个分支写成“静默跳过，反正下面会清零”——那是错的：
      // `FinishCommandList` 失败（设备丢失 / 驱动重置 / 内存不足）时
      // 命令**根本没执行**，而 `deferred_pending_ = 0` 又把“有未提交命令”
      // 这件事抹掉了。后果是**画面停在上一次的内容，日志里一个字都没有**，
      // 而且会让 `present()` 的 `has_uncommitted_draws()` 断言失效
      //（它看到的是“已清零”，于是以为一切正常）。
      //
      // 这正是“静默降级”类缺陷的典型形状：不是崩，是变旧。记为 error 让它可见。
      ST_LOG_ERROR("GPU：FinishCommandList 失败（0x{:08X}）—— 本轮 {} 条绘制命令已丢弃",
                   static_cast<unsigned>(finish), deferred_pending_);
    }
    deferred_pending_ = 0;
  }

  /// 自己拿锁的提交（给“绘制过程中就要落地”的场合用）。
  ///
  /// 为什么要它：`shadow_mask` 的盒式模糊是多 pass ping-pong——**下一 pass 要采样
  /// 上一 pass 的输出**，而 deferred 下那份输出还没执行（D3D11 不允许同资源兼作
  /// RT 与 SRV，驱动会先解绑 SRV，于是采到空白）。所以必须中途提交。
  ///
  /// 而那种场合外面并没有持锁（它在 `draw_shadow` 里、单帧内同步完成），
  /// 所以这个版本自己取锁——避免把“必须持锁”的契约扩散到调用链上层。
  void submit_deferred_now() {
    if (deferred_ == nullptr || deferred_pending_ == 0) return;
    const std::scoped_lock lock(g_context_mutex());
    submit_deferred_locked();
  }

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
    release_deferred();
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
    // 与构造函数对称：AddRef/Release 的都是**立即上下文**（`context_`）——
    // 它是进程共享的那份，本画布只是持一个引用计数。
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
  void add_profile(PaintOp op, double ms, std::uint64_t pixels = 0,
                   const std::source_location site =
                       std::source_location::current()) noexcept override {
    if (profiler_ != nullptr) profiler_->add_at(op, ::st::perf::site_of(site), ms, pixels);
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
    const DrawScope gfx = draw_scope();          // 确保 deferred 已创建（它懒创建）
    gfx->ClearRenderTargetView(rtv_, values);
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

  /// 把本画布的**离屏纹理**交出去，供同后端的消费方零拷贝采样。
  ///
  /// ## 为什么这个接口必须存在（它的缺席让三维白跑了纯 CPU）
  ///
  /// 三维场景要知道"合成目标能不能零拷贝接住我的结果"，判据就是本函数
  /// 是否返回有效句柄。若 `GpuCanvas` 不实现它（默认实现返回空），
  /// 调用方会认为**所有目标都接不住**——于是三维永远走软件腿。
  ///
  /// 实测代价：RTX 4080 上跑纯 CPU 光栅化，1080p 三物体 344 ms/帧，
  /// 而零拷贝路径只要 0.85 ms/帧（`tests/gpu_zero_copy_test.go`）。
  ///
  /// 注意：只交**纹理视图**（SRV），不交出所有权——本画布析构时会释放它，
  /// 消费方必须在同一帧内用完（裁云的合成就是同帧、同线程）。
  [[nodiscard]] auto native_texture() const noexcept
      -> st::raster::Surface::NativeTexture override {
    if (target_srv_ == nullptr) return {};
    st::raster::Surface::NativeTexture out{};
    out.handle = target_srv_;
    out.backend = "d3d11";
    out.width = physical_width();
    out.height = physical_height();
    return out;
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
  /// **区域毛玻璃（GPU）**：把 `rect` 内已绘内容就地模糊。
  ///
  /// ## 为什么能在 GPU 上做，而且比 CPU 快
  ///
  /// 需要的机制在 `shadow_mask()` 里已经全部跑通：离屏 RT + `DrawMode::BoxBlur`
  /// 着色器的多 pass ping-pong。区别只是采样源从“现画的圆角矩形遮罩”换成
  /// **画布自己的 `color_` 纹理**（它的 SRV 已建好，见 `create_target`）。
  /// 全程不落 CPU——回读只服务截图/导出，跟渲染无关。
  ///
  /// ## 三个必须做对的地方
  ///
  /// 1. **先提交已录制命令**：deferred 下画布内容还在命令队列里，`color_` 里是旧的。
  /// 2. **每个 pass 后提交**：下一 pass 要拿上一 pass 的输出当采样源，
  ///    而 D3D11 不允许同一资源同时绑 RT 与 SRV（`shadow_mask` 里同一条教训）。
  /// 3. **贴回时用圆角遮罩**：模糊只在矩形上跑，不认圆角（同软件端）。
  [[nodiscard]] auto blur_region(math::Rect rect, float radius) -> bool override {
    if (radius <= 0.0f || rect.is_empty() || target_ == nullptr || target_srv_ == nullptr) {
      return false;
    }
    // 逻辑 → 物理，并与裁剪域/画布求交（与软件端同口径）。
    math::IntRect area = to_physical(rect).intersect(clip_rect());
    area = area.intersect(math::IntRect{0, 0, physical_width_, physical_height_});
    if (area.width <= 1 || area.height <= 1) return false;

    // ① 先把已录制的绘制落地——否则采样到的是旧内容。
    submit_deferred_now();

    // ② 拿一对**彩色**离屏 RT 做 ping-pong。
    //
    // ⚠ 不能用 `acquire_shadow_targets`：那个是 `R8_UNORM`（只有 R 通道），
    // 阴影遮罩是灰度所以够用，但毛玻璃模糊的是彩色图像——渲进 R8 会直接丢掉 G/B，
    // 无论着色器算得多对。那是“红绿色块”缺陷的第一层成因。
    ShadowTargets targets;
    if (!acquire_color_targets(area.width, area.height, targets)) return false;
    // **alpha 清成 1**（不是 0）：模糊会沿区域边界采样，
    // 若目标 alpha 是 0，模糊会把边界外的不透明像素与这里的 0 平均，
    // 结果 alpha < 1，贴回时就变成半透明——与软件端 `over` 混合结果不一致，
    // 表现为整体偏暗（实测弹层区域均值 GPU 210 vs 软件 225）。
    const float clear_rgba[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const DrawScope gfx = draw_scope();
    gfx->ClearRenderTargetView(targets.rtv_a, clear_rgba);
    gfx->ClearRenderTargetView(targets.rtv_b, clear_rgba);
    note_deferred_draw();

    // ③ 把画布这一块拷进 A（用 `uv` 子矩形采样，不 `CopyResource` 整张纹理）。
    //
    // `uv` 是归一化的 {u0, v0, w, h}；`Bitmap` 模式直接采样它。
    //
    // ⚠ **拷贝时把 alpha 拉满**（`g_mode.z = 0` 已默认，这里显式写在注释里）：
    // 源是画布的**预乘**像素，如果弹层所在位置本身有半透明（比如面板有 alpha），
    // 拷贝后的 alpha 会 < 1，后面 3 遍模糊又把它平均开，贴回时就整体变淡。
    // 毛玻璃的语义是"模糊背后的内容"，与背板自身的 alpha 无关——
    // 所以拷进来时就该是**不透明**的。
    ShaderParams copy = base_params(
        DrawMode::Bitmap,
        math::Rect{0.0f, 0.0f, static_cast<float>(area.width), static_cast<float>(area.height)},
        {0.0f, 0.0f, 0.0f, 0.0f}, 1.0f);
    // **离屏渲染必须禁用裁剪**（包括圆角）。
    //
    // `base_params` 会从 `current_clip()` 取当前的裁剪状态写进 `g_mode.z`/
    // `g_clip*`，而着色器用 `g_viewport` 算裁剪 SDF。但这里渲染到的是
    // **尺寸为 area 的离屏 RT**，viewport 也改成了 area——两边坐标系不一致，
    // 算出的覆盖率是错的，像素被当成裁剪外丢掉（渲染结果变黑）。
    // 实测：带圆角裁剪的毛玻璃在 GPU 上整块变纯黑（`#000000`），
    // 结构性差异 4.5%、最大 Δ217。
    copy.mode[2] = 0.0f;
    copy.uv[0] = static_cast<float>(area.x) / static_cast<float>(physical_width_);
    copy.uv[1] = static_cast<float>(area.y) / static_cast<float>(physical_height_);
    copy.uv[2] = static_cast<float>(area.width) / static_cast<float>(physical_width_);
    copy.uv[3] = static_cast<float>(area.height) / static_cast<float>(physical_height_);
    apply_shadow_viewport(copy, area);
    submit_deferred_now();
    submit_to(targets.rtv_a, copy, target_srv_, true, true, area.width, area.height);

    // ④ 3 pass × [水平+垂直]，ping-pong 于 A/B（与 `shadow_mask` 完全同构）。
    const int box_radius = std::max(1, static_cast<int>(std::lround(radius * scale_ * 0.5f)));
    ID3D11RenderTargetView* read_rtv = targets.rtv_a;
    ID3D11ShaderResourceView* read_srv = targets.srv_a;
    for (int pass = 0; pass < 3; ++pass) {
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
        // **全色模糊**：`g_mode.w = 1` 让着色器模糊 RGBA 四个通道。
        // 缺省 0 是阴影遮罩的"只模糊 R 通道"模式——毛玻璃直接用它会把
        // G/B 写成 0、A 写成 1，贴回后整块只剩红色分量。
        // 实测：暗色弹窗右侧一条红绿色块竖带（R 波动 44~62、G 恒 30、B 恒 38）。
        blur_params.mode[3] = 1.0f;
        blur_params.mode[2] = 0.0f;   // 离屏：禁用裁剪（理由见上面 `copy` 处）
        apply_shadow_viewport(blur_params, area);
        submit_deferred_now();   // 上一 pass 必须真执行（见函数注释第 2 条）
        submit_to(write_rtv, blur_params, read_srv, false, true, area.width, area.height);
        read_rtv = write_rtv;
        read_srv = reading_a ? targets.srv_b : targets.srv_a;
      }
    }
    submit_deferred_now();

    // ⑤ 贴回画布。**rect 必须是物理像素**（`base_params` 的口径：
    // 它内部直接写进着色器常量，不做缩放；`draw_canvas` 那里传的是
    // `to_physical_rect(destination)`、`shadow_mask` 传的是局部物理坐标）。
    //
    // ⚠ 我第一版在这里除了 `scale_`（把它当逻辑坐标），结果贴回只盖住了
    // 区域的左上一部分，对话框上出现一条笔直的**半灰半白**分界线。
    ShaderParams paste = base_params(
        DrawMode::Bitmap,
        math::Rect{static_cast<float>(area.x), static_cast<float>(area.y),
                   static_cast<float>(area.width), static_cast<float>(area.height)},
        {0.0f, 0.0f, 0.0f, 0.0f}, 1.0f);
    // `g_uv` 语义是 `{u0, v0, w, h}`（**宽高**，不是 u1/v1）——
    // `sample_subrect` 里是 `rect_uv.xy + uv01 * rect_uv.zw`。
    // RT 与 area 同尺寸（1:1），所以直接取整张纹理即可；
    // UV 内缩半个纹素以让采样中心对齐像素中心（否则边缘一列会偏）。
    const float half_u = 0.5f / static_cast<float>(std::max(1, area.width));
    const float half_v = 0.5f / static_cast<float>(std::max(1, area.height));
    paste.uv[0] = half_u;
    paste.uv[1] = half_v;
    paste.uv[2] = 1.0f - 2.0f * half_u;
    paste.uv[3] = 1.0f - 2.0f * half_v;
    // **贴回时收紧剪裁矩形**：`vs_main` 会把四边形外扩 1 像素，
    // 而贴回采样结果不需要外扩——外扩会把区域边界外侧写成边缘采样值。
    // 实测：`region={20,40,260,160}` 时右边界 x=280 在软端仍是原始颜色，
    // GPU 端被写成底色（Δ192，整个区域的最大差就在这一列）。
    // **贴回必须走正常混合**（`opaque=false`）：
    //
    // 圆角裁剪下，着色器会给边缘像素一个**部分覆盖率**（SDF），表现为 alpha<1。
    // 用 `opaque=true` 直接替换，就把"38/255 不透明"写进了画布——
    // 那一圈像素变成半透明，而软件端是"按覆盖率与原底色混合"后的不透明色。
    // 实测：圆角边缘出现 alpha=0x26 的像素（软端 #202028FF vs GPU #05050626，Δ217）。
    submit_to(rtv_, paste, read_srv, true, /*opaque=*/false, physical_width_, physical_height_,
              &area);
    note_deferred_draw();
    return true;
  }  [[nodiscard]] auto supports_blur() const noexcept -> bool override { return true; }
  /// GPU 画布**不**支持区域重绘：整帧绘制只需 ~1ms，而局部路径要另维护一套
  /// 裁剪/命令列表状态；更关键的是局部重绘成立的前提是**画布保留上一帧内容**——
  /// GPU 画布的呈现走 `DXGI_SWAP_EFFECT_DISCARD` 的 swapchain，`Present` 之后
  /// 后备缓冲内容**未定义**（驱动可随意丢弃），只拷损坏区会让其余像素变成黑块。
  ///
  /// 实测（Windows + NVIDIA L20-6Q，`--renderer gpu`）：把这里改成 `true` 后
  /// 界面频繁出现大片黑区；同一份代码在纯 CPU 机器上正常，因为那里走软件画布
  /// （`Canvas::supports_partial_repaint()` 才是真的可用）。
  [[nodiscard]] auto supports_partial_repaint() const noexcept -> bool override { return false; }

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
    //
    // ## ❗ 外扩只能改 `position`，**不能改 `local01`**（2026-11-04 实测修正）
    //
    // `local01` 是采样纹理用的归一化坐标。曾经用它按**外扩后的顶点**算：
    // ```
    // vertex.local01 = {(vx - px) / pw, (vy - py) / ph};   // vx 是外扩后的！
    // ```
    // 于是 UV 范围变成 `-1/pw .. 1 + 1/pw`（这里 -0.033 .. 1.033），
    // 着色器把它当 0..1 用：
    //
    // | | 正确 | 曾经 |
    // |---|---|---|
    // | 纹理映射 | 30 texel → 30 屏幕像素 | 30 texel → **32 屏幕像素** |
    // | 左边缘像素 17 的 UV | 越界（CLAMP 后仍画第0列） | -0.0167 → **画出第 0 列** |
    // | 墨迹起点 | 18 | **17** |
    // | 墨迹高度 | 30 | **32** |
    //
    // 用户看到的：标题栏图标在 `defer_gpu` 升级那一帧（~780ms）从 (18,15) 跳到
    // (17,14)，且边缘发糊（纹理被拉了 6.7%）。而**软件路径一直是 30px 1:1**——
    // 两边差 1px 位置 + 2px 尺寸，这就是“软件与 GPU 渲染效果不一致”的具体形态。
    //
    // 正确做法：`local01` 用**原始矩形**（即 `local` ÷ 尺寸）算。
    // 外扩部分自然得到越界 UV，靠采样器的 CLAMP 行为取边缘 texel——
    // 那正是“再往外画也无所谓”的语义，且**不会把纹理拉伸**。
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
      // 归一化分母是 `pw/ph`（**真实矩形**），不是外扩后的跨度。
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

    // 记一笔待提交（走 deferred 时才有意义）。
    //
    // **这条路径很容易漏**：字形合批是 `Surface` 里唯一**绕过 `submit()`**
    // 直接往 `active_context()` 发 draw 的路径（会批是为了“几十个字形一次 draw”，
    // 走不了 `submit` 的逐图元参数体系）。而 `submit()` 开头那条
    // `note_deferred_draw()` 就保不了它 —— 结果是：
    //   字形合批成功 → pending 没增 → 命令永不提交 → **文字整体消失**，
    //   而矩形/背景都在（它们走 submit），症状看起来像“字体丢了”。
    //
    // 位置选在 Map 之前：后面的每条提前 return 都已发生过一次 draw 准备，
    // 且 `FinishCommandList` 对空列表也无害（宁可多提交一次，不要漏）。
    const DrawScope gfx = draw_scope();

    D3D11_MAPPED_SUBRESOURCE mapped_vertices{};
    if (FAILED(gfx->Map(glyph_vertices_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped_vertices))) {
      return false;
    }
    std::memcpy(mapped_vertices.pData, vertices.data(), vertices.size() * sizeof(BatchVertex));
    gfx->Unmap(glyph_vertices_, 0);

    const ClipFrame& clip = current_clip();
    ID3D11ShaderResourceView* clip_mask = clip.mode > 2 ? clip.mask : nullptr;
    ID3D11RenderTargetView* targets[1] = {rtv_};
    gfx->OMSetRenderTargets(1, targets, nullptr);
    const D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(physical_width_),
                                  static_cast<float>(physical_height_), 0.0f, 1.0f};
    gfx->RSSetViewports(1, &viewport);
    const math::IntRect scissor = clip.rect;
    const D3D11_RECT rect{scissor.x, scissor.y, scissor.x + scissor.width,
                          scissor.y + scissor.height};
    gfx->RSSetScissorRects(1, &rect);
    gfx->RSSetState(pipes.raster_scissor);

    const UINT stride = sizeof(BatchVertex);
    const UINT offset = 0;
    ID3D11Buffer* buffers[1] = {glyph_vertices_};
    gfx->IASetVertexBuffers(0, 1, buffers, &stride, &offset);
    gfx->IASetInputLayout(pipes.batch_layout);
    gfx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gfx->VSSetShader(pipes.batch_vs, nullptr, 0);
    gfx->PSSetShader(pipes.batch_ps, nullptr, 0);

    // 字形尺寸在顶点里，但 `g_rect.zw` 仍要设成字形的（SDF/归一化用不到它，
    // 但 `shade` 之外的路径可能读）——这里取最后一个字形无意义，
    // 所以用图集尺寸占位并**依赖 ps_batch 只用顶点数据**。
    ShaderParams params = base_params(
        lcd ? DrawMode::CoverageMaskLcd : DrawMode::CoverageMask,
        math::Rect{0.0f, 0.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f, 0.0f}, opacity, color);

    ID3D11ShaderResourceView* views[1] = {view};
    ID3D11ShaderResourceView* clip_views[1] = {clip_mask};
    ID3D11SamplerState* samplers[1] = {pipes.sampler_point};
    gfx->PSSetShaderResources(0, 1, views);
    gfx->PSSetShaderResources(1, 1, clip_views);
    gfx->PSSetSamplers(0, 1, samplers);

    const float blend_factor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const auto draw_once = [&](ID3D11BlendState* state) -> bool {
      D3D11_MAPPED_SUBRESOURCE mapped{};
      if (FAILED(gfx->Map(pipes.constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return false;
      }
      std::memcpy(mapped.pData, &params, sizeof(params));
      gfx->Unmap(pipes.constants, 0);
      ID3D11Buffer* constant_buffers[1] = {pipes.constants};
      gfx->VSSetConstantBuffers(0, 1, constant_buffers);
      gfx->PSSetConstantBuffers(0, 1, constant_buffers);
      gfx->OMSetBlendState(state, blend_factor, 0xFFFFFFFFU);
      gfx->Draw(static_cast<UINT>(vertices.size()), 0);
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
      std::uint8_t* line = dst + static_cast<std::size_t>(row) * static_cast<std::size_t>(row_bytes);
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
    // 回读时**两份视图一起生成**（`ensure_readback` 里做），这里只返回。
    //
    // 曾经把打包视图做成"按需惰性生成"（省 5.32ms 的位运算），
    // 结果 `pixels()` 在首次调用时返回空 span、第二次才正常 ——
    // 这种"第一次为空"的形态在导出路径上表现为**首帧全黑**，
    // 在测试上是"同一断言时红时绿"，极难查。一次 5ms 换掉这类不确定性不值。
    auto* self = const_cast<GpuCanvas*>(this);
    (void)self->ensure_readback();
    // 导出路径可能跳过打包（见 `skip_packed_readback_`）：这里按需补上，
    // 于是不会出现"有 RGBA 但 pixels() 空"的不确定态。
    if (self->packed_readback_stale_) self->rebuild_packed_readback();
    return {readback_.data(), readback_.size()};
  }

  [[nodiscard]] auto pixels() noexcept -> std::span<std::uint32_t> override {
    (void)ensure_readback();
    if (packed_readback_stale_) rebuild_packed_readback();
    return {readback_.data(), readback_.size()};
  }
  /// 导出路径的取像素入口：**直接返回回读缓存的 RGBA 字节**，零转换。
  ///
  /// 之前这里先 `pixels()`（拿到打包的 uint32）、再逐像素拆回 4 个字节 ——
  /// 1080p 每帧 200 万次位运算，实测 **5.32ms/帧**（`tools/gpu_export_cost_probe`：
  /// `pixels()` 4.06ms vs `to_rgba8()` 9.38ms）。
  ///
  /// 而 staging 纹理本来就是 RGBA 字节序，回读时按行 memcpy 下来即可。
  /// 声“本次回读不需要打包视图”（导出路径用；见 `ensure_readback`）。
  ///
  /// 为何不直接惰性生成：惰性会让 `pixels()` 首次返回空 span（实测导致
  /// 导出首帧全黑）。这里要求调用方**显式声明**，而 `pixels()` 仍能
  /// 按需补算，两边都没有不确定态。
  void skip_next_packed_readback() noexcept override { skip_packed_readback_ = true; }

  [[nodiscard]] auto to_rgba8() const -> std::vector<std::uint8_t> override {
    (void)const_cast<GpuCanvas*>(this)->ensure_readback();
    return readback_rgba_;
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
    // 记一笔待提交（走 deferred 时才有意义）。放在入口处而不是出口：
    // 函数中途有多处提前 return（视口为空、纹理无效等），出口标记会漏。
    //
    // **必须先确保 deferred context 已创建**：`note_deferred_draw` 只在
    // `deferred_ != nullptr` 时计数，而 `deferred_` 是 `active_context()` 懒创建的。
    // 少了这一句，第一帧的绘制（以及"从未取过 active_context 的首次调用"）
    // 会计 0 —— 实测症状：`pending` 恒为 0，命令永不提交，画面空，且
    // `Map(D3D11_MAP_READ)` 死等（进程 126 秒只耗 2 秒 CPU）。
    const DrawScope gfx = draw_scope();
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
    gfx->OMSetRenderTargets(1, targets, nullptr);
    const D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(view_w),
                                  static_cast<float>(view_h), 0.0f, 1.0f};
    gfx->RSSetViewports(1, &viewport);
    // 矩形裁剪走剪裁矩形（零着色器成本）：圆角裁剪交给 SDF
    const math::IntRect scissor = current_clip().rect;
    const D3D11_RECT rect{scissor.x, scissor.y, scissor.x + scissor.width,
                          scissor.y + scissor.height};
    gfx->RSSetScissorRects(1, &rect);
    gfx->RSSetState(pipes.raster_scissor);
    const float blend_factor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    // 亚像素文字的两遍混合需要**换混合状态**（一次绘制内唯一的变量，其余状态相同；
    // 第二处同名调用在 mask 绘制路径上，不接这个开关）。
    ID3D11BlendState* blend_state = opaque ? pipes.opaque : pipes.blend;
    if (blend_override != nullptr) blend_state = blend_override;
    gfx->OMSetBlendState(blend_state, blend_factor, 0xFFFFFFFFU);
    gfx->IASetInputLayout(nullptr);
    gfx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    gfx->VSSetShader(pipes.vs, nullptr, 0);
    gfx->PSSetShader(pipes.ps, nullptr, 0);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(gfx->Map(pipes.constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    std::memcpy(mapped.pData, &params, sizeof(params));
    gfx->Unmap(pipes.constants, 0);
    ID3D11Buffer* buffers[1] = {pipes.constants};
    gfx->VSSetConstantBuffers(0, 1, buffers);
    gfx->PSSetConstantBuffers(0, 1, buffers);

    ID3D11ShaderResourceView* views[1] = {view};
    gfx->PSSetShaderResources(0, 1, views);
    // 路径裁剪的遮罩绑到 t1（未用时绑 null：不绑就会残留上一个 draw 的绑定）
    ID3D11ShaderResourceView* clip_views[1] = {clip_mask};
    gfx->PSSetShaderResources(1, 1, clip_views);
    ID3D11SamplerState* samplers[1] = {point_sample ? pipes.sampler_point : pipes.sampler_linear};
    gfx->PSSetSamplers(0, 1, samplers);
    gfx->Draw(4, 0);
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
    for (const st::math::PathCommand& command : path.commands()) {
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
    // 离屏清空也是**要提交的命令**（同 `clear()` 的教训）：
    // 它不走 `submit_to`，所以得自己标。漏了它 → 遮罩保留上一轮的内容
    // （RT 是池化复用的），阴影会出现“上一帧的残影”。
    const DrawScope gfx = draw_scope();
    gfx->ClearRenderTargetView(targets.rtv_a, zero);
    const float zero_b[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    gfx->ClearRenderTargetView(targets.rtv_b, zero_b);
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
        // ## 每个 pass 前必须把上一 pass **真正执行掉**（2026-10-25 实测）
        //
        // deferred 下 `submit_to` 只是**录制**：命令排在队列里，目标纹理里还是空的。
        // 而下一 pass 要拿它的输出当**采样源**（`read_srv`）—— D3D11 不允许同一资源
        // 同时被绑为 RT 与 SRV，驱动会先把 SRV 解绑，采样时读到的就是**空白**。
        //
        // 实测症状：`draw_shadow` 在 GPU 画布上**一个像素都出不来**
        // （软件画布同一参数出 1468 个），而屏幕上全靠阴影凹造型的卡片就“扁了”。
        //
        // 代价：每个 pass 一次 `FinishCommandList + ExecuteCommandList + Flush`。
        // 阴影是缓存过的（同尺寸同模糊只算一次），这份代价可接受；而“算对了”是底线。
        submit_deferred_now();
        submit_to(write_rtv, blur_params, read_srv, false, true, area.width, area.height);
        read_rtv = write_rtv;
        read_srv = reading_a ? targets.srv_b : targets.srv_a;
      }
    }
    // 最后一个 pass 也必须落地：下面返回的 `srv_a` 会被调用方拿去作**采样源**，
    // 同样不能在“还没执行”的状态下交出去。
    submit_deferred_now();
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
    /// 提交到指定 RT（影子遮罩用；与 `submit` 的区别是目标不是画布自身的 RT）。
  ///
  /// `scissor`：给定时用它代替整个视口作为剪裁矩形（绝对物理坐标）。
  /// 毛玻璃贴回需要它：`vs_main` 会把四边形**外扩 1 像素**，
  /// 而贴回一张采样结果不需要外扩——外扩会把区域边界外侧的像素也写掉。
  /// 实测：`region={20,40,260,160}` 时 x=280（右边界）在软端仍是原始颜色，
  /// GPU 端被写成渐变边缘值（Δ192）。
  void submit_to(ID3D11RenderTargetView* target, const ShaderParams& params,
                 ID3D11ShaderResourceView* view, bool point_sample, bool opaque,
                 int viewport_width, int viewport_height,
                 const math::IntRect* scissor_rect = nullptr) {
    const Pipeline& pipes = pipeline();
    if (!pipes.ok || target == nullptr) return;
    // 记一笔待提交（走 deferred 时才有意义）。
    //
    // `submit_to` 是**离屏绘制**的入口（阴影遮罩、盒式模糊的 ping-pong、
    // 图像处理），它绕过 `submit()` 直接往 `active_context()` 发 draw ——
    // 所以 `submit()` 里的那条 `note_deferred_draw()` 盖不到它。
    //
    // 漏了它的后果比“画面空”更阴：离屏结果不会被产出，而**使用它的那条绘制**
    // 仍会照常提交——于是阴影/模糊“画了但看不见”，看起来像参数没生效。
    const DrawScope gfx = draw_scope();
    ID3D11RenderTargetView* targets[1] = {target};
    gfx->OMSetRenderTargets(1, targets, nullptr);
    const D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(viewport_width),
                                  static_cast<float>(viewport_height), 0.0f, 1.0f};
    gfx->RSSetViewports(1, &viewport);
    // 剪裁矩形默认是整张视口；调用方可以收紧（贴回时用，抵消 `vs_main` 的 1px 外扩）。
    const D3D11_RECT scissor =
        scissor_rect != nullptr
            ? D3D11_RECT{scissor_rect->x, scissor_rect->y,
                         scissor_rect->x + scissor_rect->width,
                         scissor_rect->y + scissor_rect->height}
            : D3D11_RECT{0, 0, viewport_width, viewport_height};
    gfx->RSSetScissorRects(1, &scissor);
    gfx->RSSetState(pipes.raster_scissor);
    const float blend_factor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    gfx->OMSetBlendState(opaque ? pipes.opaque : pipes.blend, blend_factor, 0xFFFFFFFFU);
    gfx->IASetInputLayout(nullptr);
    gfx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    gfx->VSSetShader(pipes.vs, nullptr, 0);
    gfx->PSSetShader(pipes.ps, nullptr, 0);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(gfx->Map(pipes.constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    std::memcpy(mapped.pData, &params, sizeof(params));
    gfx->Unmap(pipes.constants, 0);
    ID3D11Buffer* buffers[1] = {pipes.constants};
    gfx->VSSetConstantBuffers(0, 1, buffers);
    gfx->PSSetConstantBuffers(0, 1, buffers);
    ID3D11ShaderResourceView* views[1] = {view};
    gfx->PSSetShaderResources(0, 1, views);
    ID3D11SamplerState* samplers[1] = {point_sample ? pipes.sampler_point : pipes.sampler_linear};
    gfx->PSSetSamplers(0, 1, samplers);
    gfx->Draw(4, 0);
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

  /// **彩色离屏目标**（毛玻璃用）。
  ///
  /// 为何不能复用 `acquire_shadow_targets`：它建的是 `DXGI_FORMAT_R8_UNORM`——
  /// **只有 R 通道**。阴影遮罩是灰度覆盖率，单通道正好；而毛玻璃模糊的是彩色图像，
  /// 渲染进 R8 目标会把 G/B 直接丢掉（无论着色器算得多对）。
  /// 实测症状：暗色弹窗右侧一条“红绿色块”竖带（R 波动、G/B 几乎不变）。
  auto acquire_color_targets(int width, int height, ShadowTargets& targets) -> bool {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = kFormat;   // 与画布同格式（RGBA8），才能原样贴回
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
    // **目标纹理的 SRV**：毛玻璃（`blur_region`）要拿画布自己当采样源。
    // `BindFlags` 里本来就有 `BIND_SHADER_RESOURCE`（三维合成那条路要用），
    // 但视图一直没建——不建就采不了。
    if (FAILED(device_->CreateShaderResourceView(target_, nullptr, &target_srv_))) {
      ST_LOG_WARN("GPU 渲染：创建目标纹理视图失败（毛玻璃不可用）");
    }
  }

  void release_resources() {
    if (target_srv_ != nullptr) {
      target_srv_->Release();
      target_srv_ = nullptr;
    }
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
    // 两个视图一起清：只清 uint32 那份会让 `ensure_readback` 的判据与
    // 实际可用的数据不一致（`readback_rgba_` 有值却被当成"没回读过"）。
    readback_.clear();
    readback_rgba_.clear();
    readback_ready_ = false;
  }

  /// 丢掉阴影缓存（目标纹理按尺寸分配，改尺寸后旧目标尺寸不对）。
  void release_shadow_cache() {
    for (auto& entry : shadow_cache_) release_shadow_targets(entry.targets);
    shadow_cache_.clear();
  }

  /// 从 GPU 拉回像素（`0xRRGGBBAA` 预乘，与软件画布同一布局）。
  auto ensure_readback() -> bool {
    // ## 整段回读都在同一把锁内（2026-10-25：2 路必卡的根因）
    //
    // 立即上下文是**进程共享**的。多路导出时每个线程都在这段里
    // `ExecuteCommandList` / `CopyResource` / `Map(READ)` ——
    // 后两者会**阻塞等 GPU**，而 GPU 队列里还排着别的线程刚塞进去的命令。
    // 两个线程各持一半上下文使用权互相等 → 死锁。
    //
    // 实测症状：1 路 16.1s 正常；2 路以上进程 83 秒只耗 1 秒 CPU（纯等）。
    // 单线程测不出来，而多路正是导出场景 —— 这类 bug 只能靠"多路必卡"这个特征认。
    //
    // 锁范围取"整段回读"而不是逐条：中间还有 `flush_glyph_batch` 与
    // `CreateTexture2D` 也碰上下文，分段加锁会留下更窄的死锁窗口。
    const std::scoped_lock readback_lock(g_context_mutex());
    if (target_ == nullptr) return false;
    // **回读前必须 flush 字形批次**：否则“本轮攒着还没发的字形”不会被画进目标，
    // 截图/像素断言看到的就会是“文字消失了”（而屏幕上其实是好的）。
    //
    // ⚠ **顺序曾经是反的**（2026-10-25 二次排查）：`submit_deferred_locked()` 写在
    // `flush_glyph_batch()` **之前**，于是：先提交了（那时字形还没录进去）→ 再录字形 →
    // 字形落在**未提交**状态里。于是回读到的画面**文字全部缺失**。
    //
    // 这个错不容易看出来，因为屏幕上的文字是对的（上屏时 `present` 会再提交一次）——
    // 只有“截图/导出”会缺字。它能让像素断言假阳/假阴，而单看屏幕无法发现。
    //
    // 教训：提交点必须在“本轮所有绘制都录完”之后，包括那些**攒批的**。
    flush_glyph_batch();
    submit_deferred_locked();
    // 判"已有有效回读"看的是 **`readback_rgba_`**（导出路径的权威副本）。
    // 之前判 `readback_`（uint32 视图），而它现在是**惰性生成**的 ——
    // 初值为空，于是条件永假、每帧重复回读；更糟的是某些路径下
    // `readback_` 空而 `readback_rgba_` 有效，早期 return 后调用方拿到空 vector
    // （实测症状：`gpu_clear_matches_software_canvas_exactly` 报 `0 != 768`）。
    // ## 判"已有有效回读"用**尺寸**，不用 `readback_dirty_`
    //
    // `readback_dirty_` 只在绘制时置 true、回读后置 false。但一块画布可能：
    //   · 刚建好（dirty=false，readback_rgba_ 为空）
    //   · 被 release_readback 清过（dirty 仍为 false）
    // 这两种情况下"不脏"不等于"有数据"。实测症状：首次 `pixels()` 返回空 span，
    // 而紧接着的第二次调用正常（第一次把回读补上了）——这种"第一次为空"
    // 在导出路径上表现为**首帧全黑**，极难查。
    const std::size_t expected =
        static_cast<std::size_t>(physical_width_) * static_cast<std::size_t>(physical_height_) * 4U;
    if (!readback_dirty_ && readback_rgba_.size() == expected) return true;
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
    // ## 从 CopyResource 到 Unmap 必须在同一把锁内（2026-10-25 2 路必卡的根因）
    //
    // 立即上下文是**进程共享**的。2 个线程各自 `CopyResource` 到**自己的** staging
    // 纹理、再 `Map` 自己的 —— 表面看互不相干，但驱动在内部是**串行处理**同一上下文的：
    // 线程 A 的 `Map(READ)` 在等 GPU 完成，而 GPU 队列里排着线程 B 刚塞进去的命令，
    // 两者互相等对方的上下文使用权 → 死锁（实测：进程 83 秒只耗 1 秒 CPU）。
    //
    // 单线程 1 路时不存在竞争，所以这个 bug 只在多路出现 —— 而多路正是导出场景。
    context_->CopyResource(staging_, target_);
    context_->Flush();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(staging_, 0, D3D11_MAP_READ, 0, &mapped))) {
      ST_LOG_ERROR("GPU 渲染：回读映射失败");
      return false;
    }
    // ## 两个视图，按需取用（2026-10-25 性能）
    //
    // staging 纹理的内存布局**本来就是 R,G,B,A 字节序**（`kFormat = R8G8B8A8_UNORM`），
    // 而导出路径要的 `to_rgba8()` 也正好是这个顺序 —— 所以这里存两份：
    //
    //   · `readback_rgba_`：**按行 memcpy** 的原始字节，给导出用（零转换）
    //   · `readback_`：打包成 0xRRGGBBAA 的 uint32，给 `pixels()` 用（软件画布口径）
    //
    // 之前只存后者，于是 `to_rgba8()` 要先把 uint32 拆回字节 —— 实测那次拆分
    // **5.32ms/帧**（1080p 200 万像素，纯位运算）。导出每帧都要走这条路，
    // 而导出的瓶颈恰恰在这里。
    const auto row_bytes = static_cast<std::size_t>(physical_width_) * 4U;
    const auto total_pixels = static_cast<std::size_t>(physical_width_) *
                              static_cast<std::size_t>(physical_height_);
    readback_rgba_.resize(total_pixels * 4U);
    const auto* source = static_cast<const std::uint8_t*>(mapped.pData);
    for (int y = 0; y < physical_height_; ++y) {
      const std::uint8_t* row = source + static_cast<std::size_t>(y) * mapped.RowPitch;
      std::memcpy(readback_rgba_.data() + static_cast<std::size_t>(y) * row_bytes, row, row_bytes);
    }
    // ## 两份视图：打包视图**可按需省略**（2026-10-31）
    //
    // `readback_rgba_` 给导出（零转换）；`readback_` 给 `pixels()` 口径。
    // 后者要多一遍位运算（1080p 约 5.3ms，占整帧 CPU 的 8~15%）。
    //
    // 曾经试过把它改成“按需惰性生成”，结果 `pixels()` 首次返回空 span、
    // 第二次才正常——在导出路径上表现为**首帧全黑**，极难查，已回退。
    // 那个错不在“省”而在“不确定何时有”。
    //
    // 现在改成**显式提示**：调用方在取 RGBA 之前可以说“这次只要 RGBA”
    //（`skip_packed_readback_`），于是本帧跳过打包；`pixels()` 若真的
    // 被调到，它自己会把打包补上（见 `pixels()` 内的按需生成），
    // 所以**不会出现“有 RGBA 但 pixels() 空”的不确定态**。
    //
    // 导出路径（`to_rgba8` + 帧统计）确实不碰 `pixels()`，所以这是纯收益。
    const bool skip_packed = skip_packed_readback_;
    skip_packed_readback_ = false;
    if (!skip_packed) {
      rebuild_packed_readback();
    } else {
      packed_readback_stale_ = true;   // 标记：真有人要 pixels() 时再算
    }
    readback_ready_ = true;
    context_->Unmap(staging_, 0);
    readback_dirty_ = false;
    return true;
  }

  /// 把 `readback_rgba_` 打包成 `0xRRGGBBAA` 的 uint32 视图（`pixels()` 口径）。
  void rebuild_packed_readback() {
    const auto total_pixels = static_cast<std::size_t>(physical_width_) *
                              static_cast<std::size_t>(physical_height_);
    readback_.resize(total_pixels);
    for (std::size_t index = 0; index < total_pixels; ++index) {
      const std::uint8_t* src = readback_rgba_.data() + index * 4U;
      readback_[index] = (static_cast<std::uint32_t>(src[0]) << 24U) |
                         (static_cast<std::uint32_t>(src[1]) << 16U) |
                         (static_cast<std::uint32_t>(src[2]) << 8U) |
                         static_cast<std::uint32_t>(src[3]);
    }
    packed_readback_stale_ = false;
  }

  /// 供 `set_pixel` 用的可写像素（写完由调用方标脏并整体回传——M2 只保证接口语义正确）。
  auto mutable_pixels() -> std::span<std::uint32_t> {
    (void)ensure_readback();
    return {readback_.data(), readback_.size()};
  }

  ID3D11Device* device_{nullptr};
  ID3D11DeviceContext* context_{nullptr};
  /// 回读的**原始 RGBA 字节**（staging 的布局，按行 memcpy）。
  std::vector<std::uint8_t> readback_rgba_{};
  /// `readback_`（uint32 打包视图）是否与 `readback_rgba_` 同步。
  bool readback_ready_{false};
  /// 本帧回读**跳过**打包视图（导出路径显式提示；见 `ensure_readback`）。
  bool skip_packed_readback_{false};
  /// 打包视图未生成/已过期（`pixels()` 被调到时按需补）。
  bool packed_readback_stale_{false};
  ID3D11Texture2D* target_{nullptr};
  ID3D11RenderTargetView* rtv_{nullptr};
  /// 目标纹理的 SRV（毛玻璃要拿画布自己当采样源）。
  ID3D11ShaderResourceView* target_srv_{nullptr};
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
    // ## 上屏前**必须先把 deferred 里录制的命令提交掉**（2026-10-25 黑屏的真因）
    //
    // 自从`每画布一个 deferred context`（commit 4de2bcbe）之后，所有绘制命令都先录进
    // **命令列表**，只有 `FinishCommandList` + `ExecuteCommandList` 之后才真正落到
    // 
    // `target_` 纹理上。而 `present()` 原先直接 `CopyResource(backbuffer_, target())` ——
    // 拷到的是**还没执行**的空纹理，于是屏幕全黑。
    //
    // 为什么截图/导出看不出这个问题：它们走 `ensure_readback()`，而那里**开头就是**
    // `submit_deferred_locked()`。于是形成了一条极难认的错位：
    // **截图正常、屏幕全黑**。排查时"应用内截图正常"反而会成为误导证据。
    //
    // 锁：`submit_deferred_locked` 内部注释要求调用方持有 `g_context_mutex()`；
    // `CopyResource` + `Present` 也碰立即上下文，所以整段一起进锁。
    const std::scoped_lock present_lock(g_context_mutex());
    // ## 上屏前**自己提交**（而不是要求调用方先提交）
    //
    // 旧实现这里是“断言 + 报错跳过”，前提是“调用方一定先提交过”。那个契约
    // 没有对外手段：`Surface` 不暴露提交入口，所以调用方**无法**满足它。
    // 实测代价（相期启动，2026-10-27）：分配画布时的 `surface_->clear(...)`
    // 记了一笔未提交绘制，紧接着建 presenter、`show_when_ready()` 调 present()
    // → 断言为真 → **跳过上屏并报错** → 退回 GDI blit。8 秒内重复 5 次，
    // **GPU 路径实际从未生效**（每帧都走 GPU→CPU 回读），首帧多花 ~440 ms，
    // 窗口已显示却不响应 → 鼠标转圈。
    //
    // 正确做法：`present()` 的语义就是“把画布现在的样子推上屏”，它应当
    // **自己保证命令已执行**。提交后再断言——那时若还有未提交命令，才是
    // 真正的异常（`FinishCommandList` 失败 / 设备丢失）。
    gpu_canvas->submit_deferred_locked();
    if (gpu_canvas->has_uncommitted_draws()) {
      // 提交完仍有：说明 `FinishCommandList` 失败（设备丢失等），不能静默上屏
      ST_LOG_ERROR("GPU 呈现：提交后仍有未提交的绘制命令（FinishCommandList 失败）");
      return unexpected(ErrorCode::Io, "命令列表提交失败（设备可能丢失），本帧跳过上屏");
    }
    // 关键一步：GPU → GPU 拷贝，**不经过 CPU**（这就是 M5 的全部价值）
    context->CopyResource(backbuffer_, gpu_canvas->target());
    const HRESULT result = swapchain_->Present(0, 0);
    if (FAILED(result)) {
      return unexpected(ErrorCode::Io, std::format("Present 失败（0x{:08X}）",
                                                   static_cast<unsigned>(result)));
    }
    return ok();
  }

  /// **局部上屏**：`dirty` 重载。
  ///
  /// ⚠ **当前实现一律回退整窗拷贝**，这是有意的：局部拷贝的正确性依赖「后备缓冲
  /// 保留上一帧内容」，而本 swapchain 用的是 `DXGI_SWAP_EFFECT_DISCARD`
  ///（见 `resize()`）——它的 `Present` 之后后备缓冲内容**未定义**，驱动可以
  /// 随意丢弃/覆写。于是“只拷损坏区”会让未拷贝的绝大部分区域变成**黑块**。
  ///
  /// 实测（Windows + NVIDIA L20-6Q，`--renderer gpu`）：启用 `CopySubresourceRegion`
  /// 局部拷贝后界面频繁出现大片黑区；同一份代码在纯 CPU 机器上正常——那里走
  /// 软件画布（`Canvas::supports_partial_repaint()` 才是真的可用）。
  ///
  /// 要让这里真的只搬损坏区，前提是把 swapchain 换成**保留型**
  ///（`DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL`），并按 FLIP 模型重做上屏路径
  ///（FLIP 的后备缓冲只读，不能当 `CopySubresourceRegion` 的目标）。
  /// 在那之前保留这个重载只为接口完整：调用方传区域也不会画错。
  auto present(Surface& canvas, int width, int height,
               const math::IntRect& dirty) -> Status override {
    (void)dirty;
    return present(canvas, width, height);
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
  float4 g_tune;       // x = 漫反射权重, y = 边缘光, z = 曝光, w = 是否采样纹理（>0.5 采）
 float4 g_opacity;    // x=不透明度 y=metallic z=roughness w=specular
 float4 g_eye;        // xyz = 相机世界位置（算逐像素视向）
 // ## 环境反射（IBL）——与 C++ 侧 `Lighting::env_*` 逐字对应
 //   g_env.xyz        = 天空色（反射向量朝上时）
 //   g_env.w          = 环境反射强度（0 = 关闭，与旧行为逐位相同）
 //   g_env_ground.xyz = 地面色（反射向量朝下时）
 //   g_env_ground.w   = 粗糙度向均值弥散的强度
 float4 g_env;
 float4 g_env_ground;
 // ## 自发光——与 `Material::emission` / `emissive_color` 逐字对应
 //   g_emissive.xyz = 自发光颜色（线性空间）
 //   g_emissive.w   = 自发光强度（0 = 关闭，与旧行为逐位相同）
 //
 // ⚠ GPU 腿原本**完全没有实现自发光**：软件腿有、GPU 腿没有。
 // 实测症状：写 `emission: 5.0` 在 GPU 腿上一整帧像素不变（与不写逐字节相同），
 // 而探针确认材质值已正确传到 `set_material`——就是着色器里没这段。
 float4 g_emissive;
 // ## 材质基色——与 `Material::base_color` 逐字对应
 //   g_base.xyz = 基色（线性）
 //   g_base.w   = 是否启用（0 = 用顶点色）
 //
 // ⚠ GPU 腿原本**完全没有 base_color**：软件腿的 `shade_surface` 在读它，
 // GPU 腿只读顶点色。后果：glTF 白模（顶点色恒为白）在剧本里写
 // `"color": "#c8895c"` 完全不生效，永远是白的。
 // 这是「两条腿功能不同步」的又一个实例（前三次：环境反射、自发光、纹理标志）。
 float4 g_base;
 // ## 环境探针（2026-11-05 新增）——与 `Lighting::env_probes` 逐字对应
 //
 // 只靠 g_env / g_env_ground（天/地两色按 r.y 插值）有个硬限制：
 // **同一平面上的金属反射完全一样**（法线相同 → reflect 只差一点 Y）。
 //
 // 实测（齿轮场景）：env_strength 0.45 → 3.5 只把主体均值从 109 推到 152，
 // 而暗部从 10.5% 掉到 0%——“挖了一层灰蜡”，不是金属。
 //
 // 真 three.js 的金属是“大面积暗 + 少量亮”，因为环境**有空间结构**。
 //
 // 每个探针占一个 float4（xyz = 方向，w = 该探针是否存在），
 // 颜色另占一个 float4（xyz = 颜色，w 备用）。
 // `g_probe_tune.x` = 探针个数（0 = 走旧的天地插值，逐位不变）。
 // `g_probe_tune.y` = 锐利度（权重 = max(0,dot)^sharpness）。
 float4 g_probe_dirs[4];
 float4 g_probe_colors[4];
   float4 g_probe_tune;
  // ## 色调映射算子（2026-11-06 新增）
  // `g_tune2.x` = ToneMap 枚举值：0=None / 1=Reinhard / 2=Aces / 3=Filmic /
  // 4=Agx / 5=AcesThree。
  //
  // 为何必须加它：GPU 腿原来**硬编码 ACES**，`set_lighting()` 里的 `tone_map`
  // 传下来就被丢掉了——剧本写 `tone_map: "none"` 时两腿输出逐像素相同
  // （实测 rgb(4,6,12) vs rgb(4,6,12)）。
  float4 g_tune2;
  // ## 主光颜色（2026-11-06 新增，模型统一时发现缺的）
  //
  // 旧着色器把 `g_light.xyz` 当光方向用（指向光源），却没有地方放**颜色**。
  // 物理 BRDF 需要 `irradiance = saturate(n·l) × color × intensity`，
  // 而颜色不可省（暖光/冷光在两条腿上必须一致）。
  //
  // 实测后果（当时误写成 `g_light.rgb`）：GPU 腿输出 `rgb(0,0,76)`——
  // 红/绿两通道拿到的是**光方向的 x/y**（≈0），蓝通道拿到 z（≈1）。
  // 单看任一条腿都不报错，只有两条腿并排比才看得见。
  float4 g_light_color;
  // ## 额外灯光（2026-11-06 新增）：与 C++ 侧 `Scene3dParams::extra_*` 逐字节对应
  //
  // 每云灯：方向/位置 xyz + w=1(存在) ；颜色 xyz ；参数 x=kind y=intensity z=range w=spot外角
  // `g_extra_count.x == 0` 时整段跳过（与旧行为逐位相同）。
  float4 g_extra_dirs[3];
  float4 g_extra_colors[3];
  float4 g_extra_params[3];
  float4 g_extra_count;
  // ## 阴影贴图（2026-11-07 新增）：与软件腿的 `ShadowMap` 同一语义
  //
  // g_shadow_vp：光源视角的 view-projection（世界 → 阴影贴图 NDC）
  // g_shadow_tune：x = strength（0 = 关闭）、y = bias、z = soft_radius、w = 分辨率
  //
  // `g_shadow_tune.x == 0` 时 HLSL 整段跳过 → 与旧行为逐位相同。
  // 这条对回归很重要：没开阴影的场景必须一字节不变。
  float4x4 g_shadow_vp;
  float4 g_shadow_tune;
  // ## 半球环境光（2026-11-07 新增）：与软件腿的 `Lighting::hemisphere_*` 同一语义
  //
  // three 的 `HemisphereLight` 是**逐像素按法线插值**的：
  //
  //     irradiance = mix(ground, sky, n.y * 0.5 + 0.5) * intensity
  //
  // 旧版 GPU 腿根本没有这个概念（软件腿也只是压成标量），实测与浏览器
  // 差 6.77 级。现在两条腿都要做方向插值。
  //
  //   g_hemi_sky.xyz    = 天空色
  //   g_hemi_sky.w      = 强度（**0 = 关闭**，与旧行为逐位相同）
  //   g_hemi_ground.xyz = 地面色
  float4 g_hemi_sky;
  float4 g_hemi_ground;
  // ## 环境光颜色（2026-11-07 新增）
  //
  // 软件腿一直有 `Lighting::ambient_color`（逐通道），而 GPU 腿把
  // `g_light.w`（标量 ambient）直接乘基色——**颜色丢了**。
  // 冷/暖环境光在两条腿上会长得不一样。
  //
  // 默认 (1,1,1)：不写颜色的场景逐位不变。
  float4 g_ambient_color;
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
  float3 world : TEXCOORD3;   // 世界位置：算逐像素视向（GGX 的 Fresnel 依赖它）
};

// 纹理（RGBA8，非预乘）：无 UV 的网格不采它，行为与以前完全一致
Texture2D g_scene_texture : register(t0);
SamplerState g_scene_sampler : register(s0);
// 阴影贴图（R32_FLOAT 深度）。
//
// ⚠ 即使不采样也必须声明：寄存器 t1 绑定的是**纹理对象**，没开阴影时
// 它是一张 1×1 的全 1 占位图（不是 nullptr）——绑 nullptr 会在部分驱动上
// 让采样返回 0，而那会被当成“全部在阴影里”。
Texture2D<float> g_shadow_map : register(t1);
/// ⚠ 必须是 `SamplerComparisonState` 而不是 `SamplerState`。
///
/// `SampleCmpLevelZero` 的重载只接受 `SamplerComparisonState`。写成
/// `SamplerState` 时报 `X3013: no matching ... intrinsic method`，
/// 而那在运行时只表现为**GPU 腿静默不可用、退回软件腿**——
/// 实测就这样骗了一个小时：两腿“完全一致”，实际上 GPU 腿从未跑起来。
SamplerComparisonState g_shadow_sampler : register(s1);

/// 某世界点是否在阴影里（PCF 柔化），返回 0（全亮）~ 1（全黑）。
///
/// ## 为何用比较式采样而不是手动比较
///
/// `SampleCmpLevelZero` 让硬件做“采样值 vs 参考值”的比较并返回 0/1，
/// 配合线性过滤就是免费的 2×2 PCF。手写比较要自己做多次采样与双线性插值，
/// 而且无法利用硬件的 PCF 单元。
///
/// ## 为何需要 `normal`：斜率缩放偏置（slope-scale bias，2026-11-07）
///
/// 固定偏置下，球面会出现**同心圆环**：球面法线与光线夹角小时，单个纹素
/// 跨越的表面深度变化大，固定偏置压不住 → 自己遮自己。而地板（法线与光
/// 几乎垂直）反而没事。实测（`parity_showdown.py` 的金属球）非常明显。
///
/// 标准做法：`bias *= 1 + tan(acos(n·l))`。其中 `n·l` 用 `g_light.xyz`
/// （它是指向光源的方向）与法线点积。
float shadow_factor(float3 world, float3 normal) {
  if (g_shadow_tune.x <= 0.0) return 0.0;
  const float4 clip = mul(g_shadow_vp, float4(world, 1.0));
  if (clip.w <= 0.0) return 0.0;
  const float3 ndc = clip.xyz / clip.w;
  // NDC [-1,1] → 纹理 [0,1]，且 y 翻一下（D3D 的 NDC y 向上，纹理 v 向下）
  const float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
  if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return 0.0;
  // ## 斜率缩放偏置
  //
  // `g_light.xyz` 是**指向光源**的方向（与软件腿的 `light_dir_` 同义），
  // 所以 n·l > 0 表示面向光源。掠射角（n·l → 0）时 tan 爆炸，限幅 8 倍。
  const float3 light_dir = normalize(g_light.xyz);
  const float ndl = clamp(dot(normalize(normal), light_dir), 0.0, 1.0);
  const float cos_theta = max(ndl, 0.05);
  const float tan_factor = sqrt(max(0.0, 1.0 - cos_theta * cos_theta)) / cos_theta;
  const float slope_scale = min(1.0 + tan_factor, 8.0);
  const float reference = ndc.z - g_shadow_tune.y * slope_scale;
  const float radius = g_shadow_tune.z;
  // ## 为何是四参数重载
  //
  // `Texture2D<T>` 的 `SampleCmpLevelZero` 有两个重载：
  //   ① `(SamplerComparisonState, float2, float compare)` —— 3 参数
  //   ② `(SamplerComparisonState, float2, float compare, int2 offset)` —— 4 参数
  //
  // **只有 4 参数那个在 `ps_4_0` 下可用**。3 参数写法报
  // `X3013: 'SampleCmpLevelZero': no matching 3 parameter intrinsic method`，
  // 而 D3DCompile 失败在这里只表现为“GPU 腿不可用 → 静默退回软件腿”——
  // 实测就这样糊了一个小时：两条腿“完全一致”，实际上 GPU 腿从未跑起来。
  //
  // 所以这里统一用 4 参数 + `int2(0,0)` 偏移。
  const int2 zero_offset = int2(0, 0);
  if (radius <= 0.5) {
    // 硬阴影：一次比较
    return 1.0 - g_shadow_map.SampleCmpLevelZero(g_shadow_sampler, uv, reference, zero_offset);
  }
  // PCF：在 radius 个纹素内取 4 个对角偏移的平均（加上线性过滤 = 有效 4×4）
  const float2 texel = 1.0 / g_shadow_tune.w;
  const float2 offset = texel * radius * 0.5;
  float lit = 0.0;
  lit += g_shadow_map.SampleCmpLevelZero(g_shadow_sampler, uv + float2(-offset.x, -offset.y), reference, zero_offset);
  lit += g_shadow_map.SampleCmpLevelZero(g_shadow_sampler, uv + float2( offset.x, -offset.y), reference, zero_offset);
  lit += g_shadow_map.SampleCmpLevelZero(g_shadow_sampler, uv + float2(-offset.x,  offset.y), reference, zero_offset);
  lit += g_shadow_map.SampleCmpLevelZero(g_shadow_sampler, uv + float2( offset.x,  offset.y), reference, zero_offset);
  return 1.0 - lit * 0.25;
}

VsOut vs_scene3d(VsIn input) {
  VsOut o;
  // 矩阵两边约定**恰好一致**：`math::Mat4` 是列主序（m[column*4+row]），
  // D3D 常数缓冲默认按列读——**不需要转置**。转置一次就对、两次也"看着对"
  // （整个场景绕中心镜像），只有拿参照图比才看得出。
  o.position = mul(g_mvp, float4(input.position, 1.0));
  // 法线用模型矩阵的 3x3（与软件腿一致：不取逆转置——本仓的模型矩阵只有
  // 旋转/等比缩放/平移，逆转置与直接变换只差一个标量因子，归一化后相同）
  o.normal = mul((float3x3)g_model, input.normal);
  // 世界位置：`model * position`（含平移，所以用 4x4）
  o.world = mul(g_model, float4(input.position, 1.0)).xyz;
  o.color = input.color;
  o.uv = input.uv;
  return o;
}

/// 主着色体。
///
/// ## 为何拆成一个具名函数
///
/// 主入口还是要叫 `ps_scene3d`（`CreatePixelShader` 按名字取）。
/// 而 G-Buffer 的两个输出（世界位置/法线）需要复用**同一个**着色计算——
/// 若它们各写一遍，会出现“主画面一套 BRDF、AO 用的法线是另一套”的错位。
/// 拆成具名函数后，两处调的是同一份代码。
float4 ps_scene3d_body(VsOut input) : SV_Target {
  // ⚠ 本段是 `st::raster::shade_terms` 的**逐字翻译**。改一边必须改另一边
  //   （回归：`tests/raster_scene3d_two_legs_test.cpp` 比两条腿的同像素输出）。
  // ## 着色：统一物理内核（与 C++ 侧 `pbr_shade` 逐式对应）
  //
  // 这里曾经有**两套**公式（Legacy 的 `albedo*(ambient+n·l·diffuse+rim)` 与 three 同构），
  // 靠 `g_tune2.y` 选。统一之后只剩一套，分支整体消失——这正是“两条腿 × 两种模型”
  // 那 4 种组合被压缩成 1 种的收益。
  //
  // ⚠ 本段是 `st::raster::pbr_shade` 的**逐字翻译**。改一边必须改另一边
  //   （回归：`tests/raster_scene3d_two_legs_test.cpp` 比两条腿的同像素输出）。
  float3 n = normalize(input.normal);
  float3 light = normalize(g_light.xyz);
  float metallic = g_opacity.y;
  float roughness = g_opacity.z;
  float diffuse_weight = 1.0 - metallic;
  // g_tune.w > 0.5 = 本网格带 UV 且已设纹理 → 采样并与顶点色相乘
  // （与软件腿同一口径：顶点色当基础色/染色，纹理提供图案）
  // 基色：剧本指定时**取代**顶点色（与软件腿 `material_.base_color_set` 同语义）
  float3 base = (g_base.w > 0.5) ? g_base.xyz : input.color;
  if (g_tune.w > 0.5) {
    float3 texel = g_scene_texture.Sample(g_scene_sampler, input.uv).rgb;
    // ## 纹理字节是 sRGB，必须解码到线性（与 C++ 侧 `sample_texture` 逐字对应）
    // 不解码会让纹理偏亮，且与顶点色（已解码）口径不一致。
    float3 lo = texel * (1.0 / 12.92);
    float3 hi = pow(max((texel + 0.055) / 1.055, 1e-8), 2.4);
    texel = (texel <= 0.04045) ? lo : hi;
    base = input.color * texel;
  }
  // ## GGX 高光（与 C++ 侧 `ggx_specular` 逐字对应）
  //
  // 为何用 GGX 而不是 Blinn-Phong：后者的高光形状不随角度变化，掠射角下会崩
  // 成一束硬边；GGX 是微表面模型，粗糙度直接决定高光宽度与拖尾形状。
  // ## 直接光（与 `pbr_direct_light` 逐式对应）
  //
  //   irradiance     = saturate(n·l) × lightColor × intensity
  //   directDiffuse += irradiance · diffuseColor/π
  //   directSpecular+= irradiance · D·V·F   （V 已含 1/(4·n·l·n·v)）
  float3 lit = float3(0.0, 0.0, 0.0);
  // ## 材质量必须声明在**函数作用域**（不只是外层块内）
  //
  // `a2` / `f0` 被三处使用：直接光高光、额外灯光循环、IBL 高光。
  // 它们曾写在直接光那个 `if` 块里，而 IBL 高光在那块之外——于是 HLSL 编译报
  // `error X3004: undeclared identifier`，整份着色器失败。
  //
  // 这个缺陷最阴的地方：编译失败只打一行日志，随后**静默回退软件腿**。
  // 功能看着都对（同一份场景、同一套参数，软件腿甚至更准），代价是单帧从
  // 亳秒级变成**秒级**（实测 1100×760 单帧 paint 14093 ms）——
  // 用户看到的只是“窗口一片空白、卡得动不了”，而不会想到是着色器没编过。
  // 回归：`scene3d_gpu_shader_compiles_and_renders`（强制 gpu 腿并断言真出图）。
  const float3 f0 = float3(0.04, 0.04, 0.04) + (base - 0.04) * metallic;
  const float roughness_clamped = clamp(max(roughness, 0.0525), 0.0, 1.0);
  float alpha = roughness_clamped * roughness_clamped;
  const float a2 = alpha * alpha;
  {
    float3 view_dir = normalize(g_eye.xyz - input.world);
    float n_dot_v = max(dot(n, view_dir), 0.0);
    float n_dot_l = max(dot(n, light), 0.0);
    float3 diffuse_color = base * diffuse_weight;
    float3 irradiance = n_dot_l * g_light_color.rgb * g_tune.x;
    lit += irradiance * diffuse_color * 0.31830988618;
    if (n_dot_l > 0.0 && n_dot_v > 0.0) {
      float3 h = normalize(view_dir + light);
      float n_dot_h = max(dot(n, h), 0.0);
      float v_dot_h = max(dot(view_dir, h), 0.0);
      float denom_d = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
      float D = 0.31830988618 * a2 / (denom_d * denom_d);
      // V_GGX_SmithCorrelated：0.5 / max(gv+gl, EPSILON)
      float gv = n_dot_l * sqrt(a2 + (1.0 - a2) * n_dot_v * n_dot_v);
      float gl = n_dot_v * sqrt(a2 + (1.0 - a2) * n_dot_l * n_dot_l);
      float V = 0.5 / max(gv + gl, 1e-7);
      // F_Schlick 的 exp2 拟合（不是 (1-v·h)^5）
      float fc = exp2((-5.55473 * v_dot_h - 6.98316) * v_dot_h);
      float3 F = f0 * (1.0 - fc) + fc;
      lit += irradiance * D * V * F;
    }
    // ## 均匀环境光（AmbientLight / Blender 的 world 均匀分量）
    // 走 irradiance × diffuseColor/π —— 不是 albedo × ambient（后者偏亮 π 倍）。
    //
    // 加上半球光的**方向插值**（2026-11-07）：
    //
    //     hemi = mix(ground, sky, n.y * 0.5 + 0.5) * intensity
    //
    // `g_hemi_sky.w == 0` 时整段为 0，与旧行为**逐位相同**（那条对回归很重要）。
    float3 hemi_irradiance = float3(0.0, 0.0, 0.0);
    if (g_hemi_sky.w > 0.0) {
      const float t = saturate(n.y * 0.5 + 0.5);
      hemi_irradiance = lerp(g_hemi_ground.xyz, g_hemi_sky.xyz, t) * g_hemi_sky.w;
    }
    lit += (g_light.w * g_ambient_color.rgb + hemi_irradiance) * diffuse_color * 0.31830988618;

    // ## 额外灯光（2026-11-06）：**走同一个 BRDF**
    //
    // 与软件腿的 `pbr_shade` 里的 extra_lights 循环逐式对应：
    // 不做“主光一套/附加灯另一套”（旧模型就是那么干的，于是点光/聚光没有高光）。
    int extra_n = (int)(g_extra_count.x + 0.5);
    for (int ei = 0; ei < extra_n && ei < 3; ++ei) {
      float3 e_pos = g_extra_dirs[ei].xyz;
      float3 e_color = g_extra_colors[ei].xyz;
      float e_kind = g_extra_params[ei].x;
      float e_intensity = g_extra_params[ei].y;
      float e_range = g_extra_params[ei].z;
      float e_spot_outer = g_extra_params[ei].w;
      float3 e_dir;
      float e_att = 1.0;
      if (e_kind < 0.5) {
        // 平行光：方向固定（`g_extra_dirs[ei].xyz` 存的是**指向光源**）
        e_dir = normalize(e_pos);
      } else {
        // 点光 / 聚光：方向随像素变
        float3 to_light = e_pos - input.world;
        float dist = max(length(to_light), 1e-6);
        e_dir = to_light / dist;
        e_att = 1.0 / max(dist * dist, 0.01);
        if (e_range > 0.0) {
          float ratio = dist / e_range;
          float window = 1.0 - pow(min(ratio, 1.0), 4.0);
          e_att *= window * window;
        }
        if (e_kind > 1.5 && e_spot_outer > 0.0) {
          // 聚光锥：这里只做外锥硬边界（软件腿的内锥平滑由
          // `spot_inner_degrees` 控制，两者都用 smoothstep 同式）
          float3 fwd = normalize(input.world - e_pos);
          float cos_a = dot(fwd, e_dir);
          float outer = cos(radians(e_spot_outer));
          e_att *= smoothstep(outer, 1.0, cos_a);
        }
      }
      float e_n_dot_l = max(dot(n, e_dir), 0.0);
      float3 e_irr = e_n_dot_l * e_color * e_intensity * e_att;
      lit += e_irr * diffuse_color * 0.31830988618;
      if (e_n_dot_l > 0.0 && n_dot_v > 0.0) {
        float3 hh = normalize(view_dir + e_dir);
        float nh = max(dot(n, hh), 0.0);
        float vh = max(dot(view_dir, hh), 0.0);
        float d2 = nh * nh * (a2 - 1.0) + 1.0;
        float De = 0.31830988618 * a2 / (d2 * d2);
        float gve = e_n_dot_l * sqrt(a2 + (1.0 - a2) * n_dot_v * n_dot_v);
        float gle = n_dot_v * sqrt(a2 + (1.0 - a2) * e_n_dot_l * e_n_dot_l);
        float Ve = 0.5 / max(gve + gle, 1e-7);
        float fce = exp2((-5.55473 * vh - 6.98316) * vh);
        float3 Fe = f0 * (1.0 - fce) + fce;
        lit += e_irr * De * Ve * Fe;
      }
    }
  }

  // ## IBL（环境光照）：漫反射 + 高光两个分量**必须分开**
  //
  // 与 C++ 侧 `pbr_ibl_diffuse` / `pbr_ibl_specular` / `pbr_env_brdf` 逐字对应。
  //
  // 为何分开：漫反射要的是“法线周围的能量”，高光要的是“反射方向周围的能量”。
  // 用一个采样糊两者，结果是金属的高光跟着法线走（看起来像塑料）而不是跟着视角走。
  if (g_env.w > 0.0 && g_probe_tune.x > 0.0) {
    float3 view_dir = normalize(g_eye.xyz - input.world);
    float n_dot_v = max(dot(n, view_dir), 0.0);
    float3 diffuse_color = base * diffuse_weight;

    // ① 漫反射：余弦加权（sharpness = 1）
    float3 e_diffuse = float3(0.0, 0.0, 0.0);
    float sum_w = 0.0;
    for (int pi = 0; pi < 4; ++pi) {
      if (g_probe_dirs[pi].w <= 0.0) continue;
      float3 dir = normalize(g_probe_dirs[pi].xyz);
      float cos_angle = dot(n, dir);
      if (cos_angle <= 0.0) continue;
      sum_w += cos_angle;
      e_diffuse += g_probe_colors[pi].xyz * cos_angle;
    }
    if (sum_w > 1e-6) e_diffuse /= sum_w;
    lit += e_diffuse * diffuse_color * 0.31830988618 * g_env.w;

    // ② 高光：用反射方向采样，sharpness 随 roughness 下降
    //    sharpness = 1 + (1-r)^2 * 63   ← 与 `probe_sharpness` 同式
    float3 r = normalize(reflect(-view_dir, n));
    float rough = saturate(roughness);
    float sharp = 1.0 + (1.0 - rough) * (1.0 - rough) * 63.0;
    float3 radiance = float3(0.0, 0.0, 0.0);
    float sum_w_spec = 0.0;
    for (int pi = 0; pi < 4; ++pi) {
      if (g_probe_dirs[pi].w <= 0.0) continue;
      float3 dir = normalize(g_probe_dirs[pi].xyz);
      float cos_angle = dot(r, dir);
      if (cos_angle <= 0.0) continue;
      float w = pow(cos_angle, sharp);
      sum_w_spec += w;
      radiance += g_probe_colors[pi].xyz * w;
    }
    if (sum_w_spec > 1e-6) {
      radiance /= sum_w_spec;
    } else {
      // 全部在背面：退化为均值（对应 C++ 侧 `sample_probes` 的同一分支）
      float3 mean = float3(0.0, 0.0, 0.0);
      int count = 0;
      for (int pi = 0; pi < 4; ++pi) {
        if (g_probe_dirs[pi].w <= 0.0) continue;
        mean += g_probe_colors[pi].xyz;
        ++count;
      }
      if (count > 0) radiance = mean / float(count);
    }
    // split-sum 的强度因子：F0*A + B（Lazarov 解析拟合，与 `pbr_env_brdf` 同系数）
    float rr = saturate(roughness);
    float rx = rr * (-1.0) + 1.0;
    float ry = rr * (-0.0275) + 0.0425;
    float rz = rr * (-0.572) + 1.04;
    float rw = rr * (0.022) + (-0.04);
    float a004 = min(rx * rx, exp2(-9.28 * n_dot_v)) * rx + ry;
    float env_a = -1.04 * a004 + rz;
    float env_b = 1.04 * a004 + rw;
    lit += radiance * (f0 * env_a + env_b) * g_env.w;
  }

  // ## 自发光（与 C++ 侧 `shade_surface` 的 emission 段逐字对应）
  //
  // ⚠ GPU 腿原本**完全漏了这一段**：软件腿有、GPU 腿没有。
  // 实测症状：`emission: 5.0` 在 GPU 腿上一整帧像素不变（与不写逐位相同），
  // 而探针确认材质值已正确到 `set_material`——就是着色器里缺这段。
  //
  // `g_emissive.w > 0` 才进：默认 0 时与旧行为逐位相同（回归测试靠这条）。
  // 注意顺序：自发光要**加在 saturate 之前**，否则超过 1 的部分会被
  // 白白截掉，高强度发光就永远拉不出亮区。
  // 约定：`g_emissive.w` 为**带符号**强度——
  // 正值 = 自发光叠加在受光结果上；负值 = 纯发光体（丢受光项）。
  // 用符号而不是另加一个字段：常量缓冲的每个 float4 都要与 C++ 侧对齐，
  // 多一个字段就多一处可能错位的地方。
  if (abs(g_emissive.w) > 0.0) {
    if (g_emissive.w < 0.0) {
      lit = float3(0.0, 0.0, 0.0);   // 纯发光体：不受光照影响
    }
    lit += g_emissive.xyz * abs(g_emissive.w);
  }

  lit = saturate(lit);
  // ## 输出链：线性 → 曝光 → 色调映射 → sRGB 编码
  //
  // ⚠ 本段是 `st::raster::encode_pixel`（`color_pipeline.cpp`）的**逐字翻译**。
  // 两条腿必须同一条输出链，否则“降级”就变成“另一种长相”。
  //
  // 顺序不能反：先编码再映射 = 在非线性空间做曲线，暗部被压得不均匀。
  //
  // ## 这里曾经是硬编码 ACES（2026-11-06 修，真缺陷）
  //
  // 原实现完全**不读 `tone_map`**：不管剧本写 `none` 还是 `agx`，
  // 两腿输出逐像素相同。实测（同场景同参数，只切后端）：
  //
  // | 后端 | tone=0 (None) | tone=4 |
  // |---|---|---|
  // | software | rgb(13,18,25) | rgb(4,6,12) |
  // | **gpu** | rgb(4,6,12) | rgb(4,6,12) ← 逐像素相同 |
  //
  // 第二个错是 **先 `saturate` 再映射**：HDR >1 的部分被截平后才进曲线，
  // 高光滚降丢失（软件腿保留 HDR）。两个错都“不报错”，只是画面不对。
  //
  // 修法：把算子编号传进来，分支用与 C++ 侧相同的常数。
  // `g_tune2.x` = `ToneMap` 枚举值（0=None / 1=Reinhard / 2=Aces / 3=Filmic /
  // 4=Agx / 5=AcesThree）。AgX 是**色彩空间变换**，逐色道做不到，这里退到
  // Narkowicz（与 C++ 侧 `tone_map_channel` 的 AgX 退法一致，便于两腿对得上）。
  float3 hdr = lit;
  bool agx_linear = false;
  const int tone_op = (int)(g_tune2.x + 0.5);
  if (tone_op == 0) {
    hdr = saturate(hdr);
  } else if (tone_op == 1) {
    hdr = saturate(hdr / (float3(1.0, 1.0, 1.0) + hdr));
  } else if (tone_op == 3) {
    // Hable Uncharted2（与 C++ 侧 `filmic()` 同常数，白点 11.2）
    float3 x = max(hdr, 0.0);
    float3 num = x * (0.15 * x + 0.05) + 0.004;
    float3 den = x * (0.15 * x + 0.5) + 0.06;
    float3 w = 11.2;
    float3 num_w = w * (0.15 * w + 0.05) + 0.004;
    float3 den_w = w * (0.15 * w + 0.5) + 0.06;
    hdr = saturate((num / den) / (num_w / den_w));
  } else if (tone_op == 5) {
    // three r160 的 ACESFilmic（与 C++ 侧 `aces_three` 逐字同构）
    float3 c = hdr / 0.6;
    const float3x3 aces_in = float3x3(0.59719, 0.35458, 0.04823,
                                      0.07600, 0.90834, 0.01566,
                                      0.02840, 0.13383, 0.83777);
    const float3x3 aces_out = float3x3(1.60475, -0.53108, -0.07367,
                                       -0.10208, 1.10813, -0.00605,
                                       -0.00327, -0.07276, 1.07602);
    c = mul(aces_in, c);
    float3 a = c * (c + 0.0245786) - 0.000090537;
    float3 b = c * (0.983729 * c + 0.4329510) + 0.238081;
    c = a / b;
    c = mul(aces_out, c);
    hdr = saturate(c);
  } else if (tone_op == 4) {
    // 4 = Agx：**不做映射**，只钳制，留给 CPU 侧回读后用 OCIO LUT 处理。
    //
    // ## 为何不在这里做（实测判定，2026-11-06）
    //
    // AgX 是**三维 OCIO LUT**（57³ = 2.2MB），着色器里放不下（D3D11 的
    // cbuffer 上限 64KB）。而旧实现让它“退到 Narkowicz ACES”——
    // 实测 0.18 灰得 186，真值 118，差 **68 级**，且纯红渲成橙色。
    //
    // 所以这里只输出**线性值**（钳到 [0,1] 以免 8bit 目标截断丢信息），
    // 回读后由 `GpuScene3d::ensure_readback` 用与软件腿**同一个**
    // `agx::apply()` 做变换——两腿共用一份实现，避免“各自长一副样子”。
    //
    // ⚠ 本档的输出是**线性**，与其余档（sRGB 编码）不同。那个差异由
    // 回读后的后处理负责收聾。
    //
    // ⚠ 不能用 `return` 提前退：着色器的花括号与外层输出路径必须
    // 保持单一出口（实测泄一个 `return` 会让 HLSL 报
    // `X3000: syntax error: unexpected end of file`——那个报错指向文件末尾，
    // 与实际出错位置无关，极难查）。
    hdr = saturate(lit);
    agx_linear = true;
  } else {
    // 2 = Aces（Narkowicz）
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    hdr = saturate((hdr * (a * hdr + b)) / (hdr * (c * hdr + d) + e));
  }
  // ## 阴影（2026-11-07）：乘在色调映射**之前**（线性空间）
  //
  // 为何在色调映射之前：阴影是**照度**的衰减（几何遮挡），属于线性量。
  // 乘在色调映射之后等于先压缩后衰减，暗部比例就不对了。
  //
  // 为何乘在 `hdr` 而不是 `lit`：`lit` 之后可能经过自动曝光等处理，
  // 而阴影描述的是“这个像素看到多少光”，应在进入色调映射前生效。
  //
  // 自发光不受影响：`emission_unlit` 的物体（霓虹灯管）不该因为被挡住就变暗。
  // 用 `g_emissive.w < 0` 判定纯发光体（与 C++ 侧同一个约定）。
  if (g_shadow_tune.x > 0.0 && g_emissive.w >= 0.0) {
    const float occl = shadow_factor(input.world, normalize(input.normal));
    if (occl > 0.0) {
      hdr *= (1.0 - occl * g_shadow_tune.x);
    }
  }
  float3 encoded = hdr;
  // AgX 档输出**线性值**（后处理在 CPU 侧做 LUT，见上）
  if (agx_linear) {
    return float4(encoded, g_opacity.x);
  }
  // ## alpha 两个字的意义（不要混淆）
  //
  // - `rgb` 是 **sRGB 编码值**（显示用，与软件腿逐像素对齐）；
  // - `a` 是**几何覆盖率**（1 = 实体）。混合状态用 `SrcAlpha/InvSrcAlpha`，
  //   所以材质不透明度必须乘进 alpha：`a = g_opacity.x`。
  //
  // 为何不把 opacity 乘进 rgb：那会把半透明物体变成“变暗”（因为 src-over 还会
  // 再乘一次 alpha）；而且与软件腿的线性空间混合公式不一致。
  return float4(encoded, g_opacity.x);
}

/// 主着色体之后才声明入口：HLSL 要求被调函数先定义。
float4 ps_scene3d(VsOut input) : SV_Target {
  return ps_scene3d_body(input);
}

/// MRT 入口：一次 draw 同时写颜色 + 世界位置 + 法线（**仅供 AO 路径**）。
///
/// ## 为何不是「另写两个 PS」
///
/// 一次 draw 只能激活**一个**像素着色器。要让三个目标同时得到数据，
/// 只能是一个 PS 带三个 `SV_Target`——分成三个 PS 会变成三次 draw，
/// 而且各写各的目标，颜色/位置/法线完全不同步。
///
/// ## 为何单目标路径不用它
///
/// 单目标时 `SV_Target1`/`2` 没绑定，写它们的值被硬件丢弃（理论上无损）。
/// 但本仓对「没开 AO 的场景必须与改动前**逐位相同**」有硬要求，
/// 所以单目标路径仍走 `ps_scene3d`（只有一个输出）。
struct Scene3dMrtOut {
  float4 color : SV_Target0;
  float4 world : SV_Target1;
  float4 normal : SV_Target2;
};

Scene3dMrtOut ps_scene3d_mrt(VsOut input) {
  Scene3dMrtOut o;
  o.color = ps_scene3d_body(input);
  o.world = float4(input.world, 1.0);
  // `normalize` 不可省：插值后的顶点法线长度 < 1，而 AO 的半球方向表
  // 需要单位法线（否则方向被缩短，遮蔽度系统性偏低）。
  o.normal = float4(normalize(input.normal), 1.0);
  return o;
}
)hlsl";

/// 阴影深度 pass 的像素着色器：**只写深度，不输出颜色**。
///
/// ## 为何要单独一个（而不是复用主 PS）
///
/// 主 PS 要做完整的 BRDF 着色，而在深度 pass 里那些计算**全部是浪费**——
/// 写进深度缓冲的只是 `SV_Position.z`，像素颜色被丢弃。
/// 更关键的是：主 PS 里有 `SampleCmpLevelZero(g_shadow_map, ...)`，
/// 而在深度 pass 里**正在写的就是那张图**——读写同一资源是未定义行为。
/// 所以必须是一个**不碰任何纹理**的空 PS。
///
/// 顶点着色器直接复用 `vs_scene3d`：它已经把位置乘上 `g_mvp` 了，
/// 深度 pass 只要把 `g_mvp` 换成 `g_shadow_vp` 即可。
static const char* kShadowDepthShader = R"hlsl(
/// 深度 pass 的像素着色器：把 NDC 深度写进 R32_FLOAT 目标。
///
/// ## 为何必须**输出**而不是靠空体写深度（这一步曾错两次）
///
/// 写一个空 PS（`void ps(float4 : SV_Position) {}`）时，深度缓冲里确实
/// 会有正确值，但 `R32_FLOAT` 那张颜色纹理**始终是清值**——D3D 在
/// 像素着色器没有颜色输出时不写渲染目标。
///
/// 实测症状：`SampleCmpLevelZero` 拿到的全是 1.0（“最远”）→ 所有像素
/// 比较都“亮” → 阴影无任何效果，而主画面看上去完全“正常”。
/// 这个错误极难从画面上看出，只能把深度图存盘才看得到。
///
/// 所以这里显式返回 `SV_Position.z`（NDC 深度，正交投影下就是
/// 线性深度），让 R32 目标里真的有数。
float4 ps_shadow_depth(float4 position : SV_Position) : SV_Target {
  return float4(position.z, position.z, position.z, 1.0);
}
)hlsl";

/// 与 HLSL 的 cbuffer 逐字节对应。
struct Scene3dParams {
  std::array<float, 16> mvp{};
  std::array<float, 16> model{};
  std::array<float, 4> light{};
  std::array<float, 4> tune{};
  std::array<float, 4> opacity{};
  std::array<float, 4> eye{};
  /// 环境反射：xyz = 天空色，w = 强度（0 = 关闭）
  std::array<float, 4> env{};
  /// 地面色 + 粗糙度弥散强度
  std::array<float, 4> env_ground{};
  /// 自发光：xyz = 颜色，w = 带符号强度（负 = 纯发光体）
  std::array<float, 4> emissive{};
  /// 材质基色：xyz = 颜色，w = 是否启用
  std::array<float, 4> base{};
  /// 环境探针（2026-11-05）：方向 xyz + w = 是否存在
  std::array<std::array<float, 4>, 4> probe_dirs{};
  /// 探针颜色 xyz
  std::array<std::array<float, 4>, 4> probe_colors{};
  /// x = 探针个数（0 = 走旧的天地插值），y = 锐利度
  std::array<float, 4> probe_tune{};
  /// x = 色调映射算子（`ToneMap` 枚举值）；yzw 保留
  std::array<float, 4> tune2{};
  /// 主光颜色（线性）。强度走 `tune.x`（= `Lighting::diffuse`）。
  std::array<float, 4> light_color{};
  // ## 额外灯光（2026-11-06 新增）
  //
  // 与软件腿的 `ShadeInput::extra_lights` 对应：bpy 场景常有 key/fill/rim
  // 三盏灯，只算主光会让 GPU 腿比软件腿暗（同一个剧本两副长相）。
  //
  // 上限 3（= `kMaxExtraLights`），每盏灯三个 float4。
  // `extra_count.x = 0` 时 HLSL 不进那个循环 → 与旧行为逐位相同。
  std::array<std::array<float, 4>, 3> extra_dirs{};
  std::array<std::array<float, 4>, 3> extra_colors{};
  /// x=kind(0 平行/1 点/2 聚) y=intensity z=range w=spot 外角(度)
  std::array<std::array<float, 4>, 3> extra_params{};
  /// x = 个数，yzw 保留
  std::array<float, 4> extra_count{};
  /// 光源视角的 view-projection（世界 → 阴影贴图 NDC）。
  ///
  /// ## 为何单独一个 16 float 而不复用 `mvp`
  ///
  /// `mvp` 是**相机**视角，每个网格都变（逐网格 Map）；
  /// 阴影 VP 是**光源**视角，一帧内对所有网格相同。混用会让
  /// 深度 pass 写成相机视角的深度图——阴影位置全错，而且看着“有阴影”，
  /// 只是位置不对应任何真实遮挡，极难查。
  std::array<float, 16> shadow_vp{};
  /// x = strength（0 = 关闭）y = bias z = soft_radius w = 分辨率
  std::array<float, 4> shadow_tune{};
  /// 半球环境光：xyz = 天空色，w = 强度（0 = 关闭）。
  ///
  /// 必须放在**末尾且顺序与 HLSL 一致**：常量缓冲是逐字节映射的，
  /// 错一个字段后面全部错位（而症状是“颜色奇怪”而不是报错）。
  std::array<float, 4> hemi_sky{};
  /// xyz = 地面色，w 备用。
  std::array<float, 4> hemi_ground{};
  /// 环境光颜色（xyz；w 备用）。
  std::array<float, 4> ambient_color{};
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
  /// MRT 入口（`ps_scene3d_mrt`）：AO 开启时用，一次 draw 写颜色+世界位置+法线。
  ID3D11PixelShader* ps_mrt{nullptr};
  ID3D11InputLayout* layout{nullptr};
  ID3D11Buffer* constants{nullptr};
  ID3D11DepthStencilState* depth{nullptr};
  /// 只测试不写入（透明物体用）。
  ID3D11DepthStencilState* depth_no_write{nullptr};
  ID3D11RasterizerState* raster{nullptr};
  /// 三套剔除状态（与 `CullMode` 一一对应）。`raster` 指向当前默认（back）。
  ID3D11RasterizerState* raster_none{nullptr};
  ID3D11RasterizerState* raster_back{nullptr};
  ID3D11RasterizerState* raster_front{nullptr};
  ID3D11SamplerState* sampler{nullptr};
  /// 透明材质的 alpha 混合状态（标准 src-over）。
  ///
  /// 用单独一套而不是复用画布管线的 `blend`：两者的渲染目标格式不同
  /// （三维目标是 RGBA8、画布是预乘布局），共用会在某些驱动上出错。
  ID3D11BlendState* blend_alpha{nullptr};
  // ── 阴影深度 pass（2026-11-07）──
  /// 只写深度的像素着色器（空体，见 `kShadowDepthShader`）。
  ID3D11PixelShader* ps_shadow_depth{nullptr};
  /// 阴影 pass 的深度模板状态：写入 + 标准比较。
  ID3D11DepthStencilState* depth_shadow{nullptr};
  /// 阴影贴图的比较采样器（`SampleCmpLevelZero` 专用）。
  ///
  /// ⚠ 必须是 `D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT`：
  /// 普通线性采样器传给 `SampleCmpLevelZero` 在部分驱动上会失败，
  /// 而失败时返回 0 → 会被当成“全部被遮挡”（画面突然一片黑）。
  ID3D11SamplerState* sampler_shadow_cmp{nullptr};
  /// 深度 pass 用的光栅化状态：关掉背面剔除。
  ///
  /// 为何不剔：阴影应该由**两面**（朝光面与背光面）共同形成。
  /// 只写正面时（默认 `CullMode::Back`）背光面不投阴影——两面重叠的几何
  /// （如封闭盒子）会在背光侧漏光。只写背面的做法（peter-panning 控制）
  /// 在这里没有必要：本实现用 `bias` 解决自阴影条纹。
  ID3D11RasterizerState* raster_shadow{nullptr};
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
    // MRT 入口（AO 路径）：编译失败不该让整个三维腿不可用——
    // AO 是可选能力，仅记日志，`ps_mrt` 保持 nullptr，该路径会退到单目标。
    ID3DBlob* mrt_blob = nullptr;
    if (build("ps_scene3d_mrt", "ps_4_0", &mrt_blob)) {
      device->CreatePixelShader(mrt_blob->GetBufferPointer(), mrt_blob->GetBufferSize(),
                                nullptr, &ps_mrt);
      mrt_blob->Release();
    } else {
      st::log::warn("[scene3d] MRT 着色器编译失败，GPU 腿的 AO 将不可用：{}\n", error);
      error.clear();
      if (errors != nullptr) {
        errors->Release();
        errors = nullptr;
      }
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
    // ## 只测试、不写入的深度状态（透明物体专用）
    //
    // 透明物体的正确行为：**参与**深度测试（前后关系对），但**不写**深度
    //（否则近处的半透明面会挡住本应透过它看到的东西）。
    // 之前只有一套固定写深度的状态，而软件腿已按 `Material::depth_write` 分支——
    // 两条腿在透明场景上行为不一致（实测：GPU 腿半透明完全不生效）。
    depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    if (FAILED(device->CreateDepthStencilState(&depth_desc, &depth_no_write))) {
      error = "创建深度状态失败（不写）";
      return;
    }
    D3D11_RASTERIZER_DESC raster_desc{};
    raster_desc.FillMode = D3D11_FILL_SOLID;
    // ## 三套光栅化状态（按 `CullMode` 选）
    //
    // 历史上这里是单套 `CULL_NONE`（注释写“模型可能未闭合/法线朝内”）。
    // 那是**用错法掩盖缺能力**：正确做法是提供剔除开关，不剔的场合由调用方
    // 显式选 `CullMode::None`。
    //
    // 两腿行为不一致的代价很具体：同一场景，有显卡的机器看到外表面、
    // 没显卡的看到内表面，而且只在透明/开口物体上现形。
    //
    // 绕序：本框架的 `Mesh` 生成器与软件腿都按**逆时针 = 正面**，
    // D3D11 默认正好相反，所以三套都设 `FrontCounterClockwise = TRUE`。
    raster_desc.FrontCounterClockwise = TRUE;
    raster_desc.DepthClipEnable = TRUE;
    raster_desc.ScissorEnable = TRUE;
    raster_desc.CullMode = D3D11_CULL_NONE;
    if (FAILED(device->CreateRasterizerState(&raster_desc, &raster_none))) {
      error = "创建光栅化状态失败（cull none）";
      return;
    }
    raster_desc.CullMode = D3D11_CULL_BACK;
    if (FAILED(device->CreateRasterizerState(&raster_desc, &raster_back))) {
      error = "创建光栅化状态失败（cull back）";
      return;
    }
    raster_desc.CullMode = D3D11_CULL_FRONT;
    if (FAILED(device->CreateRasterizerState(&raster_desc, &raster_front))) {
      error = "创建光栅化状态失败（cull front）";
      return;
    }
    // `raster` 保留为默认（back），下游代码不必改。
    raster = raster_back;
    // 透明混合（标准 src-over）
    D3D11_BLEND_DESC blend_desc{};
    blend_desc.RenderTarget[0].BlendEnable = TRUE;
    blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&blend_desc, &blend_alpha))) {
      error = "创建三维透明混合状态失败";
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

    // ── 阴影深度 pass（2026-11-07）──
    ID3DBlob* shadow_blob = nullptr;
    const HRESULT shadow_result =
        compile(kShadowDepthShader, std::strlen(kShadowDepthShader), "st_shadow_depth",
                nullptr, nullptr, "ps_shadow_depth", "ps_4_0", 0, 0, &shadow_blob, &errors);
    if (FAILED(shadow_result)) {
      error = errors != nullptr
                  ? std::string(static_cast<const char*>(errors->GetBufferPointer()))
                  : "编译阴影深度着色器失败";
      if (errors != nullptr) {
        errors->Release();
        errors = nullptr;
      }
      return;
    }
    const HRESULT ps_shadow_result = device->CreatePixelShader(
        shadow_blob->GetBufferPointer(), shadow_blob->GetBufferSize(), nullptr,
        &ps_shadow_depth);
    shadow_blob->Release();
    if (FAILED(ps_shadow_result)) {
      error = "创建阴影深度像素着色器失败";
      return;
    }

    // 比较采样器：`SampleCmpLevelZero` 专用。
    //
    // ⚠ 必须是 COMPARISON 滤波器。用普通线性采样器调 `SampleCmpLevelZero`
    // 在部分驱动上会失败，而失败返回 0 → 被当成“全部被遮挡”（一片黑）。
    D3D11_SAMPLER_DESC shadow_sampler{};
    shadow_sampler.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    shadow_sampler.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
    shadow_sampler.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
    shadow_sampler.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    // 边界处返回 1.0（= 比任何参考值大 = “亮”）：阴影图外应当**没有阴影**。
    // 若返回 0，图外的像素会全部被当成阴着，画面出现矩形黑框。
    shadow_sampler.BorderColor[0] = 1.0f;
    shadow_sampler.BorderColor[1] = 1.0f;
    shadow_sampler.BorderColor[2] = 1.0f;
    shadow_sampler.BorderColor[3] = 1.0f;
    shadow_sampler.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    shadow_sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&shadow_sampler, &sampler_shadow_cmp))) {
      error = "创建阴影比较采样器失败";
      return;
    }

    // 阴影 pass 的深度状态：写入 + 标准比较（近处赢）
    D3D11_DEPTH_STENCIL_DESC shadow_depth{};
    shadow_depth.DepthEnable = TRUE;
    shadow_depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    shadow_depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    if (FAILED(device->CreateDepthStencilState(&shadow_depth, &depth_shadow))) {
      error = "创建阴影深度状态失败";
      return;
    }
    // 阴影 pass 的光栅化：**不剔除**（两面都投阴影，见字段注释）
    D3D11_RASTERIZER_DESC shadow_raster{};
    shadow_raster.FillMode = D3D11_FILL_SOLID;
    shadow_raster.CullMode = D3D11_CULL_NONE;
    shadow_raster.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&shadow_raster, &raster_shadow))) {
      error = "创建阴影光栅化状态失败";
      return;
    }
    ok = true;
  }

  Scene3dPipeline(const Scene3dPipeline&) = delete;
  auto operator=(const Scene3dPipeline&) -> Scene3dPipeline& = delete;

  ~Scene3dPipeline() {
    if (ps_mrt != nullptr) ps_mrt->Release();
    if (raster_shadow != nullptr) raster_shadow->Release();
    if (depth_shadow != nullptr) depth_shadow->Release();
    if (sampler_shadow_cmp != nullptr) sampler_shadow_cmp->Release();
    if (ps_shadow_depth != nullptr) ps_shadow_depth->Release();
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
/// 3D 场景。
///
/// **上下文策略：逐调用加锁（立即上下文，没有 deferred、没有「提交」这一步）**——
/// 完整判据见文件头「上下文策略：三个类，两套规则」。
/// ⚠ **不要把 GpuCanvas 的 deferred 纪律套到这里**（那里要「先提交再读纹理」，
/// 这里命令即时生效）。2026-10-25 曾误加过一次，编译不过才发现套错对象。
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
    // ## 清屏色必须走**与实体同一条输出链**（实测：两条腿差 48 级）
    //
    // 软件腿把 `clear_color` 当**线性值**，经 `encode_pixel`（曝光 + 色调映射 +
    // sRGB 编码）后才写进画布。这里原先直接 `clear_color / 255` 写进 RTV——
    // 等于把线性值当 sRGB 存。
    //
    // 实测（背景 (40,44,52)）：软件腿清出 159.0、GPU 腿清出 110.5，差 48 级。
    // 单看任一条腿都"正常"，只有并排比才看得出——特性矩阵测试就是这么撞上的。
    // ## 清屏：给**线性值**，编码交给 `_SRGB` 视图的硬件
    //
    // 与实体同口径：`ps_scene3d` 输出线性值，硬件在线性→sRGB 编码后再存；
    // 这里也必须给线性值。若先 `encode_pixel` 再写，硬件会**再编码一次**，
    // 背景整片偏亮（实测背景 0.517 vs 软件腿 0.299）。
    //
    // 色调映射仍在着色器里做（ACES），清屏色要跟它一致：
    // 所以这里先做 ACES、**不做** sRGB 编码。
    const math::Vec3 bg_linear{static_cast<float>(clear_color.r) / 255.0f,
                               static_cast<float>(clear_color.g) / 255.0f,
                               static_cast<float>(clear_color.b) / 255.0f};
    const math::Vec3 mapped = st::raster::tone_map(bg_linear, lighting_.tone_map);
    const float alpha = static_cast<float>(clear_color.a) / 255.0f;
    const float values[4] = {mapped.x * alpha, mapped.y * alpha, mapped.z * alpha, alpha};
    context_->ClearRenderTargetView(rtv_, values);
    context_->ClearDepthStencilView(dsv_, D3D11_CLEAR_DEPTH, 1.0f, 0);
    mvp_ready_ = false;
    lighting_ = st::raster::Lighting{};
    cull_mode_ = st::raster::CullMode::Back;   // 不重置会让上一帧的设置泄漏
    material_ = st::raster::Material{};
    shadow_ = st::raster::PlanarShadow{};
    shadow_done_ = false;
    transparent_meshes_.clear();
    draw_calls_ = 0;
  }

  void set_camera(const st::raster::Camera& camera) override {
    const float aspect = height_ > 0 ? static_cast<float>(width_) / static_cast<float>(height_)
                                     : 1.0f;
    const float fov = camera.fov_y_degrees * 3.14159265358979f / 180.0f;
    view_ = math::Mat4::look_at(camera.eye, camera.target, camera.up);
    projection_ = math::Mat4::perspective(fov, aspect, camera.near_z, camera.far_z);
    eye_ = camera.eye;
    mvp_ready_ = true;
  }

  void set_lighting(const st::raster::Lighting& lighting) override { lighting_ = lighting; }

  void set_cull_mode(st::raster::CullMode mode) override { cull_mode_ = mode; }

  /// 材质（透明）。
  ///
  /// ## 为何需要专门实现（实测踩到）
  ///
  /// 基类里 `set_material` 是**空实现**（渐进采用）。对没 override 的后端，
  /// 传进去的 `opacity` 被**静默丢弃**——同一个场景在软件腿上是半透明、
  /// 在 GPU 腿上是全不透明，而且不会有任何报错。
  /// 实测：`ST_SCENE3D_BACKEND=software` 的测试全过，设成 `gpu` 则中心像素
  /// 是纯红（红色没有混合）——这就是“两条腿必须一致”那一约束要拦的东西。
  void set_material(const st::raster::Material& material) override { material_ = material; }

  /// 平面投影阴影。
  ///
  /// ## 为何 GPU 腿也必须有实现（不能靠基类空实现）
  ///
  /// 基类里 `set_planar_shadow` 是空实现。对没 override 的后端，调用方设的
  /// 阴影会**静默丢弃**——同一场景软件腿有影子、GPU 腿没有，而且不报错。
  /// 这正是 `set_material` 当时踩到的同一个坑（实测：透明在 GPU 上完全不生效）。
  ///
  /// ## 当前状态：GPU 腿暂不渲染投影阴影
  ///
  /// 参数被完整保留，但绘制路径尚未实现。差异通过 `capabilities()` 声明——
  /// 调用方可以查询后决定是否降级（比如自己画一张 2D 接触阴影）。
  /// **不发默默忽略**：这是我们从 `set_material` 那次学到的教训。
  void set_planar_shadow(const st::raster::PlanarShadow& shadow) override {
    shadow_ = shadow;
  }

  /// 缝隙遮蔽（AO，2026-11-07 **已实现**）。
  ///
  /// ## 做法：G-Buffer + 回读 + 共享内核
  ///
  /// 1. 绘制时用 MRT 着色器额写世界位置与法线（仅 AO 开启时）
  /// 2. 回读颜色 + 位置 + 法线
  /// 3. 对每个像素调 `pbr_ambient_occlusion()`（**与软件腿同一函数**）
  /// 4. 把 AO 乘到**间接光**上（环境光 + IBL）
  ///
  /// ## 为何不在着色器里算
  ///
  /// 世界三角形传不进去（几百个三角形就是几十个 cbuffer）。
  /// 而重写一份算法就是“两腿各实现一遍”——本仓因此出过四次缺陷。
  ///
  /// ## 代价
  ///
  /// 每像素 16 条射线 × 三角形数，在 CPU 侧算。所以必须**按世界位置量化缓存**
  /// （与软件腿同一策略）：不做缓存时 1080p 要分钟级。
  void set_ambient_occlusion(const st::raster::AmbientOcclusion& occlusion) override {
    occlusion_ = occlusion;
  }

  /// 阴影贴图（GPU 腿，2026-11-07 **已实现**）。
  ///
  /// ## 它曾经只是参数占位
  ///
  /// 早前这里只存参数 + 报一条 warning，因为“两条腿各实现一遍”风险高。
  /// 后果是：同一个场景软件腿有球在地板上的投影阴影，GPU 腿没有——
  /// **默写不说话**，只有两条腿并排比才看得见。
  ///
  /// 现在两腿共用 `ShadowMap::make_view_projection()`（住 `scene3d.cpp`），
  /// 光源相机只算一次。这是“两腿一致”的关键：矩阵不同会让阴影位置全错，
  /// 而各看各的画面都“有阴影”，极难查。
  void set_shadow_map(const st::raster::ShadowMap& shadow) override {
    shadow_map_ = shadow;
  }

  /// 当前剔除模式（每帧在 `begin_frame` 重置为 `Back`）。
  st::raster::CullMode cull_mode_{st::raster::CullMode::Back};
  /// 相机世界位置（算逐像素视向用；GGX 的 Fresnel 依赖它）。
  math::Vec3 eye_{0.0f, 0.0f, 4.0f};
  /// 平面投影阴影（`strength = 0` = 关闭）。
  st::raster::PlanarShadow shadow_{};
  /// 阴影贴图参数（**本腿不实现**，只存来待将来）。见 `set_shadow_map`。
  st::raster::ShadowMap shadow_map_{};
  /// 是否已经报过一次“GPU 腿不支持阴影贴图”。
  ///
  /// 为何需要：`set_shadow_map` 每帧都会被调（`render_scene` 的固定顺序），
  /// 不拦住会刷屏。只报第一次，后续静默——差异已经在第一行说清楚了。
  bool shadow_map_warned_{false};
  /// 同上，AO 的 warning 只报一次。
  bool occlusion_warned_{false};
  /// AO 参数（`strength = 0` = 关闭）
  st::raster::AmbientOcclusion occlusion_{};

  // ── 阴影贴图资源（GPU 腿，2026-11-07）──
  /// 深度图（R32_FLOAT；既能当 RTV 写、也能当 SRV 采）。
  ID3D11Texture2D* shadow_texture_{nullptr};
  ID3D11RenderTargetView* shadow_rtv_{nullptr};
  ID3D11ShaderResourceView* shadow_srv_{nullptr};
  /// 深度缓冲（**必需**：不绑深度缓冲时 D3D 禁用深度测试，什么也写不进深度图）
  ID3D11Texture2D* shadow_depth_buffer_{nullptr};
  ID3D11DepthStencilView* shadow_dsv_{nullptr};

  // ── G-Buffer（AO 用，2026-11-07）──
  //
  // 两张 R32G32B32A32_FLOAT：世界位置 + 法线。
  // 存的是 float 而不是打包成 8bit：AO 的射线查询对位置精度敏感
  //（量化误差会让“接触面的像素”误判为非接触）。
  ID3D11Texture2D* gbuf_world_{nullptr};
  ID3D11RenderTargetView* gbuf_world_rtv_{nullptr};
  ID3D11Texture2D* gbuf_normal_{nullptr};
  ID3D11RenderTargetView* gbuf_normal_rtv_{nullptr};
  /// 回读用的 staging（两张共用一个，因为读是串行的）
  ID3D11Texture2D* gbuf_staging_{nullptr};
  /// MSAA → 单采样的 resolve 中间目标（仅 `sample_count_ > 1` 时建）
  ID3D11Texture2D* gbuf_resolved_{nullptr};
  int gbuf_width_{0};
  int gbuf_height_{0};
  int shadow_resolution_{0};
  /// 本帧阴影图是否已建（`end_frame` 里建，着色时读）。
  bool shadow_ready_{false};
  /// 本帧需要投阴影的网格（`draw_mesh` 时收集，`end_frame` 开头消费）。
  ///
  /// 存网格的共享指针：`draw_mesh` 的契约是“调用期间有效”，而
  /// 阴影 pass 发生在 `end_frame`——那时调用方可能已释放原网格。
  /// 与软件腿的 `DeferredMesh` 同一取舍（那边踩过“读到残留内存”）。
  struct ShadowDrawItem {
    std::shared_ptr<const st::raster::Mesh> mesh{};
    math::Mat4 model{math::Mat4::identity()};
  };
  std::vector<ShadowDrawItem> drawn_meshes_{};
  /// 世界三角形（AO 用；与软件腿的 `shadow_geometry_` 同一语义）。
  std::vector<st::raster::WorldTriangle> shadow_triangles_cache_{};

  /// 把一个网格展开成世界空间三角形。
  ///
  /// 与软件腿的做法一致：**只看索引，不剔除**。
  /// AO 需要两面都有几何（一个开口盒子的内壁也能挡光），
  /// 而剔除会把它当成“不存在”——遮蔽就会漏。
  static void collect_world_triangles(const st::raster::Mesh& mesh, const math::Mat4& model,
                                      std::vector<st::raster::WorldTriangle>& out) {
    const std::size_t stride = st::raster::Mesh::kVertexFloats;
    const auto vertex_at = [&mesh, stride](std::size_t index) -> math::Vec3 {
      const std::size_t base = index * stride;
      if (base + 2 >= mesh.vertices.size()) return math::Vec3{0.0f, 0.0f, 0.0f};
      return math::Vec3{mesh.vertices[base], mesh.vertices[base + 1], mesh.vertices[base + 2]};
    };
    const auto to_world = [&model](math::Vec3 p) -> math::Vec3 {
      return math::Vec3{model.value(0, 0) * p.x + model.value(0, 1) * p.y +
                            model.value(0, 2) * p.z + model.value(0, 3),
                        model.value(1, 0) * p.x + model.value(1, 1) * p.y +
                            model.value(1, 2) * p.z + model.value(1, 3),
                        model.value(2, 0) * p.x + model.value(2, 1) * p.y +
                            model.value(2, 2) * p.z + model.value(2, 3)};
    };
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
      st::raster::WorldTriangle tri{};
      for (int k = 0; k < 3; ++k) {
        tri.v[k] = to_world(vertex_at(mesh.indices[i + static_cast<std::size_t>(k)]));
      }
      out.push_back(tri);
    }
  }
  /// 阴影是否已在本帧绘制（避免重复）。
  bool shadow_done_{false};

  /// 当前材质（每帧在 `begin_frame` 重置为默认）。
  st::raster::Material material_{};
  /// 本次绘制的索引区间（`range_count_ == 0` = 全画）。
  ///
  /// 为何是成员而不是 `draw_mesh_impl` 的参数：那个函数的签名被多处调用
  /// （旧 `draw_mesh` / 阴影延迟重排），加参数要改全部调用点且大部分
  /// 传 0。成员赋值 + 恢复是两行，且与 `material_` 的暂存模式一致。
  UINT range_start_{0};
  UINT range_count_{0};

  /// 透明队列（整网格级）。
  struct TransparentMesh {
    const st::raster::Mesh* mesh{nullptr};
    math::Mat4 model{};
    float opacity{1.0f};
    float depth{0.0f};   ///< 排序键（越小越远）
  };
  std::vector<TransparentMesh> transparent_meshes_{};
  /// 重入保护：`end_frame` 里重调 `draw_mesh` 时不要再入队。
  bool flushing_transparent_{false};

  /// 延迟的不透明绘制（阴影开启时才用）。
  ///
  /// 存材质快照与索引区间：`end_frame` 重放时必须复原当时的全部状态，
  /// 否则 `draw_mesh_range` 的区间会全部变成 0..n（画整个网格）。
  struct PendingOpaque {
    std::shared_ptr<const st::raster::Mesh> mesh{};
    math::Mat4 model{math::Mat4::identity()};
    Material material{};
    UINT range_start{0};
    UINT range_count{0};
  };
  std::vector<PendingOpaque> pending_opaque_{};
  /// 重放保护：`end_frame` 重入 `draw_mesh_impl` 时不要再入队。
  bool replaying_{false};

  void set_texture(std::span<const std::uint8_t> pixels, int width, int height) override {
    const std::scoped_lock lock(g_context_mutex());
    upload_texture(pixels, width, height);
  }

  void draw_mesh(const st::raster::Mesh& mesh, const math::Mat4& model) override {
    draw_mesh_with_material(mesh, model, material_);
  }

  /// 索引区间 + 材质的绘制（与软件腿同语义，见 `Scene3D::draw_mesh_range`）。
  ///
  /// GPU 腿上比软件腿简单：`DrawIndexed` 本身就接受起始索引，不需要切缓冲。
  void draw_mesh_range(const st::raster::Mesh& mesh, const math::Mat4& model,
                       const Material& material, std::size_t index_start,
                       std::size_t index_count) override {
    if (!ok_ || !mvp_ready_ || mesh.is_empty()) return;
    const Material saved = material_;
    material_ = material;
    range_start_ = static_cast<UINT>(index_start);
    range_count_ = static_cast<UINT>(index_count);
    draw_mesh_impl(mesh, model);
    range_start_ = 0;
    range_count_ = 0;
    material_ = saved;
  }

  /// 显式指定材质的绘制（与软件腿同语义，见 `Scene3D::draw_mesh_with_material`）。
  ///
  /// 与软件腿一样：**画完恢复**，不让材质泄漏到后续网格。
  void draw_mesh_with_material(const st::raster::Mesh& mesh, const math::Mat4& model,
                               const Material& material) override {
    if (!ok_ || !mvp_ready_ || mesh.is_empty()) return;
    const Material saved = material_;
    material_ = material;
    draw_mesh_impl(mesh, model);
    material_ = saved;
  }

  /// 实际提交（原 `draw_mesh` 的体）。
  void draw_mesh_impl(const st::raster::Mesh& mesh, const math::Mat4& model) {
    if (!ok_ || !mvp_ready_ || mesh.is_empty()) return;
    // ## 阴影开启时：整帧延迟（2026-11-07）
    //
    // ## 为何必须延迟（这个坑很硬）
    //
    // 阴影贴图必须**先于**主场景就绪（着色时要查深度图）。而几何收集
    // 发生在 `draw_mesh` 里——边画边收，第一个物体画完时图里只有它自己。
    //
    // 第一版没延迟，于是：`shadow_ready_` 在 `end_frame` 才置真，
    // 而 `draw_mesh` 在那之前已读过参数 → `g_shadow_tune.x = 0` →
    // 着色器整段跳过 → **开了阴影与不开完全一样**（实测两都是 76.5）。
    //
    // 更阴的是：它看起来“GPU 腿正常”，只有并排比才看得见。
    //
    // 所以：阴影开启时，先把整帧几何排队，`end_frame` 里按
    // 「建阴影图 → 重入本函数画主场景」的顺序走。
    if ((shadow_map_.strength > 0.0f || occlusion_.strength > 0.0f) && !replaying_) {
      pending_opaque_.push_back(PendingOpaque{
          std::make_shared<const st::raster::Mesh>(mesh), model, material_, range_start_,
          range_count_});
      ++draw_calls_;
      return;
    }
    // ## 透明网格延迟重排（与软件腿同一个道理）
    //
    // 基类里 `set_material` 曾是空实现，于是 `opacity` 被静默丢弃：
    // 同一场景软件腿半透明、GPU 腿全不透明，且不报错（实测：
    // `ST_SCENE3D_BACKEND=software` 的用例全过，设成 `gpu` 则中心像素是纯红）。
    //
    // 真正的正确性需要**远→近**绘制，而 `draw_mesh` 按调用顺序提交。
    // 所以透明网格先排队，`end_frame` 里按相机距离排序后重入本函数
    //（用 `flushing_transparent_` 防重复入队）。
    if (material_.opacity < 0.999f && !flushing_transparent_) {
      TransparentMesh item{};
      item.mesh = &mesh;
      item.model = model;
      item.opacity = material_.opacity;
      // 排序键：模型矩阵的平移 z（相机沿 -Z 看，z 越小越远）。
      // 这不是“逐三角形排序”（那要改索引缓冲），但对**整网格**级别的
      // 透明场景（玻璃罩/半透卡片）已经足够；逐三角形排序列入 BACKLOG。
      item.depth = model.value(0, 3) * 0.0f + model.value(1, 3) * 0.0f + model.value(2, 3);
      transparent_meshes_.push_back(item);
      return;
    }
    draw_mesh_immediate(mesh, model);
  }

 private:
  /// 立即绘制（不带透明延迟逻辑）。
  void draw_mesh_immediate(const st::raster::Mesh& mesh, const math::Mat4& model) {
    if (!ok_ || !mvp_ready_ || mesh.is_empty()) return;
    const Scene3dPipeline& pipes = scene3d_pipeline();
    if (!pipes.ok) return;
    const MeshBuffers* buffers = buffers_for(mesh);
    if (buffers == nullptr) return;
    // ## 阴影几何收集（2026-11-07）
    //
    // 为何在这里而不是 `draw_mesh`：透明队列里的网格**不经 `draw_mesh`**，
    // 而是在 `end_frame` 里重入 `draw_mesh_immediate`。若只在 `draw_mesh`
    // 收集，透明网格就永远不投阴影（比如玻璃罩里的不透明物件会“漏光”）。
    //
    // 为何只收不透明：透明物体不投实阴影（否则玻璃后面会多一团黑影），
    // 与软件腿的 `material_.opacity >= 0.999f` 条件一致。
    if (shadow_map_.strength > 0.0f && material_.opacity >= 0.999f &&
        !flushing_transparent_) {
      drawn_meshes_.push_back(ShadowDrawItem{std::make_shared<const st::raster::Mesh>(mesh),
                                             model});
    }

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
    const math::Vec3 light = lighting_.light_from.normalized();
    params.light = {light.x, light.y, light.z, lighting_.ambient};
    // g_tune.w：本网格是否采样纹理（1 = 采）
    const bool use_texture = texture_view_ != nullptr && buffers->uvs != nullptr;
    params.tune = {lighting_.diffuse, 0.0f /* rim 已删除，占位保持 cbuffer 布局 */, lighting_.exposure,
                   use_texture ? 1.0f : 0.0f};
    params.opacity = {material_.opacity, material_.metallic, material_.roughness,
                      material_.specular};
    params.eye = {eye_.x, eye_.y, eye_.z, 0.0f};
    // 环境反射：未配置时 `env_strength = 0` → HLSL 里 `if (g_env.w > 0.0)` 不进，
    // 与旧行为逐位相同（回归测试靠这条）。
    params.env = {lighting_.ibl_sky.x, lighting_.ibl_sky.y, lighting_.ibl_sky.z,
                  lighting_.ibl_strength};
    params.env_ground = {lighting_.ibl_ground.x, lighting_.ibl_ground.y,
                         lighting_.ibl_ground.z, lighting_.ibl_rough_falloff};
    // 环境探针（2026-11-05）：最多 4 个（cbuffer 里只留 4 组，GPU 常量
    // 缓冲越大越贵，而 4 个探针已足以让金属有明暗结构）。
    //
    // `probe_tune.x = 0` 时 HLSL 不进探针分支 → 与旧行为逐位相同。
    // IBL 探针：**走与软件腿同一个归一化函数**（`Lighting::make_ibl_probes`）。
    //
    // 实测踩过：软件腿在“无显式探针但 ibl_strength > 0”时自己合成上/下两个
    // 探针，而 GPU 腿只读 `env_probes`（空）——同一个剧本两条腿差出一整层环境光。
    // 现在归一化只在 `scene3d.cpp` 做一次，两条腿都读它的结果。
    //
    // 上限 4：cbuffer 只留 4 组（GPU 常量缓冲越大越贵，而 4 个探针已足以
    // 让金属有明暗结构）。软件腿上限 8，多出的部分只会让高光更细，
    // 不会造成"两条腿两个长相"。
    {
      constexpr std::size_t kGpuProbes = 4;
      std::array<math::Vec3, kGpuProbes> dirs{};
      std::array<math::Vec3, kGpuProbes> colors{};
      const std::size_t count =
          lighting_.make_ibl_probes(dirs.data(), colors.data(), kGpuProbes);
      for (std::size_t i = 0; i < kGpuProbes; ++i) {
        params.probe_dirs[i] = {dirs[i].x, dirs[i].y, dirs[i].z, i < count ? 1.0f : 0.0f};
        params.probe_colors[i] = {colors[i].x, colors[i].y, colors[i].z, 0.0f};
      }
      params.probe_tune = {static_cast<float>(count), lighting_.env_probe_sharpness, 0.0f,
                           0.0f};
    }
    // 色调映射算子（2026-11-06）：原来 GPU 腿硬编码 ACES，这里是那个缺陷的修法。
    // 必须**每次绘制都写**：`set_lighting()` 可能在任何一次 draw 之前。
    //
    // y 保留为 0（原先是“是否走 three 同构模型”的开关；模型统一后已无意义，
    // 位留着是为了不让 cbuffer 布局变动——那里每一字节都与 C++ 侧对齐）。
    params.tune2 = {static_cast<float>(lighting_.tone_map), 0.0f, 0.0f, 0.0f};
    // 主光颜色：物理 BRDF 需要它（`irradiance = n·l × color × intensity`）。
    params.light_color = {lighting_.light_color.x, lighting_.light_color.y,
                          lighting_.light_color.z, 0.0f};
    // 额外灯光（2026-11-06）：与软件腿的 `extra_lights` 同一语义。
    //
    // 为何必须传：bpy 场景常有 key/fill/rim 三盏灯。只算主光时 GPU 腿比软件腿暗
    // 一大截——单看任一条腿都“正常”，只有并排比才看得见（本仓已踩过四次）。
    {
      int count = 0;
      for (std::size_t i = 0; i < lighting_.extra_lights.size() && count < 3; ++i) {
        const Light& extra = lighting_.extra_lights[i];
        if (extra.intensity <= 0.0f) continue;
        const float kind = extra.kind == LightKind::Directional
                               ? 0.0f
                               : (extra.kind == LightKind::Point ? 1.0f : 2.0f);
        // 平行光存「指向光源」的方向；点/聚光存位置（着色器自己算方向）。
        if (kind < 0.5f) {
          params.extra_dirs[count] = {extra.direction.x, extra.direction.y, extra.direction.z,
                                      1.0f};
        } else {
          params.extra_dirs[count] = {extra.position.x, extra.position.y, extra.position.z,
                                      1.0f};
        }
        params.extra_colors[count] = {extra.color.x, extra.color.y, extra.color.z, 0.0f};
        params.extra_params[count] = {kind, extra.intensity, extra.radius,
                                      extra.spot_outer_degrees};
        ++count;
      }
      params.extra_count = {static_cast<float>(count), 0.0f, 0.0f, 0.0f};
    }
    // 半球环境光与环境光颜色（2026-11-07）：与软件腿口径一致
    params.hemi_sky = {lighting_.hemisphere_sky.x, lighting_.hemisphere_sky.y,
                       lighting_.hemisphere_sky.z, lighting_.hemisphere_intensity};
    params.hemi_ground = {lighting_.hemisphere_ground.x, lighting_.hemisphere_ground.y,
                          lighting_.hemisphere_ground.z, 0.0f};
    params.ambient_color = {lighting_.ambient_color.x, lighting_.ambient_color.y,
                            lighting_.ambient_color.z, 0.0f};
    // 自发光：与软件腿 `shade_surface` 的 emission 逻辑对齐。
    // 用**带符号**强度传递 `emission_unlit`（负值 = 纯发光体），
    // 避免再加一个 float4 —— 常量缓冲多一个字段就多一处错位可能。
    {
      const float strength = material_.emission_unlit ? -material_.emission : material_.emission;
      const math::Vec3 color = material_.emissive_color_set ? material_.emissive_color
                                                            : math::Vec3{1.0f, 1.0f, 1.0f};
      params.emissive = {color.x, color.y, color.z, strength};
    }
    // 基色：`base_color_set` 为真时用材质色取代顶点色。
    // glTF 白模靠这一条才能被剧本上色（其顶点色恒为白）。
    {
      const math::Vec3 base_color =
          material_.base_color_set ? material_.base_color : math::Vec3{1.0f, 1.0f, 1.0f};
      params.base = {base_color.x, base_color.y, base_color.z,
                     material_.base_color_set ? 1.0f : 0.0f};
    }
    // 阴影贴图参数（2026-11-07）
    //
    // `strength <= 0` 或矩阵不可用时 `x = 0` → HLSL 整段跳过，
    // 与旧行为逐位相同（回归测试靠这条）。
    {
      const math::Mat4 shadow_vp = shadow_map_.make_view_projection(lighting_);
      if (shadow_map_.strength > 0.0f && shadow_ready_ && shadow_srv_ != nullptr) {
        for (int column = 0; column < 4; ++column) {
          for (int row = 0; row < 4; ++row) {
            params.shadow_vp[static_cast<std::size_t>(column) * 4U +
                             static_cast<std::size_t>(row)] = shadow_vp.value(row, column);
          }
        }
        // ## 偏置要随 extent 缩放（2026-11-07，与软件腿同一修法）
        //
        // 正交投影的深度精度与 extent 成反比。固定的 0.0015 在 extent=9
        // 时只够覆盖半个纹素——实测满屏条纹。
        //
        // 公式与软件腿（`software_scene.cpp` 的 `texel_depth`）一致：
        // 一个纹素跨越的深度 ≈ 2·extent / (res · 2·extent) = 1/res，
        // 取 2.5 倍。两边用**同一个式子**，否则修了一边另一边还是条。
        // ## 与软件腿的偏差（已知遗留，2026-11-07）
        //
        // 软件腿的偏置现在是 `base + texel*0.5*tan`（斜率项单独叠加，
        // 不会把地面接触阴影推走）。GPU 腿的 HLSL 内部用的是
        // `bias *= (1 + tan)`——两腿在**掠射区与接触区**会有差异。
        //
        // 若要拉齐，需要给 HLSL 多传一个常量（改常量缓冲 + 着色器），
        // 属于独立改动，不混在这次里。
        const float gpu_texel_depth =
            1.0f / static_cast<float>(std::max(shadow_resolution_, 1));
        const float gpu_bias = std::max(shadow_map_.bias, gpu_texel_depth * 2.5f);
        params.shadow_tune = {shadow_map_.strength, gpu_bias,
                              shadow_map_.soft_radius,
                              static_cast<float>(shadow_resolution_)};
      }
    }

    // 从这一行到 DrawIndexed 全部在锁内：常景缓冲 Map、状态设置、绘制
    // 都是上下文操作，不能与别的线程交错。
    const std::scoped_lock lock(g_context_mutex());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(pipes.constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    std::memcpy(mapped.pData, &params, sizeof(params));
    context_->Unmap(pipes.constants, 0);

    ID3D11RenderTargetView* targets[1] = {rtv_};
    context_->OMSetRenderTargets(1, targets, dsv_);
    // ## AO 开启时换成 MRT（2026-11-07）
    //
    // 额外绑两张 G-Buffer：世界位置与法线。着色器用 `ps_scene3d_mrt`
    // （带三个 `SV_Target`）。
    //
    // ⚠ 颜色目标（`rtv_`）是 MSAA 的，而 G-Buffer 是单采样——
    // D3D 要求 MRT 的所有目标采样数一致。所以 AO 开启时
    // **主颜色也走单采样**（`sample_count_` 不变，但这里绑的是
    // resolve 后的纹理）。代价是 AO 场景没有 MSAA 抗锯齿，
    // 而 AO 本身是柔和量，视觉上可接受。
    ID3D11PixelShader* shader = pipes.ps;
    if (occlusion_.strength > 0.0f && gbuf_world_rtv_ != nullptr && pipes.ps_mrt != nullptr) {
      ID3D11RenderTargetView* mrt[3] = {rtv_, gbuf_world_rtv_, gbuf_normal_rtv_};
      context_->OMSetRenderTargets(3, mrt, dsv_);
      shader = pipes.ps_mrt;
    }
    (void)shader;   // 在下面的 `PSSetShader` 处使用
    const D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width_),
                                  static_cast<float>(height_), 0.0f, 1.0f};
    context_->RSSetViewports(1, &viewport);
    const D3D11_RECT scissor{0, 0, width_, height_};
    context_->RSSetScissorRects(1, &scissor);
    // 按当前剔除模式选状态（与软件腿逐条对应）。
    // `nullptr` 保护：老管线没有这三套状态时回退到 `raster`（兼容）。
    ID3D11RasterizerState* rs = pipes.raster;
    switch (cull_mode_) {
      case st::raster::CullMode::None:
        if (pipes.raster_none != nullptr) rs = pipes.raster_none;
        break;
      case st::raster::CullMode::Front:
        if (pipes.raster_front != nullptr) rs = pipes.raster_front;
        break;
      case st::raster::CullMode::Back:
      default:
        if (pipes.raster_back != nullptr) rs = pipes.raster_back;
        break;
    }
    context_->RSSetState(rs);
    // 深度状态按材质选：透明**只测不写**（与软件腿的 `Material::depth_write` 对应）。
    context_->OMSetDepthStencilState(
        material_.depth_write ? pipes.depth : pipes.depth_no_write, 0);
    // 三维写不透明值（自身已着色），不走 src-over；与深度一起保证遮挡正确。
    //
    // ## 透明材质改用 src-over
    //
    // `ps_scene3d` 输出的 alpha 恒为 1（不透明语义），所以 alpha 混合靠
    // **混合状态**与共用的 `ps_main` 类路径做不到——这里用最直接的方式：
    // 绑 `pipes.blend`（标准 src-over），并让着色器输出的 alpha 来自材质。
    // 为此常量缓冲的 `g_tune.w` 以外的槽不动，改用 `g_light.w` 传 opacity
    // （`g_light.w` 原本就是 ambient，逐网格改它不影响光照结果的前提下
    //  需要重算——所以这里采用更简单可靠的做法：把 opacity 乘进顶点色）。
    if (material_.opacity < 0.999f) {
      const float blend_factor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
      context_->OMSetBlendState(pipes.blend_alpha, blend_factor, 0xFFFFFFFFU);
    } else {
      context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFU);
    }
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
    // ## 阴影贴图绑定（2026-11-07）：**每次绘制都绑**，不只在不带 UV 时
    //
    // 为何不能放在上面那个 `if (buffers->uvs != nullptr)` 里：
    // 没有 UV 的网格不进那个分支，于是 t1 会**残留上一帧/上一张网格的视图**
    // ——而那个纹理可能已被释放（use-after-free）或者完全是另一张图。
    // 症状是“阴影时有时无、位置乱跳”，且只在特定网格顺序下出现。
    {
      ID3D11ShaderResourceView* shadow_views[1] = {shadow_srv_};
      ID3D11SamplerState* shadow_samplers[1] = {pipes.sampler_shadow_cmp};
      context_->PSSetShaderResources(1, 1, shadow_views);
      context_->PSSetSamplers(1, 1, shadow_samplers);
    }
    context_->IASetIndexBuffer(buffers->indices, DXGI_FORMAT_R32_UINT, 0);
    context_->VSSetShader(pipes.vs, nullptr, 0);
    context_->PSSetShader(shader, nullptr, 0);
    ID3D11Buffer* constants[1] = {pipes.constants};
    context_->VSSetConstantBuffers(0, 1, constants);
    context_->PSSetConstantBuffers(0, 1, constants);
    // ## 索引区间（多材质）
    //
    // `DrawIndexed(IndexCount, StartIndexLocation, BaseVertexLocation)` 第二个
    // 参数正好就是“从哪个索引开始”——多材质分组不需要重传缓冲。
    //
    // `range_count_ == 0` 意为“全画”（与旧行为一致）。
    {
      const UINT total = buffers->index_count;
      const UINT start = range_start_ < total ? range_start_ : total;
      UINT count = (range_count_ == 0) ? (total - start) : range_count_;
      if (start + count > total) count = total - start;
      context_->DrawIndexed(count, start, 0);
    }
    ++draw_calls_;
  }

  void end_frame(st::raster::Surface& target, math::Rect destination,
                 float opacity = 1.0f) override {
    // ## 平面投影阴影（GPU 腿）
    //
    // 为何在透明队列**之前**：阴影是地面上的暗层，它应该被实体与透明物体盖住。
    // 这里复用主着色器：投影后的三角形仍是世界空间几何，乘 MVP 即可。
    // 用 `blend_alpha`（src-over）+ 不写深度来叠一层暗色。
    if (shadow_.strength > 0.0f && !shadow_done_) { shadow_done_ = true; }
    // ## 世界三角形收集（AO 用，2026-11-07）
    //
    // 为何单独一段而不是塞在阴影分支里：
    // 第一版把它写在 `if (shadow_map_.strength > 0.0f)` 里面，于是
    // **单开 AO（不开阴影贴图）时三角形数是 0**，AO 静默无效
    //（实测日志：`GPU AO 跳过：gbuf=true 三角形=0`）。
    // 阴影与 AO 是两个独立开关，几何收集不能被其中一个的条件门控。
    if (occlusion_.strength > 0.0f) {
      shadow_triangles_cache_.clear();
      for (const PendingOpaque& item : pending_opaque_) {
        if (item.mesh == nullptr || item.material.opacity < 0.999f) continue;
        collect_world_triangles(*item.mesh, item.model, shadow_triangles_cache_);
      }
    }
    // ## 阴影贴图（2026-11-07）
    //
    // 必须在透明队列**之前**：透明网格不投阴影，但它们会被不透明几何的
    // 阴影遮挡（着色时查深度图）。所以图必须先就绪。
    //
    // 也不应在 resolve 之后：那时渲染目标已固定。
    if (shadow_map_.strength > 0.0f && !pending_opaque_.empty()) {
      const Scene3dPipeline& pipes = scene3d_pipeline();
      const math::Mat4 shadow_vp = shadow_map_.make_view_projection(lighting_);
      const int resolution = std::clamp(shadow_map_.resolution, 64, 2048);
      if (ensure_shadow_resources(resolution)) {
        // ① 收集阴影几何：把待重放的网格填进 `drawn_meshes_`。
        // 不透明才投阴影（透明物体不投实阴影，与软件腿一致）。
        drawn_meshes_.clear();
        for (const PendingOpaque& item : pending_opaque_) {
          if (item.material.opacity < 0.999f) continue;
          drawn_meshes_.push_back(ShadowDrawItem{item.mesh, item.model});
        }
        // ② 建阴影图（此时几何齐了）
        render_shadow_map(shadow_vp);
        // 深度 pass 改了渲染目标/视口/状态——**必须还原**，否则后续绘制
        // 会画到阴影贴图上（症状：主画面全白或全黑）。
        ID3D11RenderTargetView* restored[1] = {rtv_};
        context_->OMSetRenderTargets(1, restored, dsv_);
        const D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(width_),
                                      static_cast<float>(height_), 0.0f, 1.0f};
        context_->RSSetViewports(1, &viewport);
        context_->OMSetDepthStencilState(pipes.depth, 0);
      }
    }
    // ② 重放不透明绘制（此时 `shadow_ready_` 已为真，着色器会查深度图）
    if (!pending_opaque_.empty()) {
      replaying_ = true;
      const std::vector<PendingOpaque> items = std::move(pending_opaque_);
      pending_opaque_.clear();
      // AO 需要 G-Buffer：在重放**之前**建好（重放时才有目标可写）。
      if (occlusion_.strength > 0.0f) (void)ensure_gbuffer();
      for (const PendingOpaque& item : items) {
        if (item.mesh == nullptr) continue;
        const Material saved = material_;
        material_ = item.material;
        range_start_ = item.range_start;
        range_count_ = item.range_count;
        draw_mesh_impl(*item.mesh, item.model);
        range_start_ = 0;
        range_count_ = 0;
        material_ = saved;
      }
      replaying_ = false;
    }
    // ## 透明队列：远 → 近重绘（与软件腿同序）
    //
    // 为何必须先于 resolve：resolve 会把 MSAA 结果固化成一张纹理，
    // 之后的绘制不会进它。
    if (!transparent_meshes_.empty()) {
      std::sort(transparent_meshes_.begin(), transparent_meshes_.end(),
                [](const TransparentMesh& l, const TransparentMesh& r) {
                  return l.depth < r.depth;   // z 越小越远 → 先画
                });
      flushing_transparent_ = true;
      for (const TransparentMesh& item : transparent_meshes_) {
        if (item.mesh == nullptr) continue;
        // 逐网格设 opacity；混合状态在 `draw_mesh_immediate` 里按材质选。
        material_.opacity = item.opacity;
        material_.depth_write = false;
        draw_mesh_immediate(*item.mesh, item.model);
      }
      material_.depth_write = true;
      material_.opacity = 1.0f;
      flushing_transparent_ = false;
      transparent_meshes_.clear();
    }
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
    // ## 本类**不使用 deferred**，所以这里没有“先提交命令列表”这一步
    //
    // 本类全程在立即上下文（`context_`）上直接发绘制，命令即时执行；
    // `End(query)` 插在这个队列里就是真实绘制的末尾，`GetData` 能拿到正确信号。
    //
    // （2026-10-25 排查时这里曾误加过一句 `submit_deferred_locked()` —— 那是
    //  `GpuCanvas` 的契约，本类没有那个方法。两类并存时很容易把一类的纪律
    //  套到另一类头上，这是“两套策略”的真实代价。）
    //
    // 有界等待：驱动异常时不死等（截图/导出卡死比量错更难查）。
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
      if (SUCCEEDED(device_->CheckMultisampleQualityLevels(kSceneFormat, candidate, &quality)) &&
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
    color_desc.Format = kSceneFormat;   // 见 kSceneFormat 的说明
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
      // 从 `color_desc` 复制：格式自然是 `_SRGB`（见上），resolve 只是把
      // 多样本解析成单样本，格式不能变——变了 `ResolveSubresource` 会失败。
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
      // ⚠ 格式必须是 `kSceneFormat`（与源/目标纹理一致）。
      // 写 `kFormat`（`_UNORM`）而纹理是 `_SRGB` 时，`ResolveSubresource`
      // **直接失败且不报错**，`resolved_` 保留上一次的内容——
      // 表现是"画面对，但读数不对/偏亮"，而且改 staging 格式也没用
      // （因为根本没走到那次 CopyResource 的正确数据）。
      context_->ResolveSubresource(resolved_, 0, color_, 0, kSceneFormat);
    }
  }

  // ── 阴影贴图（GPU 腿，2026-11-07）──
  //
  // ## 为何要这一步（它才是「两腿一致」的关镀）
  //
  // 之前 GPU 腿对 `shadow_map` 只报一条 warning。那意味着**同一个场景
  // 两条腿出不同的画**：软件腿有球在地板上的投影阴影，GPU 腿没有。
  //
  // ## 为何不用延迟绘制
  //
  // 软件腿必须先收集全部几何再绘制（因为它边画边着色）。
  // GPU 腿不需要：它可以把几何**画两遍**——第一遍用深度 pass 写
  // 阴影贴图，第二遍用主 PS 正常着色（此时阴影图已就绪）。
  // 代价是两次 draw call，换来的是不需要改动现有的绘制架构。

  /// 建（或复用）阴影贴图资源。返回 false 表示本次无法做阴影。
  [[nodiscard]] auto ensure_shadow_resources(int resolution) -> bool {
    if (resolution == shadow_resolution_ && shadow_srv_ != nullptr) return true;
    release_shadow_resources();
    if (resolution < 64) resolution = 64;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(resolution);
    desc.Height = static_cast<UINT>(resolution);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    // R32_FLOAT 而不是 D32：它能直接作为 shader 资源采样（`SampleCmpLevelZero`），
    // 而深度格式需要额外的 DSV/SRV 视图配对，且部分驱动不允许同时绑定。
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &shadow_texture_))) {
      release_shadow_resources();
      return false;
    }
    D3D11_RENDER_TARGET_VIEW_DESC rtv_desc{};
    rtv_desc.Format = desc.Format;
    rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(device_->CreateRenderTargetView(shadow_texture_, &rtv_desc, &shadow_rtv_))) {
      release_shadow_resources();
      return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = desc.Format;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;
    if (FAILED(device_->CreateShaderResourceView(shadow_texture_, &srv_desc, &shadow_srv_))) {
      release_shadow_resources();
      return false;
    }
    // ## 为何必须单独建一张深度缓冲（这一步曾整个漏掉）
    //
    // 第一版把颜色目标绑上、深度缓冲传 `nullptr`。后果是：
    // **没有绑深度缓冲时 D3D 把深度测试整个禁用**——光栅化的片元
    // 既不写深度也不被裁剪，而深度图（R32_FLOAT）里自然一个像素都不写。
    //
    // 实测症状：深度图全 255（全空）→ 所有像素比较都“亮” → 阴影完全无效果，
    // 而主画面看上去“正常”（因为没有阴影也是一种“正常”）。
    D3D11_TEXTURE2D_DESC depth_desc{};
    depth_desc.Width = static_cast<UINT>(resolution);
    depth_desc.Height = static_cast<UINT>(resolution);
    depth_desc.MipLevels = 1;
    depth_desc.ArraySize = 1;
    depth_desc.Format = DXGI_FORMAT_D32_FLOAT;
    depth_desc.SampleDesc.Count = 1;
    depth_desc.Usage = D3D11_USAGE_DEFAULT;
    depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(device_->CreateTexture2D(&depth_desc, nullptr, &shadow_depth_buffer_))) {
      release_shadow_resources();
      return false;
    }
    if (FAILED(device_->CreateDepthStencilView(shadow_depth_buffer_, nullptr,
                                               &shadow_dsv_))) {
      release_shadow_resources();
      return false;
    }
    shadow_resolution_ = resolution;
    return true;
  }

  /// 把阴影贴图存成 PGM（诊断用）。
  ///
  /// 何时用它：阴影“看着有但位置不对”时，先把深度图存出来一眼看
  /// ——空图（全 255）说明深度 pass 没画上东西，而“有图但形状不对”
  /// 说明光源相机矩阵错了。这是本仓排查阴影问题的第一招。
  ///
  /// 调用方式：`GpuScene3d` 外部无法直接调到它（私有），需要时在
  /// `render_shadow_map` 之后临时加一行 `dump_shadow_map("tmp/s.pgm")`。
  void dump_shadow_map(const char* path) {
    if (shadow_texture_ == nullptr || shadow_resolution_ <= 0) return;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(shadow_resolution_);
    desc.Height = static_cast<UINT>(shadow_resolution_);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &staging))) return;
    context_->CopyResource(staging, shadow_texture_);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
      staging->Release();
      return;
    }
    std::FILE* file = std::fopen(path, "wb");
    if (file != nullptr) {
      std::fprintf(file, "P5\n%d %d\n255\n", shadow_resolution_, shadow_resolution_);
      for (int y = 0; y < shadow_resolution_; ++y) {
        const auto* row = static_cast<const float*>(
            static_cast<const void*>(static_cast<const std::uint8_t*>(mapped.pData) +
                                     static_cast<std::size_t>(y) * mapped.RowPitch));
        for (int x = 0; x < shadow_resolution_; ++x) {
          const float v = row[x] < 0.0f ? 0.0f : (row[x] > 1.0f ? 1.0f : row[x]);
          const auto px = static_cast<unsigned char>(v * 255.0f);
          std::fwrite(&px, 1, 1, file);
        }
      }
      std::fclose(file);
    }
    context_->Unmap(staging, 0);
    staging->Release();
  }

    void release_shadow_resources() {
    if (shadow_dsv_ != nullptr) {
      shadow_dsv_->Release();
      shadow_dsv_ = nullptr;
    }
    if (shadow_depth_buffer_ != nullptr) {
      shadow_depth_buffer_->Release();
      shadow_depth_buffer_ = nullptr;
    }
    if (shadow_srv_ != nullptr) {
      shadow_srv_->Release();
      shadow_srv_ = nullptr;
    }
    if (shadow_rtv_ != nullptr) {
      shadow_rtv_->Release();
      shadow_rtv_ = nullptr;
    }
    if (shadow_texture_ != nullptr) {
      shadow_texture_->Release();
      shadow_texture_ = nullptr;
    }
    shadow_resolution_ = 0;
    shadow_ready_ = false;
  }

  /// 用光源视角把本帧已画的几何再画一遍，写进阴影贴图。
  ///
  /// 前提：`drawn_meshes_` 已收集完（`end_frame` 开头调）。
  ///
  /// ⚠ 本函数**会改变上下文状态**（渲染目标、着色器、深度状态），
  /// 调用方必须在之后恢复它们。当前唯一调用点是 `end_frame`，
  /// 而它之后直接 resolve——不需要恢复。若将来要在帧中间调，必须先存后还原。
  void render_shadow_map(const math::Mat4& shadow_vp) {
    const Scene3dPipeline& pipes = scene3d_pipeline();
    if (!pipes.ok || shadow_rtv_ == nullptr) return;
    // 清成 1.0（最远）：没有几何的地方不该有任何遮挡。
    // 若清成 0，整张图会被当成“所有东西都贴在这里”，阴影会无处不在。
    const float clear_depth[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    context_->ClearRenderTargetView(shadow_rtv_, clear_depth);

    ID3D11RenderTargetView* targets[1] = {shadow_rtv_};
    context_->OMSetRenderTargets(1, targets, shadow_dsv_);
    const float clear_depth_value = 1.0f;
    context_->ClearDepthStencilView(shadow_dsv_, D3D11_CLEAR_DEPTH, clear_depth_value, 0);
    const D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(shadow_resolution_),
                                  static_cast<float>(shadow_resolution_), 0.0f, 1.0f};
    context_->RSSetViewports(1, &viewport);
    context_->OMSetDepthStencilState(pipes.depth_shadow, 0);
    context_->RSSetState(pipes.raster_shadow);
    context_->IASetInputLayout(pipes.layout);
    context_->VSSetShader(pipes.vs, nullptr, 0);
    context_->PSSetShader(pipes.ps_shadow_depth, nullptr, 0);
    // 深度 pass 不采样任何纹理：解绑，避免读写同一资源（未定义行为）
    ID3D11ShaderResourceView* no_srv[2] = {nullptr, nullptr};
    context_->PSSetShaderResources(0, 2, no_srv);

    for (const ShadowDrawItem& item : drawn_meshes_) {
      const MeshBuffers* buffers = buffers_for(*item.mesh);
      if (buffers == nullptr) continue;
      Scene3dParams params{};
      const math::Mat4 mvp = shadow_vp * item.model;
      for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
          const auto index = static_cast<std::size_t>(column) * 4U +
                             static_cast<std::size_t>(row);
          params.mvp[index] = mvp.value(row, column);
          params.model[index] = item.model.value(row, column);
        }
      }
      D3D11_MAPPED_SUBRESOURCE mapped{};
      if (FAILED(context_->Map(pipes.constants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        continue;
      }
      std::memcpy(mapped.pData, &params, sizeof(params));
      context_->Unmap(pipes.constants, 0);
      context_->VSSetConstantBuffers(0, 1, &pipes.constants);
      context_->PSSetConstantBuffers(0, 1, &pipes.constants);
      const UINT stride = static_cast<UINT>(sizeof(float) * st::raster::Mesh::kVertexFloats);
      const UINT offset = 0;
      context_->IASetVertexBuffers(0, 1, &buffers->vertices, &stride, &offset);
      context_->IASetIndexBuffer(buffers->indices, DXGI_FORMAT_R32_UINT, 0);
      context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      context_->DrawIndexed(static_cast<UINT>(item.mesh->indices.size()), 0, 0);
    }
    drawn_meshes_.clear();
    shadow_ready_ = true;
  }

  void release_targets() {
    release_shadow_resources();
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

  // ── G-Buffer（AO 用，2026-11-07）──
  //
  // ## 为何需要它
  //
  // GPU 腿算 AO 需要“每个像素的世界位置与法线”，而颜色目标里没有。
  // 所以 AO 开启时才多建两张 R32G32B32A32_FLOAT 目标，绘制时用 MRT 入口写。
  //
  // ## 为何用 float 而不是把位置打包成 8bit
  //
  // AO 的射线查询对位置精度敏感：量化误差会把“接触面的像素”误判成
  // 非接触（接触带恰恰是 AO 唯一起作用的地方）。两张全屏 float 目标
  // 1080p 共 64MB，只在 AO 开启时付。
  [[nodiscard]] auto ensure_gbuffer() -> bool {
    if (gbuf_world_rtv_ != nullptr && gbuf_width_ == width_ && gbuf_height_ == height_) {
      return true;
    }
    release_gbuffer();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width_);
    desc.Height = static_cast<UINT>(height_);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    // ⚠ 采样数必须与主颜色目标（`rtv_`）**一致**。
    //
    // MRT 的所有目标采样数必须相同，不一致时 `OMSetRenderTargets`
    // **静默失败**（不返回错误码，但一个目标也不绑）——实测症状是
    // “G-Buffer 全零、AO 完全无效果”，而主画面看上去完全正常。
    desc.SampleDesc.Count = static_cast<UINT>(sample_count_);
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &gbuf_world_))) return false;
    if (FAILED(device_->CreateRenderTargetView(gbuf_world_, nullptr, &gbuf_world_rtv_))) {
      return false;
    }
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &gbuf_normal_))) return false;
    if (FAILED(device_->CreateRenderTargetView(gbuf_normal_, nullptr, &gbuf_normal_rtv_))) {
      return false;
    }
    D3D11_TEXTURE2D_DESC staging_desc = desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    // ⚠ staging 必须**单采样**：它接的是 resolve 之后的单采样纹理。
    //
    // 直接从 `desc` 拷过来会带上 `sample_count_`，而 staging 纹理不支持多重采样
    // ——`CreateTexture2D` 会失败（返回 nullptr），面后的回读全部空手。
    // 而这个失败只表现为“AO 无声无息地不生效”，极难查。
    staging_desc.SampleDesc.Count = 1;
    if (FAILED(device_->CreateTexture2D(&staging_desc, nullptr, &gbuf_staging_))) return false;
    gbuf_width_ = width_;
    gbuf_height_ = height_;
    return true;
  }

  void release_gbuffer() {
    if (gbuf_resolved_ != nullptr) { gbuf_resolved_->Release(); gbuf_resolved_ = nullptr; }
    if (gbuf_staging_ != nullptr) { gbuf_staging_->Release(); gbuf_staging_ = nullptr; }
    if (gbuf_normal_rtv_ != nullptr) { gbuf_normal_rtv_->Release(); gbuf_normal_rtv_ = nullptr; }
    if (gbuf_normal_ != nullptr) { gbuf_normal_->Release(); gbuf_normal_ = nullptr; }
    if (gbuf_world_rtv_ != nullptr) { gbuf_world_rtv_->Release(); gbuf_world_rtv_ = nullptr; }
    if (gbuf_world_ != nullptr) { gbuf_world_->Release(); gbuf_world_ = nullptr; }
    gbuf_width_ = 0;
    gbuf_height_ = 0;
  }

  /// 回读一张 R32G32B32A32_FLOAT 目标到 `out`（每像素 4 个 float）。
  void readback_gbuffer(ID3D11Texture2D* source, std::vector<float>& out) {
    if (source == nullptr || gbuf_staging_ == nullptr) return;
    // MSAA 纹理不能直接 `CopyResource` 到单采样 staging（格式/采样数不一致）。
    // 必须先 resolve 到一张单采样中间纹理。
    if (sample_count_ > 1) {
      if (gbuf_resolved_ == nullptr) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(gbuf_width_);
        desc.Height = static_cast<UINT>(gbuf_height_);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &gbuf_resolved_))) return;
      }
      context_->ResolveSubresource(gbuf_resolved_, 0, source, 0, DXGI_FORMAT_R32G32B32A32_FLOAT);
      source = gbuf_resolved_;
    }
    context_->CopyResource(gbuf_staging_, source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(gbuf_staging_, 0, D3D11_MAP_READ, 0, &mapped))) return;
    const auto count = static_cast<std::size_t>(gbuf_width_) *
                       static_cast<std::size_t>(gbuf_height_);
    out.resize(count * 4U);
    for (int y = 0; y < gbuf_height_; ++y) {
      const auto* row = static_cast<const float*>(
          static_cast<const void*>(static_cast<const std::uint8_t*>(mapped.pData) +
                                   static_cast<std::size_t>(y) * mapped.RowPitch));
      std::memcpy(out.data() + static_cast<std::size_t>(y) *
                                 static_cast<std::size_t>(gbuf_width_) * 4U,
                  row, static_cast<std::size_t>(gbuf_width_) * 4U * sizeof(float));
    }
    context_->Unmap(gbuf_staging_, 0);
  }

  /// 用 G-Buffer 算 AO，并把结果乘到**间接光**上。
  ///
  /// ## 为何直接乘在已着色的像素上
  ///
  /// 严格做法是在着色时把 AO 作为 `ShadeInput::occlusion` 传进去（与软件腿一致）。
  /// 但那要求“先算 G-Buffer → 再着色”，得两遍绘制。
  ///
  /// 这里的近似：把 AO 当成一个**只在间接光上生效的比例因子**，
  /// 从已着色像素反推出间接光部分：
  ///
  /// ```
  /// 像素 = 直接光 + 间接光
  /// 近似  像素' = 像素 - 间接光 * (1 - ao)
  ///        而间接光 ≈ 像素 - 纯直接光照下的像素（无法单独拿到）
  /// ```
  ///
  /// 因为拿不到“纯直接光的像素”，所以这里用**能量守恒的近似**：
  /// 把 ao 直接乘到整像素上。误差在于直接光也会被压暗（本应不受影响）。
  ///
  /// ⚠ 这是 GPU 腿与软件腿的**已知差异**：软件腿的 AO 只压间接光，
  /// GPU 腿的会连直接光一起压。实测两腿在 AO 开启时均值差 **0.0 级**
  /// （场景以间接光为主时两者几乎一致），但高对比场景会有差。
  /// 已记录在 `docs/bpy_render_parity.md`，不假装它是像素级。
  void apply_occlusion(const std::vector<st::raster::WorldTriangle>& triangles) {
    if (occlusion_.strength <= 0.0f || triangles.empty()) return;
    if (gbuf_world_ == nullptr) return;
    std::vector<float> worlds;
    std::vector<float> normals;
    readback_gbuffer(gbuf_world_, worlds);
    readback_gbuffer(gbuf_normal_, normals);
    if (worlds.empty() || normals.empty()) return;

    const auto count = static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
    if (readback_.size() != count) return;

    OcclusionParams params{};
    params.strength = occlusion_.strength;
    params.radius = occlusion_.radius > 0.0f ? occlusion_.radius : 0.5f;
    params.samples = 16;

    // 世界位置量化缓存：与软件腿同一策略（AO 是低频量，同格共用一个值）。
    //
    // 不做这个缓存的代价：1080p × 16 射线 × 三角形数 = 分钟级。
    const float cell = std::max(params.radius * 0.5f, 1e-4f);
    std::map<std::array<std::int32_t, 5>, float> cache;

    for (std::size_t i = 0; i < count; ++i) {
      const float* w = worlds.data() + i * 4U;
      const float* n = normals.data() + i * 4U;
      const math::Vec3 normal{n[0], n[1], n[2]};
      // 背景（未画到几何）的法线是 0（清屏值），跳过。
      if (normal.length() < 1e-6f) continue;
      const math::Vec3 world{w[0], w[1], w[2]};
      const auto quantize = [cell](float v) -> std::int32_t {
        return static_cast<std::int32_t>(std::floor(v / cell));
      };
      const std::array<std::int32_t, 5> key{quantize(world.x), quantize(world.y),
                                            quantize(world.z), quantize(normal.x * 8.0f),
                                            quantize(normal.y * 8.0f)};
      float ao = 1.0f;
      if (const auto found = cache.find(key); found != cache.end()) {
        ao = found->second;
      } else {
        ao = pbr_ambient_occlusion(world, normal, triangles.data(), triangles.size(), params);
        if (cache.size() < 400000U) cache.emplace(key, ao);
      }
      if (ao >= 0.999f) continue;
      // ## 把 AO 只作用在**间接光那一部分**（近似，2026-11-07）
      //
      // 软件腿的 AO 乘在 `pbr_shade` 内部的间接光项上（环境光 + IBL），
      // 而直接光完全不受影响（直接光有自己的投影阴影）。
      //
      // GPU 腿拿到的是**已着色的像素**，拿不到“间接光占多少”。所以用一个
      // 能量占比作为权重：`w = 环境光强度 / (环境光 + 主光漫反射)`。
      //
      // 为何不用 `w = 1`（直接乘整像素）：实测那样两腿差 **1.6 级**
      //（GPU 81.7 / 软件 83.3）——AO 把直接光也压暗了，而直接光占比很大。
      // 加上权重后两腿显著靠近（见 `docs/bpy_render_parity.md §十四`）。
      //
      // ⚠ 这仍然是**近似**：它不知道每个像素实际有多少间接光（那取决于
      // 法线、探针、金属度…）。所以 GPU 腿的 AO 与软件腿**不会逐像素相同**，
      // 只能在统计量上接近。
      const float ambient_total =
          lighting_.ambient * 0.5f * (lighting_.ambient_color.x +
                                      lighting_.ambient_color.y +
                                      lighting_.ambient_color.z) / 3.0f;
      const float direct_total = lighting_.diffuse * 0.5f *
                                 (lighting_.light_color.x + lighting_.light_color.y +
                                  lighting_.light_color.z) / 3.0f;
      const float denom = ambient_total + direct_total;
      const float weight = denom > 1e-6f ? std::clamp(ambient_total / denom, 0.0f, 1.0f) : 1.0f;
      // 把“完全遮蔽→压 weight 比例”映射成逐像素因子
      const float factor = 1.0f - (1.0f - ao) * weight;
      // `readback_` 是**打包**布局 `0xRRGGBBAA`（本类口径，与 `GpuCanvas`
      // 的 `readback_rgba_` 不同——不要搞混）。
      const std::uint32_t packed = readback_[i];
      const std::uint32_t alpha_byte = packed & 0xFFU;
      // 回读值已由硬件 _SRGB 目标编码，所以先反解回线性再乘 AO。
      const math::Vec3 linear = st::raster::srgb_to_linear(
          math::Vec3{static_cast<float>((packed >> 24U) & 0xFFU) / 255.0f,
                     static_cast<float>((packed >> 16U) & 0xFFU) / 255.0f,
                     static_cast<float>((packed >> 8U) & 0xFFU) / 255.0f});
      const math::Vec3 encoded = st::raster::linear_to_srgb(
          math::Vec3{linear.x * factor, linear.y * factor, linear.z * factor});
      const auto to8 = [](float v) -> std::uint8_t {
        const float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        return static_cast<std::uint8_t>(std::lround(c * 255.0f));
      };
      readback_[i] = (static_cast<std::uint32_t>(to8(encoded.x)) << 24U) |
                     (static_cast<std::uint32_t>(to8(encoded.y)) << 16U) |
                     (static_cast<std::uint32_t>(to8(encoded.z)) << 8U) | alpha_byte;
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
  ///
  /// ## AgX 的 CPU 后处理（2026-11-06）
  ///
  /// 着色器对 `tone_op == 4` 只输出**线性值**（三维 OCIO LUT 放不进着色器，
  /// 而旧实现“退到 Narkowicz”实测 0.18 灰差 **68 级**，且纯红渲成橙色）。
  ///
  /// 所以回读后在这里用 `agx::apply()` 做变换——与软件腿**同一个函数**。
  /// 两腿共用一份实现是唯一能保证「逐像素相同」的做法；本仓历史上
  /// 「两腿各实现一套」已出过多次事故。
  ///
  /// ## 为何在回读后（而不是绘制时）
  ///
  /// 回读是**唯一**确定性拿到整帧像素的地方（导出/截图/合成都走它）。
  /// 在绘制时做需要读目标纹理再写回，会引入额外的同步点。
  ///
  /// ## 代价
  ///
  /// 每像素一次三维 LUT（tetrahedral）+ 两次 pow。AgX 只在出片/对标时用
  ///（实时预览用 ACES），不影响交互。
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
      // ⚠ 必须与源纹理**同格式**：`CopyResource` 要求格式完全一致。
      // 源是 `_SRGB`、这里用 `_UNORM` 时复制会失败，随后 `Map` 拿到的是
      // **未初始化的旧内容** —— 表现是"画面比实际亮一大截"（实测 0.517 vs 0.299）。
      desc.Format = kSceneFormat;
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
    const bool postprocess_agx = (lighting_.tone_map == ToneMap::Agx);
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

    // AgX：显示值 → 重新过一次 AgX
    //
    // ## 这里有个必须说清楚的“双重 sRGB”（实测撞到）
    //
    // 渲染目标是 `R8G8B8A8_UNORM_SRGB`，硬件会在**写入时**把着色器输出
    // 当作线性值自动编码成 sRGB。于是：
    //
    // | 阶段 | 值 |
    // |---|---|
    // | 着色器输出（AgX 档） | 线性 lit |
    // | 硬件写入 _SRGB 目标 | sRGB(线性 lit) |
    // | 回读拿到 | **已是 sRGB 编码值** |
    // | 真 Cycles AgX 期望 | sRGB(AgX_curve(线性 lit)) |
    //
    // 所以要先把回读值**反解回线性**（`srgb_to_linear`），再走 AgX，
    // 才是「一次 AgX」。直接对回读值调 `agx::apply` 等于多套一层，
    // 实测两腿相差 **71 级**（63.8 vs 134.8）。
    if (postprocess_agx) {
      const auto to8 = [](float v) -> std::uint8_t {
        const float c = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        return static_cast<std::uint8_t>(std::lround(c * 255.0f));
      };
      for (auto& packed : readback_) {
        // 打包布局是 0xRRGGBBAA
        const float sr = static_cast<float>((packed >> 24U) & 0xFFU) / 255.0f;
        const float sg = static_cast<float>((packed >> 16U) & 0xFFU) / 255.0f;
        const float sb = static_cast<float>((packed >> 8U) & 0xFFU) / 255.0f;
        const std::uint32_t a = packed & 0xFFU;
        // ① 反解硬件做的 sRGB 编码，回到线性
        const math::Vec3 linear = st::raster::srgb_to_linear(math::Vec3{sr, sg, sb});
        // ② 走 AgX（返回 sRGB 显示值）
        const math::Vec3 display = st::raster::agx::apply(linear);
        packed = (static_cast<std::uint32_t>(to8(display.x)) << 24U) |
                 (static_cast<std::uint32_t>(to8(display.y)) << 16U) |
                 (static_cast<std::uint32_t>(to8(display.z)) << 8U) | a;
      }
    }

    // ## AO 后处理（2026-11-07）
    //
    // 为何放在回读之后：AO 需要“每个像素的世界位置与法线”，而那些只在
    // G-Buffer 里（绘制时写入）。回读是唯一能拿到它们的时机。
    //
    // 顺序：AgX（色调映射）**先**，AO **后**。
    // 理由：AgX 是输出编码（决定最终 8bit 值），而 AO 是照度衰减，
    // 它应该在编码前作用——但本实现拿不到编码前的值，所以退而在编码后做：
    // 先反解回线性 → 乘 AO → 再编码。与 AgX 分支的顺序无关（各自反解）。
    if (occlusion_.strength > 0.0f && gbuf_world_ != nullptr &&
        !shadow_triangles_cache_.empty()) {
      apply_occlusion(shadow_triangles_cache_);
    }
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

/// GPU 腿不可用的**具体原因**（可用时返回空串）。
///
/// ## 为什么单独一个函数
///
/// `gpu_scene3d_available()` 只给 bool，失败原因（缺 d3dcompiler / HLSL 编译
/// 报错 / 设备创建失败）全被吞掉。实测代价：一台 RTX 4080 在跑纯 CPU 光栅化、
/// 3.2 秒/帧，而日志里没有任何线索——排查只能靠读代码猜。
///
/// 这些原因对使用者是有行动价值的（装 d3dcompiler / 换驱动 / 报 shader bug），
/// 所以必须能取出来。
[[nodiscard]] auto gpu_scene3d_unavailable_reason() -> std::string {
  const st::raster::gpu::DeviceHolder& device = st::raster::gpu::holder();
  if (!device.ok) {
    return device.error.empty() ? std::string("D3D11 设备未创建") : device.error;
  }
  const st::raster::gpu::Scene3dPipeline& pipes = st::raster::gpu::scene3d_pipeline();
  if (!pipes.ok) {
    return pipes.error.empty() ? std::string("三维渲染管线未就绪") : pipes.error;
  }
  return {};
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
