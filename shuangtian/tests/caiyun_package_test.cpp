/// 工程包（`.cypkg`）的往返保真与历史层。
///
/// ## 这组用例守的是什么
///
/// 包的唯一承诺是「同一份工程，既能分六层维护，也能一键退回单文件出片」。
/// 只要 `expand → flatten` 不精确，包就成了第二套真值源，两份数据迟早漂移，
/// 而漂移是**静默**的：画面照常出，只是作者写过的某个字段在存盘那一步消失了。
///
/// 所以这里测的不是"功能有没有"，而是"**来回之后是否还是原样**"。
#include <cstdio>
#include <string>
#include <vector>

#include "examples/caiyun/director.hpp"
#include "examples/caiyun/package.hpp"
#include "examples/caiyun/package_expand.hpp"
// 测试目标不链接 examples/caiyun，所以**直接包含实现**（仓库惯例：
// 见 caiyun_layer_test.cpp 的 `#include "examples/caiyun/layer.cpp"`）。
#include "examples/caiyun/package.cpp"
#include "examples/caiyun/package_expand.cpp"
#include "examples/caiyun/package_history.cpp"
#include "st/core/fs.hpp"
#include "st/core/print.hpp"
#include "st/test/test.hpp"

namespace {

/// 每个用例独占一个临时目录（并行跑测试时不会互相踩）。
[[nodiscard]] auto fresh_dir(std::string_view name) -> std::string {
  const std::string dir = st::fs::join(st::fs::temp_dir(), "st_cypkg_" + std::string{name});
  // 先清掉上一次运行的残留：否则"包已存在"会让用例在第二次运行时表现不同
  // （这类"首次绿、复跑红"的用例比没有用例更糟）。
  (void)st::fs::remove_all(dir);
  return dir;
}

/// 一份覆盖主要特性的脚本，作为往返基准。
[[nodiscard]] auto sample_script() -> caiyun::Script {
  const std::string json = R"({
  "name": "包往返探针",
  "fps": 24,
  "size": [1600, 900],
  "transition": 0.42,
  "fade_in": 0.6,
  "fade_out": 0.7,
  "brand_wordmark": "PKG",
  "brand_latin": "ROUNDTRIP",
  "footer_note": "p1",
  "audio": {"enabled": true, "style": "calm", "bpm": 96, "gain": 0.66},
  "scenes": [
    {
      "kind": "custom",
      "duration": 2.75,
      "title": "第一幕",
      "subtitle": "副标",
      "lines": ["甲", "乙"],
      "accent": "#3355aa",
      "background": ["#010203", "#040506"],
      "wrap": false,
      "dof": 0.22,
      "grain": 512,
      "image": "assets/bg.png",
      "carry_out": {
        "name": "承接",
        "into": 1,
        "at": 2.1,
        "dur": 0.55,
        "from": {"x": 1, "y": 2, "w": 3, "h": 4, "radius": 5},
        "to": {"x": 6, "y": 7, "w": 8, "h": 9, "radius": 10},
        "fill": "#abcdef",
        "fade_out": 0.3
      },
      "layers": [
        {"kind": "rect", "name": "底", "x": "0%", "y": "0%", "w": "100%", "h": "100%",
         "fill": "#101820", "radius": 4},
        {"kind": "text", "name": "字", "text": "内容", "x": 40, "y": 60, "w": 300, "h": 90,
         "size": 52, "color": "#ffffff", "align": 1},
        {"kind": "model3d", "name": "模型", "x": "10%", "y": "10%", "w": "40%", "h": "40%",
         "model_shape": "obj:assets/mesh.obj", "model_color": "#d0b090",
         "cam_azimuth": 18.0, "cam_distance": 3.4}
      ]
    },
    {
      "kind": "outro",
      "duration": 1.25,
      "title": "收尾"
    }
  ]
})";
  const auto parsed = caiyun::parse_script(json);
  ST_CHECK(parsed.has_value());
  return parsed.has_value() ? *parsed : caiyun::Script{};
}

}  // namespace

