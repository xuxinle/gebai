/// `MermaidView` 与 `st::md::mermaid` 的回归。
///
/// ## 三类判据（对应三类会真的坏掉的东西）
///
/// | 判据 | 坏掉时的现象 |
/// |---|---|
/// | 解析：节点/边/参与者/扇形数、标签、形状、方向 | 图少了几块，且**不报错**（最难查） |
/// | 布局：确定性、同级不重叠、层序单调、内容尺寸非零 | "图看着怪"：节点压在一起、边穿框 |
/// | 视图：属性面自洽、fit/zoom 分离、滚轮/拖拽、像素冒烟 | 用户按了没反应，或缩放回到原点 |
///
/// ## 为什么布局断言都写成"不变量"而不是"期望坐标"
///
/// 坐标会随标签字号、字体栈、内边距一起变，写死数值的测试改一次参数就要全线重写，
/// 而这种"每次都要改的测试"最后一定会被改成恒绿。所以断言的是**关系**：
/// 同一层不重叠、层的顺序与 rank 一致、内容包住所有节点。
#include "st/test/test.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <string>
#include <vector>

#include "st/math/geometry.hpp"
#include "st/md/mermaid.hpp"
#include "st/raster/canvas.hpp"
#include "st/ui/components/all.hpp"
#include "st/ui/theme.hpp"

#include "tests/support/text_port_fixtures.hpp"

namespace {

using st::md::mermaid::DiagramKind;
using st::md::mermaid::Direction;
using st::md::mermaid::EdgeStyle;
using st::md::mermaid::LayoutOptions;
using st::md::mermaid::NodeShape;
using st::ui::MermaidView;

/// 定宽度量：每个码点 8px。几何断言只关心"关系"，用桩可让数字可复算。
auto stub_measure(float per_char = 8.0f) {
  return [per_char](std::string_view text, float /*font_size*/) {
    return per_char * static_cast<float>(text.size());
  };
}

auto overlaps(st::math::Rect a, st::math::Rect b) -> bool {
  return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
}

struct Fixture {
  st::ui::Theme theme = st::ui::Theme::light();
  st::test::FixedAdvanceTextPort port{};
  st::ui::RenderContext context{theme, &port, 0.0};

  void layout(st::ui::Element& element, float width, float height) {
    element.apply_theme(theme);
    st::ui::Constraints constraints{};
    constraints.max_width = width;
    constraints.max_height = height;
    constraints.available_width = width;
    constraints.available_height = height;
    element.measure(context, constraints);
    element.arrange(context, st::math::Rect{0.0f, 0.0f, width, height});
  }

  void paint(const st::ui::Element& element, st::raster::Canvas& canvas) {
    const_cast<st::ui::Element&>(element).paint(context, canvas);
  }
};

}  // namespace

// ————————————————————————————————————————————————————————————————————————————
// 解析
// ————————————————————————————————————————————————————————————————————————————

/// 流程图：节点/边/标签/形状/流向都要对。链式写法（`A --> B --> C`）是真实文档里
/// 最常见的形态，漏支持会让用户以为组件"只认一行一条边"。
ST_TEST(mermaid_parse_flowchart_basic) {
  const auto document = st::md::mermaid::parse(
      "flowchart LR\n"
      "  A[开始] --> B{判断}\n"
      "  B -->|是| C(处理)\n"
      "  B -->|否| D((结束))\n"
      "  C --> D\n");

  ST_CHECK(document.ok);
  ST_CHECK(document.kind == DiagramKind::Flowchart);
  ST_CHECK(document.flowchart.direction == Direction::LeftRight);
  ST_CHECK_EQ(document.flowchart.nodes.size(), 4U);
  ST_CHECK_EQ(document.flowchart.edges.size(), 4U);

  ST_CHECK_EQ(document.flowchart.nodes[0].label, std::string("开始"));
  ST_CHECK(document.flowchart.nodes[0].shape == NodeShape::Rect);
  ST_CHECK(document.flowchart.nodes[1].shape == NodeShape::Diamond);
  ST_CHECK(document.flowchart.nodes[2].shape == NodeShape::Rounded);
  ST_CHECK(document.flowchart.nodes[3].shape == NodeShape::Circle);
  ST_CHECK_EQ(document.flowchart.edges[1].label, std::string("是"));
  ST_CHECK(document.diagnostics.empty());
}

