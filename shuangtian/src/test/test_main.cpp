#include <string_view>

#include "st/test/test.hpp"

/// 测试可执行入口：`st_test [filter]`（filter 为用例名子串，缺省跑全部）。
auto main(int argc, char** argv) -> int {
  const std::string_view filter = argc > 1 ? std::string_view(argv[1]) : std::string_view{};
  return st::test::run_all(filter);
}
