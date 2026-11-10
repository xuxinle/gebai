// **`st::search` 的吞吐量尺**——与 ripgrep（`rg`）在同机同参下对比计时。
//
// ## 量什么
//
// 三件事分别计时，因为它们的瓶颈不同：
//   A) **枚举**（`enumerate`）：只走目录、不读内容 —— 量的是目录遍历的固定开销；
//   B) **内容搜索**（`search_paths`）：mmap + SIMD 扫描 + 线程池 —— 量的是真正的主成本；
//   C) **路径过滤**（`filter_paths`）：在已枚举清单上做子序列模糊匹配 —— 「快速打开」每次
//      按键的成本，应当是**微秒级**（不是磁盘级）。
//
// ## 与 `rg` 对照的口径（关键）
//
// 要能对比，两边的「搜索集合」必须一致。`rg` 默认遵循 `.gitignore` 并跳过二进制/隐藏，
// 本库 `enumerate` 默认同理。所以：
//   · 本库 `enumerate(root)` 得到的文件清单，与 `rg --files` 大体同集合；
//   · `rg <query> <root>` 的内容搜索与 `search_paths(同一清单)` 比。
// 计时口径：
//   · `rg` 用 `--no-config` 关掉用户配置干扰、`--threads` 显式给线程数（本库用
//     `ThreadPool` 默认 = 硬件并发）；均取**多次中的最好一次**（稳态、去抖动）；
//   · ⚠ 只报数不设「必须比 rg 快」的硬门禁——磁盘缓存/杀软/文件数都会主宰实测值，
//     把它做成 fail 门槛会产生随机器波动的红。这里 `ok` 表示「跑通了、数字有值」，
//     真正的判据留给人工读表（bench 探针按 `check_probes.py` 惯例不设 exit 门槛）。
//
// 用法：`build/dev/bin/search_bench <目录> [查询词] [轮数]`
//   缺省目录 = 当前仓库根（`.`），缺省查询 = `the`，缺省轮数 = 5。
//   找不到 `rg` 时只报本库数字并说明「未对照」。

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <functional>
#include <string>
#include <vector>

#include "st/core/fs.hpp"
#include "st/core/log.hpp"
#include "st/core/print.hpp"
#include "st/core/process.hpp"
#include "st/probe/probe.hpp"
#include "st/search/search.hpp"

namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] auto ms_since(Clock::time_point start) -> double {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

/// 取多次运行中的最好一次（毫秒）。
template <typename Fn>
[[nodiscard]] auto best_of(int rounds, Fn&& body) -> double {
  double best = 1e18;
  for (int round = 0; round < rounds; ++round) {
    const double ms = body();
    if (ms < best) best = ms;
  }
  return best;
}

/// 跑一条外部命令（stdout 丢弃）并返回 wall-clock 毫秒。
/// 走 `st::process::run`（平台实现集中在 core）：比 `_popen` 的 shell 字符串更稳
/// （无引号拼接、无 Win/POSIX 分支），`elapsed_ms` 是它自带的测量。
/// 非零退出返回 nullopt。
[[nodiscard]] auto time_command_ms(const std::string& program,
                                   const std::vector<std::string>& args) -> std::optional<double> {
  st::process::Options options;
  options.capture_output = true;
  auto result = st::process::run(program, args, options);
  if (!result.has_value() || result->exit_code != 0) return std::nullopt;
  return static_cast<double>(result->elapsed_ms);
}

/// `rg` 是否可用（`which` 找到即视为可用）。
[[nodiscard]] auto rg_available() -> bool { return st::process::which("rg").has_value(); }

}  // namespace

