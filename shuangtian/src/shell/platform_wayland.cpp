// 平台边界：Wayland 窗口后端的**第一步骨架**（本轮先落地 X11，本文件先如实报
// `Unsupported`，随后在下一步补上协议绑定与实现）。
//
// 为什么要先建这个文件而不是把工厂里的 wayland 分支留着报错：
// `create_backend("wayland")` 的语义必须是**单一来源**——"这个平台有没有 wayland 后端"
// 这件事只该在 `platform_wayland.cpp` 里回答。留在 `backend.cpp` 里临时写一句
// `Unsupported` 会让"实现状态"分散在两处，而实现状态正是最容易过期的东西
//（历史上 `x11`/`wayland` 的桩就是这么留在 `backend.cpp` 里两年）。

#if !defined(_WIN32)

#include <memory>
#include <string>

#include "st/core/error.hpp"
#include "st/shell/shell.hpp"
#include "backend_util.hpp"

namespace st::shell {

[[nodiscard]] auto create_wayland_backend() -> Result<std::unique_ptr<Backend>> {
  // 运行时探测（不产生链接期依赖）：`libwayland-client` 在真实 Wayland 会话里必有。
  const std::string library = detail::library_available("libwayland-client.so.0");
  if (library.empty()) {
    return unexpected(ErrorCode::Unsupported, "未探测到 libwayland-client");
  }
  return unexpected(ErrorCode::Unsupported,
                    "wayland 后端实现尚未落地（已探测到 libwayland-client）；"
                    "请使用 --backend=x11 或 headless：控制通道可完成全部开发与验证");
}

}  // namespace st::shell

#endif  // !defined(_WIN32)
