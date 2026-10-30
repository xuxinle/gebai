/// 文档地图的完整性测试。
///
/// ## 为什么把"文档收录"做成测试
///
/// `docs/README.md` 是所有人的入口。实测发现它曾长期只收录 **26/49 份**——
/// 包括 `geometry.md`（三维能力的唯一权威）都在外面。
///
/// 根因不是"忘了写"，而是**纯人工维护必然失效**：加文档时想不起来同步。
/// 所以把它变成可执行的判据——漏登记会红，而不是等半年后有人发现地图不准。
///
/// ## 判据口径（只要求"提到名字"，不查链接格式）
///
/// 地图的写法会演进（表格、列表、分段），绑死格式会让测试变成维护负担。
/// 而"文件名是否出现"是**必要条件**且足够抓住真正的失效（孤儿文档）。
///
/// ## 为什么这条测试读文件而不用编译期常量
///
/// 它检查的对象就是仓库里的文件本身——把清单编译进测试等于把地图复制一份，
/// 那份副本又会漂移。直接读 `docs/` 才能保证"看到的就是真的"。
#include <algorithm>
#include <string>
#include <vector>

#include "st/core/fs.hpp"
#include "st/core/log.hpp"
#include "st/test/test.hpp"

namespace {

/// 仓库根：测试进程的工作目录是 `shuangtian/`（`st test` 的约定）。
constexpr std::string_view kDocsDir = "docs";

}  // namespace

ST_TEST(docs_map_covers_every_file) {
  // ## 地图必须收录 docs 下的每一份 .md
  //
  // 这是本轮的核心判据：漏一份就红。加新文档忘登记会立刻被抓到。
  const auto map_text = st::fs::read_text(st::fs::join(kDocsDir, "README.md"));
  if (!map_text.has_value()) {
    st::log::err("[docs] 读取 docs/README.md 失败：{}\n", map_text.error().message);
    ST_REQUIRE(map_text.has_value());
  }

  const auto entries = st::fs::list_dir(kDocsDir);
  if (!entries.has_value()) {
    st::log::err("[docs] 列目录失败：{}\n", entries.error().message);
    ST_REQUIRE(entries.has_value());
  }

  std::vector<std::string> markdown_files;
  for (const auto& entry : *entries) {
    const std::string name = entry.name;
    if (name.size() > 3U && name.compare(name.size() - 3U, 3U, ".md") == 0) {
      markdown_files.push_back(name);
    }
  }
  std::sort(markdown_files.begin(), markdown_files.end());

  // 地图自身不必登记自己
  std::vector<std::string> missing;
  for (const std::string& name : markdown_files) {
    if (name == "README.md") continue;
    if (map_text->find(name) == std::string::npos) missing.push_back(name);
  }

  st::log::out("[docs] docs/ 下 {} 份 .md，地图覆盖 {} 份\n", markdown_files.size(),
               markdown_files.size() - missing.size() - 1);
  if (!missing.empty()) {
    for (const std::string& name : missing) {
      st::log::err("[docs] 未登记到地图：{}\n", name);
    }
  }
  // 空 = 全部登记
  ST_CHECK(missing.empty());
  // 同时要求地图确实有一定规模（防止有人把它清空后"全都不算遗漏"）
  ST_CHECK(markdown_files.size() >= 20U);
}

ST_TEST(docs_map_has_intent_index) {
  // ## 地图必须有「按意图索引」，且覆盖主要场景
  //
  // 只有分类表的地图对模型没用——模型的第一个问题永远是"我要做 X，读哪篇"，
  // 而不是"docs 里有哪些文件"。所以意图索引是**必要的**，不是装饰。
  const auto map_text = st::fs::read_text(st::fs::join(kDocsDir, "README.md"));
  ST_REQUIRE(map_text.has_value());
  const std::string& text = *map_text;

  ST_CHECK(text.find("按意图索引") != std::string::npos);

  // 覆盖的主要意图（用关键词检查，不绑死文案）
  const std::vector<std::string> required_intents{
      "我要开始",       // 第一次接触
      "我要画东西",     // 三维/视觉
      "我要写代码",     // 编码
      "我要验收",       // 自查
      "我要定方向",     // 规划
  };
  for (const std::string& intent : required_intents) {
    if (text.find(intent) == std::string::npos) {
      st::log::err("[docs] 意图索引缺少一类：{}\n", intent);
      ST_CHECK(false);
    }
  }

  // 必须给出**怎么读**的指引（体量档），否则模型无法估算上下文成本
  ST_CHECK(text.find("速查") != std::string::npos);
  ST_CHECK(text.find("档案") != std::string::npos);
}

ST_TEST(docs_map_flags_largest_documents) {
  // ## 超大文档必须标明"不要通读"
  //
  // `BACKLOG.md` 有 29 万字、`PITFALLS.md` 有 8.7 万字。
  // 模型若按"读文档"的默认策略整篇读，会一次烧掉大半上下文。
  // 地图必须显式提醒，并且给出"怎么定点查"。
  const auto map_text = st::fs::read_text(st::fs::join(kDocsDir, "README.md"));
  ST_REQUIRE(map_text.has_value());
  const std::string& text = *map_text;

  // 这两份是全仓最大的文档，必须被点名提醒
  for (const char* name : {"BACKLOG.md", "PITFALLS.md"}) {
    if (text.find(name) == std::string::npos) {
      st::log::err("[docs] 地图未收录大文档：{}\n", name);
      ST_CHECK(false);
    }
  }
  // 必须存在"不要通读 / 只查不读"这类明确劝阻
  const bool has_warning =
      text.find("不要通读") != std::string::npos || text.find("只查不读") != std::string::npos;
  ST_CHECK(has_warning);
}
