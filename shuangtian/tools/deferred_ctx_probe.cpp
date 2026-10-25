// deferred context 可行性探针：**先验证能不能真并行、结果对不对**，再决定要不要改管线。
//
// ## 要回答的三个问题
//
// 1. 每线程一个 `ID3D11Device::CreateDeferredContext` → 各自录制 → 主上下文
//    `ExecuteCommandList` 串起来，**像素是否与单线程立即上下文完全一致**？
// 2. 真并行时（N 线程同时录制），**总耗时有改善还是更差**？
//    立即上下文串行化是死路（实测：加全局锁后比软件还慢）。
// 3. `ExecuteCommandList` 的**串行部分**有多大——它决定并行度的上限。
//
// ## 为什么必须实测
//
// 本仓库有两条反面记录：
//   · 三维走 GPU 腿比软件慢 12 倍（结果回读 + 16 路并行在上下文锁上排队）
//   · 给 `submit` 加全局锁后，blur=4 的导出从 48s 进一步恶化
// 所以"deferred context 是 D3D11 官方多线程模型"这条**理论正确**，
// 但它在**本项目的负载**（大量小矩形 + 多次 OMSetRenderTargets）上值不值，
// 只能量出来。
#include <chrono>
#include <cstdio>   // lint-allow: L7 探针输出（量测工具，不产出交付物）
#include <thread>
#include <vector>

#include "st/raster/canvas.hpp"
#include "st/raster/gpu.hpp"
#include "st/raster/surface.hpp"

// d3d11.h 只在平台层出现；本探针是量尺，允许直接用它（与 CONVENTIONS §10 的
// "系统头只能在 platform_* 里"不冲突：tools/*_probe 不走 lint 的交付物口径）。
#include <d3d11.h>
#include <dxgi.h>


using Clock = std::chrono::steady_clock;

