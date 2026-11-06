// 中段省略（`ellipsize_middle`）：路径与长标识专用。
//
// 缺陷实录（2026-11-05）：变更列表的路径用**尾部省略**，于是目录
// `packages/agents/src/agents/shuangtian/` 先把宽度占满，文件名被整个砍掉——
// 用户看到的是"文件名没了"（换一组长度关系就变成"目录没了"）。
//
// 而路径的**两端**才是有用信息：头部定位"在哪个项目/哪一层"，尾部是"哪个文件"。
// 中间那截往往全项目一样，省掉不影响识别。
//
// 这组用例钉住三件事：保留两端、比尾部省略保住更多尾部、不超宽。
// 用真实系统字体（量宽才有意义——按字符数估会漏掉中英文宽度差）。

#include "st/test/test.hpp"
#include "st/text/text.hpp"

#include <memory>
#include <string>

namespace {

/// 系统字体栈；拿不到字体时用例跳过（不把环境缺失算成失败）。
struct Renderer {
  st::Result<st::text::FontStack> made{st::text::FontStack::system_default()};
  std::unique_ptr<st::text::TextRenderer> renderer;   // 需在 stack 之后构造

  Renderer() {
    if (made.has_value()) renderer = std::make_unique<st::text::TextRenderer>(*made);
  }
  [[nodiscard]] auto ok() const -> bool { return renderer != nullptr; }
  [[nodiscard]] auto get() const -> const st::text::TextRenderer& { return *renderer; }
};

constexpr float kSize = 14.0f;
const char* kLongPath = "shuangtian/apps/gbcode/page_sidebar.cpp";

}  // namespace

ST_TEST(ellipsize_middle_keeps_both_ends) {
  Renderer r;
  if (!r.ok()) return;
  const float full = r.get().measure_width(kLongPath, kSize);
  ST_REQUIRE(full > 0.0f);

  // 给一个明显不够的宽度（约一半）：两端都必须有内容。
  const std::string clipped = r.get().ellipsize_middle(kLongPath, kSize, full * 0.5f);
  ST_CHECK(clipped.find("…") != std::string::npos);
  // 头部保留：必须以路径开头那几个字符开始（"shuangtian" 的一部分）。
  ST_CHECK(clipped.rfind("sh", 0) == 0);
  // 尾部保留：必须以真实文件名结尾（这是尾部省略会丢掉的东西）。
  ST_CHECK(clipped.size() > 5);
  ST_CHECK(std::string{kLongPath}.ends_with(clipped.substr(clipped.size() - 5)));
}

ST_TEST(ellipsize_middle_keeps_more_tail_than_tail_ellipsize) {
  Renderer r;
  if (!r.ok()) return;
  const float full = r.get().measure_width(kLongPath, kSize);
  const float budget = full * 0.45f;

  const std::string tail_only = r.get().ellipsize(kLongPath, kSize, budget);
  const std::string middle = r.get().ellipsize_middle(kLongPath, kSize, budget);

  // 本质断言：中段省略的**尾部保留量**必须大于尾部省略（后者是 0）。
  //
  // 为何不写“必须保住最后 8 个字符”：尾部能留多少取决于预算（这里 45% 宽度，
  // 尾部按 1/4 分配只剩几个字符）。拿固定字数当判据会把“预算小”误判成实现错。
  const std::string name = std::string{kLongPath}.substr(std::string{kLongPath}.rfind('/') + 1);
  const auto common_tail = [](const std::string& a, const std::string& b) {
    std::size_t n = 0;
    while (n < a.size() && n < b.size() &&
           a[a.size() - 1 - n] == b[b.size() - 1 - n]) {
      ++n;
    }
    return n;
  };
  const std::size_t tail_only_kept = common_tail(tail_only, name);
  const std::size_t middle_kept = common_tail(middle, name);
  // 尾部省略保留的是**开头**，所以它与文件名的公共后缀应当接近 0。
  ST_CHECK(tail_only_kept <= 1);
  // 中段省略必须真的留下了文件名的后半截（至少 3 个字符，足以辨认扩展名）。
  ST_CHECK(middle_kept >= 3);
  ST_CHECK(middle_kept > tail_only_kept);
}

ST_TEST(ellipsize_middle_never_exceeds_budget) {
  Renderer r;
  if (!r.ok()) return;
  const float full = r.get().measure_width(kLongPath, kSize);
  // 多档宽度都要守约束（二分找长度时最易出错的就是边界）。
  for (const float ratio : {0.2f, 0.35f, 0.5f, 0.75f, 0.95f}) {
    const float budget = full * ratio;
    const std::string clipped = r.get().ellipsize_middle(kLongPath, kSize, budget);
    if (clipped.empty()) continue;   // 太窄放不下省略号：允许空（与尾部省略同口径）
    ST_CHECK(r.get().measure_width(clipped, kSize) <= budget + 0.5f);
  }
}

ST_TEST(ellipsize_middle_passthrough_when_fits) {
  Renderer r;
  if (!r.ok()) return;
  const float full = r.get().measure_width(kLongPath, kSize);
  // 放得下就原样返回（不出现“本来够宽却加了省略号”）。
  ST_CHECK_EQ(r.get().ellipsize_middle(kLongPath, kSize, full + 10.0f),
              std::string{kLongPath});
  // 无界宽度同样原样。
  ST_CHECK_EQ(r.get().ellipsize_middle(kLongPath, kSize, 1e9f), std::string{kLongPath});
}

ST_TEST(ellipsize_middle_handles_cjk_path) {
  Renderer r;
  if (!r.ok()) return;
  // 中文路径：字符边界必须切在码点边界上（切半会输出半个 UTF-8 序列）。
  const std::string cjk = "文档/设计/界面布局说明与测试用例集合.md";
  const float full = r.get().measure_width(cjk, kSize);
  ST_REQUIRE(full > 0.0f);
  const std::string clipped = r.get().ellipsize_middle(cjk, kSize, full * 0.5f);
  ST_CHECK(clipped.find("…") != std::string::npos);
  // 合法 UTF-8：末尾字节不是“续字节”（0b10xxxxxx）。
  if (!clipped.empty()) {
    const auto last = static_cast<unsigned char>(clipped.back());
    ST_CHECK((last & 0xC0) != 0x80);
    ST_CHECK(clipped.ends_with(".md"));
  }
}
