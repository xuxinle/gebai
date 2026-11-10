#pragma once

/// @file search.hpp
/// @brief 高性能文件枚举与内容搜索基础库（对标 ripgrep 的「快」的地基）。
///
/// ## 为什么要有这一层
///
/// 「快速打开文件」与「工作区全局搜索」是两个看起来无关、实则同源的需求：
/// 都要**先有一份工作区文件清单**，再在清单上做**逐文件内容扫描**。过去它们各自
/// 在应用层写一份递归 `list_dir` + `read_text` 循环——每帧/每次按键都重扫磁盘，
/// 大仓库（几万文件、几千 MB）里就是几百毫秒到几秒的卡顿，且两份实现的口径
/// （跳过哪些目录、多大文件、怎么切行）必然漂移。
///
/// 本库把它们收敛成一份**可缓存、可增量、可并行**的基础设施：
/// - `Enumerator`：一次枚举产出**去重后的文件清单**（默认跳过 `.git`/`build`/
///   `node_modules` 等与产物无关的目录、超限文件、二进制文件），带大小与 mtime；
/// - `search_file` / `search_buffer`：对单文件/内存缓冲做内容搜索（字面量 + 迷你
///   正则子集 + 大小写/全词开关），返回 `(行号, 列, 行文本)`；
/// - `search_paths`：对一批文件并行搜索（线程池），聚合结果。
///
/// ## 性能要点（与 ripgrep 同档的关键）
///
/// 1. **内存映射**而非整读：`MappedFile` 把文件页映射进地址空间，搜索时直接扫
///    映射区，省掉一次「内核→用户」的整文件拷贝；大文件尤其明显（几 MB 的
///    一次拷贝就是几毫秒）。
/// 2. **向量化字节扫描**：行尾 `\n` 定位与子串首字节定位都走 `simd_scan`
///    （SSE2/AVX2），不是逐字节循环——这是内容扫描的主成本。
/// 3. **按线程切文件**：文件之间独立，用 `ThreadPool` 并行；单文件内部保持顺序
///    以保证命中顺序稳定（结果先按文件、再按行）。
/// 4. **清单缓存**：`Enumerator` 的结果可跨多次查询复用（工作区不变时），
///    只在文件系统可能变化时重扫——「快速打开」每次按键不再重扫磁盘。
///
/// ## 边界约定
///
/// - 路径一律 UTF-8（`std::string`），与 `st::fs` 同口径；
/// - 失败安全：读不了/编码不识别的文件**跳过**（记入 `stats.skipped`），不中断整批；
/// - 二进制判定与 gbcode 的 `looks_binary` 同一口径（头 8 KiB 出现 `\0` 或控制字符
///   占比 > 5%）——搜索不应把 PE/PNG 当文本。
///
/// 本库属 `st.search` 模块（依赖仅 `st::core` + `st::perf`，不含任何 UI/raster），
/// 因此可被非 GUI 程序与命令行工具复用。

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"
#include "st/core/fs.hpp"

