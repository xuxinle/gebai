// **三维渲染自检工具**：把"画面对不对"变成一份可读的报告。
//
// ## 这个工具解决的具体问题
//
// 三维的失败几乎从不报错。相机在物体里、物体在取景框外、光方向反了、
// 颜色口径错——解析器与渲染器全都"成功"，产出一张黑屏或空白图。
// 而**从截图判断这件事恰恰是模型的弱项**（这是本项目被反复确认的事实）。
//
// 所以闭环不能建立在"看图"上，必须建立在"读数字"上：
//
//   写场景（three.js / JSON）→ 本工具 → 文本报告（FAIL + 具体原因 + 可照抄的改法）
//        ↑                                                    │
//        └────────────────── 按建议改 ─────────────────────────┘
//
// ## 两种输入
//
// | 参数 | 用途 |
// |---|---|
// | `--three 文件.js` | three.js 源码（模型最擅长写的那种） |
// | `--scene 文件.json` | 本框架的声明式场景 JSON |
//
// ## 输出
//
// · 默认打印**人/模型可读的文本报告**（第一行就是 PASS/FAIL 结论）；
// · `--json` 打印结构化 JSON（给程序与自动化用）；
// · `--png 路径` 另存一张画面（人眼复核用——但**判据不依赖它**）。
//
// ## 退出码
//
// `0` = 结论 ok；`1` = 有 error；`2` = 参数/IO/解析失败。
// 于是它可以直接当 CI 门禁用：`st three-check --three hero.js || fail`。

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "st/codec/png.hpp"
#include "st/core/fs.hpp"
#include "st/core/json.hpp"
#include "st/core/print.hpp"
#include "st/raster/canvas.hpp"
#include "st/raster/scene_report.hpp"
#include "st/raster/scene3d_script.hpp"

namespace {

void usage() {
  std::printf(
      "用法: st three-check [选项]\n"
      "\n"
      "  --three <文件.js>    three.js 源码场景（模型最擅长写的形态）\n"
      "  --scene <文件.json>  本框架的声明式场景 JSON\n"
      "  --size <WxH>         渲染分辨率（默认 480x320）\n"
      "  --probe <N>          逐物体探测分辨率（默认 120；越大越慢越准）\n"
      "  --json               输出结构化 JSON（给程序用）\n"
      "  --png <文件>         另存一张画面（人眼复核用）\n"
      "  --quiet              只输出结论行（CI 用）\n"
      "\n"
      "退出码: 0 = ok，1 = 有 error（画面不可用），2 = 参数/IO/解析失败\n");
}

auto read_text_file(const std::string& path) -> st::Result<std::string> {
  const auto exists = st::fs::exists(path);
  if (!exists) return st::unexpected(st::ErrorCode::NotFound, "文件不存在: " + path);
  std::FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return st::unexpected(st::ErrorCode::Io, "打不开文件: " + path);
  }
  std::string content;
  char buffer[8192];
  while (true) {
    const std::size_t read = std::fread(buffer, 1, sizeof(buffer), file);
    if (read == 0) break;
    content.append(buffer, read);
  }
  std::fclose(file);
  return content;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  std::string three_path;
  std::string scene_path;
  std::string png_path;
  int width = 480;
  int height = 320;
  int probe = 120;
  bool as_json = false;
  bool quiet = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    const auto next = [&]() -> std::string {
      if (i + 1 >= argc) return {};
      return argv[++i];
    };
    if (arg == "--help" || arg == "-h") {
      usage();
      return 0;
    }
    if (arg == "--three") {
      three_path = next();
    } else if (arg == "--scene") {
      scene_path = next();
    } else if (arg == "--png") {
      png_path = next();
    } else if (arg == "--json") {
      as_json = true;
    } else if (arg == "--quiet") {
      quiet = true;
    } else if (arg == "--probe") {
      probe = std::atoi(next().c_str());
      if (probe < 32) probe = 32;
    } else if (arg == "--size") {
      const std::string value = next();
      const std::size_t split = value.find('x');
      if (split == std::string::npos) {
        std::printf("--size 需要 WxH（如 480x320）\n");
        return 2;
      }
      width = std::atoi(value.substr(0, split).c_str());
      height = std::atoi(value.substr(split + 1).c_str());
      if (width < 32) width = 32;
      if (height < 32) height = 32;
    } else {
      std::printf("未知参数: %s\n", std::string(arg).c_str());
      usage();
      return 2;
    }
  }

  if (three_path.empty() && scene_path.empty()) {
    std::printf("需要 --three 或 --scene（至少一个）\n\n");
    usage();
    return 2;
  }
  if (!three_path.empty() && !scene_path.empty()) {
    std::printf("--three 与 --scene 只能选一个\n");
    return 2;
  }

  // ── 读取并构建 SceneSpec ──
  st::raster::SceneSpec spec{};
  if (!three_path.empty()) {
    const auto source = read_text_file(three_path);
    if (!source.has_value()) {
      std::printf("%s\n", source.error().message.c_str());
      return 2;
    }
    spec.three_script = *source;
    spec.three_script_name = three_path;   // 报错要指到真实文件
  } else {
    const auto source = read_text_file(scene_path);
    if (!source.has_value()) {
      std::printf("%s\n", source.error().message.c_str());
      return 2;
    }
    auto parsed = st::raster::parse_scene_spec_text(*source);
    if (!parsed.has_value()) {
      std::printf("FAIL parse — %s\n", parsed.error().message.c_str());
      return 2;
    }
    spec = *parsed;
  }

  // ── 渲染并自检 ──
  const auto report = st::raster::inspect_scene_render(spec, width, height,
                                                       st::math::Color{8, 10, 16, 255}, probe);
  if (report.verdict == "render_failed") {
    std::printf("FAIL render_failed — 三维后端不可用\n");
    return 2;
  }

  // ── 输出 ──
  if (as_json) {
    std::printf("%s\n", st::raster::report_json(report).dump(2).c_str());
  } else if (quiet) {
    std::printf("%s %s coverage=%.4f luma=%.1f\n", report.ok() ? "PASS" : "FAIL",
                report.verdict.c_str(), report.coverage, report.mean_luma);
  } else {
    std::printf("%s", st::raster::report_text(report).c_str());
  }

  // ── 可选落盘 ──
  if (!png_path.empty()) {
    st::raster::Canvas canvas(width, height);
    const auto status =
        st::raster::render_scene(spec, canvas, st::math::Color{8, 10, 16, 255});
    if (status.has_value()) {
      const auto rgba = canvas.to_rgba8();
      const auto encoded = st::codec::png_encode(st::codec::PngImage{
          static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), rgba});
      if (encoded.has_value()) {
        if (std::FILE* file = std::fopen(png_path.c_str(), "wb")) {
          std::fwrite(encoded.value().data(), 1, encoded.value().size(), file);
          std::fclose(file);
          if (!quiet && !as_json) std::printf("画面已落盘: %s\n", png_path.c_str());
        }
      }
    }
  }

  return report.has_error() ? 1 : 0;
}
