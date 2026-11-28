/// 默认字体取向的契约测试。
///
/// 需求来源（用户）：**UI 字体内置 Inter、中文首选系统 Noto Sans（没有则回退平台主力字体）、
/// 代码用内置 Fira Code**。
///
/// 这条约定是**分层**的，每层各管一件事：
/// ① 拉丁 → 内置 Inter（全平台同一字形；标签/按钮/数字是界面里占比最大的字）；
/// ② 汉字 → 系统 Noto Sans（首选）→ 平台主力字体（雅黑/苹方，回退）；
/// ③ 等宽 → 内置 Fira Code（字形比 Consolas 更开，同字号下不那么“小”）；
/// ④ 粗体链与常规链**逐位同族**（下标配对，否则拉丁粗体会接管中文字）。
///
/// 为何中英文**不再**强求“同一个字族”（旧契约是雅黑）：同时覆盖两者的字族必然
/// 在某一侧妥协，而 `find_face` 本来就被码点逐字查找——分层不需要任何额外排版逻辑。
/// 真正必须成立的替代约束是“汉字标点与汉字同族”（见 `layers_latin_and_han`）。

#include "st/core/fs.hpp"
#include "st/core/font_platform.hpp"
#include "st/test/test.hpp"

#include <algorithm>
#include <atomic>
#include <format>
#include <memory>
#include <string>
#include <thread>

#include "st/core/font_platform.hpp"
#include "st/text/font.hpp"
#include "st/text/text.hpp"

namespace {

using st::text::FontRole;
using st::text::FontStack;

struct Fixture {
  std::unique_ptr<FontStack> stack{};
  bool ok{false};

  Fixture() {
    if (auto loaded = FontStack::system_default(); loaded.has_value()) {
      stack = std::make_unique<FontStack>(std::move(*loaded));
      ok = true;
    }
  }
};

/// emoji 字体面的文件名（空 = 该平台没有可用的 emoji 字体）。
[[nodiscard]] auto emoji_face_file() -> const char* {
#if defined(_WIN32)
  return "seguiemj.ttf";
#elif defined(__APPLE__)
  return "Apple Color Emoji.ttc";
#else
  return "NotoColorEmoji.ttf";
#endif
}

}  // namespace

ST_TEST(font_stack_load_is_cached_across_calls) {
  // **缓存生效**的回归护栏（2026-10-08 重写）。
  //
  // 第一版写的是耗时阈值：`ST_CHECK(hot_ms < 1.0)`——它有两个毛病，而且都真的发作了：
  //
  // ① **不可靠**：命中缓存仍要重拼候选链（内含 `is_regular_file`）再装 `FontStack`，
  //    实测量到 0.87 ms，卡在 1.0 门槛边上。共享机器上两边都会翻车——
  //    §7 说的「机器慢/快两个方向都会失败」就是这个形态。
  // ② **没抓住真缺陷**：`load_face` 当时是「猜 face 下标 0..11 试到失败为止」，
  //    而 `FontFace::load` 先读完整文件（`msyh.ttc` 20 MB）才校验越界，那条失败又不进缓存
  //    ⇒ 每次调用重付 ~44 ms。它把这个用例逼红了好几个月，却被读成“环境抖动”——
  //    因为**耗时这个量分不清“缓存没命中”与“机器很忙”**。
  //
  // 改成数**确定量**：热调用必须零新增读盘/解析。与机器快慢无关，且直接量的是契约本身。
  if (!st::text::FontStack::system_default()) {
    ST_SKIP("系统无可用字体（无字体环境）");
    return;   // `ST_SKIP` 只标记、**不转移控制流**，必须自己 return
  }

  // 第一次（预热）：把系统字体链装进缓存
  auto warm = st::text::FontStack::system_default();
  ST_CHECK(warm.has_value());
  const auto warm_stats = st::text::font_cache_stats();
  ST_CHECK(warm_stats.face_loads > 0);   // 预热确实读了盘（否则下面的“零新增”毫无意义）

  // 之后每次都必须**纯命中**：不重读盘、不重读 face 目录。
  for (int round = 0; round < 3; ++round) {
    auto again = st::text::FontStack::system_default();
    ST_CHECK(again.has_value());
    const auto now = st::text::font_cache_stats();
    ST_CHECK_EQ(now.face_loads, warm_stats.face_loads);
    ST_CHECK_EQ(now.count_reads, warm_stats.count_reads);
  }

  // 命中数必须**真的在涨**——否则上面两条会被“什么都不做”满足（假绿）。
  const auto final_stats = st::text::font_cache_stats();
  ST_CHECK(final_stats.face_hits > warm_stats.face_hits);
}

