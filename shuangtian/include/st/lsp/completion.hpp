#pragma once

/// 补全的**语义层**（LSP 阶段 4）：把 `textDocument/completion` 的响应解析成
/// UI 能直接吃的形态，并处理 `textEdit` 的插入计算。
///
/// 为什么不放进客户端（`st/lsp/client`）：那里只管"消息收发"，不解释业务语义；
/// 也不放进 UI 组件（`CompletionPopup`）：组件不该知道 LSP 的字段名。这一层是
/// **协议 → 视图**的翻译器，单测可以完全脱离进程与 UI（喂 JSON、断言结果）。
///
/// ## `textEdit` 的坑（实测过）
///
/// server 常同时给 `insertText` 与 `textEdit`（带 range）。**`textEdit` 优先**：
/// 它给出"要替换的区间 + 新文本"——适用于"输入 `wdt` 补成 `width`"这类场景
/// （range 覆盖已输入的 `wdt`）。只看 `insertText` 会得到 `wdtwidth`。
///
/// `insertTextFormat` 为 `Snippet` 时文本含 `$1`/`${1:name}` 占位符——本层做
/// **最小展开**（去掉占位符号、保留默认值），不做完整的 snippet 引擎：
/// 完整引擎要处理多光标与 tabstop 跳转，那是另一个独立特性（阶段 6 之后再说）。
///
/// ## 触发字符
///
/// `triggerCharacters` 由 server 在 `initialize` 里声明（如 clangd 给 `.`/`>`/`:`）。
/// 本层提供 `should_trigger` 判断，避免应用侧硬编码一套与 server 不一致的字符。

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "st/ext/json.hpp"
#include "st/lsp/protocol.hpp"

namespace st::lsp {

/// 一条补全候选（协议字段的可用子集）。
struct CompletionEntry {
  /// 显示文本（`label`）。
  std::string label{};
  /// 右侧细节（类型/签名等；`detail` 字段，可空）。
  std::string detail{};
  /// 实际插入文本（已处理 textEdit / snippet / insertText 的优先级）。
  std::string insert_text{};
  /// 过滤文本（`filterText`，空则用 label）。
  std::string filter_text{};
  /// 排序文本（`sortText`；server 按它排——我们**保持 server 的顺序**，
  /// 只有 server 没给才用 label。所以这里存起来供将来用，不参与当前排序）。
  std::string sort_text{};
  /// 类型徽标（由 `kind` 推导的单字符）。
  std::string badge{};
  /// 该候选的原始 JSON（`completionItem/resolve` 要用它的全部字段回传）。
  Json raw{};
  /// 文档/详情（`documentation` 字段；string 或 MarkupContent 两种形态）。
  std::string documentation{};
};

/// 解析 `textDocument/completion` 的响应。
///
/// 兼容两种返回形态：`CompletionItem[]`（数组）与 `CompletionList`（对象，
/// 含 `items`/`isIncomplete`）。后者更常见。
/// `replace_range`：当 server 没给 textEdit 时，应用侧要用"替换已输入前缀"的
/// 区间——由调用方传入（本层不猜，因为它不知道编辑器的光标与已输入词）。
[[nodiscard]] auto parse_completion(const Json& result) -> std::vector<CompletionEntry>;

/// 响应是否是"不完整列表"（`isIncomplete: true`）——意味着 server 希望我们在
/// 用户继续输入时**重新请求**（而不是本地过滤）。应用据此决定过滤策略。
[[nodiscard]] auto completion_is_incomplete(const Json& result) -> bool;

/// 从 `CompletionItem` 的 `kind` 推一个徽标字符（UI 显示用；未知返回空）。
[[nodiscard]] auto completion_badge(std::int64_t kind) -> std::string_view;

/// 展开 snippet 的**最小实现**：去掉 `$1`/`${1}`/`${1:default}` 的占位语法，
/// 保留默认值；`$0` 与 `${0:...}` 同处理（光标位置信息本层不产出——没有多光标支持）。
/// 普通文本原样返回。
[[nodiscard]] auto expand_snippet_minimal(std::string_view text) -> std::string;

/// 从 `completionItem/resolve` 响应里取详情文本（`documentation` + `detail`）。
[[nodiscard]] auto parse_completion_detail(const Json& item) -> std::string;

/// 该字符是否应触发补全（按 server 声明的 `triggerCharacters`）。
///
/// ⚠ 只认**单字符**触发（协议里 triggerCharacters 本来就是单字符）；
/// 多字符序列（如 `->`）需要比对该字符前的内容，由调用方处理（本层只做单字符判断）。
[[nodiscard]] auto should_trigger(const std::vector<std::string>& trigger_characters, char input)
    -> bool;

/// 取"光标前一个词"的范围（用于未给 textEdit 时的兜底替换区间）。
///
/// 词的构成：字母/数字/下划线（与 C 家族标识符一致）；`::`/`.`/`->` 之后的
/// 段单独算（`foo.bar` 在 `.` 后补全时，替换区间应只覆盖 `bar`）。
/// 返回 `{begin, end}` 字节偏移（`end` = 光标位置）。
[[nodiscard]] auto word_range_before_cursor(std::string_view text, std::size_t cursor)
    -> std::pair<std::size_t, std::size_t>;

}  // namespace st::lsp
