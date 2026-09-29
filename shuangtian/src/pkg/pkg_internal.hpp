#pragma once

/// 内部接口（不对外，pkg 层共用；风格同 `src/raster/rasterize_internal.hpp`）：
/// URL/路径小工具、HTTP 客户端入口、指纹稳定的目录复制。
/// 说明：`http_get` 的实现集中在 `src/pkg/platform_http.cpp`（系统 socket 边界），
/// 本文件只声明，不引入系统头。

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "st/core/error.hpp"

namespace st::pkg::detail {

/// HTTP 客户端选项（超时与响应体上限：防永久阻塞与内存失控）。
struct HttpOptions {
  int connect_timeout_ms{5000};
  int io_timeout_ms{20000};
  std::uint64_t max_body_bytes{512ULL * 1024ULL * 1024ULL};  ///< 默认 512 MiB
  int max_redirects{5};
};

struct HttpResponse {
  int status{0};
  std::vector<std::uint8_t> body{};
};

/// 明文 `http://` GET（跟随重定向；分块传输解码；超时与体积上限）。
/// 错误：`Unsupported`（非 http:// 或 https://）、`Invalid`（URL 非法）、`Io`（连接/收发/状态码异常）、
///       `Timeout`、`Overflow`（响应体超上限）。
[[nodiscard]] auto http_get(std::string_view url, const HttpOptions& options) -> Result<HttpResponse>;

[[nodiscard]] inline auto has_prefix(std::string_view text, std::string_view prefix) -> bool {
  return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

/// `file://` 前缀剥离（`file:///abs` → `/abs`，`file://localhost/abs` → `/abs`）；无前缀原样返回。
[[nodiscard]] inline auto strip_file_scheme(std::string_view location) -> std::string {
  if (!has_prefix(location, "file://")) return std::string(location);
  std::string_view rest = location.substr(7);
  if (has_prefix(rest, "localhost/")) rest = rest.substr(9);
  return std::string(rest);
}

/// 是否明文 http://（唯一支持的远端协议）。
[[nodiscard]] inline auto is_http(std::string_view location) -> bool {
  return has_prefix(location, "http://");
}

/// HTTPS 一律拒绝并给出替代方案（TLS 需运行时 dlopen，本期未接入）。
[[nodiscard]] inline auto reject_tls(std::string_view location) -> Status {
  if (has_prefix(location, "https://")) {
    return unexpected(ErrorCode::Unsupported,
                      std::string("HTTPS 需系统 TLS 支持（运行时 dlopen，本期未接入），"
                                  "请改用 http:// 或本地路径：")
                          .append(location));
  }
  return ok();
}

/// 包名是否可安全用作目录名（`vendor/<name>`、缓存子目录）：非空、无路径分隔符、非 `.`/`..`。
[[nodiscard]] inline auto is_safe_package_name(std::string_view name) -> bool {
  if (name.empty() || name == "." || name == "..") return false;
  for (const char character : name) {
    if (character == '/' || character == '\\' || character == ':' || character == '\0') return false;
  }
  return true;
}

/// 相对地址解析：绝对路径原样返回；否则相对 `base`（`base` 为空则相对当前目录）。
[[nodiscard]] auto resolve_location(std::string_view location, std::string_view base) -> std::string;

/// 递归复制目录树并**保留文件 mtime**（使 `st::hash::tree_fingerprint` 跨副本稳定，
/// 缓存/vendor 固化后仍可校验同一 sha256）。错误：`NotFound`/`Io`。
[[nodiscard]] auto copy_tree_stable(std::string_view from, std::string_view to) -> Status;

}  // namespace st::pkg::detail