/// 边线型：实线/虚线/粗线 + 无箭头。线型认错是"看着还行但语义丢了"的典型。
ST_TEST(mermaid_parse_flowchart_edge_styles) {
  const auto document = st::md::mermaid::parse(
      "graph TD\n"
      "  A --> B\n"
      "  B -.-> C\n"
      "  C ==> D\n"
      "  D --- E\n");

  ST_REQUIRE(document.flowchart.edges.size() == 4U);
  ST_CHECK(document.flowchart.edges[0].style == EdgeStyle::Solid);
  ST_CHECK(document.flowchart.edges[0].arrow);
  ST_CHECK(document.flowchart.edges[1].style == EdgeStyle::Dashed);
  ST_CHECK(document.flowchart.edges[1].arrow);
  ST_CHECK(document.flowchart.edges[2].style == EdgeStyle::Thick);
  ST_CHECK(document.flowchart.edges[2].arrow);
  // `---` 是无箭头实线（不是"画漏了箭头"）。
  ST_CHECK(document.flowchart.edges[3].style == EdgeStyle::Solid);
  ST_CHECK(!document.flowchart.edges[3].arrow);
  ST_CHECK(document.flowchart.direction == Direction::TopDown);
}

/// 时序图：参与者声明 + 消息（实线/虚线）+ 自发消息。
ST_TEST(mermaid_parse_sequence) {
  const auto document = st::md::mermaid::parse(
      "sequenceDiagram\n"
      "  participant A as 客户端\n"
      "  participant B as 服务端\n"
      "  A->>B: 请求\n"
      "  B-->>A: 响应\n"
      "  A->>A: 重试\n");

  ST_CHECK(document.ok);
  ST_CHECK(document.kind == DiagramKind::Sequence);
  ST_REQUIRE(document.sequence.participants.size() == 2U);
  ST_CHECK_EQ(document.sequence.participants[0].label, std::string("客户端"));
  ST_REQUIRE(document.sequence.messages.size() == 3U);
  ST_CHECK_EQ(document.sequence.messages[0].text, std::string("请求"));
  ST_CHECK(!document.sequence.messages[0].dashed);
  ST_CHECK(document.sequence.messages[1].dashed);
  ST_CHECK_EQ(document.sequence.messages[2].from, document.sequence.messages[2].to);
}

/// 未声明参与者也要自动补（mermaid 允许直接 `A->>B: 文本`）。
ST_TEST(mermaid_parse_sequence_implicit_participants) {
  const auto document = st::md::mermaid::parse(
      "sequenceDiagram\n  Alice->>Bob: Hi\n");
  ST_REQUIRE(document.sequence.participants.size() == 2U);
  ST_CHECK_EQ(document.sequence.participants[0].id, std::string("Alice"));
  ST_CHECK_EQ(document.sequence.participants[1].id, std::string("Bob"));
}

/// 饼图：标题（可写在首行）+ 标签 + 数值。
ST_TEST(mermaid_parse_pie) {
  const auto document = st::md::mermaid::parse(
      "pie title 流量来源\n"
      "  \"搜索\" : 45\n"
      "  \"直连\" : 30\n"
      "  \"外链\" : 25\n");

  ST_CHECK(document.ok);
  ST_CHECK(document.kind == DiagramKind::Pie);
  ST_CHECK_EQ(document.pie.title, std::string("流量来源"));
  ST_REQUIRE(document.pie.slices.size() == 3U);
  ST_CHECK_EQ(document.pie.slices[0].label, std::string("搜索"));
  ST_CHECK(std::abs(document.pie.slices[0].value - 45.0) < 1e-9);
}

/// 不支持的图类型 / 语句要**如实上报**，不能静默产出空图。
ST_TEST(mermaid_parse_reports_unsupported) {
  const auto class_diagram = st::md::mermaid::parse("classDiagram\n  A <|-- B\n");
  ST_CHECK(!class_diagram.ok);
  ST_CHECK(class_diagram.kind == DiagramKind::Unknown);
  ST_CHECK(!class_diagram.diagnostics.empty());

  const auto with_subgraph = st::md::mermaid::parse(
      "flowchart TD\n"
      "  subgraph 一\n"
      "    A --> B\n"
      "  end\n");
  ST_CHECK(with_subgraph.ok);            // 能画的照画
  ST_CHECK(!with_subgraph.diagnostics.empty());  // 缺的部分有线索
  ST_CHECK_EQ(with_subgraph.flowchart.nodes.size(), 2U);
}

/// 空文本不是"崩溃"也不是"静默空白"：要有一条诊断。
ST_TEST(mermaid_parse_empty_reports_diagnostic) {
  const auto document = st::md::mermaid::parse("   \n\n");
  ST_CHECK(!document.ok);
  ST_CHECK(!document.diagnostics.empty());
}

