/// 验证：`OffscreenTextPort` 与主线程端口的宽度缓存是否真的隔离。
///
/// 判据：用主端口量一批词（填热缓存）→ 用离屏端口量**另一批大量**词（模拟后台排版）
/// → 再用主端口量第一批词，耗时应几乎不变（缓存仍热）。
/// 若两者共享缓存，第二批会把第一批挤掉，第三段明显变慢。
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "st/app/text_port.hpp"
#include "st/text/text.hpp"

using Clock = std::chrono::steady_clock;

auto main() -> int {
  const auto fonts = st::text::FontStack::system_default();
  if (!fonts) {
    std::printf("SKIP（无系统字体）\n");
    return 0;
  }
  st::text::TextRenderer main_renderer{*fonts, 1.0f};
  auto main_port = st::app::make_text_port(main_renderer);
  st::app::OffscreenTextPort offscreen{*fonts, 1.0f};

  std::vector<std::string> hot;      // 主线程"正在用"的词
  for (int i = 0; i < 500; ++i) hot.push_back("hot 热词 " + std::to_string(i));
  std::vector<std::string> bulk;     // 后台排版要处理的大量词
  for (int i = 0; i < 20000; ++i) bulk.push_back("bulk 批量词 " + std::to_string(i));

  const auto measure_all = [](const st::ui::TextPort& port, const std::vector<std::string>& words) {
    const auto start = Clock::now();
    double sink = 0.0;
    for (const auto& word : words) sink += static_cast<double>(port.measure_width(word, 14.0f));
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    return std::pair{ms, sink};
  };

  // ① 预热主端口
  (void)measure_all(*main_port, hot);
  const auto [baseline_ms, s1] = measure_all(*main_port, hot);

  // ② 离屏端口灌大量（模拟后台排版）——不应影响主端口
  (void)measure_all(offscreen, bulk);

  // ③ 主端口热态复测
  const auto [after_ms, s2] = measure_all(*main_port, hot);

  std::printf("主端口热态复测：灌入前 %.2fms → 灌入后 %.2fms（%.2f×）\n", baseline_ms, after_ms,
              baseline_ms > 0.001 ? after_ms / baseline_ms : 0.0);
  const bool isolated = after_ms < baseline_ms * 2.0 + 0.5;
  std::printf("离屏端口缓存隔离：%s\n", isolated ? "PASS（主端口缓存未被刷）" : "FAIL（仍共享缓存）");
  (void)s1;
  (void)s2;
  return isolated ? 0 : 1;
}
