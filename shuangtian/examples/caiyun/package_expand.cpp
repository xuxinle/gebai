#include "package_expand.hpp"

#include <algorithm>
#include <ctime>
#include <format>
#include <set>

#include "st/core/fs.hpp"
#include "st/core/log.hpp"

namespace caiyun {

using st::ErrorCode;
namespace {

/// 场景层的键：把 `Script` 里与"片子"有关、但与"单镜"无关的字段写在这里。
///
/// 为何场景层要带上这些顶层字段：它们是**片子的身份**（尺寸/帧率/转场/品牌），
/// 单独放一个 `film.json` 会多一层无谓的间接。`scene.json` 因此是
/// "整份脚本减去渲染层与资产"——它仍然是完整的可运行脚本。
[[nodiscard]] auto node_now_iso8601() -> std::string {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  return std::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}Z", tm.tm_year + 1900,
                     tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/// 包里资产目录的绝对路径。
///
/// 单独抽出来是因为它被三处用（搬迁 / 建目录 / 算哈希），
/// 三处口径不一致就会出现“搬到了 A、哈希去看 B”这类静默空白。
[[nodiscard]] auto assets_dir_for(std::string_view root) -> std::string {
  return st::fs::join(std::string(root), "assets");
}

/// 从场景/图层里收集**素材引用**（贴图、网格），返回相对包根的路径。
///
/// ## 为何不能只看 `assets/` 前缀
///
/// 存量脚本的素材习惯用 `media/`（`media/bg-aurora.png`），而包内约定的
/// 资产目录是 `assets/`。两个前缀都存在，且**都可以是相对子目录**
///（`media/x.png`、`img/a/b.jpg`）。只看一个前缀就会把另一类全部漏掉——
/// 表现为“建包成功、素材目录是空的”，而画面在原作者机器上照常出图。
///
/// 判据改成“像个文件路径”：有扩展名，且不是程序层的东西（`.js` / `.json`）。
/// 宁可把个别非素材的引用算进来（后续 `verify_assets` 会报它不存在），
/// 也不要静默漏收集。
[[nodiscard]] auto is_asset_ref(const std::string& ref) -> bool {
  if (ref.empty()) return false;
  // 绝对路径 / URL 不搬：它们不随包走，搬进来反而会让“引用”与“实际”脱钩。
  if (st::fs::is_absolute(ref)) return false;
  if (ref.find("://") != std::string::npos) return false;
  const std::string leaf = st::fs::file_name(ref);
  const std::size_t dot = leaf.rfind('.');
  if (dot == std::string::npos || dot == 0) return false;
  const std::string ext = leaf.substr(dot);
  // 程序层与清单不是素材（`layout` 单独处理）
  if (ext == ".js" || ext == ".json" || ext == ".mjs") return false;
  return true;
}

[[nodiscard]] auto collect_asset_refs(const Script& script) -> std::vector<std::string> {
  std::set<std::string> refs;
  const auto note = [&](const std::string& ref) {
    if (is_asset_ref(ref)) refs.insert(ref);
  };
  for (const Scene& scene : script.scenes) {
    note(scene.image);
    note(scene.image_card);
    for (const Layer& layer : scene.layers) {
      note(layer.src);
      // `model_shape` 的网格引用写作 `obj:<路径>`。
      //
      // 前缀必须剥掉再当路径用：直接拿 `obj:assets/mesh.obj` 去拼路径会得到
      // 一个带冒号的文件名（Windows 上非法、POSIX 上找不到），
      // 表现为“网格模型没被收进包”，而画面在原作者机器上正常。
      constexpr std::string_view kObjPrefix = "obj:";
      const std::string& shape = layer.model_shape;
      if (shape.rfind(kObjPrefix, 0) == 0) {
        note(shape.substr(kObjPrefix.size()));
      }
      // `model_script` 里的网格引用（`{"shape": "obj:a/b.obj"}`）
      // 同样要收：三维场景脚本内的模型与单网格路径是两个独立入口，
      // 漏了任何一个都会出现“建包成功但模型不在”。
      if (!layer.model_script.empty()) {
        const auto parsed = st::json_parse(layer.model_script);
        if (parsed && parsed->is_object()) {
          const auto objs = parsed->find("objects");
          if (objs != parsed->end() && objs->is_array()) {
            for (const auto& obj : *objs) {
              if (!obj.is_object()) continue;
              const auto shape_it = obj.find("shape");
              if (shape_it == obj.end() || !shape_it->is_string()) continue;
              const std::string inner = shape_it->get<std::string>();
              if (inner.rfind(kObjPrefix, 0) == 0) note(inner.substr(kObjPrefix.size()));
            }
          }
        }
      }
    }
  }
  return {refs.begin(), refs.end()};
}

}  // namespace

auto expand_package(std::string_view root) -> Result<ExpandedPackage> {
  const auto manifest = read_manifest(root);
  if (!manifest) return st::forward_error(manifest.error());

  // 预设表：包内 `<root>/cameras/*.json` 与 `<root>/lighting/*.json` 覆盖内置。
  //
  // 必须在 `parse_script` **之前**装好：场景层里的 `preset` 引用
  // 在解析期就展开（见 director.cpp 的 model_script 分支），
  // 晚一步就会报“预设不存在”。
  set_render_presets_from_package(root);

  ExpandedPackage out{};
  out.manifest = *manifest;

  // 意图层：可选。读不到不算错（一个刚创建的包可能还没有意图）。
  if (!manifest->intent_path.empty()) {
    const std::string path = st::fs::join(std::string(root), manifest->intent_path);
    if (st::fs::exists(path)) {
      const auto text = st::fs::read_text(path);
      if (text) out.intent_text = *text;
    }
  }

  // 场景层：**必需**。它是可运行脚本的主体。
  if (manifest->scene_path.empty()) {
    return st::unexpected(ErrorCode::Invalid, "工程包未声明 scene 层（package.json 的 scene 为空）");
  }
  const std::string scene_path = st::fs::join(std::string(root), manifest->scene_path);
  const auto scene_text = st::fs::read_text(scene_path);
  if (!scene_text) {
    return st::unexpected(ErrorCode::NotFound,
                          "工程包缺少场景层：" + scene_path);
  }
  out.scene_text = *scene_text;

  // `asset_basis` 传包根：包里所有相对引用（`assets/x.obj`、`program/x.js`）
  // 都相对包根，而不是相对 `scene.json` 所在目录——因为场景层就在包根，
  // 两者此时恰好相同，但用包根表达意图更清楚（将来场景层可以挪进子目录）。
  const auto script = parse_script(*scene_text, std::string(root));
  if (!script) return st::forward_error(script.error());
  out.script = *script;

  // 程序层：把目录下的文本文件全部读出来（工具侧要能列出、能改）。
  if (!manifest->program_dir.empty()) {
    const std::string dir = st::fs::join(std::string(root), manifest->program_dir);
    if (st::fs::is_directory(dir)) {
      const auto entries = st::fs::list_dir(dir);
      if (entries) {
        for (const auto& entry : *entries) {
          if (entry.is_dir) continue;
          const auto text = st::fs::read_text(entry.path);
          if (!text) continue;
          out.program_files.emplace_back(entry.name, *text);
        }
        std::sort(out.program_files.begin(), out.program_files.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
      }
    }
  }
  // ## 两件“包一打开就该知道”的事
  //
  // ① 意图偏差：和当初说好的不一样（时长/画幅/必现元素）
  // ② 资产漂移：素材被换了或丢了
  //
  // 都不是**错误**（包仍然可用、仍然能出片），所以不阻断展开，
  // 只把结果交给调用方。
  if (!out.intent_text.empty()) {
    const auto spec = parse_intent(out.intent_text);
    if (spec) {
      out.intent_deviations = check_intent(*spec, out.script);
    } else {
      // 意图块存在但解析失败 = 作者写了约束却没生效，必须让人知道。
      // 用一条 must_show 形态的偏差承载不适配——这里单独打日志。
      ST_LOG_WARN("工程包意图层解析失败：{}", spec.error().message);
    }
  }
  out.asset_check = verify_assets(root, *manifest);

  return out;
}

auto create_package(std::string_view root, const Script& script, std::string_view source_basis,
                    std::string_view project_id, bool overwrite_existing) -> st::Status {
  const std::string dir{root};
  if (st::fs::exists(dir) && !overwrite_existing) {
    if (is_package(dir)) {
      return st::unexpected(ErrorCode::Exists,
                            "工程包已存在：" + dir + "（覆盖会丢掉历史与意图层）");
    }
  }
  if (const auto made = st::fs::create_directories(dir); !made) return made;

  // ## 把程序层文件搬到**包根的同一个相对位置**
  //
  // 关键约束：场景层里 `layout` 的字符串是**存量脚本的一部分**，不能改。
  // 展开时它按包根解析，所以文件必须就在包根的那个相对位置——
  // 而不是收进 `program/`（那样要在序列化侧改写引用，改一字就换一个语义）。
  //
  // 实测代价：第一版收进了 `program/`，于是 32 份存量脚本里有 22 份拍平后
  // 丢掉了 `layout` 那一行——整幕编排动效静默降级为纯 JSON。
  for (const Scene& scene : script.scenes) {
    if (scene.layout.empty() || !scene.layout.from_file) continue;
    const std::string name = scene.layout.raw_field;
    if (name.empty()) continue;
    const std::string dest = st::fs::join(dir, name);
    (void)st::fs::create_directories(st::fs::parent(dest));
    if (st::fs::exists(dest)) continue;  // 已搬过（同一 JS 被多幕引用）
    const std::string src = st::fs::join(std::string(source_basis), name);
    if (!st::fs::exists(src)) continue;  // 源里没有：交给后续验证报缺
    if (const auto copied = st::fs::copy_file(src, dest); !copied) return copied;
  }

  // ## 素材同样要搬
  //
  // 场景里的 `image` / `model_shape: obj:...` 都是相对编写时的目录。
  // 不搬的话包在别的机器上打开就是“素材缺失占位框”。
  //
  // 拷到包根的**同一相对位置**（`media/x.png` 还是 `media/x.png`）：
  // 引用字符串是存量脚本的一部分，改它就等于改脚本语义。
  // 包要做的是“把那个路径变成真的”，不是重新组织作者的目录结构。
  const std::vector<std::string> refs = collect_asset_refs(script);
  for (const std::string& rel : refs) {
    const std::string src = st::fs::join(std::string(source_basis), rel);
    if (!st::fs::exists(src)) continue;  // 源里没有就不搬（后续 verify 会报 missing）
    const std::string dest = st::fs::join(dir, rel);
    // 源与目标是同一个文件（“就地建包”，或 source_basis 就是包目录本身）
    // 时不能 copy：`copy_file` 在目标已存在时会拒绝，而这不是错误。
    if (src == dest) continue;
    if (st::fs::exists(dest)) continue;  // 已搬过（多幕引用同一素材）
    (void)st::fs::create_directories(st::fs::parent(dest));
    if (const auto copied = st::fs::copy_file(src, dest); !copied) return copied;
  }

  // 场景层 = 整份脚本（含片子身份字段）。这是"运行视图"的直接落盘。
  // 放在文件搬完之后写：这样万一搬迁失败，包不会处于“看起来完整”的状态。
  //
  // ## 写盘前先把预设展开
  //
  // 包必须**自包含**：`scene.json` 里不能残留 `preset: "studio"` 这种引用，
  // 否则把包拷到没有 `lighting/studio.json` 的机器上，相机/光照会静默退回默认值。
  //
  // 代价是“改了预设，已有包不会自动跟随”——但那是正确的：
  // 一个已经交片的工程不应该因为别人调了预设而自己变了。要跟随就重跑 `--pack`。
  Script expanded = script;
  if (const auto applied = expand_script_presets(expanded); !applied) {
    return st::forward_error(applied.error());
  }
  const std::string scene_text = script_to_json(expanded);
  if (const auto wrote = st::fs::write_text(st::fs::join(dir, "scene.json"), scene_text);
      !wrote) {
    return wrote;
  }

  // 资产管理目录（空目录也要建：它是约定的一部分，不是"有文件才有"）
  const std::string assets_dir = assets_dir_for(dir);
  if (const auto made = st::fs::create_directories(assets_dir); !made) return made;

  // 历史层：建目录但**不写任何条目**——`create` 这一条由调用方决定要不要记
  //（有的场景是"另存为"，不该在历史里出现一次创建）。
  if (const auto made = st::fs::create_directories(st::fs::join(dir, "history")); !made) {
    return made;
  }

  PackageManifest manifest{};
  manifest.project_id = project_id.empty() ? script.name : std::string(project_id);
  manifest.created_at = node_now_iso8601();
  manifest.scene_path = "scene.json";
  // 意图层：无则生成骨架（带约束块），有则**不动**。
  //
  // 为何新包要生成骨架而不是留空：没有骨架时，作者不知道约束块该怎么写，
  // 于是永远不会写；而意图层不写等于这个层不存在。骨架里把已能从脚本
  // 推出来的值（时长/画幅）先填上，作者只需改不对的那几项。
  const std::string intent_file = st::fs::join(dir, "intent.md");
  if (!st::fs::exists(intent_file)) {
    IntentSpec seed{};
    double total = 0.0;
    for (const Scene& scene : expanded.scenes) total += static_cast<double>(scene.duration);
    seed.duration_s = total;
    seed.size = std::pair<int, int>{expanded.width, expanded.height};
    const std::string body = std::format(
        "# 意图\n\n"
        "<!-- 写清楚：给谁看、看完记住什么、有意识舍弃了什么。 -->\n\n"
        "## 目标\n\n（一句话）\n\n"
        "## 验收标准\n\n"
        "- 时长与画幅见下方约束块\n"
        "- 必现内容：把“观众应当记住的东西”逐条写进 `must_show`\n\n"
        "## 有意识舍弃的\n\n（写下来，否则下个人会把它当缺陷补回去）\n\n"
        "{}",
        render_intent_block(seed));
    if (const auto wrote = st::fs::write_text(intent_file, body); !wrote) return wrote;
  }
  manifest.intent_path = "intent.md";
  manifest.render_path = "";  // 见头文件「关于 render.json 的诚实说明」
  manifest.program_dir = "";
  manifest.assets_dir = "assets";
  manifest.history_dir = "history";

  // 资产清单：只登记**场景里真的引用了、且磁盘上存在**的文件。
  //
  // 基准是**包根**（引用路径本身已相对包根，`media/x.png` / `assets/y.obj`）。
  // 与前面搬迁用同一口径：搬到哪里就以哪里为基准算哈希。
  manifest.assets = collect_asset_hashes(dir, refs);

  return write_manifest(dir, manifest);
}

auto flatten_to_json(std::string_view root) -> Result<std::string> {
  const auto expanded = expand_package(root);
  if (!expanded) return st::forward_error(expanded.error());

  // ## 拍平必须**自包含**，所以 JS 编排层要内联
  //
  // `layout` 的两种写法：文件路径、内联源码。包里存的是文件（便于 git diff），
  // 而拍平出来的单文件脚本一旦离开包目录，那个路径就解析不到了——
  // 表现是 `layout 字段存在但解析失败`、**静默降级为纯 JSON**（画面照出，只是没动效）。
  //
  // 实测代价：迁移七套模板后从包拍平再渲染，282 帧里只有 43 帧与原脚本相同。
  // 脚本内容逐字节相同，差异全在“JS 没被加载”。
  //
  // 所以：拍平时把文件内容内联进 `LayoutScript::raw_field`，并把 `from_file` 置假。
  // 代价是导出的 JSON 会变大（一份 JS 几百行）——那是“自包含”的必要代价。
  Script flat = expanded->script;
  for (Scene& scene : flat.scenes) {
    if (scene.layout.empty() || !scene.layout.from_file) continue;
    // `expand_package` 已把文件内容读进 `source`（按包根解析）。
    scene.layout.raw_field = scene.layout.source;
    scene.layout.from_file = false;
    scene.layout.filename = "<inlined>";
  }
  return script_to_json(flat);
}

}  // namespace caiyun
