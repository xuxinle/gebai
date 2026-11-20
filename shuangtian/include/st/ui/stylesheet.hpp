// 声明式样式的**唯一解析层**：CSS 式子集 → `Style`。
//
// ## 为什么要有它（这一轮实测的代价）
//
// 改造前，同一个效果有四种写法、四套解析、四份键表：
//
// | 入口 | 写法 | 解析位置 |
// |---|---|---|
// | DSL | `BoxProps{.backdrop_blur = 16, .highlight_width = 1.5}` | `src/ui/dsl.cpp` 逐字段抄 |
// | XML | `glass="blur:16,ring:1.5"` | `src/ui/actions.cpp` |
// | 控制通道 | `set glass=...` / `set backdrop_blur=16` | `src/ui/actions.cpp` 另一条 |
// | 主题 JSON | `"metrics": {"glass_tint": 0.32}` | `src/ui/theme_io.cpp` 第三份 |
//
// 由此产生的缺陷**全部同型**：一个属性两个落点、谁后写谁赢，而顺序藏在调用链里。
// 实测踩到五次：`fill` 被 `elevation` 盖、`backdrop_tint` 被 `apply_theme_glass` 盖、
// `top_highlight` 被 `apply_theme` 盖、`border_width` 被重置、`highlight_color`
// 读到了回放前的值。每次都表现为「`set` 报成功、回读也对、画面不动」。
//
// ## 设计：四组原语 + 一份键表
//
// ```
// layout: width 428, gap 10, grow 1
// paint:  fill #FFFFFF99, radius 14, border 0, shadow 0 2 8 #00000018
// glass:  blur 16, tint 0.32, refraction 6, highlight 0.85, ring 1, edges all
// text:   color #1A1A1A, size 14, weight semibold
// ```
//
// 四条硬约定：
//
// 1. **一份键表**：所有键在 `sheet_key_names()` 里登记一次，四套入口都查它。
//    拼错立刻返回错误（不是静默忽略——`border_none` 那种补丁就是静默的产物）。
// 2. **一个落点**：每个键只写 `Style` 的**一个**字段。需要“标记声明”的
//    （tint / 高光色）在这里同步打，不允许调用方各自记得。
// 3. **组内可省、组间正交**：`glass: blur 16` 只改模糊，不碰 tint。
// 4. **写法宽容**：逗号/分号/换行都算分隔，`:` 或空格都算赋值——
//    CSS 习惯与大模型习惯都能写对。
//
// 消费方：`apply_stylesheet(element, text)`。DSL 的 `BoxProps::style` 字段、
// XML 的 `style` 属性、控制通道的 `set style=...` 都走它。
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "st/ui/style.hpp"

namespace st::ui {

class Element;

/// 四组原语（用于分组报错与文档生成）。
enum class StyleGroup : std::uint8_t { Layout, Paint, Glass, Text };

/// 一个键的元信息。
///
/// 为何把「组」和「取值说明」也放进来：错误信息要能直接告诉调用方
/// “这个键属于哪一组、该怎么写”，而不是只回一句 “unknown property”。
struct SheetKey {
  std::string_view name;   ///< 键名（小写，`-` 与 `_` 等价）
  StyleGroup group;
  std::string_view syntax; ///< 取值写法（写进错误信息）
};

/// **全部合法键**（唯一权威）。四套入口、文档生成、控制通道白名单都读它。
[[nodiscard]] auto sheet_key_names() -> const std::vector<SheetKey>&;

/// 解析结果：成功的键数 + 未知键清单（空 = 全部认识）。
struct SheetReport {
  std::size_t applied{0};
  std::vector<std::string> unknown;
  [[nodiscard]] auto ok() const noexcept -> bool { return unknown.empty(); }
};

/// 把一段 CSS 式声明写进 `element.style()`。
///
/// 返回值：认识并应用的键数。未知键/未知值收集到 `report`（调用方决定
/// 是报错还是忽略——控制通道要报给用户，主题 JSON 要直接失败）。
auto apply_stylesheet(Element& element, std::string_view text, SheetReport* report = nullptr)
    -> std::size_t;

/// 同 `apply_stylesheet`，但**只解析不应用**（供校验器与测试用）。
[[nodiscard]] auto validate_stylesheet(std::string_view text) -> SheetReport;

}  // namespace st::ui