ST_TEST(font_face_loading_uses_memory_mapping_not_whole_file_reads) {
  // **字体不再整读进 vector**（2026-10-14 内存评估 P0）。
  //
  // 为何要这条用例：字体链 15 个 face 的原始文件字节**整读且永不释放**，实测 ~86MB
  // 常驻——而轮廓解析本来就是惰性的，那部分内存纯属白付。改造后走内存映射
  // （文件页的共享副本，内存压力下可回收）。
  //
  // ⚠ 为何不直接断言内存：RSS 在共享机器上受 GC/换页影响，判据会飘（本仓
  // `bench_thresholds_have_bounded_headroom` 已有先例）。改成数**这次加载走的哪条路**
  // ——确定量，且直接量的是契约本身。
  //
  // 反向验证：把 `FontFace::load` 里的 `MappedFile::open` 分支删掉（回到无条件
  // `read_bytes`），本条的两个断言同时变红。
  auto stack = st::text::FontStack::system_default();
  if (!stack) {
    ST_SKIP("系统无可用字体（无字体环境）");
    return;
  }

  const auto stats = st::text::font_cache_stats();
  ST_CHECK(stats.face_loads > 0);                            // 确实加载过（否则下面无意义）
  ST_CHECK_EQ(stats.mapped_loads, stats.face_loads);         // 每次加载都走了映射
  ST_CHECK_EQ(stats.read_fallbacks, static_cast<std::size_t>(0));   // 一次都没退回整读
}

ST_TEST(font_face_cache_is_process_wide_not_thread_local) {
  // **缓存必须是进程级共享**（2026-10-14）。
  //
  // 起因（真隐患）：`load_face_cached` 的缓存原先是 `thread_local`——Tasks 池
  // （上限 8 线程）里任何非首线程首次碰字体都会**重新走 `FontFace::load`**：
  // 在映射改造前那是**整读全部字体文件一遍**（最坏 8×86MB）。
  // 当前没爆只因 UI 渲染恰好在主线程——那是偶然，不是保证。
  //
  // 判据：主线程预热后，工作线程建字体栈必须**纯命中**（零新增加载）。
  // 反向验证：把 `load_face_cached` 的缓存改回 `thread_local`，本用例变红
  // （工作线程各持一份空表 → `face_loads` 增量等于 face 数）。
  //
  // 为何用 `FontStack::system_default()` 而不是直接调 `FontFace::load`：前者走的才是
  // **缓存层**（`load_face` → `load_face_cached`）；后者是裸加载，本来就该真读。
  if (!st::text::FontStack::system_default()) {
    ST_SKIP("系统无可用字体（无字体环境）");
    return;
  }
  const auto warm = st::text::font_cache_stats();

  std::atomic<std::size_t> loads_seen_in_worker{0};
  std::thread worker([&loads_seen_in_worker] {
    (void)st::text::FontStack::system_default();   // 工作线程里重建一次
    loads_seen_in_worker.store(st::text::font_cache_stats().face_loads);
  });
  worker.join();

  ST_CHECK_EQ(loads_seen_in_worker.load(), warm.face_loads);
}

