/// 量 gbcode 的 `LanguageService::request_document_symbols` 是否真能拿到符号。
///
/// 存在理由：headless 下大纲恒空（`symbols_` 为空），而单独跑 clangd 用同样的
/// 请求能返回 10KB 结果——必须把这层拆开看：是握手没完成、请求没发出，还是响应没解析。
#include <chrono>
#include "st/core/fs.hpp"
#include <cstdio>
#include <string>
#include <thread>

#include "../examples/gbcode/lsp_bridge.hpp"

auto main(int argc, char** argv) -> int {
  const std::string path = argc > 1 ? argv[1] : "examples/gbcode/page_sidebar.cpp";
  gbcode::LanguageService service;
  int symbols_seen = 0;
  service.on_symbols = [&](std::int64_t id, const std::vector<st::lsp::DocumentSymbol>& symbols) {
    std::printf("  [回调] id=%lld 符号数=%zu\n", static_cast<long long>(id), symbols.size());
    symbols_seen = static_cast<int>(symbols.size());
    for (std::size_t i = 0; i < symbols.size() && i < 5; ++i) {
      std::printf("    - %s (kind=%lld depth=%zu)\n", symbols[i].name.c_str(),
                  static_cast<long long>(symbols[i].kind), symbols[i].depth);
    }
  };

  std::printf("① ensure_started(%s)…\n", path.c_str());
  if (!service.ensure_started(path, "cpp")) {
    std::printf("  ✗ 启动失败：%s\n", service.error().c_str());
    return 1;
  }
  std::printf("  启动请求已发出\n");

  // 等握手完成（`active()` = started && Ready）
  for (int i = 0; i < 100; ++i) {
    (void)service.pump();
    if (service.active()) {
      std::printf("② 握手完成（第 %d 次 pump，约 %dms）\n", i + 1, (i + 1) * 100);
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (!service.active()) {
    std::printf("✗ 握手 10s 未完成（active() 仍为 false）\n");
    return 1;
  }

  // ⚠ 必须先 `didOpen`（`sync_document`）再请求符号：clangd 对**没打开过的文件**
  // 回空数组（协议上 `documentSymbol` 只对已同步的文档有效）。
  // 这正是 gbcode 大纲恒空的真根因之一：调用顺序若颠倒就永远拿不到符号。
  std::printf("③ sync_document（didOpen）…\n");
  std::string text;
  if (const auto read = st::fs::read_text(path)) text = *read;
  std::printf("  文件 %zu 字节\n", text.size());
  service.sync_document(path, text);

  std::printf("④ 发送 documentSymbol 请求…\n");
  const auto id = service.request_document_symbols(path);
  std::printf("  请求 id=%lld\n", static_cast<long long>(id));
  for (int i = 0; i < 100; ++i) {
    (void)service.pump();
    if (symbols_seen > 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  std::printf("④ 结果：符号数=%d\n", symbols_seen);
  return symbols_seen > 0 ? 0 : 1;
}