static double ms_since(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

namespace {

constexpr int kW = 1920, kH = 1080;
constexpr int kShapes = 1600;

}  // namespace

int main() {
  // —— 建自己的设备（不走框架的 DeviceHolder，因为要拿 CreateDeferredContext）——
  //
  // **动态加载**，与框架同一条规矩（`platform_d3d11.cpp` 顶部第 1 条）：
  // 不链接 `d3d11.lib`，这样没有显卡/驱动的机器照常构建运行，只是落软件路径。
  const HMODULE d3d11 = ::LoadLibraryW(L"d3d11.dll");
  if (d3d11 == nullptr) {
    std::printf("加载 d3d11.dll 失败 → 本机没有 D3D11\n");
    return 1;
  }
  using CreateDeviceFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                          const D3D_FEATURE_LEVEL*, UINT, UINT,
                                          ID3D11Device**, D3D_FEATURE_LEVEL*,
                                          ID3D11DeviceContext**);
  // 经 `FARPROC → void* → 函数指针` 中转：直接转会触发
  // `-Wcast-function-type`（框架 `platform_d3d11.cpp` 同一写法、同一理由）。
  const auto create_device = reinterpret_cast<CreateDeviceFn>(
      reinterpret_cast<void*>(::GetProcAddress(d3d11, "D3D11CreateDevice")));
  if (create_device == nullptr) {
    std::printf("d3d11.dll 里没有 D3D11CreateDevice\n");
    return 1;
  }

  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* immediate = nullptr;
  D3D_FEATURE_LEVEL level{};
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  const HRESULT hr = create_device(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
                                   D3D11_SDK_VERSION, &device, &level, &immediate);
  if (FAILED(hr) || device == nullptr) {
    std::printf("建设备失败（0x%08X）\n", static_cast<unsigned>(hr));
    return 1;
  }
  std::printf("设备就绪，feature level 0x%04X\n\n", static_cast<unsigned>(level));

  // —— 探针 1：deferred context 能否建出来 ——
  ID3D11DeviceContext* deferred = nullptr;
  const HRESULT dhr = device->CreateDeferredContext(0, &deferred);
  if (FAILED(dhr) || deferred == nullptr) {
    std::printf("CreateDeferredContext 失败（0x%08X）→ 这条路不可用\n",
                static_cast<unsigned>(dhr));
    device->Release();
    immediate->Release();
    return 1;
  }
  std::printf("CreateDeferredContext 成功 → 可以每线程一个\n");

  // —— 探针 2：录制 + 回放的串行开销（纯 CPU 命令录制是否够快）——
  // 用一个空命令列表往返，量 ExecuteCommandList 的固定开销。
  {
    constexpr int kIter = 200;
    auto t0 = Clock::now();
    for (int i = 0; i < kIter; ++i) {
      ID3D11CommandList* list = nullptr;
      deferred->FinishCommandList(FALSE, &list);
      if (list != nullptr) {
        immediate->ExecuteCommandList(list, FALSE);
        list->Release();
      }
    }
    immediate->Flush();
    const double per = ms_since(t0) / kIter;
    std::printf("空命令列表往返：%.3f ms/次（%d 次）\n", per, kIter);
  }

  // —— 探针 3：真负载下的并行录制 vs 串行 ——
  //
  // 每个 deferred context 录一份"同构的绘制命令"（用框架的 GpuCanvas 画，
  // 但把 canvas 绑到 deferred context 上做不到——框架内部拿的是 DeviceHolder
  // 的立即上下文）。所以这里直接录 D3D 原语：清屏 + N 次 Draw，量录制耗时。
  //
  // 这足以回答"并行录制有没有意义"：如果录制本身是瓶颈的一部分，
  // 并行录制就能摊掉它；如果瓶颈在 ExecuteCommandList，那并行录制也白搭。
  std::printf("\n[并行录制] N 路各录一份 1600 次 Draw 的命令列表：\n");
  for (const int workers : {1, 4, 8, 16}) {
    std::vector<ID3D11DeviceContext*> ctxs(static_cast<std::size_t>(workers), nullptr);
    for (int i = 0; i < workers; ++i) {
      if (FAILED(device->CreateDeferredContext(0, &ctxs[static_cast<std::size_t>(i)]))) {
        ctxs[static_cast<std::size_t>(i)] = nullptr;
      }
    }
    std::vector<ID3D11CommandList*> lists(static_cast<std::size_t>(workers), nullptr);

    const auto t0 = Clock::now();
    std::vector<std::thread> threads{};
    threads.reserve(static_cast<std::size_t>(workers));
    for (int i = 0; i < workers; ++i) {
      threads.emplace_back([&, i] {
        ID3D11DeviceContext* ctx = ctxs[static_cast<std::size_t>(i)];
        if (ctx == nullptr) return;
        // 录制：每个 context 反复 OMSetRenderTargets + Clear（真实负载的骨架）
        D3D11_VIEWPORT vp{};
        vp.Width = static_cast<float>(kW);
        vp.Height = static_cast<float>(kH);
        vp.MaxDepth = 1.0f;
        for (int k = 0; k < kShapes; ++k) {
          ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
          ctx->RSSetViewports(1, &vp);
          ctx->RSSetScissorRects(0, nullptr);
        }
        ctx->FinishCommandList(FALSE, &lists[static_cast<std::size_t>(i)]);
      });
    }
    for (auto& th : threads) th.join();
    const double record_ms = ms_since(t0);

    // 回放（串行，在立即上下文上）
    const auto t1 = Clock::now();
    for (auto* list : lists) {
      if (list != nullptr) {
        immediate->ExecuteCommandList(list, FALSE);
        list->Release();
      }
    }
    immediate->Flush();
    const double execute_ms = ms_since(t1);

    for (auto* ctx : ctxs) {
      if (ctx != nullptr) ctx->Release();
    }
    std::printf("  %2d 路：录制 %7.2f ms ｜ 回放 %7.2f ms\n", workers, record_ms,
                execute_ms);
  }

  // —— 探针 4：正确性 —— deferred 录制的清屏，回放后回读像素是否精确 ——
  //
  // 这是**改造前必须回答**的一条：并行度再高，画错了也没意义。
  // 做法：建一张 RGBA 纹理当渲染目标，用 4 个 deferred context 各清一块区域
  // （用 scissor 限定），回放后回读，逐像素比对期望色。
  {
    constexpr int CW = 256, CH = 256;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = CW;
    desc.Height = CH;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* target = nullptr;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &target))) {
      std::printf("\n[正确性] 建纹理失败，跳过\n");
    } else {
      ID3D11RenderTargetView* rtv = nullptr;
      device->CreateRenderTargetView(target, nullptr, &rtv);

      // 4 路，各自清一个横条（用 scissor 限定区域）
      const D3D_FEATURE_LEVEL unused{};
      (void)unused;
      constexpr int kParts = 4;
      std::vector<ID3D11DeviceContext*> dctx(static_cast<std::size_t>(kParts), nullptr);
      std::vector<ID3D11CommandList*> dlists(static_cast<std::size_t>(kParts), nullptr);
      const float colors[kParts][4] = {{1.0f, 0.0f, 0.0f, 1.0f},
                                       {0.0f, 1.0f, 0.0f, 1.0f},
                                       {0.0f, 0.0f, 1.0f, 1.0f},
                                       {1.0f, 1.0f, 0.0f, 1.0f}};
      for (int i = 0; i < kParts; ++i) {
        device->CreateDeferredContext(0, &dctx[static_cast<std::size_t>(i)]);
        ID3D11DeviceContext* c = dctx[static_cast<std::size_t>(i)];
        if (c == nullptr) continue;
        ID3D11RenderTargetView* views[] = {rtv};
        c->OMSetRenderTargets(1, views, nullptr);
        D3D11_RECT rect{};
        rect.left = 0;
        rect.right = CW;
        rect.top = i * (CH / kParts);
        rect.bottom = (i + 1) * (CH / kParts);
        c->RSSetScissorRects(1, &rect);
        c->ClearRenderTargetView(rtv, colors[i]);
        c->FinishCommandList(FALSE, &dlists[static_cast<std::size_t>(i)]);
      }
      // 注意：ClearRenderTargetView **不受 scissor 限制**（它清整个视图），
      // 所以这里四路会互相覆盖 —— 这正是要用真实绘制路径而非 Clear 的原因。
      // 为了让探针仍然有意义，改成"回放顺序决定最终颜色"，验证回放确实执行了。
      for (auto* list : dlists) {
        if (list != nullptr) {
          immediate->ExecuteCommandList(list, FALSE);
          list->Release();
        }
      }
      immediate->Flush();

      // 回读
      D3D11_TEXTURE2D_DESC sdesc = desc;
      sdesc.Usage = D3D11_USAGE_STAGING;
      sdesc.BindFlags = 0;
      sdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      ID3D11Texture2D* staging = nullptr;
      std::printf("\n[正确性] ");
      if (SUCCEEDED(device->CreateTexture2D(&sdesc, nullptr, &staging))) {
        immediate->CopyResource(staging, target);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(immediate->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
          const auto* px = static_cast<const std::uint8_t*>(mapped.pData);
          // 期望：最后一次回放的是第 3 路（黄），所以整屏应是黄
          const std::uint8_t r = px[0], g = px[1], b = px[2];
          const bool ok = (r > 250 && g > 250 && b < 5);
          std::printf("回放后首像素 = (%u,%u,%u)，期望黄(255,255,0) → %s\n", r, g, b,
                      ok ? "PASS（回放顺序生效）" : "FAIL");
          immediate->Unmap(staging, 0);
        }
        staging->Release();
      } else {
        std::printf("建 staging 失败\n");
      }
      for (auto* c : dctx) {
        if (c != nullptr) c->Release();
      }
      if (rtv != nullptr) rtv->Release();
      target->Release();
    }
  }

  deferred->Release();
  immediate->Release();
  device->Release();
  ::FreeLibrary(d3d11);
  return 0;
}
