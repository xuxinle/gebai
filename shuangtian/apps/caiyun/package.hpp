/// 工程包（`.cypkg`）：把裁云脚本升级成**可分层的可版本化工程**。
///
/// ## 为什么需要它
///
/// 单文件 `film.json` 能表达"画面长什么样"，但表达不了另外五件事，而它们
/// 恰恰是长期维护与 AI 协作真正需要的：
///
/// | 层 | 单文件里没有，于是 | 包里放哪 |
/// |---|---|---|
/// | 意图 | "为什么这样建"只留在对话里，下一个会话从零猜 | `intent.md` |
/// | 语义 | 对象的身份只有 `name` 一个字符串，没有类型/关系/材质槽 | `scene.json` |
/// | 程序 | `layout` JS 在独立文件里可以，但和场景的从属关系靠约定 | `program/` |
/// | 几何 | 素材靠相对路径，**改了没人知道**（没有内容哈希） | `assets/` |
/// | 渲染 | 与场景混在一份 JSON 里，分不清哪些是"片子"哪些是"渲染器配置" | `render.json` |
/// | 历史 | 完全没有——改坏了只能靠 git，而 git 看不到"视觉差异" | `history/` |
///
/// ## 关键兼容决定：包不是新格式，是**同一份 Script 的分层落盘**
///
/// 载入时包被**拍平**（`flatten`）成一份普通 `Script`，之后走与单文件**完全相同**
/// 的渲染路径——渲染侧零改动，存量 35 份单文件脚本一个都不用动。
///
/// 这条决定的代价是要维护"展开 ↔ 拍平"的一对逆运算，收益是：
/// **包坏了不影响出片能力**。想退回单文件随时可以（`flatten` 就是导出）。
///
/// ## 目录布局
///
/// ```
/// proj.cypkg/
///   package.json    清单：schema 版本、各层文件索引、资产哈希
///   intent.md       意图层（自然语言，人写 AI 读写）
///   scene.json      语义场景图（scenes/layers/层级/材质槽）
///   render.json     渲染层（相机/灯光/采样/AOV/色调映射）
///   program/        程序层（layout JS、修改器栈）
///   assets/         几何/贴图（外置，带 sha256）
///   history/        历史层（JSONL 追加日志 + 提案 + 快照）
/// ```
///
/// **为什么是目录而不是压缩包**：`.zip` 会让 `git diff` 与视觉 diff 同时失效，
/// 而这两件事正是"可版本化"的全部意义。
///
/// ## 拍平/展开的稳定性契约
///
/// `flatten(expand(s)) == s`（在同一 schema 版本下）是**硬要求**，
/// 由 `tests/caiyun_package_roundtrip_test.cpp` 守着。没有这条，
/// 每次存盘都会静默丢一点东西，而画面照常出——最难查的一类缺陷。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"
#include "st/core/json.hpp"

namespace caiyun {

// 框架的 `Result<T>` / `Status` 在 `st` 命名空间下（与 director.hpp 同口径）。
using st::Result;
using st::Status;

/// 包格式的 schema 版本。**改变落盘布局就必须递增**。
///
/// 递增不等于破坏兼容：`read_package` 按版本决定怎么读，
/// 旧包永远能读（否则"可版本化"就是假的）。
constexpr int kPackageSchemaVersion = 1;

/// 清单（`package.json` 的解析结果）。
struct PackageManifest {
  int schema_version{kPackageSchemaVersion};
  /// 工程名（与 `Script::name` 是两件事：改名不该改变工程身份）。
  std::string project_id{};
  /// 创建时间（ISO-8601，UTC）。只用于展示与排序，不参与任何判定。
  std::string created_at{};
  /// 各层文件相对包根的路径。空串 = 该层不存在（允许缺层：一个包可以只有剧本）。
  std::string scene_path{"scene.json"};
  std::string intent_path{};
  std::string render_path{};
  /// 程序层目录（含 layout JS 等）。空 = 无。
  std::string program_dir{};
  /// 资产目录。
  std::string assets_dir{"assets"};
  /// 历史目录。
  std::string history_dir{"history"};