ST_TEST(font_platform_chain_is_non_empty_and_probeable) {
  // 平台层必须给出**可用的**候选链：探测不到的路径应被过滤掉，
  // 而不是留下一串死路径让引擎逐条失败。
  const auto text = st::platform::preferred_text_fonts();
  const auto bold = st::platform::preferred_text_fonts_bold();
  const auto mono = st::platform::preferred_mono_fonts();
  for (const auto& entry : text) {
    ST_CHECK(!entry.path.empty());
    ST_CHECK(st::fs::is_regular_file(entry.path));   // 返回的都必须是真实存在的文件
  }
  for (const auto& entry : bold) ST_CHECK(st::fs::is_regular_file(entry.path));
  for (const auto& entry : mono) ST_CHECK(st::fs::is_regular_file(entry.path));
}

ST_TEST(font_platform_bold_chain_mirrors_text_chain_by_position) {
  // **逐位同族**是硬约束：`find_face(..., bold)` 按下标把常规档配到同族粗体面，
  // 两条链顺序不一致时，拉丁粗体（segoeuib）会接管中文字 → 整串中文变豆腐块。
  const auto text = st::platform::preferred_text_fonts();
  const auto bold = st::platform::preferred_text_fonts_bold();
  // 粗体链允许更短（部分族没有粗体面），但**同下标处不能跨族**。
  const std::size_t shared = std::min(text.size(), bold.size());
  for (std::size_t index = 0; index < shared; ++index) {
    ST_CHECK_EQ(text[index].cjk, bold[index].cjk);
  }
}

ST_TEST(font_default_text_layers_latin_and_han) {
  // **分层契约**：拉丁 → 内置 Inter，汉字 → 系统 CJK（首选 Noto）——**两个面**。
  //
  // 这条取代了早先的“中英文必须同一族（雅黑）”契约。换成分层是**有意的**：
  // 同时覆盖拉丁与汉字的字族（如雅黑）必然在某一侧妥协（它的拉丁面偏窄偏竖，
  // 小字号下不如 Inter 均匀）。而混排不需要额外逻辑——`find_face` 本来就是按
  // **码点**逐字查的，所以“拉丁走 A 面、汉字走 B 面”天然成立。
  //
  // 但混排要**看得过去**，所以真正要钉的是两件事：
  // ① 两类字符各自落到该落的那一层（拉丁=内置；汉字=非内置）；
  // ② 汉字标点与汉字**同族**——这条容易破而后果明显（「。」走拉丁面会变成
  //    全宽/半宽错位的逗号，排版上看得出来）。
  Fixture fixture;
  if (!fixture.ok) {
    ST_SKIP("系统无可用字体（Fixture 未装配）");
    return;
  }
  const auto* ascii = fixture.stack->find_face(U'A', FontRole::Proportional);
  const auto* han = fixture.stack->find_face(U'霜', FontRole::Proportional);
  ST_REQUIRE(ascii != nullptr);
  ST_REQUIRE(han != nullptr);
  // ① 拉丁层与汉字层分开（分层而非同族）。
  ST_CHECK(ascii->is_embedded());
  ST_CHECK(!han->is_embedded());
  // ② 汉字标点与汉字同族。
  const auto* punct = fixture.stack->find_face(U'。', FontRole::Proportional);
  ST_REQUIRE(punct != nullptr);
  ST_CHECK_EQ(punct->path(), han->path());
}

ST_TEST(font_default_text_latin_is_the_embedded_inter) {
  // 拉丁字形由**内置 Inter** 接管（全平台统一）。
  //
  // 为何必须内置而不是“用系统拉丁”：标签/按钮/菜单/数字是界面里占比最大的字，
  // 而各平台默认拉丁（雅黑/SF/DejaVu）的字宽与 x 高相差很远——同一套界面会
  // 在三个平台上“长得不一样”。
  Fixture fixture;
  if (!fixture.ok) {
    ST_SKIP("系统无可用字体（Fixture 未装配）");
    return;
  }
  const auto* ascii = fixture.stack->find_face(U'A', FontRole::Proportional);
  ST_REQUIRE(ascii != nullptr);
  ST_CHECK(ascii->is_embedded());
  ST_CHECK_EQ(ascii->path(), std::string("<embedded:Inter-Regular>"));
}

