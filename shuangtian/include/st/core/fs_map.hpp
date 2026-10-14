#pragma once

/// 只读内存映射的**平台实现**（Windows `CreateFileMapping` / POSIX `mmap`）。
///
/// ## 为什么单独一层（而不是把实现放进 `fs.cpp`）
///
/// 两个理由，都是本仓的既有约束：
/// 1. **L14**（`CONVENTIONS.md` §10 第 1 条）：`<windows.h>` / `<fcntl.h>` 这类系统头
///    只能出现在 `platform_*` 单点封装里。第一版把映射实现直接写在 `fs.cpp` 里，
///    `st lint` 当场报出两条 L14——而那条规则的用处正在这里：**它把「顺手在通用层里
///    塞平台代码」这个动作变成了必须显式跨过的一道门**。
/// 2. **头文件卫生**：`MappedFile`（`fs.hpp`）要持有平台句柄，而 `fs.hpp` 被几百个
///    单元包含——把 `HANDLE` 直接写进去意味着所有包含者都跟着吃 `<windows.h>`。
///    因此句柄在此层表达为 `void*`，平台头只出现在本层的 `.cpp`。
///
/// ## 契约
///
/// - **安全失败**：任一步失败都返回 `Error`（`NotFound` 打不开、`Invalid` 是目录、
///   `Io` 取大小/映射失败）——调用方自行回退，不抛异常。
/// - **空文件视为失败**：映射长度 0 在 POSIX 是 `EINVAL`；对调用方而言“没有内容”
///   与“读不到”是同一件事（都会去走回退路径并得到同样的错误）。
/// - **句柄必须保活**：返回的 `MappedView::handle` 在视图使用期间不得关闭——Windows 上
///   视图随句柄关闭而失效。POSIX 侧建立映射后 fd **立即关闭**（映射独立于 fd），
///   故 `handle` 为 `nullptr`。
/// - **只读**：`PROT_READ` / `FILE_MAP_READ`。写入会崩——这是刻意的：解析层拿到的
///   是 `span<const uint8_t>`，写不进去。

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "st/core/error.hpp"

namespace st::fs {

/// 映射视图：数据指针 + 长度 + 平台句柄（句柄语义见上）。
struct MappedView {
  const std::uint8_t* data{nullptr};
  std::size_t size{0};
  /// 平台句柄。Windows 需**两个**都保活（`CreateFileMapping` 的映射对象 + 文件句柄）；
  /// POSIX 两个都为 `nullptr`（那里映射独立于 fd，建立后即关 fd）。
  /// 类型用 `void*` 而非 `HANDLE`：本头文件被几百个单元包含，不能让 `<windows.h>` 进来。
  void* handle{nullptr};        ///< Windows: 映射对象句柄
  void* handle_extra{nullptr};  ///< Windows: 文件句柄；POSIX: nullptr

  [[nodiscard]] auto valid() const noexcept -> bool { return data != nullptr; }
};

/// 只读映射整个文件。
[[nodiscard]] auto open_file_mapping(std::string_view path) -> Result<MappedView>;

/// 解映射并释放句柄（幂等；失败路径上也不会走到这里来）。
void close_file_mapping(MappedView& view) noexcept;

}  // namespace st::fs

