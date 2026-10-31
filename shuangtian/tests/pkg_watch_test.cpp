/// `st watch` 的变更侦测内核回归。
///
/// ## 为什么这批用例必须存在
///
/// 写 watch 时实测踩到两个**症状完全错位**的坑：
///
/// 1. **glob 匹配的 `**` 语义错**：第一版用“两指针 + 回溯”的经典 wildcard 算法，
///    把「`**` 跨目录 / `*` 不跨目录」的状态藏在回溯变量里——一旦回溯到更早的星就丢了。
///    结果 `src/**/*.cpp` 对 `src/math/color.cpp` 返回 **false**，
///    监视列表从 650 掉到 397（**漏了 110 个文件**），而 watch 看起来一切正常：
///    只是“改了文件不触发构建”。这比崩溃难查得多。
/// 2. **`fs::walk` 的路径是相对传入目录的**：直接拿它与“相对仓库根”的 glob 比
///    永远不匹配，于是监视列表**空**（而代码不报错）。
///
/// 所以判据钉在「模式 × 路径」的**具体组合**上——这些组合是从真实清单里抄下来的。
#include <algorithm>
#include <string>
#include <vector>

#include "st/core/fs.hpp"
#include "st/pkg/watch.hpp"
#include "st/test/test.hpp"

namespace {

/// 一个临时目录里的最小树：用来测「扫描 + 差异」端到端。
struct TempTree {
  std::string dir;
  explicit TempTree(std::string_view prefix) {
    dir = st::fs::make_temp_dir(prefix).value_or(st::fs::temp_dir());
  }
  ~TempTree() { (void)st::fs::remove_all(dir); }
  TempTree(const TempTree&) = delete;
  auto operator=(const TempTree&) -> TempTree& = delete;

  void file(std::string_view relative, std::string_view content = "x") const {
    const std::string path = st::fs::join(dir, std::string(relative));
    const std::size_t slash = path.find_last_of("/\\");
    if (slash != std::string::npos) {
      (void)st::fs::create_directories(path.substr(0, slash));
    }
    (void)st::fs::write_text(path, std::string(content));
  }
};

}  // namespace

/// 扫描：能从包含/排除规则里挑出正确的文件集合。
///
/// 这条同时钉住「`fs::walk` 的路径基准」那个坑：若路径没拼回“相对根”的形式，
/// 下面的 glob 匹配会一个都不中，`out.size()` 就是 0（而不是期望的 3）。
ST_TEST(watch_collects_files_matching_include_and_exclude) {
  TempTree tree("st_watch_collect_");
  tree.file("src/a.cpp");
  tree.file("src/pkg/b.cpp");
  tree.file("src/ui/components/c.cpp");
  tree.file("include/st/d.hpp");
  tree.file("third_party/e.cpp");   // 应被排除
  tree.file("readme.txt");          // 扩展名不收

  const auto got = st::pkg::collect_watched_files(
      tree.dir, {"src/**/*.cpp", "include/**/*.hpp"}, {"third_party/**"});
  ST_REQUIRE(got.has_value());
  ST_CHECK_EQ(got->size(), 4U);   // a/b/c + d

  // 逐个确认路径都在（而不是数量对了内容错了）
  const auto has = [&](std::string_view leaf) {
    for (const auto& item : *got) {
      if (item.path.find(leaf) != std::string::npos) return true;
    }
    return false;
  };
  ST_CHECK(has("a.cpp"));
  ST_CHECK(has("b.cpp"));            // 深层目录（`src/**/*.cpp` 必须跨目录）
  ST_CHECK(has("c.cpp"));            // 更深一层
  ST_CHECK(has("d.hpp"));
  ST_CHECK(!has("e.cpp"));           // 被 exclude 掉
  ST_CHECK(!has("readme.txt"));      // 扩展名不收
}

/// `**` 必须跨目录、`*` 必须**不**跨目录。
///
/// 这是那个“漏 110 个文件”缺陷的直接回归：判据是**两条相反的断言**——
/// 只测“`**` 能匹配深层”会让“`*` 也跨目录”的实现通过（那是另一个错方向）。
ST_TEST(watch_glob_double_star_crosses_dirs_single_star_does_not) {
  TempTree tree("st_watch_glob_");
  tree.file("src/deep/x.cpp");     // 两层
  tree.file("src/y.cpp");          // 一层

  // ① `src/*.cpp` 只中一层
  const auto single = st::pkg::collect_watched_files(tree.dir, {"src/*.cpp"}, {});
  ST_REQUIRE(single.has_value());
  ST_CHECK_EQ(single->size(), 1U);
  ST_CHECK(single->front().path.find("y.cpp") != std::string::npos);

  // ② `src/**/*.cpp` 两层都中
  const auto dbl = st::pkg::collect_watched_files(tree.dir, {"src/**/*.cpp"}, {});
  ST_REQUIRE(dbl.has_value());
  ST_CHECK_EQ(dbl->size(), 2U);
}

