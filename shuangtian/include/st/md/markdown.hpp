#pragma once

/// 原生 Markdown 解析（`DESIGN.md` §4.4）：CommonMark 子集 + GFM 扩展的块级/行内模型。
///
/// 设计要点
/// - **零第三方依赖**、不抛异常、不用 `st::Result` 报错：无法解释的输入按段落原样保留，
///   未闭合构造（围栏/强调/表格）当"已闭合到输入末尾"处理——大模型流式输出本来就是**残缺语法**，
///   解析器必须宽容（`DESIGN.md` §4.4）。
/// - **UTF-8 安全**：只在 ASCII 边界切分，多字节字符（中文/emoji）整段拷贝，绝不截成半个字符；
///   非法字节按单字节透传（不做替换字符改写，保持原文可回显）。
/// - **确定性**：`parse` 是纯函数（无隐藏状态、无全局变量），同一输入必得同一结构——
///   `MdStream` 的流式等价性（分片喂入 == 一次性解析）建立在这一点上。
///
/// 结构约定（UI 层依赖，`BlockKind` 无 `ListItem` 成员，列表项复用 `Paragraph` 承载）
/// | 块 | 字段用法 |
/// |---|---|
/// | `Paragraph` / `Heading` | `inlines` = 行内序列；`Heading.level` = 1..6 |
/// | `CodeBlock` | `language` = 围栏信息串首词（缩进代码块为空）；`code` = 原文（行尾带 `\n`） |
/// | `Quote` | `children` = 引用内的块序列（可嵌套） |
/// | `List` | `ordered` 有序；`children` = 列表项；`level` = 嵌套深度（顶层 0） |
/// | 列表项 | `kind == Paragraph`：`inlines` = 项首段行内；`children` = 该项其余块（含嵌套 `List`）；`task_item`/`checked` 为 GFM 任务标记；`level` = 所属列表深度 |
/// | `Table` | `header` + `rows`（单元格已扁平为纯文本，对齐行仅用于识别、不落库） |
/// | `Divider` | 无附加字段（`---` / `***` / `___`） |
/// | `HtmlBlock` | `code` = 原始 HTML 片段原文 |
/// - `start_line` 为源文中的**行号（0 起）**，供滚动定位/错误提示使用。
/// - 强调族（`Emphasis`/`Strong`/`Strikethrough`）的 `text` 是**内层文本已扁平**的结果
///   （行内模型是平铺序列，无嵌套子节点：`**a `b`**` → `Strong{"a b"}`）。

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace st::md {

/// 行内节点类别。
enum class InlineKind : std::uint8_t {
  Text,          ///< 普通文本
  Code,          ///< 行内代码（`text` 为代码内容，不含反引号）
  Emphasis,      ///< 强调（`*x*` / `_x_`）
  Strong,        ///< 加粗（`**x**` / `__x__`）
  Strikethrough, ///< 删除线（`~~x~~`）
  Link,          ///< 链接（`text` = 可见标签，`url`/`title` 为链接目标）
  Image,         ///< 图片（`text` = alt 文本，`url`/`title` 为图源）
  SoftBreak,     ///< 软换行（同一段内的换行）
  HardBreak,     ///< 硬换行（行尾两空格或 `\`）
};

/// 行内节点（值语义；`text`/`url`/`title` 均为 UTF-8 字节串）。
struct Inline {
  InlineKind kind{InlineKind::Text};
  std::string text{};
  std::string url{};
  std::string title{};
};

/// 块级节点类别。
enum class BlockKind : std::uint8_t {
  Paragraph, ///< 段落
  Heading,   ///< ATX 标题（`level` 1..6）
  CodeBlock, ///< 围栏/缩进代码块
  Quote,     ///< 引用（`children` 为内部块）
  List,      ///< 列表（`children` 为列表项）
  Table,     ///< GFM 表格
  Divider,   ///< 水平分隔线
  HtmlBlock, ///< HTML 块（原样保留，不渲染为 UI）
};

/// 块级节点：字段顺序即序列化布局约定（`CONVENTIONS.md` §6.5），变更需同步 `DESIGN.md`。
struct Block {
  BlockKind kind{BlockKind::Paragraph};
  std::uint32_t level{0};                        ///< Heading 层级 1..6；List 嵌套深度（顶层 0）
  std::vector<Inline> inlines{};                 ///< Paragraph/Heading/列表项的行内序列
  std::vector<Block> children{};                 ///< Quote 的子块；List 的列表项；列表项的其余块
  std::string language{};                        ///< CodeBlock 语言标注（信息串首词，小写保留原样）
  std::string code{};                            ///< CodeBlock 原文；HtmlBlock 原始片段
  bool ordered{false};                           ///< List 是否有序
  bool checked{false};                           ///< 任务项是否已勾选
  bool task_item{false};                         ///< 是否 GFM 任务列表项
  std::vector<std::string> header{};             ///< Table 表头单元格（行内已扁平为文本）
  std::vector<std::vector<std::string>> rows{};  ///< Table 数据行
  std::uint64_t start_line{0};                   ///< 起始行号（0 起）
};

/// 块类别短名（稳定，日志/控制通道/测试断言用）。
[[nodiscard]] constexpr auto to_string(BlockKind kind) noexcept -> std::string_view {
  switch (kind) {
    case BlockKind::Paragraph: return "paragraph";
    case BlockKind::Heading: return "heading";
    case BlockKind::CodeBlock: return "code_block";
    case BlockKind::Quote: return "quote";
    case BlockKind::List: return "list";
    case BlockKind::Table: return "table";
    case BlockKind::Divider: return "divider";
    case BlockKind::HtmlBlock: return "html_block";
  }
  return "unknown";
}

/// 行内类别短名（稳定，日志/控制通道/测试断言用）。
[[nodiscard]] constexpr auto to_string(InlineKind kind) noexcept -> std::string_view {
  switch (kind) {
    case InlineKind::Text: return "text";
    case InlineKind::Code: return "code";
    case InlineKind::Emphasis: return "em";
    case InlineKind::Strong: return "strong";
    case InlineKind::Strikethrough: return "del";
    case InlineKind::Link: return "link";
    case InlineKind::Image: return "image";
    case InlineKind::SoftBreak: return "soft_break";
    case InlineKind::HardBreak: return "hard_break";
  }
  return "unknown";
}

/// 解析 Markdown 为块序列。
/// - 无错误返回路径：语法残缺（未闭合围栏/强调/表格）不报错，按"已闭合到末尾"解释；
/// - 输入可为任意字节串（非法 UTF-8 透传），`start_line` 为 0 起行号；
/// - 纯度保证：同一输入必得同一输出（无全局状态），可重入、可并行。
[[nodiscard]] auto parse(std::string_view markdown) -> std::vector<Block>;

namespace detail {

/// 内部结果（非稳定 API，仅供 `MdStream` 实现增量提交使用）：一次解析的块序列 + 定型边界。
/// `sealed_blocks` / `sealed_bytes` 之前的块与字节已经**定型**——追加新内容不会改变它们，
/// 因此可以缓存起来，下次只重解析 `sealed_bytes` 之后的尾部（成本正比于未闭合尾部长度）。
struct ParseResult {
  std::vector<Block> blocks{};  ///< 本次解析出的全部块
  std::size_t sealed_blocks{0}; ///< 前若干块已定型（<= blocks.size()）
  std::size_t sealed_bytes{0};  ///< 已定型部分在输入中的字节长度（总落在行首，<= 输入长度）
};

/// 内部（非稳定 API）：解析 `markdown`，`base_line` 为 `markdown` 首行的绝对行号
/// （流式续解析时用于让 `start_line` 保持全文口径）。
[[nodiscard]] auto parse_incremental(std::string_view markdown, std::size_t base_line = 0) -> ParseResult;

}  // namespace detail

}  // namespace st::md