// ————————————————————————————————————————————————————————————————————————————
// 布局
// ————————————————————————————————————————————————————————————————————————————

/// 确定性：同输入两次布局必须逐字段相同（截图回归与缺陷复现的前提）。
ST_TEST(mermaid_layout_is_deterministic) {
  const auto document = st::md::mermaid::parse(
      "flowchart TD\n  A[甲] --> B[乙]\n  A --> C[丙]\n  B --> D[丁]\n  C --> D\n");
  const auto measure = stub_measure();
  const auto first = st::md::mermaid::layout_flowchart(document.flowchart, LayoutOptions{}, measure);
  const auto second = st::md::mermaid::layout_flowchart(document.flowchart, LayoutOptions{}, measure);
  ST_REQUIRE(first.nodes.size() == second.nodes.size());
  for (std::size_t index = 0; index < first.nodes.size(); ++index) {
    ST_CHECK(first.nodes[index].rect.x == second.nodes[index].rect.x);
    ST_CHECK(first.nodes[index].rect.y == second.nodes[index].rect.y);
    ST_CHECK(first.nodes[index].rect.width == second.nodes[index].rect.width);
  }
}

/// 同层节点不重叠，且所有框都落在内容尺寸内（否则会被裁掉一角）。
ST_TEST(mermaid_layout_nodes_do_not_overlap) {
  const auto document = st::md::mermaid::parse(
      "flowchart TD\n  A[开始] --> B[中间一]\n  A --> C[中间二]\n  B --> D[结束]\n  C --> D\n");
  const auto measure = stub_measure();
  const auto layout = st::md::mermaid::layout_flowchart(document.flowchart, LayoutOptions{}, measure);
  ST_REQUIRE(layout.nodes.size() == 4U);
  ST_CHECK(layout.size.width > 0.0f);
  ST_CHECK(layout.size.height > 0.0f);
  for (std::size_t i = 0; i < layout.nodes.size(); ++i) {
    for (std::size_t j = i + 1U; j < layout.nodes.size(); ++j) {
      ST_CHECK(!overlaps(layout.nodes[i].rect, layout.nodes[j].rect));
    }
    const auto& rect = layout.nodes[i].rect;
    ST_CHECK(rect.x >= 0.0f);
    ST_CHECK(rect.y >= 0.0f);
    ST_CHECK(rect.right() <= layout.size.width + 0.01f);
    ST_CHECK(rect.bottom() <= layout.size.height + 0.01f);
  }
}

/// 层序单调：`A --> B` 时 B 在 A 的**下游**（TD 就是 y 更大）。这条若挂了，
/// 布局算法一定退化了（例如把边长当成了无向边）。
ST_TEST(mermaid_layout_ranks_follow_direction) {
  const auto document = st::md::mermaid::parse(
      "flowchart TD\n  A --> B\n  B --> C\n");
  const auto layout =
      st::md::mermaid::layout_flowchart(document.flowchart, LayoutOptions{}, stub_measure());
  ST_REQUIRE(layout.nodes.size() == 3U);
  ST_CHECK(layout.nodes[1].rect.y > layout.nodes[0].rect.y);
  ST_CHECK(layout.nodes[2].rect.y > layout.nodes[1].rect.y);

  // 换个流向，同一条链必须相应换轴（LR 时看 x）。
  const auto horizontal = st::md::mermaid::parse("flowchart LR\n  A --> B\n  B --> C\n");
  const auto lr_layout =
      st::md::mermaid::layout_flowchart(horizontal.flowchart, LayoutOptions{}, stub_measure());
  ST_CHECK(lr_layout.nodes[1].rect.x > lr_layout.nodes[0].rect.x);
  ST_CHECK(lr_layout.nodes[2].rect.x > lr_layout.nodes[1].rect.x);
}

/// 带环的图必须**终止且可用**（分层布局在有环图上无解，去回边是必须的一步）。
ST_TEST(mermaid_layout_handles_cycles) {
  const auto document = st::md::mermaid::parse(
      "flowchart TD\n  A --> B\n  B --> C\n  C --> A\n");
  const auto layout =
      st::md::mermaid::layout_flowchart(document.flowchart, LayoutOptions{}, stub_measure());
  ST_REQUIRE(layout.nodes.size() == 3U);
  ST_CHECK_EQ(layout.edges.size(), 3U);
  ST_CHECK(layout.size.height > 0.0f);
}