ST_TEST(font_default_text_han_prefers_noto_sans) {
  // 汉字**首选系统 Noto Sans**（不是平台主力字体）。
  //
  // 为何：Noto 是跨平台同一设计的中文字族，字形比例与内置 Inter 最接近，
  // 中英混排时两者像同一套字；而雅黑的拉丁面偏窄偏竖，能看出两族。
  // 没装 Noto 的机器**必须仍能工作**（回退到平台主力字体），所以本用例分两路断言。
  Fixture fixture;
  if (!fixture.ok) {
    ST_SKIP("系统无可用字体（Fixture 未装配）");
    return;
  }
  const auto* han = fixture.stack->find_face(U'霜', FontRole::Proportional);
  ST_REQUIRE(han != nullptr);
  // 汉字**绝不能**落在内置 Inter 上：它的 cmap 没有汉字，落下就是链装配错了。
  ST_CHECK(!han->is_embedded());
  // 汉字标点必须与汉字同族（否则「。」会走另一族，排版上看得出来）。
  const auto* punct = fixture.stack->find_face(U'。', FontRole::Proportional);
  ST_REQUIRE(punct != nullptr);
  ST_CHECK_EQ(punct->path(), han->path());
  // 两类机器都要能通过：装了 Noto 就落在 Noto 上，没装就是平台主力字体——
  // 两者共同的要求是“**不能是拉丁回退层**”（如 Segoe UI：它没有汉字轮廓）。
  const std::string path = han->path();
  ST_CHECK(path.find("Noto") != std::string::npos || path.find("msyh") != std::string::npos ||
           path.find("PingFang") != std::string::npos ||
           path.find("NotoSansCJK") != std::string::npos ||
           path.find("simhei") != std::string::npos);
}

ST_TEST(font_default_text_chain_prefers_noto_over_platform_primary) {
  // 链的**顺序**本身是契约：Noto 必须在平台主力字体之前。
  //
  // 只看“汉字最后落在哪”不够：没装 Noto 的机器上两者的结果一样（都落在雅黑），
  // 顺序错了也不会红。而顺序错了的后果是在**装了 Noto 的机器上**静默失效。
  const auto chain = st::platform::preferred_text_fonts();
  ST_REQUIRE(!chain.empty());
  const auto first_platform_primary =
      std::ranges::find_if(chain, [](const st::platform::FontPreference& entry) {
        return entry.path.find("msyh") != std::string::npos ||
               entry.path.find("PingFang") != std::string::npos;
      });
  const auto first_noto = std::ranges::find_if(chain, [](const st::platform::FontPreference& entry) {
    return entry.path.find("Noto") != std::string::npos;
  });
  // 本机没装 Noto 时这条顺序断言无意义（链里根本没有它）。
  if (first_noto == chain.end()) return;
  if (first_platform_primary != chain.end()) {
    ST_CHECK(first_noto < first_platform_primary);
  }
}

