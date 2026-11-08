#pragma once

/// @file dom.hpp
/// @brief HTML 子集解析器：容错建树 + 文档顺序遍历 + 常用实体解码。
///
/// ## 为什么不复用 `st::xml`
///
/// 两者都是「尖括号扫描」，但**语义差别足够大到不该合并**：
///
/// | 维度 | `st::xml`（界面描述） | 本模块（网页内容） |
/// |---|---|---|
/// | 空白 | 折叠（缩进不该进布局） | 折叠，但 `<pre>` 内保留 |
/// | 空元素 | 只认自闭合 `<x/>` | `br`/`img`/`hr`/`input`… 是**隐式**空元素（没有斜杠） |
/// | 未闭合标签 | 按父链回退收口 | 按 HTML 的**隐式闭合**规则（`<p>` 遇块级、`<li>` 遇 `<li>`…） |
/// | 内容模型 | 文本拼进最近的元素 | 文本与元素**按出现顺序**交错（行内混排全靠这个顺序） |
/// | 特殊内容 | 无 | `<script>`/`<style>` 内容原样保留（含 `<` 与 `&`） |
///
/// 最后两条是硬差别：`st::xml` 把文本拼成一个串（丢了「文本在哪两个子元素之间」），
/// 又把 `<` 当标签开始——`<style>a > b {}</style>` 这种内联 CSS 会被它误解。
///
/// ## 容错取向（与 `st::xml` 一致：资源装载路径，不抛异常）
///
/// - 认不出的标签**照建节点**（样式表里没有它就按继承样式渲染，不静默丢内容）；
/// - 未闭合标签在输入末尾自动收口；
/// - 认不出的实体原样保留（不猜语义）。
///
/// ## 不做的事（如实说明）
///
/// 不做 HTML 的完整解析：没有 DTD、没有命名空间、不解析 `<template>` 内容、
/// 不执行 `<script>`、不取网络资源（`<img src="http…">` 不解码，交宿主）。

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace st::html {

/// 节点类别。
enum class NodeKind : std::uint8_t {
  Element,  ///< 元素（`tag` 非空）
  Text,     ///< 文本（`text` 非空；已解实体、已按上下文决定是否折叠空白）
};

/// 一个节点。子节点存在父节点的 `children` 里（**文档顺序**，文本与元素交错）。
struct Node {
  NodeKind kind{NodeKind::Element};
  /// 标签名（**小写**；文本节点为空）。
  std::string tag{};
  /// 文本内容（仅 `NodeKind::Text` 有；已解实体）。
  std::string text{};
  /// 属性（保持文档顺序；同名以第一个为准）。
  std::vector<std::pair<std::string, std::string>> attributes{};
  /// 子节点（**只有元素有**；文本是叶子）。
  std::vector<Node> children{};
  /// 该节点在原文里的行号（1 起；诊断用）。
  std::uint32_t line{1};

  [[nodiscard]] auto attr(std::string_view key) const -> std::optional<std::string_view>;
  /// 取属性并转 `float`（解析失败返回 `nullopt`）。
  [[nodiscard]] auto attr_number(std::string_view key) const -> std::optional<float>;
  /// 第一个名为 `child_tag` 的子元素（没有则 `nullptr`）。
  [[nodiscard]] auto child(std::string_view child_tag) const -> const Node*;
  /// `class` 属性里是否含某个类名（**按空白切分后整体比较**，不做子串匹配：
  /// 子串匹配会让 `btn` 命中 `btn-primary`，那是选择器最经典的误命中）。
  [[nodiscard]] auto has_class(std::string_view name) const -> bool;
  [[nodiscard]] auto empty() const noexcept -> bool {
    return text.empty() && children.empty();
  }
};

/// 解析结果。
struct Document {
  /// 文档根：`<html>` 存在时就是它；否则是一个**合成的**根（`tag == "#document"`），
  /// 子节点是全部顶层节点。调用方永远不必写「取第一个孩子」。
  Node root{};
  /// `<style>` 元素的 CSS 原文（按文档顺序拼接；**已去掉 CDATA 包裹**）。
  ///
  /// 单独抽出来是因为它不属于渲染树：样式表要在排版前整体解析一次，
  /// 让每个元素各自去找 `<style>` 会变成 O(n²)。
  std::string style_text{};
  /// 标题（`<title>` 的文本；没有为空）。
  std::string title{};
};

/// 解析错误（带行号；容错优先——出错的构造被跳过/按已闭合处理）。
struct Error {
  std::string message{};
  std::uint32_t line{1};
};

/// 解析 HTML 文本（永不失败：畸形输入也能得到一棵尽量合理的树）。
[[nodiscard]] auto parse(std::string_view source, Error* out_error = nullptr) -> Document;

/// 该标签是否为空元素（`br`/`img`/`hr`/`input`/`meta`/`link`… —— 不需要闭合）。
[[nodiscard]] auto is_void_element(std::string_view tag) noexcept -> bool;

/// 修整一段 CSS（去掉 `<!-- -->` 与 `/* */` 注释；`<style>` 里的 CDATA 包裹也去掉）。
[[nodiscard]] auto clean_style_text(std::string_view source) -> std::string;

}  // namespace st::html
