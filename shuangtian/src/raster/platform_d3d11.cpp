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
  float4 g_axis_extra;  // x=渐变种类(0线性/1径向/2扫拂) y=半径
  float4 g_viewport;    // 视口（物理像素）
};

Texture2D g_texture : register(t0);
SamplerState g_sampler : register(s0);

struct VsOut {
  float4 position : SV_Position;
  float2 local : TEXCOORD0;   // 相对矩形左上角
  float2 pixel : TEXCOORD1;   // 物理像素坐标
};

VsOut vs_main(uint vertex_id : SV_VertexID) {
  // 三角带四个角：(0,0) (1,0) (0,1) (1,1)
  float2 uv = float2(vertex_id & 1u, (vertex_id >> 1u) & 1u);
  float2 p = g_rect.xy + uv * g_rect.zw;
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

float4 ps_main(VsOut input) : SV_Target {
  const int mode = (int)g_mode.x;
  const float2 half_size = g_rect.zw * 0.5;
  float4 source = g_color;
  float coverage = 1.0;

  if (mode == 0 || mode == 1) {
    // 实心 / 渐变圆角矩形：SDF 给出解析覆盖率（1px 过渡带）
    coverage = saturate(0.5 - rounded_sdf(input.local, half_size, g_radii));
  }
  if (mode == 1) {
    // 渐变：直接从**渐变纹理**采样（CPU 侧按 stops 生成 256×1 的预乘色带）——
    // 这样 linear/radial/sweep 与任意数量的色标都是精确的，
    // 而不需要在着色器里重建多色标插值。
    float t = 0.0;
    if (g_axis_extra.x < 0.5) {
      const float2 axis = g_axis.zw - g_axis.xy;
      t = saturate(dot(input.pixel - g_axis.xy, axis) / max(dot(axis, axis), 1e-6));
    } else if (g_axis_extra.x < 1.5) {
      t = saturate(length(input.pixel - g_axis.xy) / max(g_axis_extra.y, 1e-6));
    } else {
      const float2 d = input.pixel - g_axis.xy;
      t = frac(atan2(d.y, d.x) / 6.28318530718 + 1.0);
    }
    source = g_texture.Sample(g_sampler, float2(t, 0.5));
  } else if (mode == 2) {
    // 覆盖率遮罩（R8）× 纯色：字形与路径遮罩走这条
    coverage = g_texture.Sample(g_sampler, input.local / g_rect.zw).r;
  } else if (mode == 3) {
    // 预乘 RGBA 位图（离屏画布合成）
    source = g_texture.Sample(g_sampler, input.local / g_rect.zw);
  }

  float factor = coverage * g_mode.y;
  if (g_mode.z > 1.5) {
    // 圆角裁剪：用同一个 SDF 求覆盖率（不需要额外的遮罩纹理）
    const float2 clip_half = g_clip.zw * 0.5;
    factor *= saturate(0.5 - rounded_sdf(input.pixel - g_clip.xy, clip_half, g_clip_radii));
  }
  if (factor <= 0.0) discard;
  // 预乘输出（与软件画布同一混合空间）
  return float4(source.rgb * factor, source.a * factor);
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
};
static_assert(sizeof(ShaderParams) % 16 == 0, "常量缓冲必须是 16 字节的倍数");

/// 绘制模式（与 HLSL 里的 `g_mode.x` 对应）。
enum class DrawMode : std::uint32_t {
  SolidRoundRect = 0,
  GradientRoundRect = 1,
  CoverageMask = 2,
  Bitmap = 3,
};

/// 着色器与固定状态（设备级共享；懒创建）。
struct Pipeline {
  ID3D11VertexShader* vs{nullptr};
  ID3D11PixelShader* ps{nullptr};
  ID3D11Buffer* constants{nullptr};
  ID3D11BlendState* blend{nullptr};
  ID3D11RasterizerState* raster_scissor{nullptr};
  ID3D11SamplerState* sampler_linear{nullptr};
  ID3D11SamplerState* sampler_point{nullptr};
  std::string error{};
  bool ok{false};

  explicit Pipeline(ID3D11Device* device) {
    if (!compile_shaders(device)) return;
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
    ok = vs != nullptr && ps != nullptr && sampler_linear != nullptr;
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
    if (blend != nullptr) blend->Release();
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
  void fill_path(const Path&, const Paint&, DrawOptions) override { not_yet("fill_path"); }
  void stroke_path(const Path&, const Paint&, float, DrawOptions) override { not_yet("stroke"); }
  void fill_circle(math::Point center, float radius, const Paint& paint,
                   DrawOptions options = {}) override {
    // 圆就是“半径等于半边的圆角矩形”——SDF 下两者完全等价，不需要单独的圆图元
    fill_rect(math::Rect{center.x - radius, center.y - radius, radius * 2.0f, radius * 2.0f}, paint,
              radius, options);
  }
  void draw_shadow(math::Rect, float, float, math::Color, math::Point, DrawOptions) override {
    not_yet("shadow");
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
  void draw_canvas_at(const Surface& source, int x, int y, DrawOptions options = {}) override {
    draw_canvas(source,
                math::Rect{static_cast<float>(x), static_cast<float>(y),
                           static_cast<float>(source.width()), static_cast<float>(source.height())},
                options);
  }
  /// 覆盖率位图混合（字形/路径遮罩）：上传成 R8 纹理，当一个四边形画。
  ///
  /// 这是**软件与 GPU 共享语义**的关键原语：字形渲染产出的就是覆盖率位图，
  /// CPU 逐行混合、GPU 贴纹理——两边像素结果可达一致（字形位图本身同源于
  /// 同一套字体引擎，所以字的形状完全一样，只有边缘合成方式不同）。
  void blend_coverage_bitmap(int x, int y, std::span<const float> coverage, int width, int height,
                             const Paint& paint, float opacity, BlendMode blend) override {
    if (width <= 0 || height <= 0 || opacity <= 0.0f || blend != BlendMode::SrcOver) return;
    const std::size_t expected = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (coverage.size() < expected) return;
    const std::int64_t start = st::time::now_ns();
    // `cache_by_pointer=true`：字形位图长期存活且指针稳定（字体引擎自己缓存），
    // 按指针缓存可避免每帧重复上传同一批字形。
    ID3D11ShaderResourceView* view = mask_texture(coverage.data(), width, height, true);
    if (view == nullptr) return;
    const ShaderParams params = base_params(
        DrawMode::CoverageMask,
        math::Rect{static_cast<float>(x), static_cast<float>(y), static_cast<float>(width),
                   static_cast<float>(height)},
        {0.0f, 0.0f, 0.0f, 0.0f}, opacity, paint.color());
    submit(params, view, /*point_sample=*/true);
    log_op(PaintOp::Text, start, expected);
  }

  // —— 裁剪 ——
  void push_clip_rect(math::Rect rect) override {
    ClipFrame frame;
    frame.rect = to_physical(rect).intersect(current_clip().rect);
    frame.mode = 1;
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
  void push_clip_path(const Path&) override { not_yet("push_clip_path"); }
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
  /// 裁剪帧：矩形裁剪走剪裁矩形；圆角裁剪额外带一个 SDF 形状（着色器里求覆盖率）。
  struct ClipFrame {
    math::IntRect rect{0, 0, 0, 0};
    math::Rect shape{};
    std::array<float, 4> radii{0.0f, 0.0f, 0.0f, 0.0f};
    std::uint32_t mode{0};  ///< 0=无 1=矩形（剪裁矩形） 2=圆角（SDF）
  };

  [[nodiscard]] auto current_clip() const noexcept -> const ClipFrame& {
    static const ClipFrame none{};
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
  void submit(const ShaderParams& params, ID3D11ShaderResourceView* view, bool point_sample) {
    const Pipeline& pipes = pipeline();
    if (!pipes.ok) return;
    if (rtv_ == nullptr) return;

    ID3D11RenderTargetView* targets[1] = {rtv_};
    context_->OMSetRenderTargets(1, targets, nullptr);
    const D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(physical_width_),
                                  static_cast<float>(physical_height_), 0.0f, 1.0f};
    context_->RSSetViewports(1, &viewport);
    // 矩形裁剪走剪裁矩形（零着色器成本）：圆角裁剪交给 SDF
    const math::IntRect scissor = current_clip().rect;
    const D3D11_RECT rect{scissor.x, scissor.y, scissor.x + scissor.width,
                          scissor.y + scissor.height};
    context_->RSSetScissorRects(1, &rect);
    context_->RSSetState(pipes.raster_scissor);
    const float blend_factor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    context_->OMSetBlendState(pipes.blend, blend_factor, 0xFFFFFFFFU);
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

  // —— 纹理创建与缓存 ——

  /// R8 覆盖率纹理（字形/路径遮罩）。`cache_by_pointer` 适合指针稳定的源（字形位图）。
  auto mask_texture(const float* coverage, int width, int height, bool cache_by_pointer)
      -> ID3D11ShaderResourceView* {
    if (cache_by_pointer) {
      for (const auto& entry : mask_cache_) {
        if (entry.source == coverage && entry.width == width && entry.height == height) {
          return entry.view;
        }
      }
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (std::size_t index = 0; index < bytes.size(); ++index) {
      const float value = coverage[index];
      bytes[index] = static_cast<std::uint8_t>(std::lround(std::max(0.0f, std::min(1.0f, value)) * 255.0f));
    }
    ID3D11ShaderResourceView* view =
        create_texture(DXGI_FORMAT_R8_UNORM, width, height, bytes.data(),
                       static_cast<std::size_t>(width));
    if (view != nullptr && cache_by_pointer) {
      if (mask_cache_.size() >= kMaskCacheLimit) {
        // 简单淘汰：丢最早一项（字形集在工作集稳定后很少再换）
        mask_cache_.front().view->Release();
        mask_cache_.erase(mask_cache_.begin());
      }
      mask_cache_.push_back(MaskCacheEntry{coverage, width, height, view});
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
    if (FAILED(device_->CreateTexture2D(&desc, &initial, &texture))) return nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    const HRESULT result = device_->CreateShaderResourceView(texture, nullptr, &view);
    texture->Release();
    return SUCCEEDED(result) ? view : nullptr;
  }

  struct MaskCacheEntry {
    const float* source{nullptr};
    int width{0};
    int height{0};
    ID3D11ShaderResourceView* view{nullptr};
  };
  struct RampCacheEntry {
    std::uint64_t key{0};
    ID3D11ShaderResourceView* view{nullptr};
  };
  static constexpr std::size_t kMaskCacheLimit{512};
  static constexpr std::size_t kRampCacheLimit{64};
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
  std::vector<ClipFrame> clip_stack_{};
  std::vector<MaskCacheEntry> mask_cache_{};
  std::vector<RampCacheEntry> ramp_cache_{};
  bool point_sample_{false};
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
auto capabilities() -> Capabilities {
  Capabilities caps;
  caps.solid_shapes = true;
  caps.gradients = true;
  caps.coverage_masks = true;
  caps.bitmaps = true;
  caps.clips = true;
  caps.shadows = false;  // M3b：需多遍模糊（遮罩 RT + 可分离盒式模糊）
  caps.paths = false;    // M4：任意路径填充/描边/路径裁剪
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
  auto canvas = std::make_unique<GpuCanvas>(device.device_or_null(), device.context_or_null(),
                                            physical_width, physical_height, device_scale);
  return std::unique_ptr<Surface>(std::move(canvas));
}

auto live_canvas_count() noexcept -> std::uint32_t { return g_live_canvases.load(); }

}  // namespace st::raster::gpu

#endif  // _WIN32
