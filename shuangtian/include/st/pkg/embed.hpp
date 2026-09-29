#pragma once

/// 编译期资源嵌入（`battery::embed` 的构建期生成，由 stpm 原生实现）。
///
/// 上游 `batterycenter/embed` 把「运行时」与「构建期生成」都放在一个 CMake 文件里；
/// 本框架的构建系统是自研 stpm（刻意不依赖 CMake），因此按职责切开：
/// 运行时原样 vendor（`vendor/battery/embed_impl.cpp` + 模板），生成期由本模块实现。
/// 语义与上游逐项对齐（标识符规则、查找键、编译期检查、`B_PRODUCTION_MODE`），
/// 对照表见 `vendor/battery/UPSTREAM.md`。
///
/// 产出（按目标隔离，落在 `build/<profile>/embed/<target>/`）：
/// - `include/battery/embed.hpp`：声明 + `if constexpr` 返回链（使用者 `#include` 的就是它）
/// - `src/<identifier>.cpp`：每个资源的字节数组定义
///
/// 增量：单个资源的 `.cpp` 仅在源文件更新时重生成；声明头仅在**集合或内容指纹**变化时重写。

#include <string>
#include <vector>

#include "st/core/error.hpp"
#include "st/pkg/manifest.hpp"

namespace st::pkg {

/// 一次嵌入生成的产物。
struct EmbedOutput {
  /// 生成的 `battery/embed.hpp` 所在包含目录（`-I` 用；空 = 该目标未嵌入任何资源）。
  std::string include_dir{};
  /// 需要参与编译的源文件（生成的字节数组 + vendor 的运行时实现）。
  std::vector<std::string> sources{};
  /// 嵌入的资源数量（诊断/日志用）。
  std::size_t file_count{0};
  /// 本次重新生成了几个文件（0 = 全部命中增量）。
  std::size_t regenerated{0};
};

/// 为 `target` 生成嵌入源码。
///
/// `profile` 决定 `B_PRODUCTION_MODE`（`release` 档定义之：不携带绝对路径、关闭热重载）。
/// 目标未声明 `embed`，或声明但 glob 未命中任何文件 → 返回空 `EmbedOutput`（不是错误：
/// "没有资源要嵌"是正常状态，不该让构建失败）。
/// 错误：`Invalid`（清单缺 directory / 标识符冲突）、`NotFound`（资源文件不存在）、`Io`。
/// `with_runtime` 控制是否把 vendor 的运行时实现（`embed_impl.cpp`，含进程级全局表）纳入编译。
///
/// **必须恰好有一个作用域提供它**：库级嵌入与目标级嵌入若都带上运行时，
/// 同一个可执行文件里会出现两份全局状态（实测表现为链接期 duplicate symbol / 运行期热重载表分裂）。
/// 调用约定：库级（`target` 为空）恒为 `true`；目标级仅在**库本身没有嵌入**时才为 `true`。
[[nodiscard]] auto generate_embeds(const Manifest& manifest, std::string_view target,
                                   std::string_view profile, bool with_runtime = true)
    -> Result<EmbedOutput>;

/// 把相对路径转成上游一致的标识符：`tolower(<target>_<path>)`，非 `[a-zA-Z0-9_]` → `_`。
/// 公开出来是为了让测试能直接锁住这条规则（兼容性契约）。
[[nodiscard]] auto embed_identifier(std::string_view target, std::string_view relative_path)
    -> std::string;

}  // namespace st::pkg