ST_TEST(font_default_mono_is_the_embedded_fira_code) {
  // 等宽链**首位是内置 Fira Code**（用户定位：Consolas 看着小）。
  //
  // 为何换字体而不是调字号：实测同字号下 Fira Code 字面明显更开
  // （15px：advance 9.0 / x 高 8 / 自然行高 19，而 Consolas 是 8.0 / 7 / 16）——
  // 用户看到的是“字太小”而真因是“字形偏紧”。
  Fixture fixture;
  if (!fixture.ok) {
    ST_SKIP("系统无可用字体（Fixture 未装配）");
    return;
  }
  if (!fixture.stack->has_monospace()) return;   // 没探到等宽库：本用例无意义
  const auto faces = fixture.stack->monospace_faces();
  ST_REQUIRE(!faces.empty());
  // 顺序错了等于没生效——必须是**首位**，不是“链里有没有”。
  ST_CHECK(faces.front().is_embedded());
  ST_CHECK_EQ(faces.front().path(), std::string("<embedded:FiraCode-Regular>"));
  // ASCII 的归属与链首一致
  const auto* ascii = fixture.stack->find_face(U'i', FontRole::Monospace);
  ST_REQUIRE(ascii != nullptr);
  ST_CHECK(ascii->is_embedded());
  // 汉字不在等宽字体里 → 回退正文档（代码注释里的中文仍可读）
  const auto* han = fixture.stack->find_face(U'霜', FontRole::Monospace);
  ST_REQUIRE(han != nullptr);
  ST_CHECK(!han->is_embedded());
}

ST_TEST(font_default_bold_latin_is_the_embedded_inter_bold) {
  // 粗体链与常规链**逐位同族**：拉丁粗体必须落在内置 Inter-Bold 上。
  //
  // 为何必须配对（实测踩到，2026-10-04）：`find_face(..., bold)` 按下标配档，
  // 两条链顺序不一致时，拉丁粗体（segoeuib）会**接管中文字**——它没有汉字轮廓，
  // 整串中文渲染成豆腐块。
  Fixture fixture;
  if (!fixture.ok) {
    ST_SKIP("系统无可用字体（Fixture 未装配）");
    return;
  }
  const auto* latin = fixture.stack->find_face(U'A', FontRole::Proportional, /*bold=*/true);
  ST_REQUIRE(latin != nullptr);
  ST_CHECK(latin->is_embedded());
  ST_CHECK_EQ(latin->path(), std::string("<embedded:Inter-Bold>"));
  // 汉字粗体**不能**落在那份拉丁粗体上（它没有汉字字形）。
  const auto* han = fixture.stack->find_face(U'霜', FontRole::Proportional, /*bold=*/true);
  ST_REQUIRE(han != nullptr);
  ST_CHECK(!han->is_embedded());
}

ST_TEST(font_default_symbols_still_fall_back) {
  // 符号回退不能因为换了首选字体而失效：✓(U+2713)/✗(U+2717) 在雅黑与 Consolas
  // 里都没有，必须仍能被某档覆盖——缺字的表现为"该字符整块空白"。
  Fixture fixture;
  if (!fixture.ok) {
    ST_SKIP("系统无可用字体（Fixture 未装配）");
    return;
  }
  for (const char32_t codepoint : {U'\u2713', U'\u2717'}) {
    const auto* face = fixture.stack->find_face(codepoint, FontRole::Proportional);
    ST_CHECK(face != nullptr);
  }
}

