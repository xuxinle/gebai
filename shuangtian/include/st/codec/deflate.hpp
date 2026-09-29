#pragma once

/// deflate 压缩/解压（RFC 1951 原始流、RFC 1950 zlib、RFC 1952 gzip）——霜天自研，零第三方依赖。
///
/// 能力与限制（实现细节见 `src/codec/deflate.cpp` 顶部说明）：
/// - **inflate**：存储块（BTYPE=00）/ 固定 Huffman（01）/ 动态 Huffman（10）全覆盖；
///   坏数据（码长树过完备、码字无解、LZ77 回溯越界、输出超 `max_output`）一律返回 `Error`，
///   不越界读写、不抛异常、不崩溃。
/// - **deflate**：LZ77（3 字节哈希 + 哈希链，贪心最长匹配）配**固定 Huffman 块**（BTYPE=01）；
///   `level`（0..9）只影响哈希链长与块大小，不切换动态 Huffman/存储块（level=0 亦输出固定 Huffman）。
/// - **zlib/gzip 包装**：校验 Adler-32 / CRC-32 与 ISIZE；头非法、尾校验不符、gzip 多成员一律报错。
///
/// 本层只依赖 `st::core`（`st/core/error.hpp`、`st/core/hash.hpp`）。

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "st/core/error.hpp"

namespace st::codec {

/// 字节缓冲（codec 层统一容器）。
using Bytes = std::vector<std::uint8_t>;

/// 解压 RFC 1951 原始 deflate 流。
/// 输入为单条 deflate 流（首个块结束后的尾随字节被忽略）；`max_output` 为输出字节上限。
/// 失败：`Parse`（位流损坏/码字非法/回溯越界）、`Overflow`（输出超过 `max_output`）。
[[nodiscard]] auto inflate_raw(std::span<const std::uint8_t> input,
                               std::size_t max_output = 256u * 1024u * 1024u) -> Result<Bytes>;

/// 压缩为 RFC 1951 原始 deflate 流（固定 Huffman 块）。`level` 为 0..9，越界自动夹取。
[[nodiscard]] auto deflate_raw(std::span<const std::uint8_t> input, int level = 6)
    -> Result<Bytes>;

/// 解压 RFC 1950 zlib 流（2 字节头 + deflate + Adler-32 大端尾）。
/// 失败：`Parse`（头/尾校验失败）、`Unsupported`（预设字典 FDICT）、`Overflow`（超输出上限）。
[[nodiscard]] auto zlib_inflate(std::span<const std::uint8_t> input,
                                std::size_t max_output = 256u * 1024u * 1024u) -> Result<Bytes>;

/// 压缩为 RFC 1950 zlib 流（FLEVEL 按 `level` 标注，FCHECK 自动求值）。
[[nodiscard]] auto zlib_deflate(std::span<const std::uint8_t> input, int level = 6)
    -> Result<Bytes>;

/// 解压 RFC 1952 gzip 流（单成员：10 字节头 + 可选字段 + deflate + CRC-32/ISIZE 小端尾）。
/// 失败：`Parse`（魔数/CM/保留位/尾校验失败）、`Unsupported`（多成员尾随数据）。
[[nodiscard]] auto gzip_inflate(std::span<const std::uint8_t> input,
                                std::size_t max_output = 256u * 1024u * 1024u) -> Result<Bytes>;

/// 压缩为 RFC 1952 gzip 流（mtime 置 0 保证可复现，OS 字段固定为 Unix）。
[[nodiscard]] auto gzip_deflate(std::span<const std::uint8_t> input, int level = 6)
    -> Result<Bytes>;

}  // namespace st::codec
