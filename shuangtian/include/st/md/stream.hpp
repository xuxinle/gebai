#pragma once

/// Markdown 流式增量解析（`DESIGN.md` §4.4）：面向大模型"边生成边渲染"的 `MdStream`。
///
/// 语义
/// - **等价性保证**：`feed` 返回的块序列在任何时刻都等价于 `parse(source())`——分片喂入与一次性
///   解析结果一致（`tests/md_stream_test.cpp` 用 1/3/7 字节切片断言结构等价）；
/// - **增量而不重复**：解析器把"已定型"的块（末尾行完整、最后一块已闭合、且不是可跨空行续接的
///   列表/引用）提交进缓存，后续 `feed` 只重解析未闭合的尾部——单次 `feed` 成本正比于**未闭合尾部
///   长度**而非全文长度，稳态下（每个 token 一片）近乎 O(1)；
/// - **未闭合可见**：未闭合围栏/段落/表格照常产出块（内容为已收到部分），`pending()` 为真，
///   UI 可据此显示"生成中"光标；
/// - **前缀稳定**：解析是纯函数 + 只追加，同一前缀重复喂入必得同一结果（无隐藏状态、可重入）。
///
/// 用法
/// ```cpp
/// st::md::MdStream stream;
/// for (auto token : llm_tokens) {
///   const std::vector<Block> blocks = stream.feed(token);   // 每片都拿全量最新块序列
///   view.render(blocks);
/// }
/// ```

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "st/md/markdown.hpp"

namespace st::md {

/// 流式增量解析器：分片喂入，返回当前完整块序列（最后一块可为未闭合）。
class MdStream {
 public:
  MdStream() = default;

  /// 清空累计原文与解析缓存（回到初始状态；之后的 `feed` 与新建实例等价）。
  void reset();

  /// 追加一片输入，返回**当前完整**块序列（未闭合构造按已收到部分呈现）。
  /// - `chunk` 可切在任意字节处（含 UTF-8 多字节中间）：解析器只在 ASCII 边界切分，
  ///   半个字符会暂时以原文保留，下一片补齐后自然还原；
  /// - 返回值为当前快照副本（调用方按需求渲染/缓存）。
  [[nodiscard]] auto feed(std::string_view chunk) -> std::vector<Block>;

  /// 最近一次 `feed` 的块序列（只读引用，无需重新解析）；未喂入过则为空。
  [[nodiscard]] auto snapshot() const -> const std::vector<Block>&;

  /// 是否存在未闭合块（未闭合围栏/未完段落/可能续接的列表…）：UI 据此显示"生成中"。
  [[nodiscard]] auto pending() const noexcept -> bool;

  /// 已累计的原文（与逐片拼接完全一致，可用于导出/复制）。
  [[nodiscard]] auto source() const -> std::string;

 private:
  std::string source_{};               ///< 累计原文（只追加，不重排）
  std::vector<Block> committed_{};     ///< 已定型块缓存（前缀，永不重解析）
  std::vector<Block> blocks_{};        ///< 当前完整块序列（committed_ + 尾部重解析结果）
  std::size_t tail_begin_{0};          ///< 未定型尾部的起始字节偏移（总落在行首）
  std::size_t tail_line_{0};           ///< 未定型尾部首行的绝对行号
  bool pending_{false};                ///< 是否有未闭合块
};

}  // namespace st::md
