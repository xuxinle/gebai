#pragma once

/// 文件系统与路径工具（`std::filesystem` 之上的薄封装，隔离平台差异）。
/// 约定：所有路径参数用 `std::string`（UTF-8），返回值统一为 `Result`/`std::optional`。

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"

namespace st::fs {

using Bytes = std::vector<std::uint8_t>;

struct DirEntry {
  std::string name{};
  std::string path{};  ///< 与入参同基准的路径
  bool is_dir{false};
  std::uint64_t size{0};
};

[[nodiscard]] auto read_text(std::string_view path) -> Result<std::string>;
[[nodiscard]] auto write_text(std::string_view path, std::string_view content) -> Status;
[[nodiscard]] auto append_text(std::string_view path, std::string_view content) -> Status;
[[nodiscard]] auto read_bytes(std::string_view path) -> Result<Bytes>;
[[nodiscard]] auto write_bytes(std::string_view path, std::span<const std::uint8_t> data) -> Status;

[[nodiscard]] auto exists(std::string_view path) noexcept -> bool;
[[nodiscard]] auto is_directory(std::string_view path) noexcept -> bool;
[[nodiscard]] auto is_regular_file(std::string_view path) noexcept -> bool;
[[nodiscard]] auto file_size(std::string_view path) -> Result<std::uint64_t>;
/// 最后修改时间（Unix 纪元纳秒；不存在返回错误）。
[[nodiscard]] auto modified_ns(std::string_view path) -> Result<std::int64_t>;

[[nodiscard]] auto list_dir(std::string_view path) -> Result<std::vector<DirEntry>>;
/// 递归遍历（`max_depth` 为 0 表示仅本层；结果按路径字典序）。
[[nodiscard]] auto walk(std::string_view root, std::uint32_t max_depth = 64) -> Result<std::vector<DirEntry>>;

[[nodiscard]] auto create_directories(std::string_view path) -> Status;
/// 写文件前确保父目录存在。
[[nodiscard]] auto ensure_parent(std::string_view path) -> Status;
[[nodiscard]] auto remove_all(std::string_view path) -> Status;
[[nodiscard]] auto remove_file(std::string_view path) -> Status;
/// 重命名/移动（同盘原子；目标已存在则覆盖）。
///
/// 构建链用它做"先写临时文件、成功再改名"：编译被中断（OOM 杀掉编译器、磁盘写满）时
/// 不会在产物位置留下**半截文件**。半截 `.o` 的阴险之处在它比源文件新——增量判新会认为
/// 它是最新的，于是下一次构建报出一堆莫名其妙的链接错误，而真实原因早就过去了。
[[nodiscard]] auto rename(std::string_view from, std::string_view to) -> Status;
[[nodiscard]] auto copy_file(std::string_view from, std::string_view to) -> Status;
/// 递归复制目录树（目标不存在则创建）。
[[nodiscard]] auto copy_tree(std::string_view from, std::string_view to) -> Status;

[[nodiscard]] auto current_dir() -> Result<std::string>;
[[nodiscard]] auto temp_dir() -> std::string;
[[nodiscard]] auto home_dir() -> std::string;
[[nodiscard]] auto read_env(std::string_view name) -> std::optional<std::string>;

[[nodiscard]] auto join(std::string_view base, std::string_view leaf) -> std::string;
[[nodiscard]] auto normalize(std::string_view path) -> std::string;
[[nodiscard]] auto absolute(std::string_view path) -> Result<std::string>;
[[nodiscard]] auto parent(std::string_view path) -> std::string;
[[nodiscard]] auto file_name(std::string_view path) -> std::string;
[[nodiscard]] auto stem(std::string_view path) -> std::string;
[[nodiscard]] auto extension(std::string_view path) -> std::string;
[[nodiscard]] auto is_absolute(std::string_view path) noexcept -> bool;
/// `from` 相对 `base` 的表示（不解析到父级之外）。
[[nodiscard]] auto relative_to(std::string_view path, std::string_view base) -> std::string;

/// glob 匹配：`*`（段内任意）、`?`（单字符）、`**`（跨目录任意层）；分隔符按 '/' 归一。
[[nodiscard]] auto match_glob(std::string_view pattern, std::string_view path) -> bool;
/// 在 `root` 下展开 glob 模式为实际文件列表（返回相对 root 的路径，字典序）。
[[nodiscard]] auto expand_glob(std::string_view root, std::string_view pattern) -> Result<std::vector<std::string>>;

/// 创建唯一临时目录（进程退出不自动清理，测试/工作区用）。
[[nodiscard]] auto make_temp_dir(std::string_view prefix) -> Result<std::string>;

}  // namespace st::fs
