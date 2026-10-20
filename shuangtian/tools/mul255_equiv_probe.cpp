// 验证 `(x*n + 127) / 255` 的无除法等价式，再用于 `over_scalar`。
//
// 背景：`over_scalar` 每像素做 **4 次整数除法**（`(dst * inverse + 127) / 255`），
// 而 x86 的整数除法是 20~40 周期/次。短行（圆角控件每行几像素）里这笔成本
// 完全摊不开——实测长 1~6 的 ns/px 是长 512 的 **11~20 倍**。
//
// 候选等价式（图像处理标准技巧）：
//     f1(x) = (x * n + 127) / 255
//     f2(x) = ((x * n + 128) * 257) >> 16
// 两者在 x*n 能放进 32 位时相等。这里穷举验证**本项目的实际取值域**：
//   x ∈ [0, 255]（通道值）、n ∈ [0, 255]（255 - alpha）。
//
// 判据：全部一致才 PASS（否则说明不能替换）。

#include <cstdint>
#include <format>

#include "st/core/print.hpp"
#include "st/probe/probe.hpp"

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "验证 (x*n+127)/255 的无除法等价式（用于 over_scalar 去 4 次除法）",
      .criterion = "x∈[0,255] × n∈[0,255] 全域一致",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("mul255_equiv_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  std::uint64_t checked = 0;
  std::uint64_t mismatches = 0;
  int worst_x = -1;
  int worst_n = -1;
  std::uint32_t worst_a = 0;
  std::uint32_t worst_b = 0;

  for (std::uint32_t x = 0; x <= 255; ++x) {
    for (std::uint32_t n = 0; n <= 255; ++n) {
      const std::uint32_t product = x * n;
      const std::uint32_t classic = (product + 127U) / 255U;
      const std::uint32_t fast = ((product + 128U) * 257U) >> 16U;
      ++checked;
      if (classic != fast) {
        if (mismatches == 0) {
          worst_x = static_cast<int>(x);
          worst_n = static_cast<int>(n);
          worst_a = classic;
          worst_b = fast;
        }
        ++mismatches;
      }
    }
  }

  st::print("检查 {} 组 (x, n)\n", checked);
  st::print("不一致 {} 组\n", mismatches);
  if (mismatches != 0) {
    st::print("首个不一致：x={} n={}  经典={} 快式={}\n", worst_x, worst_n, worst_a, worst_b);
    verdict.state = st::probe::status::fail;
    verdict.reason = "等价式在该取值域内不成立，不能替换";
    return st::probe::main_of(spec, opts, verdict);
  }
  st::print("\nPASS：两者在 x∈[0,255] × n∈[0,255] 上完全一致，可以安全替换。\n");
  return st::probe::main_of(spec, opts, verdict);
}
