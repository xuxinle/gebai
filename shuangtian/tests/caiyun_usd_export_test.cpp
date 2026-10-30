/// USD 导出（P4）：几何 + 材质 + 变换，不导意图/历史/动画。
///
/// ## 这组用例守的是什么
///
/// 导出类功能最容易"看起来成功、实际丢了东西"——文件生成了，大小也像样，
/// 但下游打开少一半物体。而下游是别人的管线，缺陷在那里才暴露，回来查已经很贵。
///
/// 所以这里用的是**结构断言**，不是"文件非空"：
/// 括号配平、每个对象一个 Prim、面数与形状参数吻合、变换值原样落进去、
/// 非法标识符被净化、`obj:` 加载失败**整体失败**而不是静默跳过。
#include <string>
#include <vector>

#include "examples/caiyun/usd_export.hpp"
// 测试目标不链接 examples/caiyun，所以直接包含实现（仓库惯例）。
#include "examples/caiyun/usd_export.cpp"
#include "st/core/print.hpp"
#include "st/core/log.hpp"
#include "st/test/test.hpp"

namespace {

[[nodiscard]] auto export_json(std::string_view json, std::string_view root = "World")
    -> caiyun::Result<std::string> {
  caiyun::UsdExportOptions options{};
  options.root_name = std::string{root};
  return caiyun::export_usda_from_script_text(json, options);
}

/// 括号配平（USD 是嵌套的结构，不配平就是语法错）。
[[nodiscard]] auto braces_balanced(std::string_view text) -> bool {
  int depth = 0;
  for (const char ch : text) {
    if (ch == '{') ++depth;
    if (ch == '}') --depth;
    if (depth < 0) return false;
  }
  return depth == 0;
}

/// 数某个子串出现的次数。
[[nodiscard]] auto count_of(std::string_view text, std::string_view needle) -> int {
  if (needle.empty()) return 0;
  int count = 0;
  std::size_t pos = 0;
  while ((pos = text.find(needle, pos)) != std::string_view::npos) {
    ++count;
    pos += needle.size();
  }
  return count;
}

}  // namespace

ST_TEST(caiyun_usd_header_and_axis) {
  const auto usda = export_json(R"j({
  "objects": [
    {
      "shape": "sphere",
      "size": 1.0
    }
  ]
})j");
  ST_CHECK(usda.has_value());
  if (!usda) {
    st::log::out("导出失败：{}\n", usda.error().message);
    return;
  }
  // 头必须声明 upAxis，且是 Y（与霜天三维同向，不做轴变换）
  ST_CHECK(usda->rfind("#usda 1.0", 0) == 0);
  ST_CHECK(usda->find("upAxis = \"Y\"") != std::string::npos);
  ST_CHECK(usda->find("defaultPrim = \"World\"") != std::string::npos);
  ST_CHECK(braces_balanced(*usda));
}

ST_TEST(caiyun_usd_each_object_becomes_a_prim) {
  const auto usda = export_json(R"j({
  "objects": [
    {
      "shape": "sphere",
      "size": 1.0
    },
    {
      "shape": "box",
      "size": [
        1,
        1,
        1
      ]
    },
    {
      "shape": "torus",
      "size": [
        1.08,
        0.28
      ]
    }
  ]
})j");
  ST_CHECK(usda.has_value());
  if (!usda) return;
  // 三个对象 → 三个 Mesh Prim。少一个就是"导出了但丢东西"。
  ST_CHECK(count_of(*usda, "def Mesh ") == 3);
  ST_CHECK(usda->find("\"sphere_0\"") != std::string::npos);
  ST_CHECK(usda->find("\"box_1\"") != std::string::npos);
  ST_CHECK(usda->find("\"torus_2\"") != std::string::npos);
}

