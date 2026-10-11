#pragma once

/// **XML → 界面树**：把一份 XML 描述变成真实的元素树。
///
/// ## 它解决什么
///
/// 在此之前，用霜天写界面只有两条路：C++ 声明式（`dsl::row/column/custom<T>`）
/// 或 JS 声明式（`declarative.js` 的 `row/column/Text`…）。前者要求会 C++，
/// 后者要求会 JS 且只能通过 JS 那套组件包装（44 个组件里覆盖了 15 个）。
/// 有了这一层，「界面长什么样」可以**完全写在 XML 里**：
///
/// ```xml
/// <Column gap="8" padding="12">
///   <Text id="title" size="18" weight="medium">设置</Text>
///   <Row gap="6"><Button id="ok" variant="primary">确定</Button></Row>
/// </Column>
/// ```
///
/// ## 与既有两条路的**关系**（这是它存在的理由，不是重复）
///
/// - **不是**替代声明式：它建的是**同一种元素树**，之后照样吃
///   `ui::apply_properties` / 选择器 / 控制通道 / 脚本——因为落点一致，三条路
///   可以混用（XML 树里插一棵 C++ 声明的子树，反之亦然）。
/// - **属性走的是同一条路**：标签属性先按 `BoxProps` 的字段名（`gap`/`padding`/
///   `grow`/`size`/`weight`/`surface`…）落到 `Style`，再交给 `ui::apply_properties`。
///   于是「XML 能写的属性」与「协议 `set` 能写的属性」**不会分裂**——这正是
///   `actions.cpp` 那份实现存在的意义，没必要再立第二套属性名。
///
/// ## 未识别项**如实报告**，不静默
///
/// 认不出的标签名、认不出的属性名都记进 `Report`（含行号）。理由与 `st::xml`
/// 的容错取向一致但方向相反：**XML 语法**上的小毛病（少个 `</Row>`）可以容错，
/// 但**语义**上的错（`<Buton>` 拼错、`colorr="red"` 打错）必须说出来——
/// 否则界面只是"少了一个按钮"，而没人知道为什么。

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "st/xml/xml.hpp"

namespace st::ui {

class Element;

/// 建树报告：认不出的名字与解析诊断（**都是给人看的线索**）。
struct XmlBuildReport {
  /// 认不出的标签名（去重后按首次出现顺序）。
  std::vector<std::string> unknown_tags{};
  /// 认不出的属性（`标签.属性` 形式，便于定位）。
  std::vector<std::string> unknown_attributes{};
  /// 解析期诊断（`st::xml::parse` 的容错消息；空 = 输入完全合规）。
  std::string parse_warning{};
  /// 成功建出的元素数（含容器）。
  std::size_t elements{0};

  /// 是否一切都在预期内（供"严格模式"判断，以及测试断言）。
  [[nodiscard]] auto clean() const noexcept -> bool {
    return unknown_tags.empty() && unknown_attributes.empty() && parse_warning.empty();
  }
};

/// 建树选项。
struct XmlBuildOptions {
  /// 认不出的标签是**跳过**（默认，容错）还是让整棵建树失败。
  ///
  /// 默认容错：一份界面描述里有个笔误，不该让整个应用起不来（会报进报告）。
  /// 想要"配置写错就早失败"（CI / 工具链）时打开它。
  bool strict{false};
  /// 给根元素取的名字（空 = 用 XML 根标签名当 id）。
  std::string root_id{};
};

/// 从 XML 文本建一棵元素树。
///
/// 返回 `nullptr`：文本解析不出 / 根标签认不出（且 `strict`）/ 元素工厂拒绝。
/// `report` 非空时写入诊断（**即使成功也写**——认不出的属性就是这么报警的）。
[[nodiscard]] auto build_from_xml(std::string_view source, XmlBuildReport* report = nullptr,
                                  const XmlBuildOptions& options = {})
    -> std::unique_ptr<Element>;

/// 从已解析的 XML 文档建树（要自己做预处理/复用文档时用）。
[[nodiscard]] auto build_from_xml_node(const st::xml::Node& node, XmlBuildReport* report = nullptr,
                                       const XmlBuildOptions& options = {})
    -> std::unique_ptr<Element>;

/// 从 XML 文件建树（读不到 / 解析不出返回 `nullptr`，诊断进报告）。
[[nodiscard]] auto build_from_xml_file(const std::string& path, XmlBuildReport* report = nullptr,
                                       const XmlBuildOptions& options = {})
    -> std::unique_ptr<Element>;

}  // namespace st::ui
