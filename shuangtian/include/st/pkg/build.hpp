#pragma once

/// 构建驱动：解析清单 → 生成编译命令 → **直接调用编译器**（不经 CMake/Make）→ 增量构建 → 链接。
/// 产物布局：`<root>/build/<profile>/{obj,bin}/…`。

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"
#include "st/core/fs.hpp"
#include "st/pkg/manifest.hpp"

namespace st::pkg {

struct BuildOptions {
  std::string root{};                 ///< 工程根（空=清单目录）
  std::string profile{"debug"};       ///< debug / release / san
  std::string target{};               ///< 目标名（空=库目标：只编译不链接）
  /// 产物路径覆盖（空 = 用 `build/bin/<目标><后缀>`）。
  ///
  /// ## 唯一用途：重建构建工具自身
  ///
  /// `st` 是自举产物。重建它时默认落点 `build/bin/st.exe` **正是正在运行的文件**
  /// ——Windows 上链接器会报 `failed to write output … permission denied`。
  ///
  /// 所以 `st rebuild` 先编到临时路径（本字段），再 `MoveFile` 换上去
  /// （实测：对运行中的 exe 做重命名是允许的，直接覆盖不行）。
  ///
  /// Linux 上可以直接覆盖，本字段同样可用 —— 两平台走同一条代码路径。
  std::string output_override{};
  std::size_t jobs{0};                ///< 并行编译单元数（0=按**内存预算**与硬件并发推导，见 `pkg/memory.hpp`）
  /// 超大翻译单元的并发上限（0=自动：1）。超大单元按源文件字节数判定。
  std::size_t jobs_large{0};
  /// 可用内存预算（MiB；0=自动探测 cgroup/系统上限）。推导并行度用。
  std::uint64_t max_memory_mb{0};
  /// 交叉编译工具链名（空 = 本机）。命中 `Manifest::toolchains` 中的一项：
  /// 决定编译器、目标系统库、产物后缀与平台宏，并把产物/中间目录与本地档**隔离**。
  std::string toolchain{};
  /// 测试分片数（`st test --test-jobs N`；**1 = 单进程**，与分片前逐位等价）。
  ///
  /// N > 1 时测试可执行被启动 N 次、每次带 `--shard i/N` 各跑一段用例（见 `st/test/test.hpp`）。
  /// 只对 `run_tests` 生效（`build` 忽略它）。
  std::size_t test_jobs{1};
  /// 构建结束时不顺带刷新过期的测试二进制（`st build --no-tests`）。
  ///
  /// 默认 `false`：`st build` 会在测试目标过期时把它**只构建、不运行**地更新一遍，
  /// 避免「改了测试 → 构建 → 直接跑 st_tests → 验的是旧代码」。
  /// 只想快速跑应用的人可以关掉它换取更短的构建时间。
  bool no_tests{false};
  bool verbose{false};                ///< 打印每条编译命令
  bool force{false};                  ///< 忽略增量判定，全量重编
  /// **只检查不产出**（`st check`）：编译单元全部换成 `-fsyntax-only`。
  ///
  /// 为何要在同一条路径上做：单元枚举、标志组装、框架引用、嵌入生成物、第三方放宽
  /// 这几件事在这里是**共用**的——另写一份“检查用的枚举”迟早会与构建分叉，
  /// 而“检查过了但编不过”会被归咎于编译器差异，不是两套枚举不一致（最难查的那种偏）。
  /// 检查模式下：不写 `.o`/`.d`、不查共享对象缓存、不读 PCH、不链接。
  bool check_only{false};
  /// **构建后分离调试信息**（默认 true；命令行 `--no-strip` 关闭）。
  ///
  /// 背景（2026-10-14 产物评估）：`-g1` 并没有像预期那样小——`st.exe`（dev 档）实测
  /// **76.96MB，其中 89% 是 `.debug_*`**（`.debug_info` 47.8MB + `.debug_loclists` 13.2MB），
  /// 而 `.text` 只有 4.45MB。原因是 `-g1` 对 **C++ 模板实例**（libstdc++ 头里爆炸的实例化）
  /// 仍要为每个内联/模板函数生成行号表与 DIE 骨架。
  ///
  /// 而交付物**根本不需要**它：崩溃回溯要的只是「文件名 + 行号」，链接后一份
  /// `objcopy` 就能把它们搬到隔壁的 `<产物>.debug`——产物回到 4.6MB（-94%），
  /// 调试能力一点不丢（gdb 与崩溃器的 `addr2line` 都会找分离符号）。
  ///
  /// 为何**默认开**：不分离的代价是“每个交付物白胖 70MB”，而分离的代价是“零”
  ///（同一份 DWARF 只是换了个文件，且 gdb 自动认它）。想要单文件调试时
  ///（拷一个文件到别处 gdb）用 `--no-strip`。
  bool strip_debug{true};
  bool use_pch{true};                 ///< 使用预编译头（`include/st/pch.hpp`）加速
  std::vector<std::string> extra_include_dirs{};
  std::vector<std::string> extra_flags{};
};

struct BuildStats {
  std::string artifact{};
  /// 本次是否顺手重建了过期的测试二进制。
  ///
  /// 单独记一个字段，是为了让 CLI 能**明确告诉用户「你刚才看到的构建包含测试」**——
  /// 这件事必须可见：它的整个价值就在于消除「以为验了新代码，其实跑的是旧的」。
  bool tests_rebuilt{false};
  std::size_t units_total{0};
  std::size_t units_rebuilt{0};
  std::size_t units_cached{0};
  std::int64_t elapsed_ms{0};
  std::int64_t compile_ms{0};  ///< 编译阶段耗时（不含链接）
  /// 纯链接阶段耗时。**不等同于 `elapsed - compile`**：后者还包含目标单元（应用源码）的
  /// 编译——应用单元在库编译**之后**另起一轮，两者之间还夹着链接指纹比对，
  /// 把它算成"链接"会让耗时分解失真（改一个应用 .cpp 后"链接 9 s"其实是编译 8.5 s）。
  std::int64_t link_ms{0};
  std::size_t workers{0};      ///< 并行度（按内存预算推导而来，除非显式 `--jobs`）
  /// 超大翻译单元的并发上限（源文件 ≥ `kLargeUnitBytes` 者走这道窄闸门）。
  std::size_t workers_large{0};
  /// 采用的编译内存预算（MiB；0 = 不可知，并发退回硬件数）。
  std::uint64_t memory_budget_mb{0};
  /// 并发决策的理由（直接展示给用户："为什么是这个并发数"）。
  std::string concurrency_reason{};
  bool pch_used{false};        ///< 是否用到预编译头
  bool linked{false};          ///< 本次是否真的执行了链接（产物已最新则跳过）
  /// 产物能否在宿主直接执行：交叉档里**目标平台 == 宿主平台**的产物可跑
  /// （Windows 宿主上 mingw 交叉档产出本机 PE）；目标≠宿主（Linux 宿主编 mingw）
  /// 为 false——`st run`/`st test` 拒绝执行并提示在目标平台运行。
  bool runs_on_host{false};
};

/// profile → 编译/链接标志（debug: -O0 -g / release: -O2 -DNDEBUG / san: ASan+UBSan）。
[[nodiscard]] auto profile_flags(std::string_view profile) -> Result<std::vector<std::string>>;
/// 分离产物里的调试信息到 `<产物>.debug`（objcopy 三步：keep-debug → strip-debug → DebugLink）。
///
/// 为何导出它在头文件里：它是**构建器的一个可见行为**（默认开、可 `--no-strip` 关），
/// 而不是内部工具——直接单测它（用真产物）能钉住"产物变小且 `.debug` 有符号"这两件事，
/// 而不必跑一遍完整构建。详细背景见 `src/pkg/build.cpp` 的实现注释。
[[nodiscard]] auto separate_debug_info(const std::string& artifact, std::string_view compiler_path,
                                       std::string_view executable_suffix, bool verbose = false)
    -> Status;
/// 链接所需的系统库（按平台）。
[[nodiscard]] auto default_system_libs() -> std::vector<std::string>;

/// 检测编译器（`ST_CXX`/`CXX` 环境变量 → g++ → clang++ → c++）。
/// 探测 C++ 编译器（`ST_CXX`/`CXX` 环境变量优先，其次 g++/clang++/c++）。
/// 判断一份依赖记录是否与文件当前内容一致（**只比内容哈希**）。
///
/// 增量构建、共享对象缓存、测试二进制过期判定共用这一套：任何地方按时间戳或长度
/// 判定，“`git checkout`/`touch` 白重编”与“等长改写漏报”两个错就会在别处重现。
[[nodiscard]] auto deps_entry_matches(std::string_view path, std::uint64_t recorded_hash) -> bool;

/// 文件内容的 fnv1a64（读不到 → `nullopt`）。
///
/// 选 fnv1a64 而不是 sha256：这里要的是"内容变了没有"，不是抗碰撞证明（攻击者不参与构建）；
/// 实测 151 文件 / 3.9MB：fnv1a64 18.43ms、sha256 30.51ms。
///
/// **不经过缓存**：需要“现在的”内容时用它；构建内部的热路径用带缓存的 `deps_entry_matches`。
[[nodiscard]] auto file_content_hash(std::string_view path) -> std::optional<std::uint64_t>;

/// 清空进程内的内容哈希缓存。
///
/// 缓存的生命周期是**一次构建**（同一构建里同一文件会被几十个单元引用，不缓存就是 4.6 倍重复读盘）。
/// 所以每个构建入口开头清一次：`st watch` 在**同一个进程**里反复调 build，
/// 不清就会用上一轮的旧哈希——症状是“改了文件 watch 也不重编”（完全静默）。
///
/// 公开它也是为了回归测试能在同一进程里反复改写同一个文件后重新取值。
auto clear_content_hash_cache() -> void;

/// `override_compiler` 非空时直接用它（交叉编译工具链走这条路）。
[[nodiscard]] auto detect_compiler(std::string_view override_compiler = {}) -> Result<std::string>;

/// 解析 GCC/Clang 的 `.d` 依赖文件（`目标: 依赖…`，反斜杠续行；支持转义空格）。
///
/// ## 为何导出它（原本是内部函数）
///
/// 它是**增量判定的第二半**：`.d` 吐出依赖路径 → `serialize_deps_meta` 写入 `.meta`
/// → `deps_entry_matches` 拿这些路径去校验。第一步解析错，后面两步全错，
/// 而且**症状完全静默**（改了源码不重编 / 头文件改动检测失效）。
///
/// 实测事故（2026-10-26）：原实现无条件把 `:` 当分隔符，Windows 盘符 `C:` 被吃掉，
/// 所有依赖路径变成 `/Users/...`（无盘符）⇒ `tests/pkg_memory_test.cpp` 改了不重编，
/// **连 `--force` 都治不好**（`.meta` 反复写回同样的坏记录）。
/// 只有能拿到真实 `.d` 内容直接断言，才能钉住这种“解析层”缺陷——
/// 跑一遍完整构建看不出来（它只是“不重编”，不报错）。
[[nodiscard]] auto parse_depfile(std::string_view path) -> std::vector<std::string>;

/// 构建库对象与目标产物（`options.target` 为空时只编译库对象）。
[[nodiscard]] auto build(const Manifest& manifest, const BuildOptions& options) -> Result<BuildStats>;

/// 构建测试可执行文件（库源 + tests + 测试框架入口）并运行；返回退出码。
/// `list_only` = 只列出用例名（`st test --list`，仍可带 filter）；
/// `junit_path` 非空时给测试进程设 `ST_JUNIT_XML`，逐用例结果写成 JUnit XML（CI 消费）；
/// `options.test_jobs > 1` 时按分片并行跑（每片一个测试进程，JUnit 报告由父进程合并）。
/// `include_slow` = 额外跑“慢/环境敏感”用例（见 `test.hpp` 的 `ST_TEST_SLOW`）：
/// 默认 false——它们量机器性能或需真编译，占测试壁钟近三分之一，
/// 且在共享机器上会**偶发红灯**；迭代内循环默认不附这个不确定性。
///
/// `shard_log_dir` = 分片输出的落盘目录（**空 = 系统临时目录**）。
///
/// **为何分片输出一定要落盘**：分片并行时逐用例输出交错在同一个 stdout 里，
/// 而调用方往往只看得到尾部（智能体工具截断、CI 只存末尾 N 行）——
/// “到底哪条用例红了”恰好落在那段之外。实测踩过：失败详情只在 stdout、
/// `.st/logs` 只剩三行摘要，只能重跑定位。落盘后每片一份
/// `test-shard-<i>-of-<n>.log`，且**失败分片的完整输出会追加进 st 日志**。
[[nodiscard]] auto run_tests(const Manifest& manifest, const BuildOptions& options,
                             std::string_view filter, bool list_only = false,
                             std::string_view junit_path = {}, bool include_slow = false,
                             std::string_view shard_log_dir = {})
    -> Result<int>;

/// 链接诊断文本是否表示「产物被运行中的实例占用」（跨三套工具链的文案判定；
/// 命中时 `st build` 的报错会直接给出「结束实例再重编」的下一步）。
/// 纯函数，供 `pkg_build_lock_test` 断言三套文案各命中/不误报。
[[nodiscard]] bool artifact_locked(std::string_view diagnostics);

/// 错误分类：`run_tests` 链路上哪些失败是「链接/构建失败」而非「用例断言红」。
/// 返回 true = 链接失败类（产物没出来或子进程起不来，用例根本没跑）。
/// 纯函数，供 `pkg_test_link_split_test` 断言分类边界。
[[nodiscard]] bool is_link_failure(const st::Error& error);

/// 壳层源快照：目标 `sources` 展开后的全部文件 → 内容哈希。
///
/// `st build` 把它写到 `<产物>.srcs`，`st run` 拿它判“壳层没改就能直接跑现有产物”
/// （XML/JS 应用的源码边界只有壳层 .cpp）。两边必须用**同一个函数**：各写一份
/// 必然在"盯哪几个文件"上分家，而分家的症状是“改了壳层却跑了旧产物”。
[[nodiscard]] auto target_source_stamp(const Manifest& manifest, const TargetSpec& target)
    -> std::string;

/// 测试二进制是否**过期**（需要重建）。
///
/// 判据与增量构建同一套：**比内容，不比时间**。`<binary>.srcs` 里记着这份二进制出生时
/// 各测试源的内容哈希；逐条重算现内容比对即可。时间戳在这里尤其不可用：`git checkout`
/// 会整批刷新它（白编一轮），而等长改写 + 保留时间戳又能把它完全绕过（拿旧结果当新）。
///
/// - 二进制不存在 → `false`（从没跑过测试是**正常状态**）
/// - 出生快照缺失 → `true`（无从校验，保守重建）
/// - 仉一条源内容不一致 → `true`
/// - 读不到的源不参与（它不是"过期"的证据，真正的错误由构建阶段报）
[[nodiscard]] auto test_binary_is_stale(const std::vector<std::string>& test_files,
                                        const std::string& binary) -> bool;

/// 测试二进制的出生快照（`<binary>.srcs` 的内容；写与比共用这一份）。
[[nodiscard]] auto test_source_stamp(const std::vector<std::string>& test_files) -> std::string;
[[nodiscard]] auto test_binary_stamp_path(const std::string& binary) -> std::string;

}  // namespace st::pkg