namespace st::search {

// ————————————————————————————————————————————————————————————————————————
// 枚举：工作区文件清单
// ————————————————————————————————————————————————————————————————————————

/// 枚举出的一条文件记录。
struct FileEntry {
  std::string path{};      ///< 绝对路径（UTF-8）
  std::string relative{};  ///< 相对枚举根的路径（用 '/' 归一，用于 glob 与展示）
  std::string name{};      ///< 文件名（展示用）
  std::uint64_t size{0};   ///< 字节数
};

/// 枚举时的二进制筛选策略。
///
/// 三档的区别只在于**要不要为每个文件付出一次 open+read**：
/// - `kNone`：全留（连 `.png` 也列）——最快；
/// - `kByExtension`（默认）：按扩展名排除已知二进制格式——**零 IO**，与 `rg --files`
///   的取舍一致（文件名里就写着“这是二进制”，没必要打开确认）；
/// - `kByContent`：读头部 8 KiB 真判——最准，但**每个文件一次开读**：
///   实测 2051 个文件的仓库上，这一档使枚举从 147 ms 等级退到 1100 ms（7.5×），
///   所以它不是默认值。内容搜索不需要它（`search_paths` 自己会判）。
enum class BinaryFilter : std::uint8_t {
  kNone,
  kByExtension,
  kByContent,
};

/// 枚举选项。
struct EnumerateOptions {
  /// 二进制筛选策略（默认按扩展名，零 IO）。
  BinaryFilter binary_filter{BinaryFilter::kByExtension};
  /// 单文件上限字节（默认 4 MiB）。超过则不参与内容搜索（大日志仍可被「快速打开」列出，
  /// 因为名字不需要读内容——但为简单起见，枚举阶段超限一律不入清单）。
  std::uint64_t max_file_bytes{4U * 1024U * 1024U};
  /// 递归深度上限（0 = 仅本层；默认 32）。
  std::uint32_t max_depth{32};
  /// 额外要跳过的目录名（在默认集合之外追加）。
  std::vector<std::string> extra_ignore_dirs{};
  /// 是否遵循 `.gitignore`（默认 true；仅做简单的行级 glob，不做全部 gitignore 语义）。
  bool respect_gitignore{true};
};

/// 枚举统计（可观测性：让“为什么少/慢”变成可查的数）。
struct EnumerateStats {
  std::uint64_t dirs_visited{0};
  std::uint64_t dirs_ignored{0};    ///< 被忽略目录名 / gitignore 挡下的目录
  std::uint64_t files_seen{0};      ///< 看到过的普通文件总数
  std::uint64_t files_kept{0};      ///< 进入清单的
  std::uint64_t files_binary{0};    ///< 二进制而跳过
  std::uint64_t files_large{0};     ///< 超限而跳过
  std::uint64_t files_ignored{0};   ///< gitignore 挡下
};

/// 枚举结果：文件清单 + 统计。
struct EnumerateResult {
  std::vector<FileEntry> files{};
  EnumerateStats stats{};
};

/// 递归枚举 `root` 下的文本文件清单（按 `relative` 字典序稳定排序）。
///
/// 默认跳过：`.` 开头目录、`build`/`out`/`node_modules`/`dist`/`target`/`.git` 等
/// 产物与版本控制目录、超大文件、二进制文件。所有跳过都计入 `stats`——**不静默**。
[[nodiscard]] auto enumerate(std::string_view root, const EnumerateOptions& options = {})
    -> Result<EnumerateResult>;

/// 在**已枚举清单**上按「子序列模糊匹配」过滤路径（快速打开组件的核心）。
///
/// 匹配规则（与 VSCode / fzf 的 common 形态一致，故意简单可预测）：
/// - 大小写不敏感；
/// - `query` 的各字符必须按顺序出现在 `relative` 中（子序列），不要求连续；
/// - 返回命中的**下标**，按「匹配质量」排序：① 连续段更长优先；② 命中位置越靠后
///   越差（文件名部分命中优于目录部分命中）；③ 越短路径越优先。
///
/// `limit` 限制返回条数（0 = 不限）。空 query 返回全部（截到 limit）。
[[nodiscard]] auto filter_paths(const std::vector<FileEntry>& files, std::string_view query,
                                std::size_t limit = 0) -> std::vector<std::size_t>;

// ————————————————————————————————————————————————————————————————————————
// 内容搜索
// ————————————————————————————————————————————————————————————————————————

/// 搜索选项。
struct SearchOptions {
  bool case_sensitive{false};
  bool whole_word{false};
  /// 正则模式（只支持一个**诚实的子集**：字面量、`.`、`.*`、`^`/`$` 锚定、
  /// `\d`/`\w`/`\s` 字符类。不支持集合/分组/量词——假装支持会静默给出错结果）。
  bool regex{false};
  /// 每文件最多返回多少处命中（默认 200，0 = 不限）。防止单文件刷屏。
  std::size_t max_hits_per_file{200};
  /// 整批最多命中多少处（默认 2000）。
  std::size_t max_hits_total{2000};
  /// 单文件上限字节（默认 4 MiB，与 `EnumerateOptions` 同口径）。
  std::uint64_t max_file_bytes{4U * 1024U * 1024U};
  /// 是否把行文本一并取回（某些调用方只要坐标；关掉省一次拷贝）。
  bool capture_line_text{true};
  /// 行文本最大保存长度（默认 200 字节；展示够用，也避免超长行占内存）。
  std::size_t max_line_bytes{200};
};

/// 一处命中。
struct Hit {
  std::string path{};      ///< 绝对路径（search_buffer 时为空）
  std::string relative{};  ///< 相对路径（展示/分组用）
  std::size_t line{0};     ///< 1 起行号
  std::size_t column{0};   ///< 行内**字节**列（0 起）
  std::string line_text{}; ///< 命中的整行（已 trim，截到 max_line_bytes）
};

/// 一批命中的结果 + 统计。
struct SearchResult {
  std::vector<Hit> hits{};
  std::uint64_t files_scanned{0};
  std::uint64_t files_with_hits{0};
  std::uint64_t skipped_unreadable{0};
  std::uint64_t skipped_binary{0};
  std::uint64_t skipped_large{0};
  bool truncated{false};
};

/// 在一个内存缓冲上搜索。用于「已打开文件的搜索」与单元测试（无需落盘）。
[[nodiscard]] auto search_buffer(std::string_view content, std::string_view query,
                                 const SearchOptions& options = {}) -> std::vector<Hit>;

/// 在一个文件上搜索（内存映射；失败安全——读不了返回空并计入统计）。
[[nodiscard]] auto search_file(std::string_view path, std::string_view query,
                               const SearchOptions& options = {},
                               std::string_view relative = {}) -> Result<std::vector<Hit>>;

/// 对一批文件**并行**搜索（线程池），聚合结果。
///
/// - `progress(已完成文件数, 总数)` 可选：每完成一个文件回调一次（UI 进度条用）；
/// - 结果保持输入顺序（先 file[0] 的命中、再 file[1]……），单文件内按行升序——
///   稳定顺序是「结果可复现」的前提，不能因为并行而抖。
[[nodiscard]] auto search_paths(const std::vector<FileEntry>& files, std::string_view query,
                                const SearchOptions& options = {},
                                const std::function<void(std::size_t, std::size_t)>& progress = {})
    -> SearchResult;

// ————————————————————————————————————————————————————————————————————————
// 小工具（对外暴露，供调用方与测试对齐口径）
// ————————————————————————————————————————————————————————————————————————

/// 这段字节像不像二进制（头 8 KiB 有 NUL 或控制字符 > 5%）。
[[nodiscard]] auto looks_binary(std::string_view bytes) -> bool;

/// 按扩展名判定「已知二进制格式」（`png`/`jpg`/`zip`/`exe`/`ttf` 等）。
/// **零 IO**：只看文件名——枚举阶段靠它避免为每个文件开一次头。
[[nodiscard]] auto looks_binary_by_extension(std::string_view name) -> bool;

/// 是否标识符字符（字母/数字/下划线，含 UTF-8 高位字节按「非词边界」处理）。
[[nodiscard]] auto is_word_char(unsigned char byte) noexcept -> bool;

}  // namespace st::search
