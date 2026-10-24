/// 文件时间戳的平台接口（平台无关声明；实现在 `platform_fs_time.cpp`）。
///
/// 存在的理由：`std::filesystem::last_write_time` 在 Windows/MinGW 上
/// **只到整秒**，而增量构建/清单校验都依赖 mtime 区分"谁更新"。
/// 详见实现文件的说明。
#pragma once

#include <cstdint>
#include <string>

#include "st/core/error.hpp"

namespace st::fs {

/// 文件最后写时刻，自 Unix 纪元起的**纳秒**。
///
/// 失败：`NotFound`（文件不存在/不可访问）。
[[nodiscard]] auto platform_modified_ns(const std::string& path) -> Result<std::int64_t>;

}  // namespace st::fs
