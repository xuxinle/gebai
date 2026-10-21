// **文件 IO 与缓存**：目录扫描/文件读取在真实使用中的成本。
//
// ## 为什么值得单独量
//
// `gbcode` 这类应用的核心交互都建立在文件系统上：
//   · 资源管理器展开目录 → `list_dir`
//   · 打开文件 → 读全文件 + 语法高亮
//   · 保存 → 写盘
//   · 会话恢复 → 读 session.json + 若干文件
//
// 这些都要走系统调用。而 `docs/BACKLOG` 记录过一个相关坑：
// 「`ensure_git_log()` 同步跑 9 条 git 子进程（3301ms）」——**进程/IO 的固定开销
// 远高于数据搬运**。所以关键量是**每次操作的固定延迟**，不是吞吐。
//
// ## 这个探针量什么
//
// 在真实项目目录上，量这三件事的单次成本：
//   A) `list_dir`（单层目录枚举）
//   B) 读一个中等大小的源文件（~20 KB）
//   C) 读一个小配置文件（~2 KB）
//
// 判据：找出“固定开销主导”还是“数据量主导”。
// 若固定开销主导（小文件与大文件耗时相近），则优化方向是**减少调用次数**，
// 而不是加快单次读取。

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <vector>

#include "st/core/fs.hpp"
#include "st/core/print.hpp"
#include "st/probe/probe.hpp"

namespace {

constexpr int kRounds = 300;

[[nodiscard]] auto best_ns(const std::function<void()>& body) -> double {
  double best = 1e18;
  for (int round = 0; round < kRounds; ++round) {
    const auto start = std::chrono::steady_clock::now();
    body();
    const auto end = std::chrono::steady_clock::now();
    const double ns = std::chrono::duration<double, std::nano>(end - start).count();
    if (ns < best) best = ns;
  }
  return best;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "文件 IO 的单次固定成本（判断该减少调用次数还是加快读写）",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("file_io_cost_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  // 找一个真实存在的源码目录与文件。
  const std::string dir = "src/raster";
  const std::string medium_file = "src/raster/canvas.cpp";
  const std::string small_file = "include/st/core/memory.hpp";

  if (!st::fs::is_directory(dir) || !st::fs::is_regular_file(medium_file) ||
      !st::fs::is_regular_file(small_file)) {
    st::print("跳过：需在仓库根运行（找不到 {} / {}）\n", dir, medium_file);
    return st::probe::main_of(spec, opts, verdict);
  }

  const auto medium_size = st::fs::file_size(medium_file).value_or(0);
  const auto small_size = st::fs::file_size(small_file).value_or(0);

  const double list_ns = best_ns([&] {
    const auto entries = st::fs::list_dir(dir);
    (void)entries;
  });
  const double medium_ns = best_ns([&] {
    const auto text = st::fs::read_text(medium_file);
    (void)text;
  });
  const double small_ns = best_ns([&] {
    const auto text = st::fs::read_text(small_file);
    (void)text;
  });

  st::print("目录 {} 与两个源文件\n\n", dir);
  st::print("A list_dir({:<14})              {:>9.2f} µs\n", dir, list_ns / 1000.0);
  st::print("B read_text(medium, {:>6} 字节)  {:>9.2f} µs\n", medium_size, medium_ns / 1000.0);
  st::print("C read_text(small,  {:>6} 字节)  {:>9.2f} µs\n", small_size, small_ns / 1000.0);

  const double bytes_ratio = small_size > 0 ? static_cast<double>(medium_size) /
                                                  static_cast<double>(small_size)
                                            : 0.0;
  const double time_ratio = small_ns > 0.0 ? medium_ns / small_ns : 0.0;
  st::print("\n文件大小比 {:.1f}× · 读取耗时比 {:.2f}×\n", bytes_ratio, time_ratio);
  st::print("\n判读：\n");
  if (time_ratio < bytes_ratio / 2.0) {
    st::print("  耗时比远小于大小比 → **固定开销主导**（打开/关闭/元数据）。\n");
    st::print("  优化方向是**减少调用次数**（批量读、缓存命中、避免每帧 stat），\n");
    st::print("  而不是加快单次读写。每次调用约 {:.1f} µs 的固定成本。\n",
              std::min(medium_ns, small_ns) / 1000.0);
  } else {
    st::print("  耗时比接近大小比 → **数据量主导**，方向是减少读取的字节数\n");
    st::print("  （如只读可视区域、增量加载）。\n");
  }
  st::print("\n参考：`docs/BACKLOG` 记录过 `ensure_git_log()` 同步跑 9 条 git 子进程\n");
  st::print("      = 3301 ms（每条 ~367 ms）——**进程启动**的固定成本比文件 IO 高两个量级。\n");
  return st::probe::main_of(spec, opts, verdict);
}