/// 自环（`A --> A`）不能塌成一个点：它的路径要有独立的回环段。
ST_TEST(mermaid_layout_self_loop_has_geometry) {
  const auto document = st::md::mermaid::parse("flowchart TD\n  A[自己] --> A\n");
  const auto layout =
      st::md::mermaid::layout_flowchart(document.flowchart, LayoutOptions{}, stub_measure());
  ST_REQUIRE(layout.edges.size() == 1U);
  ST_CHECK(layout.edges[0].self);
  ST_CHECK(layout.edges[0].points.size() >= 3U);
  const auto& points = layout.edges[0].points;
  float min_x = points[0].x;
  float max_x = points[0].x;
  for (const auto& point : points) {
    min_x = std::min(min_x, point.x);
    max_x = std::max(max_x, point.x);
  }
  ST_CHECK(max_x - min_x > 8.0f);
}

/// 标签必须真的装进形状：文字区（`text_rect`）不超出节点框，且每行宽度不超出文字区。
///
/// 这条钉的是“盒子尺寸与文字区各算一套”那个缺陷：菱形里的字会横向溢出到尖角外，
/// 而所有属性面都是对的（只有看图才能发现）。
ST_TEST(mermaid_layout_labels_fit_shapes) {
  const auto document = st::md::mermaid::parse(
      "flowchart TD\n  A{这个判定条件相当长}\n  B((圆里也有很长的说明))\n  C([体育场形状])\n");
  const float per_char = 8.0f;
  const auto layout =
      st::md::mermaid::layout_flowchart(document.flowchart, LayoutOptions{}, stub_measure(per_char));
  ST_REQUIRE(layout.nodes.size() == 3U);
  for (const auto& node : layout.nodes) {
    ST_CHECK(node.text_rect.x >= node.rect.x - 0.01f);
    ST_CHECK(node.text_rect.y >= node.rect.y - 0.01f);
    ST_CHECK(node.text_rect.right() <= node.rect.right() + 0.01f);
    ST_CHECK(node.text_rect.bottom() <= node.rect.bottom() + 0.01f);
    for (const std::string& line : node.lines) {
      ST_CHECK(per_char * static_cast<float>(line.size()) <=
                node.text_rect.width + 0.01f);
    }
  }
}

/// 时序图布局：消息按序自上而下，生命线在参与者头中心，内容尺寸非零。
ST_TEST(mermaid_layout_sequence_rows) {
  const auto document = st::md::mermaid::parse(
      "sequenceDiagram\n  A->>B: 一\n  B->>A: 二\n  A->>B: 三\n");
  const auto layout =
      st::md::mermaid::layout_sequence(document.sequence, LayoutOptions{}, stub_measure());
  ST_REQUIRE(layout.messages.size() == 3U);
  ST_CHECK(layout.messages[0].y < layout.messages[1].y);
  ST_CHECK(layout.messages[1].y < layout.messages[2].y);
  ST_REQUIRE(layout.lifelines.size() == 2U);
  ST_CHECK(layout.lifelines[0] < layout.lifelines[1]);
  ST_CHECK(layout.lifeline_bottom > layout.lifeline_top);
  ST_CHECK(layout.size.width > 0.0f);
  ST_CHECK(layout.size.height > 0.0f);
}

/// 饼图：百分比合计 100、角度从 12 点开始顺时针累加；零值不产生扇形但保留图例。
ST_TEST(mermaid_layout_pie_angles) {
  auto document = st::md::mermaid::parse(
      "pie\n  \"甲\" : 50\n  \"乙\" : 30\n  \"丙\" : 20\n  \"零\" : 0\n");
  const auto layout = st::md::mermaid::layout_pie(document.pie, LayoutOptions{}, stub_measure());
  ST_REQUIRE(layout.slices.size() == 4U);
  ST_CHECK(std::abs(layout.slices[0].start_angle) < 1e-9);
  double total = 0.0;
  for (const auto& slice : layout.slices) total += slice.percent;
  ST_CHECK(std::abs(total - 100.0) < 1e-6);
  // 零值那片没有角度，但图例仍在（"有这类数据，只是占比 0"）。
  ST_CHECK(layout.slices[3].sweep == 0.0);
  ST_CHECK(layout.slices[3].legend.width > 0.0f);
  ST_CHECK(layout.radius > 0.0f);
}

/// 全零数据不得除零（NaN 会污染整条绘制路径，而 NaN 在断言里"看起来正常"）。
ST_TEST(mermaid_layout_pie_all_zero_is_safe) {
  const auto document = st::md::mermaid::parse("pie\n  \"甲\" : 0\n  \"乙\" : 0\n");
  const auto layout = st::md::mermaid::layout_pie(document.pie, LayoutOptions{}, stub_measure());
  for (const auto& slice : layout.slices) {
    ST_CHECK(slice.sweep == 0.0);
    ST_CHECK(std::isfinite(slice.start_angle));
    ST_CHECK(std::isfinite(slice.percent));
  }
}