ST_TEST(caiyun_package_roundtrip_preserves_script) {
  const std::string dir = fresh_dir("roundtrip");
  const caiyun::Script original = sample_script();

  // 建包（含两个真实资产，用来验哈希与引用解析）
  (void)st::fs::create_directories(st::fs::join(dir, "assets"));
  (void)st::fs::write_text(st::fs::join(st::fs::join(dir, "assets"), "bg.png"), "png-bytes");
  (void)st::fs::write_text(st::fs::join(st::fs::join(dir, "assets"), "mesh.obj"), "o m\nv 0 0 0\n");

  const auto created = caiyun::create_package(dir, original, dir, "probe", false);
  ST_CHECK(created.has_value());
  if (!created) {
    st::print("建包失败：{}\n", created.error().message);
    return;
  }

  // ① 拍平应当与原文**逐字节相同**（这是包的核心承诺）。
  const auto flattened = caiyun::flatten_to_json(dir);
  ST_CHECK(flattened.has_value());
  if (!flattened) return;
  const std::string expected = caiyun::script_to_json(original);
  if (*flattened != expected) {
    const std::size_t limit = std::min(flattened->size(), expected.size());
    std::size_t d = 0;
    while (d < limit && (*flattened)[d] == expected[d]) ++d;
    const std::size_t from = d > 100 ? d - 100 : 0;
    st::print("拍平后与原文不同，首个差异偏移 {}\n期望 ...{}\n实际 ...{}\n", d,
                expected.substr(from, 240).c_str(),
                flattened->substr(from, 240).c_str());
  }
  ST_CHECK(*flattened == expected);

  // ② 再展开一次，关键字段仍在（防止"序列化相同但解析丢了"这类假通过）。
  const auto expanded = caiyun::expand_package(dir);
  ST_CHECK(expanded.has_value());
  if (!expanded) return;
  ST_CHECK(expanded->script.name == "包往返探针");
  ST_CHECK(expanded->script.fps > 23.9 && expanded->script.fps < 24.1);
  ST_CHECK(expanded->script.scenes.size() == 2);
  if (expanded->script.scenes.size() < 2) return;
  const caiyun::Scene& s0 = expanded->script.scenes[0];
  ST_CHECK(s0.layers.size() == 3);
  ST_CHECK(s0.carry.name == "承接");
  ST_CHECK(s0.carry.into == 1);
  ST_CHECK(s0.image == "assets/bg.png");
  // 资产哈希应当登记了两个真实存在的文件
  ST_CHECK(expanded->manifest.assets.size() == 2);

  // ③ 清单可读且版本正确
  const auto manifest = caiyun::read_manifest(dir);
  ST_CHECK(manifest.has_value());
  if (manifest) {
    ST_CHECK(manifest->schema_version == caiyun::kPackageSchemaVersion);
    ST_CHECK(manifest->project_id == "probe");
  }
}

ST_TEST(caiyun_package_create_refuses_to_clobber) {
  const std::string dir = fresh_dir("noclobber");
  const caiyun::Script script = sample_script();
  (void)st::fs::create_directories(st::fs::join(dir, "assets"));
  ST_CHECK(caiyun::create_package(dir, script, dir, "x", false).has_value());

  // 第二次不带 overwrite：必须失败。历史与意图是人的劳动，静默覆盖等于丢工作。
  const auto again = caiyun::create_package(dir, script, dir, "x", false);
  ST_CHECK(!again.has_value());

  // 带 overwrite 才允许，且应当**保留** intent.md
  (void)st::fs::write_text(st::fs::join(dir, "intent.md"), "# 我的意图\n");
  const auto forced = caiyun::create_package(dir, script, dir, "x", true);
  ST_CHECK(forced.has_value());
  const auto intent = st::fs::read_text(st::fs::join(dir, "intent.md"));
  ST_CHECK(intent.has_value());
  if (intent) ST_CHECK(!intent->empty());
}

