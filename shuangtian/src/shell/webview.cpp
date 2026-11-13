#include "st/shell/webview.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <utility>

#include "st/core/fs.hpp"

namespace st::shell::webview {
namespace {

auto lower_ascii(std::string_view text) -> std::string {
  std::string out;
  out.reserve(text.size());
  for (const char ch : text) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return out;
}

}  // namespace

auto WebAssets::normalize(std::string_view path) -> std::string {
  // 丢弃查询串与锚点：`/a.css?v=1` 与 `/a.css` 必须是同一条。
  //
  // 为什么这件事重要：前端构建产物普遍给静态资源挂 `?v=<hash>`，若查表把
  // `?v=...` 也算进键，**内嵌包的每一个带版本号的资源都会 404**——
  // 而症状是"样式没生效、模块加载失败"，看上去像 MIME 或路径配错。
  const std::size_t cut = path.find_first_of("?#");
  std::string_view trimmed = cut == std::string_view::npos ? path : path.substr(0, cut);
  if (trimmed.empty()) return "/";
  if (trimmed.front() != '/') {
    std::string out;
    out.reserve(trimmed.size() + 1);
    out.push_back('/');
    out.append(trimmed);
    return out;
  }
  return std::string(trimmed);
}

auto WebAssets::guess_mime(std::string_view path) -> std::string {
  const std::string normalized = normalize(path);
  const std::size_t dot = normalized.rfind('.');
  if (dot == std::string::npos) return "application/octet-stream";
  const std::string ext = lower_ascii(normalized.substr(dot + 1));
  if (ext == "html" || ext == "htm") return "text/html";
  if (ext == "css") return "text/css";
  // `application/javascript` 是 ES module 能被接受的那一个；`text/javascript` 也可以，
  // 但两者都不等于 `text/plain`——后者会让 `<script type=module>` 直接被拒。
  if (ext == "js" || ext == "mjs") return "text/javascript";
  if (ext == "json") return "application/json";
  if (ext == "wasm") return "application/wasm";
  if (ext == "svg") return "image/svg+xml";
  if (ext == "png") return "image/png";
  if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
  if (ext == "gif") return "image/gif";
  if (ext == "webp") return "image/webp";
  if (ext == "ico") return "image/x-icon";
  if (ext == "woff") return "font/woff";
  if (ext == "woff2") return "font/woff2";
  if (ext == "ttf") return "font/ttf";
  if (ext == "otf") return "font/otf";
  if (ext == "map") return "application/json";
  if (ext == "txt") return "text/plain";
  if (ext == "xml") return "application/xml";
  return "application/octet-stream";
}

void WebAssets::add(Asset asset) {
  asset.path = normalize(asset.path);
  if (asset.mime.empty()) asset.mime = guess_mime(asset.path);
  entries_[asset.path] = std::move(asset);
}

void WebAssets::add_text(std::string path, std::string mime, std::string_view body) {
  Asset asset;
  asset.path = std::move(path);
  asset.mime = std::move(mime);
  asset.bytes.assign(body.begin(), body.end());
  add(std::move(asset));
}

auto WebAssets::add_file(std::string path, std::string mime, const std::string& disk_path) -> Status {
  auto bytes = fs::read_bytes(disk_path);
  if (!bytes) {
    return unexpected(ErrorCode::NotFound,
                      std::format("读不到内嵌资源源文件: {}", disk_path));
  }
  Asset asset;
  asset.path = std::move(path);
  asset.mime = std::move(mime);
  asset.bytes = std::move(*bytes);
  add(std::move(asset));
  return ok();
}

auto WebAssets::find(std::string_view path) const -> const Asset* {
  const auto found = entries_.find(normalize(path));
  if (found == entries_.end()) return nullptr;
  return &found->second;
}

auto WebAssets::paths() const -> std::vector<std::string> {
  std::vector<std::string> out;
  out.reserve(entries_.size());
  for (const auto& [path, asset] : entries_) {
    (void)asset;
    out.push_back(path);
  }
  return out;
}

}  // namespace st::shell::webview
