#pragma once

/// 通用 XML 解析（元素树 + 属性 + 文本）；**不抛异常、容错优先**。
///
/// ## 为什么单独成模块
///
/// 这个仓里已经有「自己扫尖括号」的实现了（`src/ui/svg.cpp` 里那份图标专用的）。
/// 两者**形态不同**，各有各的理由：
/// - svg 那份是**光标式 + 样式上下文栈**：它需要把每个标签的**原文**切出来
///  （`symbol` 表存的是片段原文、样式靠“父上下文 + 本标签属性”递推）——流式比建树更直接。
/// - 本模块是**树式**：界面描述需要的是“这棵树长什么样”（标签→组件、属性→样式、
///   子节点递归）——树是公共产物，而不是每个消费者各自再拼一遍。
///
/// 两者共享的只是「尖括号扫描」那几十行的**知识**（注释/CDATA/自闭合/引号里的 `>`/实体），
/// 而这份知识有真实的重复代价：本模块第一版就在「引号里的 `>` 不是标签结束」上
/// 踩了一次（见 `find_tag_end` 的注释），而 svg 那份当时已经处理过了。
/// 所以这份知识写在注释与测试里（`tests/xml_test.cpp` 把它当成契约钉住），
/// 比强行合并两种形态更划算。
///
/// ## 设计取向：容错而不是严格
///
/// 这是**资源装载**路径而不是协议校验路径：一个图标文件里有多余的空格、
/// 一个界面文件里有未闭合的标签，都不该让整个应用起不来。因此：
/// - 未闭合的标签**自动收口**（按遇到的父链回退），不报错；
/// - 认不出的构造（`<!DOCTYPE>`、`<?xml?>`、注释）**跳过**；
/// - 实体只认五个预定义（`&lt; &gt; &amp; &quot; &apos;`）+ 数字引用（`&#65;` / `&#x41;`），
///   其余原样保留（宁缺毋滥，不猜语义）。
///
/// ## 内存取向：`Document` 自持字符串
///
/// 解析期用 `string_view` 切原文（零拷贝），**产出时**把名字/属性/文本拷贝进
/// `Document`。这样调用方拿到的树不依赖源文本生命周期——`load_file()` 读完就能
/// 释放缓冲。代价是一次拷贝，收益是「不用记得别把源串丢了」，后者是更贵的坑。

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace st::xml {

/// 一个元素（含子元素与文本）。
struct Node {
  std::string name{};
  /// 属性（保持**文档顺序**：重复属性以第一个为准，见 `find`）。
  ///
  /// 为何要保序而不是用 `map`：属性顺序是作者意图的一部分（调试时对照原文、
  /// 序列化回去时稳定），而 `map` 会按字典序重排，把「这个属性在第一个」
  /// 这个信息丢掉。查找是线性的，但元素属性通常 <10 个，够用。
  std::vector<std::pair<std::string, std::string>> attributes{};
  std::vector<Node> children{};
  /// 元素的文字内容（已展开实体、已拼接相邻文本段、已去掉首尾空白）。
  std::string text{};
  /// 该元素在原文里的行号（1 起；诊断用——"第几行写错了"必须能报出来）。
  std::uint32_t line{1};

  /// 取属性（不存在返回 `nullopt`）。
  [[nodiscard]] auto attr(std::string_view key) const -> std::optional<std::string_view>;
  /// 取属性并转 `double`（解析失败返回 `nullopt`）。
  [[nodiscard]] auto attr_number(std::string_view key) const -> std::optional<double>;
  /// 取属性，缺省回落到 `fallback`。
  [[nodiscard]] auto attr_or(std::string_view key, std::string_view fallback) const
      -> std::string_view;
  /// 取布尔属性：`"true"/"1"/"yes"/""`（裸属性）为真，`"false"/"0"/"no"` 为假。
  [[nodiscard]] auto attr_bool(std::string_view key, bool fallback = false) const -> bool;
  /// 第一个名为 `child_name` 的子元素（没有则 `nullptr`）。
  [[nodiscard]] auto child(std::string_view child_name) const -> const Node*;
  /// 所有名为 `child_name` 的子元素。
  [[nodiscard]] auto children_named(std::string_view child_name) const -> std::vector<const Node*>;
  /// 是否没有子元素与文本（`<br/>` 这类空元素）。
  [[nodiscard]] auto empty() const noexcept -> bool {
    return children.empty() && text.empty();
  }
};

/// 解析错误（带行号；`message` 只用于诊断，不参与控制流）。
struct Error {
  std::string message{};
  std::uint32_t line{1};
};

/// 解析结果。
struct Document {
  Node root{};
  /// 根元素的**名字**（便利：`Document` 也可能只有一个根，root 就是它）。
  [[nodiscard]] auto root_name() const noexcept -> std::string_view { return root.name; }
};

/// 解析一段 XML 文本。
///
/// 语义约定（**刻意的宽松**）：
/// - 有多个顶层元素时，它们都成为 `root` 的子元素（`root.name` 为空）；
///   只有一个顶层元素时，`root` 就是它本身 —— 于是 `doc.root` 永远可直接用，
///   不必让每个调用方写一遍「取第一个孩子」。
/// - 文本节点拼进**最近的**元素的 `text`（子元素之间的文字都算父元素的文本）。
/// - 首尾空白去掉、段落内部空白折叠成单个空格（界面描述里的换行缩进不参与布局）。
///
/// `out_error` 非空时写入诊断（解析仍然返回结果——容错优先）。
[[nodiscard]] auto parse(std::string_view source, Error* out_error = nullptr)
    -> std::optional<Document>;

/// 便利：解析文件（读不到/解析不出返回 `nullopt`）。
[[nodiscard]] auto parse_file(const std::string& path, Error* out_error = nullptr)
    -> std::optional<Document>;

/// 便利：取属性（`nullptr` 安全，供投影层用）。
[[nodiscard]] auto attr_of(const Node* node, std::string_view key) -> std::optional<std::string_view>;

/// 转义为 XML 文本（`< > & " '`），供序列化/工具输出用。
[[nodiscard]] auto escape(std::string_view text) -> std::string;

}  // namespace st::xml