ST_TEST(caiyun_package_asset_check_detects_drift) {
  const std::string dir = fresh_dir("assets");
  const caiyun::Script script = sample_script();
  (void)st::fs::create_directories(st::fs::join(dir, "assets"));
  (void)st::fs::write_text(st::fs::join(st::fs::join(dir, "assets"), "bg.png"), "v1");
  (void)st::fs::write_text(st::fs::join(st::fs::join(dir, "assets"), "mesh.obj"), "o m\n");
  ST_CHECK(caiyun::create_package(dir, script, dir, "a", false).has_value());

  const auto manifest = caiyun::read_manifest(dir);
  ST_CHECK(manifest.has_value());
  if (!manifest) return;

  // 初始应当干净
  const auto clean = caiyun::verify_assets(dir, *manifest);
  ST_CHECK(clean.ok());
  ST_CHECK(clean.modified.empty());

  // 改内容 → modified（这是"素材被换了却没人知道"的防线）
  (void)st::fs::write_text(st::fs::join(st::fs::join(dir, "assets"), "bg.png"), "v2-changed");
  const auto drifted = caiyun::verify_assets(dir, *manifest);
  ST_CHECK(!drifted.ok());
  ST_CHECK(drifted.modified.size() == 1);
  if (!drifted.modified.empty()) {
    st::print("被改动的资产：{}\n", drifted.modified[0]);
    // 登记的路径是**相对包根**的原写法（`assets/bg.png`），不是去前缀的 `bg.png`：
    // 它要能直接与脚本里的引用串对应上，否则人无法判断“哪个引用失效了”。
    ST_CHECK(drifted.modified[0] == "assets/bg.png");
  }

  // 删文件 → missing
  (void)st::fs::remove_file(st::fs::join(st::fs::join(dir, "assets"), "mesh.obj"));
  const auto lost = caiyun::verify_assets(dir, *manifest);
  ST_CHECK(!lost.missing.empty());
  if (!lost.missing.empty()) ST_CHECK(lost.missing[0] == "assets/mesh.obj");

  // 新增未登记文件 → untracked（不算错，只是提示）
  (void)st::fs::write_text(st::fs::join(st::fs::join(dir, "assets"), "extra.png"), "e");
  const auto extra = caiyun::verify_assets(dir, *manifest);
  ST_CHECK(!extra.untracked.empty());
}

ST_TEST(caiyun_package_history_is_append_only) {
  const std::string dir = fresh_dir("history");
  ST_CHECK(caiyun::create_package(dir, sample_script(), dir, "h", false).has_value());

  caiyun::HistoryEntry first{};
  first.action = "create";
  first.actor = "human";
  first.summary = "建包";
  const auto seq1 = caiyun::append_history(dir, first);
  ST_CHECK(seq1.has_value());
  if (!seq1) return;
  ST_CHECK(*seq1 == 1);

  caiyun::HistoryEntry second{};
  second.action = "edit";
  second.actor = "agent";
  second.summary = "改标题";
  second.detail = st::Json::object();
  second.detail["scene"] = 0;
  const auto seq2 = caiyun::append_history(dir, second);
  ST_CHECK(seq2.has_value());
  if (seq2) ST_CHECK(*seq2 == 2);  // 序号单调

  const auto all = caiyun::read_history(dir);
  ST_CHECK(all.has_value());
  if (!all) return;
  ST_CHECK(all->size() == 2);
  if (all->size() < 2) return;
  ST_CHECK((*all)[0].seq == 1);
  ST_CHECK((*all)[1].seq == 2);
  ST_CHECK((*all)[0].action == "create");
  ST_CHECK((*all)[1].actor == "agent");
  ST_CHECK((*all)[1].summary == "改标题");
  ST_CHECK(!(*all)[1].at.empty());
  // detail 要能结构化读回（这是审计的载体）
  ST_CHECK((*all)[1].detail.is_object());

  // 空历史不存在的包：读历史应当是空列表而不是错误
  const std::string empty_dir = fresh_dir("history_empty");
  ST_CHECK(caiyun::create_package(empty_dir, sample_script(), empty_dir, "e", false).has_value());
  const auto none = caiyun::read_history(empty_dir);
  ST_CHECK(none.has_value());
  if (none) ST_CHECK(none->empty());
}

ST_TEST(caiyun_package_rejects_future_schema) {
  const std::string dir = fresh_dir("schema");
  (void)st::fs::create_directories(dir);
  // 手工造一个"来自未来"的包
  (void)st::fs::write_text(
      st::fs::join(dir, "package.json"),
      R"({"schema_version": 9999, "project_id": "future", "scene": "scene.json"})");
  (void)st::fs::write_text(st::fs::join(dir, "scene.json"), R"({"scenes": []})");

  // 必须明确拒绝，而不是"尽力读"——半懂着读会让新字段静默消失。
  const auto manifest = caiyun::read_manifest(dir);
  ST_CHECK(!manifest.has_value());
  if (!manifest) {
    st::print("未来版本的报错：{}\n", manifest.error().message);
    ST_CHECK(manifest.error().message.find("9999") != std::string::npos);
  }

  // 普通目录（无 package.json）不是包
  ST_CHECK(!caiyun::is_package(fresh_dir("not_a_package")));
}