// ————————————————————————————————————————————————————————————————————————————
// 组件：属性面 / 动作面 / 视图
// ————————————————————————————————————————————————————————————————————————————

/// 换文本后 `kind` / 计数要跟着变（属性面是外部的唯一观测通道）。
ST_TEST(mermaid_view_reports_document_shape) {
  MermaidView view;
  view.set_source("flowchart TD\n  A --> B\n  B --> C\n");
  ST_CHECK_EQ(view.get_property("kind").value_or(""), std::string("flowchart"));
  ST_CHECK_EQ(view.get_property("node_count").value_or(""), std::string("3"));
  ST_CHECK_EQ(view.get_property("edge_count").value_or(""), std::string("2"));

  view.set_source("sequenceDiagram\n  A->>B: x\n");
  ST_CHECK_EQ(view.get_property("kind").value_or(""), std::string("sequence"));
}

/// 不支持的内容要能读出来（`error` 非空），而不是"看着空、查不到原因"。
ST_TEST(mermaid_view_exposes_diagnostics) {
  MermaidView view;
  view.set_source("gantt\n  title 计划\n");
  ST_CHECK_EQ(view.get_property("kind").value_or(""), std::string("unknown"));
  ST_CHECK(!view.get_property("error").value_or("").empty());
  view.set_source("flowchart TD\n  A --> B\n");
  ST_CHECK(view.get_property("error").value_or("x").empty());
}

/// fit 与 zoom 是两个量：`fit` 只缩不放；`zoom` 在 fit 之上相乘。
ST_TEST(mermaid_view_fit_and_zoom_are_separate) {
  Fixture fixture;
  MermaidView view;
  view.set_source("flowchart TD\n  A[很长的标签文字] --> B[也很长的标签文字]\n");
  fixture.layout(view, 800.0f, 600.0f);
  const float fit_scale = view.effective_scale();
  ST_CHECK(fit_scale > 0.0f);
  ST_CHECK(fit_scale <= 1.0f);  // 只缩不放

  // 小视口：必须缩得更小（fit 真在起作用）。
  fixture.layout(view, 120.0f, 80.0f);
  ST_CHECK(view.effective_scale() < fit_scale);

  // 关掉 fit：倍率只剩 zoom。
  view.set_fit(false);
  view.set_zoom(1.0f);
  ST_CHECK(std::abs(view.effective_scale() - 1.0f) < 1e-6f);

  // 放大受上限夹取（无限放大＝把界面变成一块色斑）。
  view.set_zoom(1000.0f);
  ST_CHECK(view.zoom() <= MermaidView::kMaxZoom);
  view.set_zoom(0.001f);
  ST_CHECK(view.zoom() >= MermaidView::kMinZoom);
}

/// 动作面：`zoom_in`/`zoom_out`/`reset_view` 要真的改状态；未知动作要如实拒绝。
ST_TEST(mermaid_view_actions) {
  MermaidView view;
  view.set_source("flowchart TD\n  A --> B\n");
  const float before = view.zoom();
  ST_CHECK(view.invoke_action("zoom_in", ""));
  ST_CHECK(view.zoom() > before);
  ST_CHECK(view.invoke_action("zoom_out", ""));
  ST_CHECK(std::abs(view.zoom() - before) < 1e-4f);
  ST_CHECK(view.invoke_action("reset_view", ""));
  ST_CHECK(std::abs(view.zoom() - 1.0f) < 1e-6f);
  ST_CHECK(!view.invoke_action("__no_such_action__", ""));
}

/// 属性面契约：未知属性拒读写、只读属性拒绝写。
ST_TEST(mermaid_view_property_contract) {
  MermaidView view;
  view.set_source("flowchart TD\n  A --> B\n");
  ST_CHECK(!view.get_property("__no_such_property__").has_value());
  ST_CHECK(!view.set_property("__no_such_property__", "x"));
  for (const std::string_view name : view.readonly_property_names()) {
    ST_CHECK(!view.set_property(name, "1"));
  }
  for (const std::string_view name : view.property_names()) {
    (void)view.get_property(name);  // 不崩即可（值可为空串）
  }
}