auto main(int argc, char** argv) -> int {
  const st::probe::spec spec{
      .title = "st::search 吞吐量尺（枚举 / 内容搜索 / 路径过滤，可选对照 ripgrep）",
      .criterion = "",
      .usage_args = "[目录] [查询词] [轮数]",
  };
  const auto opts = st::probe::options::from(argc, argv);
  if (st::probe::help_requested("search_bench", spec, opts)) return st::probe::exit_ok;

  const std::string root =
      opts.positional(0).has_value() ? std::string(*opts.positional(0)) : std::string(".");
  const std::string query =
      opts.positional(1).has_value() ? std::string(*opts.positional(1)) : std::string("the");
  const int rounds = std::max(1, opts.positional_int(2).value_or(5));

  if (!st::fs::is_directory(root)) {
    st::log::err("不是目录：{}\n", root);
    return st::probe::exit_usage;
  }

  st::probe::result verdict{};
  st::log::out("根：{}\n查询：\"{}\"\n轮数：{}\n\n", root, query, rounds);

  // ——— A) 枚举 ———
  st::search::EnumerateResult enumeration;
  const double enumerate_ms = best_of(rounds, [&]() {
    const auto start = Clock::now();
    auto result = st::search::enumerate(root);
    const double ms = ms_since(start);
    if (result.has_value()) enumeration = std::move(*result);
    return ms;
  });
  const std::size_t file_count = enumeration.files.size();
  st::log::out("[A] 枚举：{:>8.2f} ms · {} 个文件（保留 {}/看到 {}，忽略目录 {}，二进制 {}，超大 {}）\n",
               enumerate_ms, file_count, enumeration.stats.files_kept, enumeration.stats.files_seen,
               enumeration.stats.dirs_ignored, enumeration.stats.files_binary,
               enumeration.stats.files_large);
  verdict.add("enumeration_has_files", file_count > 0,
              std::format("{} 文件", file_count));

  // ——— B) 内容搜索（在枚举清单上并行搜索）———
  st::search::SearchOptions search_options;
  const double search_ms = best_of(rounds, [&]() {
    const auto start = Clock::now();
    const auto result = st::search::search_paths(enumeration.files, query, search_options);
    const double ms = ms_since(start);
    (void)result;
    return ms;
  });
  const auto search_result = st::search::search_paths(enumeration.files, query, search_options);
  const std::size_t total_bytes = [&] {
    std::uint64_t sum = 0;
    for (const auto& entry : enumeration.files) sum += entry.size;
    return sum;
  }();
  const double mb = static_cast<double>(total_bytes) / (1024.0 * 1024.0);
  st::log::out("[B] 搜索 \"{}\"：{:>8.2f} ms · {} 处命中（{} 文件）· 扫 {:.1f} MiB → {:.1f} MiB/s\n",
               query, search_ms, search_result.hits.size(), search_result.files_with_hits, mb,
               mb > 0.0 ? mb / (search_ms / 1000.0) : 0.0);
  verdict.add("search_ran", search_ms >= 0.0, std::format("{:.2f} ms", search_ms));

  // ——— C) 路径过滤（模拟「快速打开」每次按键）———
  const double filter_ms = best_of(std::max(rounds, 20), [&]() {
    const auto start = Clock::now();
    const auto hits = st::search::filter_paths(enumeration.files, query, 50);
    const double ms = ms_since(start);
    (void)hits;
    return ms;
  });
  st::log::out("[C] 路径过滤 \"{}\"：{:>8.3f} ms（含排序 + 截断 top50）\n", query, filter_ms);

  // ——— 对照 ripgrep ———
  st::log::out("\n");
  const bool have_rg = rg_available();
  if (!have_rg) {
    st::log::out("对照：**未找到 `rg`**（未对照；本机路径过滤/搜索数字如上）\n");
  } else {
    // `rg --files` 枚举、`rg <query>` 内容搜索。`--no-config` 关用户配置、`--no-messages`
    // 静音报错、`--color never` 省转义。取多次最好一次。
    const std::vector<std::string> files_args{"--files", "--no-config", "--no-messages",
                                              "--color", "never", root};
    const std::vector<std::string> search_args{"--no-config", "--no-messages", "--color", "never",
                                               "--threads", "0", "-F", query, root};
    const double rg_files_ms = best_of(rounds, [&]() { return time_command_ms("rg", files_args).value_or(-1.0); });
    const double rg_search_ms = best_of(rounds, [&]() { return time_command_ms("rg", search_args).value_or(-1.0); });
    if (rg_files_ms >= 0.0) {
      st::log::out("对照 rg --files：{:>8.2f} ms  ·  本库枚举 {:.2f} ms  ·  比 {:.2f}×\n", rg_files_ms,
                   enumerate_ms, rg_files_ms > 0.0 ? enumerate_ms / rg_files_ms : 0.0);
    }
    if (rg_search_ms >= 0.0) {
      st::log::out("对照 rg <query>：{:>8.2f} ms  ·  本库搜索 {:.2f} ms  ·  比 {:.2f}×\n", rg_search_ms,
                   search_ms, rg_search_ms > 0.0 ? search_ms / rg_search_ms : 0.0);
    }
    if (rg_files_ms < 0.0 && rg_search_ms < 0.0) {
      st::log::out("对照：`rg` 存在但命令执行失败（未对照）\n");
    }
  }

  st::log::out("\n判读：\n");
  st::log::out("  · 枚举应是「每文件固定开销 × 文件数」——若远大于 rg --files，瓶颈在逐条 stat；\n");
  st::log::out("  · 内容搜索的主成本是字节扫描——MiB/s 越高越接近 IO 上限（说明 CPU 不是瓶颈）；\n");
  st::log::out("  · 路径过滤必须是**微秒~亚毫秒级**——「快速打开」每次按键不该碰磁盘。\n");
  st::log::out("  · 只报数不设硬门禁：磁盘缓存/杀软/文件数主宰实测，做成 exit 门槛会产生随机红。\n");
  return st::probe::main_of(spec, opts, verdict);
}