ST_TEST(caiyun_usd_box_has_twelve_triangles) {
  // 立方体的已知量：6 面 × 2 三角形 = 12，24 个独立顶点（硬边，不共享）。
  const auto usda = export_json(R"j({
  "objects": [
    {
      "shape": "box",
      "size": [
        1,
        1,
        1
      ]
    }
  ]
})j");
  ST_CHECK(usda.has_value());
  if (!usda) return;
  const std::size_t counts_at = usda->find("int[] faceVertexCounts = [");
  ST_CHECK(counts_at != std::string::npos);
  if (counts_at == std::string::npos) return;
  // 开括号位置 = 匹配串末尾前一个字符（那个 '['）。
  // 不能再用 `find('[')`：`int[]` 里就有方括号，会定位到类型声明上。
  const std::size_t body_at = counts_at + std::string_view{"int[] faceVertexCounts = "}.size();
  const std::size_t close = usda->find(']', body_at);
  ST_CHECK(close != std::string::npos);
  if (close == std::string::npos) return;
  const std::string counts = usda->substr(body_at + 1, close - body_at - 1);
  // 按**逗号分隔的项**数，不是数字字符数：
  // 用 `count_of(counts, "3")` 会把 "36" 里的 3 也算进去。
  int items = 0;
  std::size_t pos = 0;
  while (pos < counts.size()) {
    const std::size_t comma = counts.find(',', pos);
    const std::size_t end = comma == std::string::npos ? counts.size() : comma;
    std::string item = counts.substr(pos, end - pos);
    while (!item.empty() && item.front() == ' ') item.erase(item.begin());
    if (!item.empty()) {
      ++items;
      ST_CHECK(item == "3");  // 只导三角形
    }
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
  ST_CHECK(items == 12);
  if (items != 12) {
    st::log::out("faceVertexCounts 项数 = {}，内容 =「{}」\n", items, counts.substr(0, 120));
  }

  // 同时校验索引数 = 面数 × 3（拓扑自洽）
  const std::size_t idx_at = usda->find("int[] faceVertexIndices = [");
  ST_CHECK(idx_at != std::string::npos);
  if (idx_at == std::string::npos) return;
  const std::size_t idx_body = idx_at + std::string_view{"int[] faceVertexIndices = "}.size();
  const std::size_t idx_close = usda->find(']', idx_body);
  const std::string indices = usda->substr(idx_body + 1, idx_close - idx_body - 1);
  int index_items = 0;
  pos = 0;
  while (pos < indices.size()) {
    const std::size_t comma = indices.find(',', pos);
    const std::size_t end = comma == std::string::npos ? indices.size() : comma;
    std::string item = indices.substr(pos, end - pos);
    while (!item.empty() && item.front() == ' ') item.erase(item.begin());
    if (!item.empty()) ++index_items;
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
  ST_CHECK(index_items == items * 3);
}

ST_TEST(caiyun_usd_transform_is_preserved) {
  const auto usda = export_json(R"j({
  "objects": [
    {
      "shape": "sphere",
      "translate": [
        1.5,
        -2.25,
        0.75
      ],
      "scale": [
        2,
        3,
        4
      ],
      "size": 1.0
    }
  ]
})j");
  ST_CHECK(usda.has_value());
  if (!usda) return;
  // 值必须原样出现（浮点格式化错位会让物体飞走，且画面上看像"相机不对"）
  ST_CHECK(usda->find("xformOp:translate = (1.5, -2.25, 0.75)") != std::string::npos);
  ST_CHECK(usda->find("xformOp:scale = (2.0, 3.0, 4.0)") != std::string::npos);
  // xformOpOrder 必须声明，否则 USD 不知道该按什么顺序合成
  ST_CHECK(usda->find("xformOpOrder") != std::string::npos);
}

ST_TEST(caiyun_usd_sanitizes_identifiers) {
  // 形状名实际只有内置几个，但对象名可能来自脚本；非法字符会让 usdcat 报语法错，
  // 而错误指向"第几行有非法字符"，离"是某个名字"很远。
  const auto usda = export_json(R"j({
  "objects": [
    {
      "shape": "sphere",
      "size": 1.0
    }
  ]
})j", "My World-1");
  ST_CHECK(usda.has_value());
  if (!usda) return;
  // 根名被净化成合法标识符
  ST_CHECK(usda->find("def Xform \"My_World_1\"") != std::string::npos);
  ST_CHECK(usda->find("defaultPrim = \"My_World_1\"") != std::string::npos);
  // 不能残留原始名（那会让 defaultPrim 与实际 Prim 对不上）
  ST_CHECK(usda->find("\"My World-1\"") == std::string::npos);
}

ST_TEST(caiyun_usd_color_is_linearized) {
  // sRGB 255 → 线性 1.0；sRGB 128 → 约 0.2158（gamma 2.2 附近）
  const auto usda = export_json(R"j({
  "objects": [
    {
      "shape": "sphere",
      "color": "#ffffff",
      "size": 1.0
    }
  ]
})j");
  ST_CHECK(usda.has_value());
  if (!usda) return;
  ST_CHECK(usda->find("displayColor = [(1.0, 1.0, 1.0)]") != std::string::npos);

  const auto mid = export_json(R"j({
  "objects": [
    {
      "shape": "sphere",
      "color": "#808080",
      "size": 1.0
    }
  ]
})j");
  ST_CHECK(mid.has_value());
  if (!mid) return;
  // 128/255 = 0.502 sRGB → 线性 0.2158。若不做转换会写成 0.501961（明显偏亮）。
  ST_CHECK(mid->find("displayColor = [(0.215861, 0.215861, 0.215861)]") != std::string::npos);
}

ST_TEST(caiyun_usd_bad_obj_fails_whole_export) {
  // `obj:` 加载失败必须**整体失败**：静默少一个物体是导出类功能最难发现的缺陷。
  const auto usda = export_json(R"j({"objects":[
    {"shape":"obj:definitely_missing_mesh.obj"}
  ]})j");
  ST_CHECK(!usda.has_value());
  if (!usda) st::log::out("缺网格的报错：{}\n", usda.error().message);
}

ST_TEST(caiyun_usd_rejects_broken_json) {
  const auto usda = export_json("{not json");
  ST_CHECK(!usda.has_value());
}

ST_TEST(caiyun_usd_no_creative_layers_leak) {
  // 意图/历史/动画**不该**出现在 USD 里（那是创作过程，不是场景内容）。
  const auto usda = export_json(R"j({
  "objects": [
    {
      "shape": "sphere",
      "size": 1.0
    }
  ]
})j");
  ST_CHECK(usda.has_value());
  if (!usda) return;
  ST_CHECK(usda->find("intent") == std::string::npos);
  ST_CHECK(usda->find("history") == std::string::npos);
  ST_CHECK(usda->find("proposal") == std::string::npos);
  // 时间轴也不该有（裁云的动效由 JS 逐帧求值，不是关键帧曲线）
  ST_CHECK(usda->find("timeSamples") == std::string::npos);
}