/// 绘制冒烟 + 像素断言：图确实画到了画布上（不是"属性全对、画面一片底"）。
ST_TEST(mermaid_view_paints_pixels) {
  Fixture fixture;
  MermaidView view;
  view.set_source("flowchart TD\n  A[开始] --> B[结束]\n");
  fixture.layout(view, 320.0f, 240.0f);

  st::raster::Canvas canvas(320, 240);
  const auto background = fixture.theme.colors().bg;
  canvas.clear(background);
  fixture.paint(view, canvas);

  std::size_t inked = 0;
  for (int y = 0; y < 240; ++y) {
    for (int x = 0; x < 320; ++x) {
      const auto pixel = canvas.pixel_at(x, y);
      const int delta = std::abs(static_cast<int>(pixel.r) - static_cast<int>(background.r)) +
                        std::abs(static_cast<int>(pixel.g) - static_cast<int>(background.g)) +
                        std::abs(static_cast<int>(pixel.b) - static_cast<int>(background.b));
      if (delta > 24) ++inked;
    }
  }
  ST_CHECK(inked > 400U);  // 两个节点框 + 连线的量级；画不出来会是个位数
}

/// 三种图都能画（换 kind 就换绘制分支，任何一个分支漏接都是"某类图永远空白"）。
ST_TEST(mermaid_view_paints_all_kinds) {
  Fixture fixture;
  const char* sources[] = {
      "flowchart LR\n  A --> B\n  B --> C\n",
      "sequenceDiagram\n  A->>B: 一\n  B-->>A: 二\n",
      "pie title 占比\n  \"甲\" : 60\n  \"乙\" : 40\n",
  };
  for (const char* source : sources) {
    MermaidView view;
    view.set_source(source);
    fixture.layout(view, 360.0f, 260.0f);
    st::raster::Canvas canvas(360, 260);
    canvas.clear(fixture.theme.colors().bg);
    fixture.paint(view, canvas);
    std::size_t inked = 0;
    for (int y = 0; y < 260; ++y) {
      for (int x = 0; x < 360; ++x) {
        const auto pixel = canvas.pixel_at(x, y);
        if (pixel.a != 0U && (pixel.r != 0U || pixel.g != 0U || pixel.b != 0U)) ++inked;
      }
    }
    ST_CHECK(inked > 300U);
  }
}

