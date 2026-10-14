/// 内存预算单点（`st::memory`）的契约测试。
///
/// 背景（2026-10-14 内存评估）：框架里一组自设上限的缓存原先各写各的魔数
/// （字形 24 MiB / 整形 16 MiB 在 `text.cpp`、SVG 16 MiB 在 `svg.hpp`），
/// 于是"这个进程最多为缓存吃多少"要翻三个文件；而 CI / 低配想收缩只能改代码。
/// 收敛到 `st::memory` 后多了个声明式档位 `ST_MEM_BUDGET=low`。
///
/// 判据选择：**不测绝对字节数**（那会在调整默认值时变成"改代码就得改用例"的噪音），
/// 只测**关系**——低档必须严格小于常态档、且不低于地板。关系是契约，数字是策略。

#include "st/core/memory_budget.hpp"
#include "st/test/test.hpp"

#include <cstdlib>

namespace {

/// 子进程式验证不可行（档位是**进程内一次性快照**），因此这里只验证：
/// ① 当前进程的档位是一个**已知值**；② 常态档下上限等于历史默认（逐位不回归）；
/// ③ 缩放函数的地板与比例关系。
///
/// `low` 档的端到端验证放在可执行探针侧（改环境变量后重启进程），
/// 见 `docs/BACKLOG.md` 的登记与 `tools/mem_sample.py` 的用法说明。

}  // namespace

ST_TEST(memory_budget_defaults_match_historical_limits) {
  // 常态档必须与改造前**逐位一致**——否则这次"集中化"就顺手改了行为。
  if (st::memory::budget() == st::memory::Budget::Low) return;   // 显式设了 low 的环境跳过
  ST_CHECK_EQ(st::memory::budget_name(), std::string("normal"));
  ST_CHECK_EQ(st::memory::glyph_cache_bytes(), static_cast<std::size_t>(24U * 1024U * 1024U));
  ST_CHECK_EQ(st::memory::shaped_cache_bytes(), static_cast<std::size_t>(16U * 1024U * 1024U));
  ST_CHECK_EQ(st::memory::svg_cache_bytes(), static_cast<std::size_t>(16U * 1024U * 1024U));
  ST_CHECK_EQ(st::memory::scale_entry_limit(32768U), static_cast<std::size_t>(32768U));
}

ST_TEST(memory_budget_scale_never_goes_below_floor) {
  // `scale_cache_limit` 的地板（1 MiB）：低档下把上限压到 1 MiB 以下会让正常界面
  // 每帧重新光栅化——"省内存"换来"CPU 打满"，比多占几 MiB 更糟。
  //
  // ⚠ 地板只在**低档**生效：常态档的契约是"逐位等于历史默认值"（见上一条用例），
  // 所以这里必须按档位分开断言——第一版写成了不分档的 "tiny >= 1 MiB"，
  // 在常态档下当场变红（`scale_cache_limit(64 KiB)` 原样返回 64 KiB）。
  constexpr std::size_t kFloor = 1U * 1024U * 1024U;
  constexpr std::size_t kNormalGlyph = 24U * 1024U * 1024U;
  const std::size_t normal = st::memory::scale_cache_limit(kNormalGlyph);
  if (st::memory::budget() == st::memory::Budget::Low) {
    // 低档：必须**真的**小于常态档，且不低于地板
    ST_CHECK(normal < kNormalGlyph);
    ST_CHECK(normal >= kFloor);
    ST_CHECK(st::memory::scale_cache_limit(64U * 1024U) >= kFloor);
  } else {
    // 常态：原样返回（不高也不低）
    ST_CHECK_EQ(normal, kNormalGlyph);
    ST_CHECK_EQ(st::memory::scale_cache_limit(64U * 1024U), static_cast<std::size_t>(64U * 1024U));
  }
}

ST_TEST(memory_budget_entry_limit_keeps_at_least_one) {
  // 条目数上限同样按档压，但**至少 1**（0 会让缓存完全失效，那是缺陷不是省内存）
  ST_CHECK(st::memory::scale_entry_limit(0U) >= 1U);
  ST_CHECK(st::memory::scale_entry_limit(4U) >= 1U);
  ST_CHECK(st::memory::scale_entry_limit(256U) >= 1U);
}
