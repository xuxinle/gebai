// 本机 GPU 到底能不能用、以及哪条腿现在会被选中——一句话答清楚。
//
// 为什么单独一个探针：`st doctor` 在无头模式下**不探测 GPU**（有意为之，见
// PITFALLS「无头路径不该探测 GPU」：`auto` 会真建 D3D11 设备，+53MB / +55 线程 /
// +4.6GB 虚拟地址，全付在任何像素产生之前）。但"显卡能不能用"正是本轮的决策依据，
// 于是显式探一次，把设备信息与失败原因都打出来。

#include <cstdint>
#include <format>
#include <string>

#include "st/core/print.hpp"
#include "st/probe/probe.hpp"
#include "st/raster/gpu.hpp"
#include "st/raster/scene3d.hpp"

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "GPU 渲染腿可用性（二维画布 + 三维场景）",
      .criterion = "硬件 GPU 可用时 gpu::available() 为真且创建画布成功；不可用时给出具体原因",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("gpu_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  const auto info = st::raster::gpu::probe();
  if (!info.has_value()) {
    st::print("GPU 不可用：{}\n", info.error().message);
    st::print("→ 二维/三维都会退回软件光栅器（这是**受支持的**状态，不是缺陷）\n");
    verdict.state = st::probe::status::skip;
    verdict.reason = std::format("GPU 不可用：{}", info.error().message);
    return st::probe::main_of(spec, opts, verdict);
  }

  const auto& device = *info;
  st::print("后端           : {}\n", device.backend);
  st::print("适配器         : {}\n", device.adapter);
  st::print("特性级别       : {}\n", device.feature_level);
  st::print("WARP（CPU 模拟）: {}\n", device.warp ? "是" : "否（真硬件）");
  st::print("专用显存       : {:.1f} MiB\n\n",
            static_cast<double>(device.vram_bytes) / (1024.0 * 1024.0));

  const auto caps = st::raster::gpu::capabilities();
  st::print("二维能力：实心形状={} 渐变={} 覆盖率={} 位图={} 裁剪={} 投影={} 路径={} 亚像素={}\n",
            caps.solid_shapes, caps.gradients, caps.coverage_masks, caps.bitmaps, caps.clips,
            caps.shadows, caps.paths, caps.lcd_text);
  st::print("二维完整可用   : {}\n\n", caps.complete() ? "是" : "否");

  // 真建一张画布：`available()` 为真不等于"能建画布"（管线可能建不起来）
  auto canvas = st::raster::gpu::create_canvas(1920, 1080, 1.0f, {});
  st::print("创建 1920×1080 GPU 画布：{}\n",
            canvas.has_value() ? "成功" : std::format("失败（{}）", canvas.error().message));
  verdict.add("创建 1920×1080 GPU 画布", canvas.has_value(),
              canvas.has_value() ? "ok" : canvas.error().message);

  // 三维腿现状：现在只有软件实现，`Scene3D::create` 恒走软件
  st::print("三维 Scene3D   : available={}（当前实现＝软件光栅，不经过 GPU）\n",
            st::raster::Scene3D::available());
  auto scene = st::raster::Scene3D::create(1920, 1080);
  st::print("创建 1920×1080 三维场景：{}\n",
            scene.has_value() ? "成功" : std::format("失败（{}）", scene.error().message));
  verdict.add("创建 1920×1080 三维场景", scene.has_value(),
              scene.has_value() ? "ok" : scene.error().message);

  verdict.conclude();
  return st::probe::main_of(spec, opts, verdict);
}
