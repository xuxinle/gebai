// `st_api` —— 输出**机读能力清单**（场景脚本的字段契约）。
//
// ## 为什么是独立可执行文件而不是 `st` 的子命令
//
// `st` 的源列表是**白名单**（`st.pkg` 里写得很清楚：黑名单会静默变胖，
// 白名单漏写会链接报错）。它刻意不依赖 raster——因为构建工具不该为了
// 打印一份清单把整个三维/渲染层拖进自举范围（那会让改 raster 触发 st 重建）。
//
// 所以做成独立工具：需要清单时调它，`st` 保持精简。
//
// ## 用法
//
//   st_api                 # 输出 JSON 清单（缩进）
//   st_api --compact       # 单行（管道给程序时用）
//   st_api --example       # 只输出起步脚本
//   st_api --scopes        # 只列各层名字（快速查"有哪些 scope"）
//   st_api --shape <名字>  # 这个形状的 size 怎么写（最高频的问题）
//   st_api --theme         # 主题令牌清单（自定主题/控制通道共用的名字表）
//   st_api --check <文件>  # 校验并自检一份场景脚本（**写完后自查**）
//   st_api --normalize <文件>  # 读入再重新写出（看引擎"理解成了什么"）
#include <cstdio>
#include <cstring>
#include <string>

#include "st/core/fs.hpp"
#include "st/core/json.hpp"
#include "st/raster/api_manifest.hpp"
#include "st/raster/scene_describe.hpp"
#include "st/raster/scene3d_script.hpp"
#include "st/raster/scene_schema.hpp"
#include "st/raster/scene_serialize.hpp"
#include "st/ui/theme.hpp"

int main(int argc, char** argv) {
  bool compact = false;
  bool example_only = false;
  bool scopes_only = false;
  std::string check_path;
  std::string normalize_path;
  std::string shape_query;
  bool theme_only = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--compact") {
      compact = true;
    } else if (arg == "--example") {
      example_only = true;
    } else if (arg == "--scopes") {
      scopes_only = true;
    } else if (arg == "--check" && i + 1 < argc) {
      check_path = argv[++i];
    } else if (arg == "--normalize" && i + 1 < argc) {
      normalize_path = argv[++i];
    } else if (arg == "--shape" && i + 1 < argc) {
      shape_query = argv[++i];
    } else if (arg == "--theme") {
      theme_only = true;
    } else if (arg == "--help" || arg == "-h") {
      std::printf(
          "用法: st_api [选项]\n"
          "  无参数      输出完整能力清单（JSON，缩进）\n"
          "  --compact   单行 JSON\n"
          "  --example   只输出最小可运行脚本\n"
          "  --scopes    只列各层名字（objects[] / animation.tracks[] ...）\n"
          "  --shape <n> 某个形状的 size 怎么写（含义/示例/旧写法换算）\n"
          "  --theme     主题令牌清单（颜色 + 尺寸）\n"
          "  --check <f>      校验场景脚本并输出自检摘要（退出码非 0 = 有问题）\n"
          "  --normalize <f>  读入后重新写出（看引擎理解成了什么）\n");
      return 0;
    }
  }

  if (example_only) {
    std::printf("%s\n", st::raster::minimal_example_script().c_str());
    return 0;
  }
  // ── 主题令牌清单 ──
  //
  // ## 为什么要输出这个
  //
  // 自定义主题与主题文件写的都是令牌名（`bg` / `surface_alt` / `text_muted` …）。
  // 没有清单时，模型只能从文档里一篇篇翻，或者猜一个名字然后被拒绝——
  // 而拒绝时也不知道"到底有哪些合法名字"。
  //
  // 这里直接列出全部令牌，按颜色/尺寸分组。
  if (theme_only) {
    std::printf("{\n  \"palette\": [\n");
    const auto& palette = st::ui::palette_token_names();
    for (std::size_t i = 0; i < palette.size(); ++i) {
      std::printf("    \"%s\"%s\n", std::string(palette[i]).c_str(),
                  i + 1 < palette.size() ? "," : "");
    }
    std::printf("  ],\n  \"metrics\": [\n");
    const auto& metrics = st::ui::metric_token_names();
    for (std::size_t i = 0; i < metrics.size(); ++i) {
      std::printf("    \"%s\"%s\n", std::string(metrics[i]).c_str(),
                  i + 1 < metrics.size() ? "," : "");
    }
    std::printf("  ]\n}\n");
    return 0;
  }

  if (!shape_query.empty()) {
    std::printf("%s\n", st::raster::shape_size_doc(shape_query).c_str());
    return 0;
  }
  if (scopes_only) {
    for (const auto& table : st::raster::scene_field_tables()) {
      std::printf("%-32s %zu 个字段\n", table.scope.c_str(), table.fields.size());
    }
    return 0;
  }
  // ── 校验 + 自检 ──
  //
  // ## 为什么把这两步合并成一个命令
  //
  // "写完脚本 → 检查 → 看诊断" 是模型/人最高频的动作序列。
  // 拆成两个命令（解析、自检）会让人少跑一步就以为没问题；
  // 合成一个，跑到就都跑了。
  if (!check_path.empty()) {
    const auto text = st::fs::read_text(check_path);
    if (!text.has_value()) {
      std::fprintf(stderr, "读取失败：%s\n", text.error().message.c_str());
      return 1;
    }
    const auto spec = st::raster::parse_scene_spec_text(*text);
    if (!spec.has_value()) {
      // 解析失败时**只打错误**（此时没有可自检的东西）
      std::fprintf(stderr, "解析失败：%s\n", spec.error().message.c_str());
      return 2;
    }
    const auto summary = st::raster::describe_scene(*spec);
    std::printf("%s", st::raster::summary_text(summary).c_str());
    std::printf("\n--- JSON ---\n%s\n", st::json_dump(st::raster::summary_json(summary), true).c_str());
    return summary.has_error() ? 3 : 0;   // 3 = 解析通过但有 error 级诊断
  }

  // ── 重新写出（看理解）──
  if (!normalize_path.empty()) {
    const auto text = st::fs::read_text(normalize_path);
    if (!text.has_value()) {
      std::fprintf(stderr, "读取失败：%s\n", text.error().message.c_str());
      return 1;
    }
    const auto spec = st::raster::parse_scene_spec_text(*text);
    if (!spec.has_value()) {
      std::fprintf(stderr, "解析失败：%s\n", spec.error().message.c_str());
      return 2;
    }
    std::printf("%s\n", st::raster::serialize_scene_text(*spec, true).c_str());
    return 0;
  }

  std::printf("%s\n", st::raster::api_manifest_text(!compact).c_str());
  return 0;
}