ST_TEST(font_default_emoji_prefer_emoji_face_over_symbol_face) {
  // **emoji 不能被符号字体接管**。
  //
  // `seguisym.ttf`（Segoe UI Symbol）也覆盖 emoji 码点，但它给的是**符号化的单色
  // 轮廓**：📁 退化成一块实心方块、✅ 退化成 ✓、🇨🇳 退化成 "CN" 两个字母。
  // 那不是“字体缺失”（有字形、能画出来），所以肉眼极易读成“渲染坏了”而不是“选错字体”。
  //
  // 判据取**文件归属**：emoji 码点必须落到 emoji 字体面，而不是符号字体面。
  // 这是一条纯属性面断言，不需要截图——换字体的错在文本层就能钉住。
  Fixture fixture;
  if (!fixture.ok) {
    ST_SKIP("系统无可用字体（无字体环境，本用例不适用）");
    return;
  }
  // ## 守卫必须是“**这个 face 能被本引擎加载**”，而不只是“链里有它”
  //
  // 旧守卫只看链里有没有 emoji 文件。那在 Linux 上必红：发行版的 emoji 字体普遍是
  // **位图彩色**（`NotoColorEmoji.ttf` 只有 CBDT/CBLC，**无 glyf/CFF 轮廓表**），
  // 而本引擎的 `FontFace::load` 明确拒绝“无轮廓表”的字体
  //（`parse_glyf`: “字体既无 glyf/loca 也无 CFF 轮廓表”）。
  // 于是 `find_face` 回空 → `ST_REQUIRE` 红。
  //
  // 这不是环境缺失，也不是本用例要验的东西：它验的是“**有 emoji 面时，emoji 码点
  // 不能被符号字体接管**”（选字顺序），而不是“引擎支不支持位图 emoji”（那是字号/
  // 轮廓能力议题）。后者不成立时本用例的前提不存在——应当**如实跳过**。
  const auto chain = st::platform::preferred_text_fonts();
  const auto bold_chain = st::platform::preferred_text_fonts_bold();
  const char* emoji_file = emoji_face_file();
  bool has_emoji_face = false;
  for (const auto& entry : chain) {
    if (emoji_file != nullptr && entry.path.ends_with(emoji_file)) has_emoji_face = true;
  }
  if (!has_emoji_face) {
    ST_SKIP("本机无 emoji 字体面（无字体环境的容器/精简安装）");
    return;
  }
  // 探一次“**emoji 码点到底落到了哪个面**”。
  //
  // ⚠ `find_face != nullptr` **不能**当守卫：符号字体（`NotoSansSymbols` 等）也覆盖
  // emoji 码点，位图 emoji 字体加载不了时它们会接手——于是 `find_face` 非空，
  // 但拿到的**不是 emoji 字体面**。那正是本用例要报红的情形……**在 emoji 面本来
  // 可用时**。
  //
  // 所以守卫要问的是“**emoji 字体面是否真的能加载**”，与“码点有没有人接”无关：
  // 先把 emoji 文件直接丢给引擎试加载，能加载才算前提成立。
  const auto emoji_probe = [&]() -> bool {
    for (const auto& entry : chain) {
      if (emoji_file == nullptr || !entry.path.ends_with(emoji_file)) continue;
      if (st::text::FontFace::load(entry.path, 0).has_value()) return true;
    }
    return false;
  };
  if (!emoji_probe()) {
    ST_SKIP(std::format(
        "emoji 字体 {} 无轮廓表（位图彩色字体），引擎不支持 → 本用例前提不存在",
        emoji_file != nullptr ? emoji_file : "?"));
    return;
  }
  for (const char32_t codepoint : {U'\U0001F600', U'\U0001F4C1'}) {
    const auto* regular = fixture.stack->find_face(codepoint, FontRole::Proportional);
    // **必须用 `ST_REQUIRE`**：`ST_CHECK` 失败不中止，下一行会解引用空指针
    // ——那是测试自己造出的 SEGV（实测 UBSan 报 “member call on null pointer”，
    // 随后 ASan 报 DEADLYSIGNAL，把真正的断言失败信息盖掉）。
    ST_REQUIRE(regular != nullptr);
    ST_CHECK(regular->path().ends_with(emoji_face_file()));
    // 粗体链同理：粗体链漏了这一档时，emoji 在粗体文本里**又变回符号字体**的
    // 单色方块（同一串文本两种字重两种字形，是那种“只在标题里坏”的缺陷）。
    const auto* bold = fixture.stack->find_face(codepoint, FontRole::Proportional, /*bold=*/true);
    ST_REQUIRE(bold != nullptr);
    ST_CHECK(bold->path().ends_with(emoji_face_file()));
  }
  // 两条链的下标必须仍然对齐（emoji 档是插在中间的一档，最容易只加进常规链）。
  const std::size_t shared = std::min(chain.size(), bold_chain.size());
  for (std::size_t index = 0; index < shared; ++index) {
    ST_CHECK_EQ(chain[index].cjk, bold_chain[index].cjk);
  }
}
