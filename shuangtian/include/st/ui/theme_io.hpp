#pragma once

/// 主题的**外部表示**：JSON 读写。
///
/// ## 为什么主题需要一份可序列化形态
///
/// `Theme` 本身是运行期对象（含 `Metrics`/`SyntaxPalette` 等一堆字段），把它直接当配置格式
/// 有两个问题：① 字段改名就破坏所有既有主题文件；② 无法表达"只覆盖两项、其余继承基准"。
/// 因此自定义主题的格式是**稀疏覆盖**：
///
/// ```json
/// {
///   "name": "my-theme",
///   "base": "dark",                 // 基准：light / dark（缺省 = dark）
///   "colors": {"primary": "#7C5CFF", "bg": "rgba(8,8,10,0.95)"},
///   "metrics": {"radius_md": 14, "shadow_strength": 1.4},
///   "font_family": "Noto Sans CJK SC"
/// }
/// ```
///
/// 语义要点（每一条都对应一个"不说清就会踩"的地方）：
///
/// - **稀疏**：只写想改的项，其余从 `base` 继承。所以用户改一个 `primary` 不必抄 30 个 token。
/// - **未知键要报错，不是忽略**：拼错 `"primry"` 若被静默忽略，用户看到的是"我改了但没生效"——
///   这是最难查的一类问题。`parse_theme_json` 收集全部未知键并一并报出。
/// - **颜色取 `#RRGGBB` / `#RRGGBBAA` / `rgb()` / `rgba()`**：设计令牌一律 8 位十六进制
///   （`Color::from_rgba_hex` 的口径，见 `theme.hpp` 的说明）。`rgba()` 是为了让
///   从 CSS（如歌白的主题文件）搬过来的值能直接粘——半透明描边靠它表达。
/// - **`base` 是必需的概念**：`Theme` 的 `mode()` 决定了很多派生行为（文本 gamma、卡片描边强度），
///   一个自定义亮色主题不能被当成暗色。
///
/// ## 失败姿态
///
/// 全部返回 `Result`，**不抛异常**（`CONVENTIONS.md` §3.1）。文件不存在/JSON 语法错/未知键/
/// 颜色串非法，都靠返回值区分，调用方（`app::Application`）据此决定"退回内置主题并告警"。

#include <string>
#include <string_view>

#include "st/core/error.hpp"
#include "st/ext/json.hpp"
#include "st/ui/theme.hpp"

namespace st::ui {

/// 内置主题名（`light` / `dark`）；不认识的基准名报错而不是猜。
[[nodiscard]] auto theme_mode_from_name(std::string_view name) -> std::optional<ThemeMode>;
[[nodiscard]] auto theme_mode_name(ThemeMode mode) noexcept -> std::string_view;

/// 把一个主题序列化成 JSON（含全部 token；`dense=true` 时不带 `base` 语义，
/// 直接写全量 —— 用作"导出当前主题、改两笔再当自定义主题"的起点）。
[[nodiscard]] auto theme_to_json(const Theme& theme) -> Json;

/// 按 `base` 构造基准主题并施加 JSON 里的稀疏覆盖。
///
/// `json` 里 `base` 缺省时用 `fallback`（由调用方按当前运行模式给）。
[[nodiscard]] auto theme_from_json(const Json& json, ThemeMode fallback) -> Result<Theme>;

/// 把 JSON 里的稀疏覆盖**施加到已有主题**（不重建基准）。
///
/// 与 `theme_from_json` 的分工：后者是"从某个基准造一份新主题"（配置文件语义），
/// 本函数是"在**当前**主题上再改几笔"（控制通道 `theme.set` 语义）。
/// 控制通道必须是后者：用户分两次发 `{"colors":{"primary":...}}` 与
/// `{"metrics":{"radius_md":...}}` 时，前一笔不能被后一笔抹掉。
[[nodiscard]] auto apply_theme_overrides(Theme& theme, const Json& json) -> Status;

/// 把 `patch` 并合进 `base`，返回新对象（不改入参）。
///
/// 语义是**两层稀疏覆盖的并集**（逐层递归，标量以 `patch` 为准）。
/// 为什么需要：控制通道的 `theme.set` 是可以分多次发的，而每次只带自己想改的几项——
/// 若直接拿最后一次替换，之前改过的 token 就没了（用户看到"只生效了最后那次"）。
/// 校验也在这里做：`patch` 的形状不合法（非对象、嵌套层级不对）即失败。
[[nodiscard]] auto merge_overrides(const Json& base, const Json& patch) -> Result<Json>;

/// 解析 JSON 文本（同 `theme_from_json` 的语义）。
[[nodiscard]] auto parse_theme_json(std::string_view text, ThemeMode fallback) -> Result<Theme>;

/// 读主题文件（UTF-8 JSON）。文件不存在/不可读/语法错都返回失败并带原因。
[[nodiscard]] auto load_theme_file(std::string_view path, ThemeMode fallback) -> Result<Theme>;

/// 只读出主题文件的**键值本身**（不构造 `Theme`），供"在同一份配置上重新建主题"用。
///
/// 为什么需要它：切主题模式时要在新模式上叠同一份配置，而 `load_theme_file` 会把
/// `base` 一并消费掉、直接给出成品主题——拿不回配置本身。
[[nodiscard]] auto load_theme_json_file(std::string_view path) -> Result<Json>;

/// 写主题文件（落到磁盘，UTF-8、带缩进）。`path` 的父目录不存在时报错。
auto write_theme_file(std::string_view path, const Theme& theme) -> Status;

}  // namespace st::ui
