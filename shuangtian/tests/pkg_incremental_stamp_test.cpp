/// 增量判定的**两个方向**在同一份记录上的端到端回归：写快照 → 改文件 → 再判定。
///
/// ## 为什么需要它（而不只是 `pkg_deps_hash_test`）
///
/// `deps_entry_matches` 只测“一个哈希比没比过”。真正会让构建出错的是**快照的写入与读出
/// 口径是否一致**：序列化写 `path<TAB>hash`、反序列化读回来，任何一边错（少一列、丢路径、
/// 把 Windows 盘符当分隔符）都会让判定恒对或恒错，而两种症状都静默。
///
/// ## 两个方向都要钉
///
/// | 方向 | 后果 |
/// |---|---|
/// | 改了却判未变 | 拿旧产物当新结果，去查被测代码白烧几轮 |
/// | 没改却判变 | 每次构建全量重编，人开始习惯性加 `--force` |
///
/// 尤其钉“**只动时间戳、内容不变**”：`git checkout` / `touch` / 编辑器原子保存
/// 在真实工作流里天天发生，而它对判定必须**完全没有影响**。
#include <algorithm>
#include <string>
#include <thread>
#include <vector>

#include "st/core/fs.hpp"
#include "st/pkg/build.hpp"
#include "st/test/test.hpp"

namespace {

/// 一个临时目录里的最小源码树，析构时清理。
struct TempTree {
  std::string dir;
  explicit TempTree(std::string_view prefix) {
    dir = st::fs::make_temp_dir(prefix).value_or(st::fs::temp_dir());
  }
  ~TempTree() { (void)st::fs::remove_all(dir); }
  TempTree(const TempTree&) = delete;
  auto operator=(const TempTree&) -> TempTree& = delete;

  void write(std::string_view relative, std::string_view content) const {
    const std::string path = st::fs::join(dir, std::string(relative));
    (void)st::fs::create_directories(st::fs::parent(path));
    (void)st::fs::write_text(path, std::string(content));
  }
};

}  // namespace

/// 目标源快照：写下的内容必须能逐条重算对上，且**只受内容影响**。
///
/// 这条覆盖 `st run` 的“壳层没改就直接跑”判据（同一份 `target_source_stamp`）。
/// 目标源快照与测试源快照的**写入/比对闭环**。
///
/// 两个快照各自被谁用：`target_source_stamp` → `<产物>.srcs`（`st run` 判能不能直接跑）；
/// `test_source_stamp` → `<binary>.srcs`（`st build` 判要不要刷新测试）。
/// 它们共享同一个不变量：**只受内容影响**。
///
/// 下面两段分别验这两个快照的闭环——是否重复写同一个文件、是否误报。
ST_TEST(target_source_stamp_tracks_content_only) {
  TempTree tree("st_target_stamp_");
  tree.write("src/main.cpp", "int main() { return 0; }\n");
  tree.write("src/other.cpp", "// other\n");

  const auto manifest_path = st::fs::join(tree.dir, "st.pkg");
  tree.write("st.pkg", R"({
    "name": "stamp_probe",
    "version": "0.1.0",
    "sources": ["src/*.cpp"],
    "targets": { "app": { "kind": "executable", "sources": ["src/*.cpp"] } }
  })");
  const auto manifest = st::pkg::Manifest::load(manifest_path);
  ST_REQUIRE(manifest.has_value());
  const st::pkg::TargetSpec* target = manifest->find_target("app");
  ST_REQUIRE(target != nullptr);

  const std::string first = st::pkg::target_source_stamp(*manifest, *target);
  ST_CHECK(!first.empty());

  // ① 不动任何文件 → 快照一模一样（否则每次 `st run` 都会白构建一轮）
  ST_CHECK_EQ(st::pkg::target_source_stamp(*manifest, *target), first);

  // ② 只动时间戳（touch 语义：写同一份内容）→ 快照仍然一模一样
  tree.write("src/main.cpp", "int main() { return 0; }\n");
  ST_CHECK_EQ(st::pkg::target_source_stamp(*manifest, *target), first);

  // ③ 内容改了（等长）→ 快照必须变
  tree.write("src/main.cpp", "int main() { return 1; }\n");
  ST_CHECK(st::pkg::target_source_stamp(*manifest, *target) != first);
}

/// 测试二进制过期判定：二进制不存在 → 不算过期（第一次构建不该白编几百个单元）；
/// 快照缺失 → 保守判过期；内容改了 → 过期；内容没改（只是时间戳动了）→ 不过期。
ST_TEST(test_binary_staleness_round_trip) {
  TempTree tree("st_test_stamp_");
  tree.write("tests/a_test.cpp", "// a\n");
  const std::string binary = st::fs::join(tree.dir, "st_tests");
  const std::vector<std::string> sources = {st::fs::join(tree.dir, "tests/a_test.cpp")};

  // ① 二进制不存在 → 不是“过期”，是“没开始”
  ST_CHECK(!st::pkg::test_binary_is_stale(sources, binary));

  // ② 二进制在、但没有出生快照 → 无从校验，保守重建
  tree.write("st_tests", "fake binary");
  ST_CHECK(st::pkg::test_binary_is_stale(sources, binary));

  // ③ 落一份快照 → 不再过期
  (void)st::fs::write_text(st::pkg::test_binary_stamp_path(binary),
                           st::pkg::test_source_stamp(sources));
  ST_CHECK(!st::pkg::test_binary_is_stale(sources, binary));

  // ④ 只动时间戳（写同一份内容）→ 仍然不过期
  tree.write("tests/a_test.cpp", "// a\n");
  ST_CHECK(!st::pkg::test_binary_is_stale(sources, binary));

  // ⑤ 内容改了（等长）→ 必须过期
  tree.write("tests/a_test.cpp", "// b\n");
  ST_CHECK(st::pkg::test_binary_is_stale(sources, binary));
}
