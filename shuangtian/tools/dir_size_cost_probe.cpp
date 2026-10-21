// **`file_size` 在目录枚举里该不该查**——按调用方需求分档。
//
// ## 实测背景
//
// `tools/list_dir_cost_probe.cpp`（`src/raster`，9 条目）：
//
// | 实现 | 耗时 | 每条目 |
// |---|---|---|
// | 只枚举（不查状态） | **342 µs** | 38 µs |
// | `status()` + 按需 `file_size()`（**当前**） | **2371 µs** | 264 µs |
// | 旧：`is_directory` + `is_regular_file` + `file_size`（三次） | 3900 µs | 433 µs |
//
// 即：把三次查询合成一次 `status()` 已省 39%，但**剩下的 2371 − 342 = 2029 µs
// 全是 `file_size`**——它单独占了 86% 的 stat 成本。
//
// ## 关键问题
//
// 调用方**真的需要文件大小吗**？
//
// | 调用方 | 用途 | 需要 size？ |
// |---|---|---|
// | `assets_lib` | 素材库列举 | 可能要（显示体积） |
// | `fetch` | 拉取清单 | 不要 |
// | `hash` | 遍历源文件算哈希 | **不要**（要内容，不是大小） |
// | `lint` | 扫源码 | **不要** |
// | `stats` | 统计 | 可能要 |
// | gbcode `page_actions` / `page_data` | 资源树 | 要（列表显示大小） |
//
// 所以值得加一个**可选开关**：不需要 size 的调用方少一次 stat/条目。
//
// 本探针量这个开关的收益上限。
//
// 判据：报“查 size / 不查 size”的差值。

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>

#include "st/core/print.hpp"
#include "st/probe/probe.hpp"

namespace {

namespace sys = std::filesystem;

constexpr int kRounds = 60;

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
      .title = "目录枚举里 file_size 的成本占比（决定要不要加可选开关）",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("dir_size_cost_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  struct Dir {
    const char* path;
    const char* note;
  };
  const Dir dirs[] = {
      {"src/raster", "9 条目，源文件"},
      {"include/st/ui", "头文件较多的目录"},
      {"src/ui/components", "少量大文件"},
  };

  st::print("{:<24}{:>8}{:>16}{:>16}{:>12}\n", "目录", "条目", "status 不查size",
            "status+size", "size 占比");
  for (const Dir& item : dirs) {
    const sys::path path{item.path};
    std::error_code ec;
    if (!sys::is_directory(path, ec)) {
      st::print("{:<24}  （不存在）\n", item.path);
      continue;
    }
    std::size_t count = 0;
    for (const auto& entry : sys::directory_iterator(path, ec)) {
      (void)entry;
      ++count;
    }

    // A) status 但不查 size
    std::size_t sink_a = 0;
    const double a_ns = best_ns([&] {
      std::error_code inner;
      std::size_t found_dirs = 0;
      for (const auto& entry : sys::directory_iterator(path, inner)) {
        const sys::file_status status = entry.status(inner);
        if (inner) {
          inner.clear();
          continue;
        }
        if (sys::is_directory(status)) ++found_dirs;
      }
      sink_a += found_dirs;
    });

    // B) status + 查 size
    std::size_t sink_b = 0;
    const double b_ns = best_ns([&] {
      std::error_code inner;
      std::size_t total = 0;
      for (const auto& entry : sys::directory_iterator(path, inner)) {
        const sys::file_status status = entry.status(inner);
        if (inner) {
          inner.clear();
          continue;
        }
        if (sys::is_regular_file(status)) {
          const auto size = entry.file_size(inner);
          if (!inner) total += static_cast<std::size_t>(size);
        }
      }
      sink_b += total;
    });

    const double size_share = b_ns > 0.0 ? (b_ns - a_ns) / b_ns * 100.0 : 0.0;
    st::print("{:<24}{:>8}{:>11.0f} µs{:>13.0f} µs{:>11.0f}%\n", item.path, count,
              a_ns / 1000.0, b_ns / 1000.0, size_share);
    (void)sink_a;
    (void)sink_b;
  }

  st::print("\n判读：\n");
  st::print("  · 若 size 占比 > 50% → 加一个“不查 size”的可选参数值得：\n");
  st::print("    不需要大小的调用方（`lint` / `hash` / `fetch`）能省掉一大半。\n");
  st::print("  · 若占比低 → 不改接口，保持简单。\n");
  return st::probe::main_of(spec, opts, verdict);
}
