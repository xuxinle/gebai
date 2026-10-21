// **`list_dir` 的 3.5 ms 花在哪**——枚举 vs 逐条 stat。
//
// ## 实测
//
// `tools/file_io_cost_probe.cpp`：
//
// | 操作 | 耗时 |
// |---|---|
// | `list_dir("src/raster")` | **3536 µs** |
// | `read_text` 113 KB 源文件 | 205 µs |
// | `read_text` 1.2 KB 小文件 | 160 µs |
//
// 枚举一个目录比读一个 113 KB 的文件贵 **17 倍**——这不合常理。
//
// ## 可能的原因
//
// `fs::list_dir` 的实现里，每个条目做了**三次系统调用**：
//   ① `is_directory()`（stat）
//   ② `is_regular_file()`（stat）
//   ③ `file_size()`（stat）
//
// 而 Windows 上每次 `stat`（`GetFileAttributesEx`）在冷缓存时要几十 µs。
// 若该目录有几十个条目，3 × N 次 stat 就能吃掉几毫秒。
//
// ## 这个探针做什么
//
// 对照三种实现方式（都在同一目录上）：
//   A) `fs::list_dir`（现状：3 次 stat/条目）
//   B) 只用 `directory_iterator` 枚举、**不查任何状态**（只数条目）
//   C) 枚举 + 每条第 1 次 `sys::directory_entry::is_directory`（directory_entry
//      内部会缓存首次 status，所以第 2、3 次查询应当免费）
//
// 若 B ≪ A 而 C ≈ A → 瓶颈在逐条 stat，且 `directory_entry` 的缓存没被利用
//（现行代码用 `iterator` 而不是 `directory_entry`，绕过缓存）。
//
// 判据：报 A/B/C 与条目数。

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>

#include "st/core/fs.hpp"
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
      .title = "list_dir 的 3.5 ms 花在哪：枚举 vs 逐条 stat",
      .criterion = "",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("list_dir_cost_probe", spec, opts)) {
    return st::probe::exit_ok;
  }
  st::probe::result verdict{};

  const std::string dir = "src/raster";
  if (!st::fs::is_directory(dir)) {
    st::print("跳过：需在仓库根运行（找不到 {}）\n", dir);
    return st::probe::main_of(spec, opts, verdict);
  }
  const auto path = sys::path(dir);

  // 条目数
  std::size_t count = 0;
  {
    std::error_code ec;
    for (const auto& item : sys::directory_iterator(path, ec)) {
      (void)item;
      ++count;
    }
  }

  // A) 现状：fs::list_dir（3 次 stat/条目）
  const double a_ns = best_ns([&] {
    const auto entries = st::fs::list_dir(dir);
    (void)entries;
  });

  // B) 只枚举，不查任何状态
  std::size_t seen_total = 0;
  const double b_ns = best_ns([&] {
    std::error_code ec;
    std::size_t seen = 0;
    for (const auto& item : sys::directory_iterator(path, ec)) {
      (void)item;
      ++seen;
    }
    seen_total += seen;
  });

  // C) 枚举 + 用 `directory_entry` 的缓存状态（is_directory 首次会 stat，
  //    同一 entry 上再查 is_regular_file/file_size 应当命中缓存）
  std::size_t bytes_total = 0;
  const double c_ns = best_ns([&] {
    std::error_code ec;
    std::size_t dirs = 0;
    sys::directory_iterator iterator(path, ec);
    for (const auto& item : iterator) {
      const bool is_dir = item.is_directory(ec);
      const bool is_file = item.is_regular_file(ec);   // 应命中 directory_entry 的缓存
      if (is_file && !is_dir) {
        const auto size = item.file_size(ec);
        if (!ec) dirs += static_cast<std::size_t>(size);
      }
    }
    bytes_total += dirs;
  });

  st::print("目录 {} 共 {} 个条目\n\n", dir, count);
  st::print("A fs::list_dir（现状）        {:>9.1f} µs  → {:>6.1f} µs/条目\n", a_ns / 1000.0,
            count == 0 ? 0.0 : a_ns / 1000.0 / static_cast<double>(count));
  st::print("B 只枚举（不查状态）          {:>9.1f} µs  → {:>6.1f} µs/条目\n", b_ns / 1000.0,
            count == 0 ? 0.0 : b_ns / 1000.0 / static_cast<double>(count));
  st::print("C 枚举 + directory_entry 查询  {:>9.1f} µs  → {:>6.1f} µs/条目\n", c_ns / 1000.0,
            count == 0 ? 0.0 : c_ns / 1000.0 / static_cast<double>(count));
  st::print("\nB/A = {:.2f}× · C/A = {:.2f}×\n", a_ns > 0.0 ? b_ns / a_ns : 0.0,
            a_ns > 0.0 ? c_ns / a_ns : 0.0);
  st::print("（自检：B 轮共看到 {} 条、C 轮共累加 {} 字节）\n", seen_total, bytes_total);

  st::print("\n判读：\n");
  const double stat_cost = a_ns - b_ns;
  if (stat_cost > b_ns) {
    st::print("  逐条 stat 的成本（A−B = {:.1f} µs）**大于枚举本身** → 瓶颈在这里。\n",
              stat_cost / 1000.0);
    if (c_ns < a_ns * 0.7) {
      st::print("  而 C（用 `directory_entry` 缓存）快 {:.2f}× → **值得改**：\n", a_ns / c_ns);
      st::print("    `directory_iterator` 迭代出的就是 `directory_entry`，\n");
      st::print("    同一 entry 上重复查 `is_directory`/`is_regular_file`/`file_size`\n");
      st::print("    应当命中它缓存的 status——但现行代码没有利用这个性质。\n");
    } else {
      st::print("  但 C 与 A 相当 → `directory_entry` 缓存也没帮上，\n");
      st::print("  说明每次查询都真的落到系统调用（Win32 实现可能如此）。\n");
      st::print("  方向只能是**减少调用次数**（上层缓存，已由 gbcode 的 dir_cache 做了）。\n");
    }
  } else {
    st::print("  枚举本身（B）就是主体 → stat 不是瓶颈，别在这上面改。\n");
  }
  return st::probe::main_of(spec, opts, verdict);
}