/// 饼图的圆必须是**真圆**（贝塞尔），不是折线逼近。
///
/// ## 这条为什么必须是像素断言
///
/// “用折线拼弧”在属性面与几何数据上全都对（`sweep`、`percent` 一个不差），
/// 唯一能看出差别的地方是**轮廓的半径误差**：n 段折线在圆上的最大亏空是
/// `R·(1 − cos(π/n))`——16 段时是 0.019R（200px 半径上 3.9px，肉眼明显），
/// 而 kappa 三次曲线不到 0.0003R。
///
/// 判据：在圆上采样若千个方向上量“最外层命中像素到圆心的距离”，
/// 与 `radius` 的最大偏差要远小于 16 段折线的亏空。
ST_TEST(mermaid_view_pie_outline_is_a_true_circle) {
  Fixture fixture;
  MermaidView view;
  view.set_source("pie\n  \"\u7532\" : 50\n  \"\u4e59\" : 50\n");
  view.set_fit(false);
  view.set_zoom(1.0f);
  fixture.layout(view, 400.0f, 400.0f);

  st::raster::Canvas canvas(400, 400);
  canvas.clear(fixture.theme.colors().bg);
  fixture.paint(view, canvas);

  // 圆心与半径：从图里拿，不自己猜（布局可能随字体/内边距变）。
  // 饼图水平居中、上下留白相等：用“非背景像素的包围盒”反算中心与半径。
  int min_x = 400;
  int min_y = 400;
  int max_x = -1;
  int max_y = -1;
  const auto background = fixture.theme.colors().bg;
  for (int y = 0; y < 400; ++y) {
    for (int x = 0; x < 400; ++x) {
      const auto pixel = canvas.pixel_at(x, y);
      const int delta = std::abs(static_cast<int>(pixel.r) - static_cast<int>(background.r)) +
                        std::abs(static_cast<int>(pixel.g) - static_cast<int>(background.g)) +
                        std::abs(static_cast<int>(pixel.b) - static_cast<int>(background.b));
      // 只认**饱和的扇形色**（排除图例文字与浅描边）：通道间差异大 = 彩色。
      const int spread = std::max({pixel.r, pixel.g, pixel.b}) - std::min({pixel.r, pixel.g, pixel.b});
      if (delta < 24 || spread < 40) continue;
      min_x = std::min(min_x, x);
      min_y = std::min(min_y, y);
      max_x = std::max(max_x, x);
      max_y = std::max(max_y, y);
    }
  }
  ST_REQUIRE(max_x > min_x);
  // 饼图左半边必被扇形覆盖：用**左半包围盒**反算中心与半径
  //（右半边可能有图例，且圆心处的分界不影响纵向极值）。
  const float center_x = static_cast<float>(min_x) + static_cast<float>(max_y - min_y) * 0.5f;
  const float center_y = static_cast<float>(min_y + max_y) * 0.5f;
  const float radius = static_cast<float>(max_y - min_y) * 0.5f;
  ST_CHECK(radius > 40.0f);

  // 沿 36 个方向量“边界半径”，再**扣掉一阶谐波**后看起伏。
  //
  // ## 两个量测坑（都实测踩过）
  //
  // ① **不能用“最外层命中像素”**：像素是整数格，量化误差能到 0.7px，
  //    而 16 段折线的半径起伏只有 1.7px 量级，同阶就量不出区别
  //    （实测：真圆也能读到 1.0px 起伏）。改成**覆盖率亚像素插值**：
  //    边界像素颜色 = 混色(bg, 扇形色, coverage)，反解 coverage 再插值出 0.5 的位置。
  // ② **圆心只能从像素反算**（布局结果在组件私域里），反算误差 ≈0.5px，
  //    它在半径上表现为**一次谐波**（`R + a cosθ + b sinθ`）。
  //    所以先把一阶谐波拟合掉，剩下的高阶分量才是“轮廓是不是圆”的判据——
  //    16 边形会在第 16 次谐波上给出 0.0095R 的幅度，而真圆只剩 <0.1px 的噪声。
  constexpr int kDirections = 36;
  std::vector<float> radii(kDirections, 0.0f);
  std::vector<float> cosines(kDirections, 0.0f);
  std::vector<float> sines(kDirections, 0.0f);
  for (int step = 0; step < kDirections; ++step) {
    const double angle = static_cast<double>(step) / kDirections * 2.0 * 3.14159265358979;
    const float dir_x = static_cast<float>(std::sin(angle));
    const float dir_y = static_cast<float>(-std::cos(angle));

    // 该方向上的扇形色：取半径中点处（同在扇形内）。
    const auto sample = [&](float t) {
      const int px = static_cast<int>(std::lround(center_x + dir_x * t));
      const int py = static_cast<int>(std::lround(center_y + dir_y * t));
      if (px < 0 || py < 0 || px >= 400 || py >= 400) return st::math::Color{0, 0, 0, 255};
      return canvas.pixel_at(px, py);
    };
    const auto foreground = sample(radius * 0.5f);
    const float delta_r = static_cast<float>(foreground.r) - static_cast<float>(background.r);
    const float delta_g = static_cast<float>(foreground.g) - static_cast<float>(background.g);
    const float delta_b = static_cast<float>(foreground.b) - static_cast<float>(background.b);
    const float norm = delta_r * delta_r + delta_g * delta_g + delta_b * delta_b;
    if (norm < 1.0f) continue;   // 该方向没有扇形（不会发生，但别除零）
    const auto coverage = [&](st::math::Color pixel) {
      const float dr = static_cast<float>(pixel.r) - static_cast<float>(background.r);
      const float dg = static_cast<float>(pixel.g) - static_cast<float>(background.g);
      const float db = static_cast<float>(pixel.b) - static_cast<float>(background.b);
      return (dr * delta_r + dg * delta_g + db * delta_b) / norm;
    };

    float edge = 0.0f;
    float previous_t = radius * 0.6f;
    float previous_coverage = coverage(sample(previous_t));
    for (float t = radius * 0.6f; t < radius + 3.0f; t += 0.2f) {
      const float current = coverage(sample(t));
      if (current <= 0.5f) {
        const float span = current - previous_coverage;
        const float ratio = std::abs(span) < 1e-4f ? 0.0f : (0.5f - previous_coverage) / span;
        edge = previous_t + (t - previous_t) * std::clamp(ratio, 0.0f, 1.0f);
        break;
      }
      previous_t = t;
      previous_coverage = current;
    }
    radii[step] = edge;
    cosines[step] = std::cos(static_cast<float>(angle));
    sines[step] = std::sin(static_cast<float>(angle));
  }

  // 最小二乘拟合 `r ≈ A + B·cos + C·sin`。基函数在均匀采样下正交，闭式解：
  // A = 均值；B = 2·mean(r·cos)；C = 2·mean(r·sin)。
  float mean_r = 0.0f;
  float mean_rc = 0.0f;
  float mean_rs = 0.0f;
  int counted = 0;
  for (int step = 0; step < kDirections; ++step) {
    if (radii[step] <= 0.0f) continue;
    mean_r += radii[step];
    mean_rc += radii[step] * cosines[step];
    mean_rs += radii[step] * sines[step];
    ++counted;
  }
  ST_REQUIRE(counted >= kDirections - 2);
  mean_r /= static_cast<float>(counted);
  mean_rc = 2.0f * mean_rc / static_cast<float>(counted);
  mean_rs = 2.0f * mean_rs / static_cast<float>(counted);

  float residual_min = 1e9f;
  float residual_max = -1e9f;
  for (int step = 0; step < kDirections; ++step) {
    if (radii[step] <= 0.0f) continue;
    const float predicted = mean_r + mean_rc * cosines[step] + mean_rs * sines[step];
    const float residual = radii[step] - predicted;
    residual_min = std::min(residual_min, residual);
    residual_max = std::max(residual_max, residual);
  }
  const float variation = residual_max - residual_min;
  // 真圆：<0.2px 噪声；16 段折线：峰-峰 ≈0.019R（半径 90 时 ≈1.7px）。
  if (variation >= radius * 0.005f) {
    ST_FAIL(std::format("圆弧半径起伏 {:.3f}px（半径 {:.2f}px，疑似折线逼近）", variation, radius));
  }
}

