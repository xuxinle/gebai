#include <cstdio>
#include "st/core/json.hpp"
#include "st/lsp/navigation.hpp"
auto main() -> int {
  // clangd 真实返回的形态（SymbolInformation 扁平数组）
  const char* raw = R"([{"containerName":"","kind":3,"location":{"range":{"end":{"character":1,"line":937},"start":{"character":0,"line":15}},"uri":"file:///x.cpp"},"name":"gbcode_ui"},{"containerName":"gbcode_ui","kind":13,"location":{"range":{"end":{"character":50,"line":20},"start":{"character":2,"line":20}},"uri":"file:///x.cpp"},"name":"kTextInset"}])";
  const auto parsed = st::Json::parse(raw);
  const auto symbols = st::lsp::parse_document_symbols(parsed);
  std::printf("解析出符号数 = %zu\n", symbols.size());
  for (const auto& s : symbols) {
    std::printf("  %s kind=%lld line=%zu depth=%zu\n", s.name.c_str(),
                static_cast<long long>(s.kind), s.range.start.line, s.depth);
  }
  return symbols.empty() ? 1 : 0;
}
