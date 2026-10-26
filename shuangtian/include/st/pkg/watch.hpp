#pragma once

/// `st watch` 的**变更侦测内核**。
///
/// ## 为何自己做轮询，而不是 inotify/FSEvents/ReadDirectoryChangesW
///
/// 三条途径都要求**平台分支**（`CONVENTIONS.md` §10 把系统头限制在 `platform_*` 里），
/// 而 watch 的价值与"监视得多准"关系不大：
///
/// · 目标场景是**人类编辑**（保存一次 → 想看到重编结果），毫秒级 vs 数百毫秒级事件无感；
/// · 各平台事件 API 的语义差异很大（inotify 要逐个文件 add watch、ReadDirectoryChangesW
///   有缓冲区溢出与重命名语义、FSEvents 是批量的且可丢事件）——统一封装反而更容易漏事件；
/// · 轮询 `(mtime, size)` 与**增量判定用的是同一份事实**（`deps_entry_matches` 也就是看这两个），
///   所以"watch 认为变了"与"构建会重编"天然同口径；事件 API 则会多一层"事件 ≠ 需重编"的偏差。
///
/// 代价是 CPU：遍历 N 个文件的 `stat`。实测（本机 28 核、仓内 ~1500 个受监视文件）
/// 一轮约 **3–6 ms**，按 300 ms 间隔轮询 ≈ 1–2% 单核——可接受，且可用 `--interval` 调。
///
/// ## 防抖为何必要
///
/// 编辑器保存一个文件常常是**多次写入**（truncate + write，或先写临时文件再 rename）。
/// 第一次写入就会看到 mtime 变化，此时文件可能只写了一半——立刻构建会读到半个文件
/// （更糟：可能编译失败，然后你看到一条"莫名其妙"的错误）。所以要求「静默期」：
/// 最后一次变更后再等 `debounce` 毫秒才认为稳定。

#include <cstdint>
#include <string>
#include <vector>

#include "st/core/error.hpp"

namespace st::pkg {

/// 一个受监视文件的快照（mtime + size）。
struct WatchedFile {
  std::string path{};
  std::int64_t mtime_ns{0};
  std::uint64_t size{0};
};

/// 扫一遍给定根目录，收集受监视文件。
///
/// `include`/`exclude` 是 glob（与 `st.pkg` 同一套 `fs::expand_glob` 语义）。
/// 真实用法里从清单推出来：源文件、测试、`include_dirs` 下的头、`st.pkg` 自身。
[[nodiscard]] auto collect_watched_files(const std::string& root,
                                        const std::vector<std::string>& include,
                                        const std::vector<std::string>& exclude)
    -> Result<std::vector<WatchedFile>>;

/// 比较两次快照，返回**发生变化的路径**（新增/删除/修改都算）。
///
/// 三个方向都报：只看"修改"会漏掉「新建了文件但没进清单」之外的另一种情形——
/// 删掉一个源文件后，构建需要重编（否则产物里留着旧代码，那是踩过的缺陷）。
[[nodiscard]] auto diff_watched(const std::vector<WatchedFile>& before,
                               const std::vector<WatchedFile>& after)
    -> std::vector<std::string>;

/// 从清单推出该监视哪些文件。
///
/// 判据：**构建会读的东西**。具体是
/// · `st.pkg` 自身（清单变了要重启构建判定）；
/// · `include_dirs` 下的 `*.hpp`（头改动是最大的一类重编来源）；
/// · 每个 target 的 `sources` 匹配到的文件；工程级 `sources` 同理；
/// · `tests` 匹配到的文件（`st watch` 默认也盯测试）。
///
/// 不监视：`build/`、`.st/`、`tmp/`（产物与日志，监视它们会自激）、
/// `.git/`（与构建无关）、`third_party/` 里的**第三方源码**（它们几乎不变，
/// 而数量大——监视成本高收益低；真要改了可以用 `--force` 全量）。
[[nodiscard]] auto watch_plan_from_manifest(const struct Manifest& manifest)
    -> Result<std::vector<WatchedFile>>;

}  // namespace st::pkg
