#include "package.hpp"

#include <algorithm>
#include <cstdio>
#include <format>
#include <utility>

#include "st/core/fs.hpp"
#include "st/core/hash.hpp"

namespace caiyun {
namespace {

using st::ErrorCode;
/// 资产清单的 JSON 形状：`[{"path": "...", "sha256": "..."}]`。
///
/// 为何用数组而不是对象：顺序稳定（按路径排序后写入），
/// 人工 diff 时不会因为键序抖动产生噪音。
[[nodiscard]] auto assets_to_json(
    const std::vector<std::pair<std::string, std::string>>& assets) -> st::Json {
  st::Json arr = st::Json::array();
  auto sorted = assets;
  std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  for (const auto& [path, digest] : sorted) {
    st::Json node = st::Json::object();
    node["path"] = path;
    node["sha256"] = digest;
    arr.push_back(std::move(node));
  }
  return arr;
}

}  // namespace

auto PackageLocation::manifest_path() const -> std::string {
  return st::fs::join(root, "package.json");
}

auto is_package(std::string_view path) -> bool {
  if (path.empty()) return false;
  if (!st::fs::is_directory(path)) return false;
  const std::string manifest = st::fs::join(path, "package.json");
  if (!st::fs::exists(manifest)) return false;
  // 有 package.json 还不够：必须能被识别为**本格式**的包（版本字段存在）。
  // 否则一个恰好有 package.json 的任意目录会被当成工程包，报错会离真因很远。
  const auto text = st::fs::read_text(manifest);
  if (!text) return false;
  const auto json = st::json_parse(*text);
  if (!json) return false;
  return st::json_find(*json, "schema_version") != nullptr;
}

auto read_manifest(std::string_view root) -> Result<PackageManifest> {
  const std::string manifest = st::fs::join(root, "package.json");
  const auto text = st::fs::read_text(manifest);
  if (!text) {
    return st::unexpected(ErrorCode::NotFound, "不是工程包：缺少 package.json（" + manifest + "）");
  }
  const auto json = st::json_parse(*text);
  if (!json) {
    return st::unexpected(ErrorCode::Parse, "package.json 不是合法 JSON：" + manifest);
  }
  if (!json->is_object()) {
    return st::unexpected(ErrorCode::Parse, "package.json 顶层需要对象");
  }

  PackageManifest out{};
  out.schema_version = static_cast<int>(st::json_get_double(*json, "schema_version", 0.0));
  if (out.schema_version <= 0) {
    return st::unexpected(ErrorCode::Parse, "package.json 缺少 schema_version（无法判定格式版本）");
  }
  if (out.schema_version > kPackageSchemaVersion) {
    // 未来版本：明确拒绝而不是"尽力读"。半懂着读会让新字段静默消失。
    return st::unexpected(
        ErrorCode::Unsupported,
        std::format("工程包 schema 版本 {} 高于本程序支持的 {}——请升级裁云",
                    out.schema_version, kPackageSchemaVersion));
  }

  out.project_id = st::json_get_string(*json, "project_id", "");
  out.created_at = st::json_get_string(*json, "created_at", "");

  // 各层路径：允许缺省（缺省则用默认值），但**显式给空串**表示"这层不存在"。
  const auto read_path = [&](std::string_view key, std::string fallback) -> std::string {
    if (st::json_find(*json, key) == nullptr) return fallback;
    return st::json_get_string(*json, key, "");
  };
  out.scene_path = read_path("scene", "scene.json");
  out.intent_path = read_path("intent", "");
  out.render_path = read_path("render", "");
  out.program_dir = read_path("program", "");
  out.assets_dir = read_path("assets", "assets");
  out.history_dir = read_path("history", "history");

  if (const st::Json* assets = st::json_find(*json, "asset_index"); assets != nullptr) {
    if (!assets->is_array()) {
      return st::unexpected(ErrorCode::Parse, "package.json 的 asset_index 需要数组");
    }
    for (const auto& node : *assets) {
      if (!node.is_object()) continue;
      const std::string path = st::json_get_string(node, "path", "");
      const std::string digest = st::json_get_string(node, "sha256", "");
      if (!path.empty()) out.assets.emplace_back(path, digest);
    }
  }
  return out;
}

auto write_manifest(std::string_view root, const PackageManifest& manifest) -> Status {
  const std::string dir{root};
  if (const auto made = st::fs::create_directories(dir); !made) return made;

  st::Json json = st::Json::object();
  // 键序固定（人工 diff 友好）：版本与身份在前，层索引居中，资产清单在后。
  json["schema_version"] = manifest.schema_version;
  json["project_id"] = manifest.project_id;
  json["created_at"] = manifest.created_at;
  json["scene"] = manifest.scene_path;
  json["intent"] = manifest.intent_path;
  json["render"] = manifest.render_path;
  json["program"] = manifest.program_dir;
  json["assets"] = manifest.assets_dir;
  json["history"] = manifest.history_dir;
  json["asset_index"] = assets_to_json(manifest.assets);

  const std::string text = json.dump(2) + "\n";
  return st::fs::write_text(st::fs::join(dir, "package.json"), text);
}

auto file_digest(std::string_view path) -> std::string {
  const auto digest = st::hash::sha256_file(path);
  return digest.has_value() ? *digest : std::string{};
}

auto collect_asset_hashes(std::string_view root, const std::vector<std::string>& relative_paths)
    -> std::vector<std::pair<std::string, std::string>> {
  std::vector<std::pair<std::string, std::string>> out;
  out.reserve(relative_paths.size());
  for (const std::string& rel : relative_paths) {
    // 路径基准：`root` 已经是资产目录本身（调用方传 `<包根>/assets`）。
    //
    // 这里曾经多拼了一层：调用方给的 rel 已去掉 `assets/` 前缀，
    // 而本函数又按包根拼一次，于是永远找不到文件——表现为
    // `asset_index` **静默为空**（不是报错），而 `verify_assets`
    // 又因为“没登记”把一切当成干净。两个缺陷互相遮蔽，最难查。
    const std::string abs = st::fs::join(std::string(root), rel);
    const std::string digest = file_digest(abs);
    // 读不到的文件不登记：登记一个空哈希会让后续“校验”永远失败，
    // 而真正的问题（文件不存在）应当由 verify_assets 的 missing 报告。
    if (!digest.empty()) out.emplace_back(rel, digest);
  }
  return out;
}

auto verify_assets(std::string_view root, const PackageManifest& manifest) -> AssetCheck {
  AssetCheck check{};

  // ## 基准是**包根**，不是清单里的 `assets_dir`
  //
  // 清单登记的相对路径已经是**相对包根**的（`media/x.png`、`assets/y.obj`），
  // 因为它们就是脚本里的原写法。再拼一层 `assets_dir` 会变成
  // `assets/media/x.png`——找不到，于是全部报缺失。
  //
  // 实测代价：P1 的测试样本恰好把素材放在 `assets/` 下，两套口径碰巧
  // 重合，于是这个缺陷没被逮到；换成真实的 `media/` 存量脚本才暴露。
  std::vector<std::string> declared;
  declared.reserve(manifest.assets.size());
  for (const auto& [rel, expected] : manifest.assets) {
    declared.push_back(rel);
    const std::string abs = st::fs::join(std::string(root), rel);
    if (!st::fs::exists(abs)) {
      check.missing.push_back(rel);
      continue;
    }
    const std::string actual = file_digest(abs);
    if (actual.empty()) continue;  // 读失败：既不算缺失也不算改动（应由调用方另报）
    if (!expected.empty() && actual != expected) check.modified.push_back(rel);
  }

  // 磁盘上有但没登记的（新增素材）：扫**资产目录**（清单声明的那个）。
  //
  // 只扫一层（不递归）：资产目录可能是 `assets/` 也可能是 `media/`，
  // 而登记路径没准在子目录里——递归比对会因“相对谁”而歧义。
  // 未登记本来就只是提示，浅扫足够。
  const std::string assets_root =
      manifest.assets_dir.empty() ? std::string(root)
                                  : st::fs::join(std::string(root), manifest.assets_dir);
  if (st::fs::is_directory(assets_root)) {
    const auto entries = st::fs::list_dir(assets_root);
    if (entries) {
      for (const auto& entry : *entries) {
        if (entry.is_dir) continue;
        const std::string rel = st::fs::join(manifest.assets_dir, entry.name);
        const bool known = std::any_of(declared.begin(), declared.end(),
                                       [&](const std::string& d) {
                                         return d == entry.name || d == rel;
                                       });
        if (!known) check.untracked.push_back(entry.name);
      }
    }
  }
  return check;
}

}  // namespace caiyun