  /// 资产清单：相对路径 → sha256（十六进制小写）。
  ///
  /// ## 为什么必须有它
  ///
  /// 素材是**外置**的，包本身管不住它。没有哈希时，"图被换了"与"图没进来"
  /// 在画面上都可能表现为"看起来不对"，而没有任何线索指向素材。
  /// 有哈希后：`verify_assets` 能直接说"哪个文件被改过/丢了"。
  std::vector<std::pair<std::string, std::string>> assets{};
};

/// 包在磁盘上的位置与形态。
struct PackageLocation {
  /// 包根目录（UTF-8）。不以 `.cypkg` 为后缀的目录也接受——
  /// 扩展名是给人与编辑器看的，不是判定依据（判定看 `package.json`）。
  std::string root{};
  /// 清单文件路径（`<root>/package.json`）。
  [[nodiscard]] auto manifest_path() const -> std::string;
};

/// 判定一个路径是不是工程包（有可读的 `package.json` 且 schema 版本可识别）。
[[nodiscard]] auto is_package(std::string_view path) -> bool;

/// 读清单。失败：路径不是包 / JSON 坏 / schema 版本不认识。
[[nodiscard]] auto read_manifest(std::string_view root) -> Result<PackageManifest>;

/// 把清单写回（创建目录 + 规范化键序，便于人工 diff）。
[[nodiscard]] auto write_manifest(std::string_view root, const PackageManifest& manifest)
    -> Status;

/// 历史层的一条记录（`history/*.jsonl` 的一行）。
///
/// ## 为什么是追加式 JSONL 而不是一个 JSON 数组
///
/// 数组的每次追加都要重写整个文件：崩溃时可能截断成半份（丢**全部**历史），
/// 而且 git diff 会显示"整段重写"。JSONL 只追加一行，崩溃最多丢最后一行。
struct HistoryEntry {
  /// 单调递增的序号（1 起）。用于排序与发现缺口。
  std::int64_t seq{0};
  /// 发生时间（ISO-8601 UTC）。
  std::string at{};
  /// 动作类型：`create` / `edit` / `render` / `propose` / `accept` / `revert`。
  std::string action{};
  /// 谁做的：`human` / `agent` / 具体名字。
  std::string actor{};
  /// 一句话说明（面向人）。
  std::string summary{};
  /// 结构化的补充（自由形状，按 action 约定）；无则 null。
  st::Json detail{};
};

/// 追加一条历史记录。返回写入后的序号（= 之前最大序号 + 1）。
///
/// 缺失 `history/` 目录时会创建。`seq`/`at` 为空时自动填。
[[nodiscard]] auto append_history(std::string_view root, HistoryEntry entry)
    -> Result<std::int64_t>;

/// 读回全部历史（按文件内顺序拼接，按 `seq` 升序）。
[[nodiscard]] auto read_history(std::string_view root) -> Result<std::vector<HistoryEntry>>;

/// 资产校验结果。
struct AssetCheck {
  /// 清单里登记、但文件不存在。
  std::vector<std::string> missing{};
  /// 文件存在但哈希与清单不符（被改过）。
  std::vector<std::string> modified{};
  /// 磁盘上有、但清单没登记（新增素材）。
  std::vector<std::string> untracked{};
  [[nodiscard]] auto ok() const noexcept -> bool {
    return missing.empty() && modified.empty();
  }
};

/// 校验资产（算 sha256 并与清单比对）。
[[nodiscard]] auto verify_assets(std::string_view root, const PackageManifest& manifest)
    -> AssetCheck;

/// 计算一组文件的 sha256，填入 `manifest.assets`（相对路径 → 哈希）。
///
/// `relative_paths` 由调用方给（本函数不猜哪些文件算资产）。
[[nodiscard]] auto collect_asset_hashes(std::string_view root,
                                        const std::vector<std::string>& relative_paths)
    -> std::vector<std::pair<std::string, std::string>>;

/// 一个文件的内容哈希（sha256，十六进制小写）。失败返回空串。
[[nodiscard]] auto file_digest(std::string_view path) -> std::string;

}  // namespace caiyun
