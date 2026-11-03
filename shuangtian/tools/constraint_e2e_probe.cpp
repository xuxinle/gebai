// 约束解算端到端探针（JSON + XML 双路径对照）：
// 定位「同一场景 XML 路径与 JSON 路径解算结果不同」的问题层。
#include <cstdio>

#include "st/raster/scene3d_script.hpp"
#include "st/raster/scene_serialize.hpp"
#include "st/raster/scene_xml.hpp"

auto main() -> int {
  const char* json = R"j({
    "objects": [
      {"name":"table","shape":"box","size":[2,0.2,2],"translate":[0,0.5,0]},
      {"name":"gear","shape":"gear","size":[0.8,0.1],"teeth":10,
       "attach":"top_of:table+0,0.05,0"},
      {"name":"post","shape":"cylinder","size":[0.1,0.6],"attach":"top_of:gear"},
      {"name":"lamp","shape":"sphere","size":0.3,"attach":"top_of:post"},
      {"name":"cable","shape":"tube","connect":"right_of:lamp -> front_of:table",
       "size":[0.05,"auto"]}
    ]
  })j";
  const char* xml = R"XML(<Scene>
  <Box name="table" size="2, 0.2, 2" translate="0, 0.5, 0"/>
  <Gear name="gear" size="0.8, 0.1" teeth="10" attach="top_of:table+0, 0.05, 0"/>
  <Cylinder name="post" size="0.1, 0.6" attach="top_of:gear"/>
  <Sphere name="lamp" size="0.3" attach="top_of:post"/>
  <Tube name="cable" connect="right_of:lamp -> front_of:table" size="0.05, auto"/>
</Scene>)XML";

  const auto spec_json = st::raster::parse_scene_spec_text(json);
  const auto spec_xml = st::raster::parse_scene_xml(xml);
  if (!spec_json.has_value()) {
    std::printf("JSON 解析失败：%s\n", spec_json.error().message.c_str());
    return 1;
  }
  if (!spec_xml.has_value()) {
    std::printf("XML 解析失败：%s\n", spec_xml.error().message.c_str());
    return 1;
  }
  for (std::size_t i = 0; i < spec_json->objects.size() && i < spec_xml->objects.size(); ++i) {
    const auto& a = spec_json->objects[i];
    const auto& b = spec_xml->objects[i];
    std::printf("%-7s J:(%.3f, %.3f, %.3f | %.2f, %.2f, %.2f | sy=%.4f)\n"
                "        X:(%.3f, %.3f, %.3f | %.2f, %.2f, %.2f | sy=%.4f)\n",
                a.name.c_str(), double(a.translate.x), double(a.translate.y),
                double(a.translate.z), double(a.rotation.x), double(a.rotation.y),
                double(a.rotation.z), double(a.size.y), double(b.translate.x),
                double(b.translate.y), double(b.translate.z), double(b.rotation.x),
                double(b.rotation.y), double(b.rotation.z), double(b.size.y));
  }
  return 0;
}
