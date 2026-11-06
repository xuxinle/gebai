/// 工程包的**拍平 / 展开**：包的语义与单文件脚本之间的一对逆运算。
///
/// ## 为什么要显式定义这对运算，而不是"包里多放几个文件"
///
/// 包的承诺是「同一份工程，既能分六层维护，也能一键退回单文件出片」。
/// 只要这个来回不精确，包就成了第二套真值源——两份数据迟早漂移，
/// 而漂移是静默的（画面照常出，只是某个字段在保存那一步消失了）。
///
/// 所以本层的设计原则只有一条：**`flatten` 的输出是唯一的真值形态**。
/// 分层的六份文件是"编辑视图"，`Script` 是"运行视图"，两者由下面两个函数连起来。
///
/// ## 各层与 `Script` 字段的对应（这就是契约）
///
/// | 层文件 | 承载的 Script 字段 |
/// |---|---|
/// | `scene.json` | `scenes`（及其下全部图层/承接/特效），外加 `name/fps/size/transition/fade_*/brand_*/show_brand` 这些"片子身份"字段 |
/// | `render.json` | 无独立字段——渲染层的相机/灯光目前内嵌在 `Scene` 与 `Layer::model_script` 里，见下方说明 |
/// | `program/` | `layout` 指向的 JS 文件（`Scene` 上的相对引用） |
/// | `assets/` | `image` / `image_card` / `model_shape` 里以 `assets/` 开头的引用 |
/// | `intent.md` | 无对应字段（纯人读；不参与渲染） |
/// | `history/` | 无对应字段（审计；不参与渲染） |
///
/// ### 关于 `render.json` 的诚实说明
///
/// 当前裁云把相机与灯光**内嵌**在场景/图层里（`cam_*`、`model_script.lighting`），
/// 没有全局的"渲染设置"概念。所以本版 `render.json` **不存在**（清单里留空），
/// 而不是伪造一份"看起来像 USD 的渲染层"。
///
/// 真要独立成层，得先回答"一个工程的相机/灯光是全局的还是每镜的"——
/// 这是设计决策，不该由文件格式倒逼。留待后续单独一轮。
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "director.hpp"
#include "intent.hpp"
#include "package.hpp"
#include "st/core/error.hpp"

namespace caiyun {

/// 展开的结果：一份可运行的 Script + 各层的原始文本（供工具读写）。
struct ExpandedPackage {
  Script script{};
  PackageManifest manifest{};
  /// 意图层原文（`intent.md`；没有则为空）。
  std::string intent_text{};
  /// 场景层原文（`scene.json`）。展开时已经解析进 `script`，这里保留原文是为了
  /// **不重排作者的格式**——工具只读不改时，文件应当逐字节不变。
  std::string scene_text{};
  /// 程序层文件：相对路径 → 内容。`layout` 引用据此解析。
  std::vector<std::pair<std::string, std::string>> program_files{};
  /// 意图约束与实现的不一致（空 = 全部符合或未声明意图）。
  ///
  /// 为何放在“展开结果”而不只打日志：调用方（GUI、`--pack`、AI 工具）
  /// 需要自己决定怎么展现——“静默警告在日志里”等于没警告。
  std::vector<IntentDeviation> intent_deviations{};
  /// 资产校验结果（missing / modified / untracked）。
  ///
  /// 单独列出而不是只返回 bool：`modified`（素材被换了）与 `missing`
  /// （素材丢了）的处置完全不同——前者要人工确认换得对不对，后者只能补文件。
  AssetCheck asset_check{};
};

/// 从磁盘读一个工程包并展开成 `Script`。
///
/// 失败：不是包 / 清单坏 / 场景层坏。**不**因缺意图层或历史层而失败
///（一个包可以只有场景——那正是从单文件迁过来的起点）。
[[nodiscard]] auto expand_package(std::string_view root) -> Result<ExpandedPackage>;

/// 把一份 `Script` 展开写成一个工程包（创建目录与各层文件）。
///
/// `source_basis`：脚本里那批**相对路径依赖**（`layout.js`、`assets/*`）
/// 当前所在的目录。包会把它们**实际拷进包内**，而不是只写一份清单——
/// 否则包离开原目录就打不开（表现是 `layout` 静默丢失、素材变成占位框）。
///
/// `overwrite_existing`：`false` 时若包已存在则失败（避免误删历史与意图）；
/// `true` 时**保留** `intent.md` 与 `history/`（那是人的劳动，不该被重新生成覆盖）。
[[nodiscard]] auto create_package(std::string_view root, const Script& script,
                                  std::string_view source_basis,
                                  std::string_view project_id = {},
                                  bool overwrite_existing = false) -> st::Status;

/// 把包拍平成单文件脚本文本（即"退回单文件"）。
///
/// 等价于 `script_to_json(expand_package(root).script)`，单独提供是因为
/// 它是**导出**动作，语义上不该要求调用方先懂 expand。
[[nodiscard]] auto flatten_to_json(std::string_view root) -> Result<std::string>;

}  // namespace caiyun