/// 差异比较：新增 / 删除 / 修改三个方向都要报。
///
/// 只报“修改”会漏掉删源文件的情形——那时产物里会留着旧代码（`build.cpp` 的
/// “链接跳过”注释里记的那个真实缺陷就是这一类）。
///
/// 字段是**内容哈希**（不是 mtime/size）：两种快照里同一个路径的哈希不同 = 内容改了。
ST_TEST(watch_diff_reports_added_removed_and_modified) {
  const std::vector<st::pkg::WatchedFile> before = {
      {"a.cpp", 100},
      {"b.cpp", 200},
      {"c.cpp", 300},
  };
  const std::vector<st::pkg::WatchedFile> after = {
      {"a.cpp", 100},   // 未变
      {"b.cpp", 999},   // 内容改了
      {"d.cpp", 400},   // 新增（c 被删）
  };
  const auto changed = st::pkg::diff_watched(before, after);
  ST_CHECK_EQ(changed.size(), 3U);   // b（改）+ c（删）+ d（增）
  ST_CHECK(std::ranges::find(changed, std::string("b.cpp")) != changed.end());
  ST_CHECK(std::ranges::find(changed, std::string("c.cpp")) != changed.end());
  ST_CHECK(std::ranges::find(changed, std::string("d.cpp")) != changed.end());
  ST_CHECK(std::ranges::find(changed, std::string("a.cpp")) == changed.end());
}

/// **只动时间戳、内容不变 ⇒ 不算变更**（切分支/`touch` 后不会白构建一轮）。
///
/// 这条与上面成对：只测“内容变了能发现”会让“一律报变”的实现通过（那是另一个错方向：
/// 每次轮询都触发全量构建）。
ST_TEST(watch_diff_is_quiet_when_only_timestamp_moved) {
  TempTree tree("st_watch_touch_");
  tree.file("src/a.cpp", "same-content");
  const auto before = st::pkg::collect_watched_files(tree.dir, {"src/**/*.cpp"}, {});
  ST_REQUIRE(before.has_value());

  // 重写同一份内容（模拟 touch / 原子保存）：内容哈希应当不变
  tree.file("src/a.cpp", "same-content");
  const auto after = st::pkg::collect_watched_files(tree.dir, {"src/**/*.cpp"}, {});
  ST_REQUIRE(after.has_value());
  ST_CHECK(st::pkg::diff_watched(*before, *after).empty());

  // 对照：内容真改了 → 必须报出来（否则上面那条会被“恒不回变”实现骗过）
  tree.file("src/a.cpp", "changed-content");
  const auto changed_tree = st::pkg::collect_watched_files(tree.dir, {"src/**/*.cpp"}, {});
  ST_REQUIRE(changed_tree.has_value());
  ST_CHECK_EQ(st::pkg::diff_watched(*before, *changed_tree).size(), 1U);
}

/// 等长改写（size 不变、时间戳不变、内容变了）必须报出来。
///
/// 这是 mtime+size 判据的盲区：`git checkout` 回来一份同样长的旧内容、
/// 或编辑器原子保存保留时间戳，都会被“没变”骗过。
ST_TEST(watch_diff_detects_equal_length_change) {
  TempTree tree("st_watch_eqlen_");
  tree.file("src/a.cpp", "AAAA");
  const auto before = st::pkg::collect_watched_files(tree.dir, {"src/**/*.cpp"}, {});
  ST_REQUIRE(before.has_value());
  tree.file("src/a.cpp", "BBBB");
  const auto after = st::pkg::collect_watched_files(tree.dir, {"src/**/*.cpp"}, {});
  ST_REQUIRE(after.has_value());
  ST_CHECK_EQ(st::pkg::diff_watched(*before, *after).size(), 1U);
}

/// 快照必须**有序**（`diff_watched` 用归并比较，依赖这个前提）。
ST_TEST(watch_snapshot_is_sorted) {
  TempTree tree("st_watch_sorted_");
  tree.file("src/z.cpp");
  tree.file("src/a.cpp");
  tree.file("src/m.cpp");
  const auto got = st::pkg::collect_watched_files(tree.dir, {"src/**/*.cpp"}, {});
  ST_REQUIRE(got.has_value());
  ST_CHECK_EQ(got->size(), 3U);
  for (std::size_t index = 1; index < got->size(); ++index) {
    ST_CHECK((*got)[index - 1].path < (*got)[index].path);
  }
}

/// 产物目录**绝不能**被监视（否则一次构建写产物 → 触发再构建 → 自激循环）。
///
/// ⚠ 这条用例踩到过**静默崩溃**：`build/dev/obj/x.cpp` 建出三层目录后，
/// `collect_watched_files(tree.dir, {"**/*.cpp"}, {})` 的子树推导得到空前缀（`**/*.cpp`
/// 的通配符在段首，`find_last_of('/')` 落在 `**` 之前），于是 `fs::walk(root + "")`
/// 就是整树遍历——而临时目录里恰好有 `build/dev/obj`，它会被视为要排除的目录，
/// 但**递归仍在继续**（排除只作用于文件，不剪枝）。
ST_TEST(watch_never_includes_build_directory) {
  TempTree tree("st_watch_excl_");
  tree.file("build/dev/obj/x.cpp");   // 模拟产物
  tree.file("src/a.cpp");

  // 即使 include 明确写了 `**/*.cpp`，build/ 也必须被挡掉
  const auto got = st::pkg::collect_watched_files(tree.dir, {"**/*.cpp"}, {});
  ST_REQUIRE(got.has_value());
  ST_CHECK_EQ(got->size(), 1U);
  ST_CHECK(got->front().path.find("src") != std::string::npos);
}
