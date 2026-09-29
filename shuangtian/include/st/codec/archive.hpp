#pragma once

/// 归档读取/写入（tar、tar.gz、zip）——霜天自研，零第三方依赖（复用本层 deflate 与 `st::hash`）。
///
/// 能力与限制（实现细节见 `src/codec/archive.cpp` 顶部说明）：
/// - **tar_read**：ustar（POSIX 1988）与 GNU tar 扩展（`L` 长名、`K` 长链接名、`x` pax `path=` 覆盖）；
///   逐块校验 header 校验和；要求末尾存在结束标记（两块全零），否则视为截断/非 tar 并报 `Parse`；
///   符号链接/硬链接/设备/FIFO 等条目**跳过**（不落盘，避免逃逸面）。
/// - **zip_read**：单盘、非 zip64；压缩方法 0(store) / 8(deflate)；解压长度与 CRC-32 均校验。
/// - **archive_read**：按魔数自动识别 zip / gzip+tar / tar。
/// - **archive_extract_to**：逐条目路径净化——拒绝绝对路径（`/`、`\`）与 `..` 逃逸（`ErrorCode::Invalid`），
///   不创建符号链接、不保留权限位。
/// - **tar_write**：ustar 打包（零时间戳、uid/gid 0，产物可复现），同样拒绝不安全路径。

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "st/codec/deflate.hpp"
#include "st/core/error.hpp"

namespace st::codec {

/// 一个归档条目：目录条目 `data` 为空，常规文件条目 `data` 长度等于 `size`。
struct ArchiveEntry {
  std::string path{};   ///< 归档内相对路径（'/' 分隔；目录条目通常以 '/' 结尾）
  bool is_dir{false};   ///< 是否为目录条目
  std::uint64_t size{0};  ///< 常规文件的原始字节数（目录为 0）
  Bytes data{};         ///< 文件内容（目录为空）
};

/// 解析 tar（含 ustar/GNU 扩展；不含 pax 全局头，`x` 头仅取 `path=`）。
/// 失败：`Parse`（块校验和/字段/数据长度异常、缺少结束标记）。
[[nodiscard]] auto tar_read(std::span<const std::uint8_t> input)
    -> Result<std::vector<ArchiveEntry>>;

/// 解析 zip（单盘、非 zip64）。
/// 失败：`Parse`（结构/CRC/长度异常）、`Unsupported`（zip64、分卷、未知压缩方法）。
[[nodiscard]] auto zip_read(std::span<const std::uint8_t> input)
    -> Result<std::vector<ArchiveEntry>>;

/// 按魔数自动识别并解析归档（zip / gzip+tar / tar）。
/// 失败：同上；无法识别时按 tar 解析并返回其错误。
[[nodiscard]] auto archive_read(std::span<const std::uint8_t> input)
    -> Result<std::vector<ArchiveEntry>>;

/// 解包到目录（自动创建父目录）。
/// 安全：任一条目路径为绝对路径或含 `..` 分量即整体失败（`ErrorCode::Invalid`，不落任何文件）。
/// 失败：`Invalid`（路径逃逸/空目标目录）、以及 `archive_read` 的全部错误、`Io`（写失败）。
[[nodiscard]] auto archive_extract_to(std::span<const std::uint8_t> input,
                                      std::string_view dest_dir) -> Status;

/// 打包为 ustar tar（目录条目按 `is_dir` 标记，路径不安全返回 `ErrorCode::Invalid`）。
/// 失败：`Invalid`（空路径/绝对路径/`..` 逃逸）、`Unsupported`（路径超 ustar 字段容量）、
/// `Overflow`（条目过大）。
[[nodiscard]] auto tar_write(const std::vector<ArchiveEntry>& entries) -> Result<Bytes>;

}  // namespace st::codec
