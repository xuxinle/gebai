#pragma once

/// SIMD 字节扫描（内部接口）：为搜索引擎的**行切分**与**字面量首字节定位**提供
/// 逐字节的向量快路径。语义基准是标量 `memchr`/逐字节比较，SIMD 路径必须与
/// 标量路径**逐字节一致**（`tests/search_simd_test.cpp` 三路对齐断言）。
///
/// 为什么下沉到这一层：内容搜索的主要成本是「把整文件扫一遍找行尾 `\n`」与
/// 「找字面量的首字节」——这两件事都是**线性字节扫描**，标量逐字节循环在几 MB
/// 文件上就是几十毫秒。向量化后同一条 `_mm_cmpeq_epi8 + movemask` 一次看 32 字节，
/// 与 ripgrep 的 memchr 同数量级。
///
/// 边界（`CONVENTIONS.md` L6）：本头/实现是 SIMD intrinsics 的受控例外点，
/// 只对 `const uint8_t*` + 长度做定长向量访存，所有访存由调用方保证区间边界；
/// 不做任何指针算术越界。

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace st::search::simd {

/// 在 `[0, size)` 中找第一个等于 `needle` 的字节，返回其下标；找不到返回 `size`。
///
/// 语义与 `memchr` 完全一致（含 `size == 0` → 返回 0）。
[[nodiscard]] auto find_byte(const std::uint8_t* data, std::size_t size,
                             std::uint8_t needle) noexcept -> std::size_t;

/// 在 `[from, size)` 中找第一个等于 `needle` 的字节，返回下标；找不到返回 `size`。
/// `from >= size` 时返回 `size`。
[[nodiscard]] auto find_byte_from(const std::uint8_t* data, std::size_t size, std::size_t from,
                                  std::uint8_t needle) noexcept -> std::size_t;

/// 在 `[0, size)` 中找**子串** `needle` 的首次出现（`needle_size == 0` 时返回 0）。
/// 找不到返回 `size`。
///
/// 实现：先对首字节做向量化 `find_byte` 定位候选，再 `memcmp` 校验余下字节。
/// 对短 needle（常见：标识符、关键字）这是最省的通用策略——不需要预计算坏字符
/// 表，也不需要 needle 足够长才划算的 rolling-hash。
[[nodiscard]] auto find_substring(const std::uint8_t* data, std::size_t size,
                                  const std::uint8_t* needle, std::size_t needle_size) noexcept
    -> std::size_t;

/// 大小写不敏感的**单字母**定位：在 `[from, size)` 里找第一个「等于 `letter` 的小写或大写」
/// 的字节（只做 ASCII 折叠），返回下标；找不到返回 `size`。
///
/// 为何单独一个函数而不是调用方跑两遍 `find_byte_from`：两遍会把整段缓冲读两次，
/// 而这里只需要**一条**向量循环、两个比较、一次 `or` 掩码——这是不区分大小写搜索
/// 的首字节候选定位，也是它比「先整文件转小写再搜」快一个量级的原因
/// （后者要额外一次全量写内存与一遍额外读）。
[[nodiscard]] auto find_byte_case_insensitive_from(const std::uint8_t* data, std::size_t size,
                                                  std::size_t from, std::uint8_t letter) noexcept
    -> std::size_t;

/// 当前编译单元启用的加速档位（"sse2" / "avx2" / "neon" / "scalar"）。
[[nodiscard]] auto backend() noexcept -> const char*;

// —— 字节视图的重解释入口（本层是 `CONVENTIONS.md` L6 的受控例外点）——
//
// 为什么放在这里而不是让调用方自己 `reinterpret_cast`："把字符/映射字节当成无符号
// 字节看"是 **位级重解释**，按 §10 的纪律只能在单点封装里发生（与平台层同一待遇）。
// 搜索主流程只需 `string_view`，与 SIMD 契约的转换全收在本层。

/// `string_view` → 字节视图（不拷贝，不要求对齐）。
[[nodiscard]] auto as_bytes(std::string_view text) noexcept -> std::span<const std::uint8_t>;

/// 字节视图 → 文本视图（不拷贝，不校验 UTF-8: 搜索按字节处理，非 UTF-8 文件照样能搜）。
[[nodiscard]] auto as_text(std::span<const std::uint8_t> bytes) noexcept -> std::string_view;

}  // namespace st::search::simd
