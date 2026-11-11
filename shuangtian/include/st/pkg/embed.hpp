#pragma once

/// 编译期资源嵌入（`battery::embed` 的构建期生成，由 stpm 原生实现）。
///
/// 上游 `batterycenter/embed` 把「运行时」与「构建期生成」都放在一个 CMake 文件里；
/// 本框架的构建系统是自研 stpm（刻意不依赖 CMake），因此按职责切开：
/// 运行时原样内联（`third_party/battery/embed_impl.cpp` + 模板），生成期由本模块实现。
/// 语义与上游逐项对齐（标识符规则、查找键、编译期检查、`B_PRODUCTION_MODE`），
/// 对照表见 `third_party/battery/UPSTREAM.md`。
///
/// ## 为什么是「多贡献方合并成一次生成」
///
/// 独立工程引用框架时**两方都有要嵌的资源**：框架要嵌脚本运行时前置
/// （`src/ui/script_api.js`），工程要嵌自己的图标/配置。若各自生成一份 `battery/embed.hpp`，
/// 同一个编译单元的命令行上就出现两个同名头——先命中的赢，另一方的资源直接
/// `static_assert` 失败（"No such file or directory"）。因此这里抽象出"贡献方"：
/// **一次生成、一个头、包含全部贡献**。
///
/// 标识符带来源前缀（工程名 / 框架名 / 目标名），多方合并天然不冲突；
/// 真撞上（同名文件同前缀）会明确报冲突而不是静默覆盖。
///
/// 产出（`<work>/build/<build_subdir>/embed/<scope>/`）：
/// - `include/battery/embed.hpp`：声明 + `if constexpr` 返回链（使用者 `#include` 的就是它）
/// - `src/<identifier>.cpp`：每个资源的字节数组定义
///
/// 增量：单个资源的 `.cpp` 仅在源文件更新时重生成；声明头仅在**渲染结果变化**时重写
/// （它被每个翻译单元包含，无谓重写会引发全量重编）。

#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"

namespace st::pkg {

/// 一个嵌入贡献方：一组 glob + 标识符前缀。
struct EmbedContribution {
  std::string base_directory{};         ///< 绝对：glob 相对根（框架根 / 工程根）
  std::vector<std::string> patterns{};  ///< 相对 `base_directory` 的 glob
  std::string identifier_prefix{};      ///< 标识符前缀（工程名 / 框架名 / 目标名）
  std::string origin{};                 ///< 诊断用来源标签（如 "框架 shuangtian"）
  /// **可选嵌入**：声明了但（部分）没命中时**不报错**，而是给未命中的路径
  /// 生成**零字节占位**条目。
  ///
  /// ## 为什么需要这个开关（2026-10-29）
  ///
  /// 默认行为是「声明了却没命中就报错」（见下方 `generate_embeds` 的 NotFound）。
  /// 那个默认对**必顶资源**是对的：路径写错时静默通过，会让运行期 `b::embed<...>()`
  /// 撞 `static_assert` 且难以定位。
  ///
  /// 但有一类资源本质上是**可选**的：它由本地工具（AI 生图管线）产出、体积大，
  /// 因体积原因**不入库**（例：格斗的 36 张立绘 ~5.4MB，`.gitignore` 有全局 `*.png`）。
  /// 这种资源在“有产线的那台机器”上存在、在"克隆下来的机器"上必然不存在——
  /// 对它按「路径写错」报错，等于把“没有素材”和“清单写错”混为一谈，
  /// 而且整个应用连**编译**都过不了（而它本可以降级为矢量绘制跑起来）。
  ///
  /// 开了这个标记后：
  /// - 未命中的路径 → 生成零字节占位，`b::embed<"…">()` 照常**编译通过**；
  /// - 运行时解码失败 → 调用方已有的降级路径生效（如 `CharacterPortrait::load`
  ///   返回 `valid()==false`，游戏退回矢量画风）；
  /// - 构建日志里**会点名**缺了哪些文件（不静默）。
  /// 注意：它**不是**“允许清单写错”——只对显式声明为 optional 的那组生效。
  bool optional{false};
};

/// 一次嵌入生成的请求。
struct EmbedRequest {
  std::string work_directory{};      ///< 生成物落点根（工程根）
  std::string template_directory{};  ///< 含 `embed.hpp.in` / `embed_source.cpp.in` 的目录
  std::string runtime_source{};      ///< `embed_impl.cpp` 绝对路径（开发期热重载运行时）
  std::string build_subdir{};        ///< 档位子目录名（`dev` / `dev-mingw`）
  std::string scope{};               ///< 作用域：`lib`（库级）或目标名
  std::vector<EmbedContribution> contributions{};
  bool production{false};   ///< 生产档：不携带绝对路径、关闭热重载
  bool with_runtime{true};  ///< 是否把运行时实现纳入编译（**恰好一处为 true**）
};

/// 一次嵌入生成的产物。
struct EmbedOutput {
  /// 生成的 `battery/embed.hpp` 所在目录（`-I` 用；无贡献时为空）。
  std::string include_dir{};
  /// 需要参与编译的源（生成的字节数组 + 运行时实现）。
  std::vector<std::string> sources{};
  /// 嵌入的资源数量（诊断/日志用）。
  std::size_t file_count{0};
  /// 其中“声明了但缺失、已用零字节占位”的数量（可选嵌入）。
  ///
  /// 单独计数而不是混进 `file_count`：两者对使用者的含义相反——
  /// 前者是“真正嵌进去了几张”，后者是“这台机器缺了几张”。
  std::size_t placeholder_count{0};
  /// 本次重新生成了几个文件（0 = 全部命中增量）。
  std::size_t regenerated{0};
};

/// 执行一次嵌入生成。
///
/// 无贡献方（`contributions` 全为空）→ 返回空 `EmbedOutput`（不是错误：
/// "没有资源要嵌"是正常状态，不该让构建失败）。
/// 错误：`NotFound`（模板/运行时缺失、glob 未命中）、`Invalid`（标识符冲突）、`Io`。
/// 例外：`EmbedContribution::optional` 为真的贡献方未命中时**不报错**，
/// 而是为每个未命中的字面路径生成零字节占位（详见该字段的注释）。
[[nodiscard]] auto generate_embeds(const EmbedRequest& request) -> Result<EmbedOutput>;

/// 把相对路径转成上游一致的标识符：`tolower(<prefix>_<path>)`，非 `[a-zA-Z0-9_]` → `_`。
/// 公开出来是为了让测试能直接锁住这条规则（与上游的兼容性契约）。
[[nodiscard]] auto embed_identifier(std::string_view prefix, std::string_view relative_path)
    -> std::string;

}  // namespace st::pkg