/// 滚轮：内容装得下时**不吞事件**（冒泡给外层，嵌套滚动正解）；放大到溢出后
/// 才捕获并纵向平移。滚轮缩放走 `Ctrl`（与 `Viewport`/`TimelineView` 同约定）。
ST_TEST(mermaid_view_wheel_and_pan) {
  Fixture fixture;
  MermaidView view;
  view.set_source("flowchart TD\n  A --> B\n  B --> C\n  C --> D\n");
  fixture.layout(view, 400.0f, 120.0f);

  // fit 开着 ⇒ 整图已装进视口，没东西可滚 ⇒ 返回 false 让外层接管。
  st::ui::Event wheel{ };
  wheel.kind = st::ui::EventKind::Wheel;
  wheel.position = st::math::Point{200.0f, 60.0f};
  wheel.wheel_delta = -1.0f;
  ST_CHECK(!view.on_event(fixture.context, wheel));

  // 关 fit + 放大 4 倍 ⇒ 内容溢出 ⇒ 滚轮被捕获且真的改了 pan。
  view.set_fit(false);
  view.set_zoom(4.0f);
  view.set_pan(0.0f, 0.0f);
  const float pan_before = view.pan_y();
  ST_CHECK(view.on_event(fixture.context, wheel));
  ST_CHECK(view.pan_y() != pan_before);

  // Ctrl+滚轮：以指针为锚缩放（缩放前后指针下的内容点不变）。
  view.reset_view();
  const float zoom_before = view.zoom();
  st::ui::Event ctrl_wheel{};
  ctrl_wheel.kind = st::ui::EventKind::Wheel;
  ctrl_wheel.position = st::math::Point{120.0f, 70.0f};
  ctrl_wheel.wheel_delta = 1.0f;
  ctrl_wheel.ctrl = true;
  ST_CHECK(view.on_event(fixture.context, ctrl_wheel));
  ST_CHECK(view.zoom() > zoom_before);

  // 拖拽平移：按下 → 移动 → 抬起。
  view.set_fit(false);
  view.set_zoom(2.0f);
  view.set_pan(0.0f, 0.0f);
  st::ui::Event down{};
  down.kind = st::ui::EventKind::MouseDown;
  down.position = st::math::Point{50.0f, 50.0f};
  ST_CHECK(view.on_event(fixture.context, down));
  st::ui::Event move{};
  move.kind = st::ui::EventKind::MouseMove;
  move.position = st::math::Point{80.0f, 66.0f};
  ST_CHECK(view.on_event(fixture.context, move));
  ST_CHECK(std::abs(view.pan_x() - 30.0f) < 0.01f);
  ST_CHECK(std::abs(view.pan_y() - 16.0f) < 0.01f);
  st::ui::Event up{};
  up.kind = st::ui::EventKind::MouseUp;
  up.position = st::math::Point{80.0f, 66.0f};
  ST_CHECK(view.on_event(fixture.context, up));
  // 抬起后再移动不再改 pan（不是"粘住指针"）。
  st::ui::Event move2{};
  move2.kind = st::ui::EventKind::MouseMove;
  move2.position = st::math::Point{200.0f, 100.0f};
  ST_CHECK(!view.on_event(fixture.context, move2));
  ST_CHECK(std::abs(view.pan_x() - 30.0f) < 0.01f);
}
