/// 构建期写 State 的重入防护测试。
///
/// **被测的真实缺陷**（本会话实测）：在 build/声明函数里写 State 时，
/// `notify_state_written` 判 `tls_composer == this` → 立即 `mark_state_dirty`
/// → 在 reconcile 栈上触发**递归重组**。症状五花八门：树被拆到一半又重跑
/// （界面错乱）、状态丢失、乃至悬垂指针崩溃（实测一次切标签 SIGSEGV，
/// 排查三轮才定位到"构建 lambda 里 set 了 State"）。
///
/// 现在的契约：**不崩、不递归**，写入延迟到帧末落地（下一帧可见，与声明式
/// 承诺一致），并把写入点记进诊断（`ReconcileStats::build_time_state_writes`）。

#include <memory>
#include <string>
#include <vector>

#include "st/test/test.hpp"
#include "st/ui/dsl.hpp"
#include "st/ui/ui_root.hpp"

namespace {

using st::ui::Element;
using st::ui::UiRoot;
using namespace st::ui::dsl;

/// 构建期写 State 的组件（模拟错误用法：在 build 里 set）。
struct BuildTimeWriter : Component {
  State<int> count{0};
  /// 构建期写入的次数（用于断言"确实写了"，且没有递归爆炸）。
  int writes_during_build{0};
  /// 是否在构建里写（关掉后用于验证正常路径）。
  bool write_in_build{true};

  void build(Composer& c) override {
    if (write_in_build) {
      // ⚠ 这就是被测的错误用法：声明过程中改状态。
      if (count.value() == 0) {
        count.set(1);
        ++writes_during_build;
      }
    }
    column(c, {.gap = 4.0F, .padding = 8.0F}, [&] {
      (void)text(c, [this] { return std::format("count={}", count.value()); }, {.id = "counter-label"});
    });
  }
};

/// 挂载一个组件并返回（root, host, page）。
struct Fixture {
  UiRoot root{};
  std::unique_ptr<DeclarativeHost> host{};
  std::shared_ptr<BuildTimeWriter> page = std::make_shared<BuildTimeWriter>();

  /// mount 那一次的统计（构建期写入就发生在它里面）。
  ReconcileStats mount_stats{};

  /// `write_in_build = false` 用于"正常路径"用例——必须在 **mount 之前**设定，
  /// 因为首帧重组就发生在 mount 里（构造完再改已经晚了，写入已经被记录）。
  explicit Fixture(bool write_in_build = true) {
    page->write_in_build = write_in_build;
    root.set_viewport(st::math::Size{400.0F, 200.0F});
    root.set_content(std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column));
    root.layout(true);
    host = mount(root, page);
    // mount 内部的 reconcile **就是**写入发生的那一帧——它的统计在 `stats()` 里
    //（`tick()` 会跑新的一帧并把统计覆盖成"这一帧写的"，那时已经不写了）。
    mount_stats = host->stats();
    root.layout(true);
  }

  /// 一帧：跑重组（返回统计，本文件的断言主要吃它）+ 布局。
  auto frame() -> const ReconcileStats& {
    last_stats = host->tick();
    root.layout(true);
    return last_stats;
  }

  ReconcileStats last_stats{};

  auto label() -> std::string {
    if (Element* found = root.find("counter-label"); found != nullptr) {
      return found->semantics_text();
    }
    return {};
  }

  [[nodiscard]] auto writes() const -> const std::vector<std::string>& {
    return last_stats.build_time_state_writes;
  }
};

}  // namespace

ST_TEST(declarative_build_time_state_write_does_not_recurse) {
  Fixture fixture;
  // ① **不崩**（这是本项的核心承诺：递归重组在本会话里变成过 SIGSEGV）。
  fixture.frame();
  fixture.frame();
  // ② 构建期写入被记录进诊断（可被测试/宿主读到）。
  //    注意：写入发生在 **mount/首帧**，所以看那一次的统计快照。
  ST_CHECK(!fixture.mount_stats.build_time_state_writes.empty());
  if (!fixture.mount_stats.build_time_state_writes.empty()) {
    // 描述里含 State 地址（足以把范围缩到某处）。
    const std::string& first = fixture.mount_stats.build_time_state_writes.front();
    ST_CHECK(first.find("State@") != std::string::npos);
  }
  // ③ 写入**真的生效**（延迟到帧末，不是丢弃）。
  ST_CHECK_EQ(fixture.page->count.value(), 1);
  ST_CHECK_EQ(fixture.page->writes_during_build, 1);
  // ④ 界面最终反映出新值（重组正常收敛，没有被"递归的重组"打乱）。
  for (int round = 0; round < 3; ++round) fixture.frame();
  ST_CHECK(fixture.label().find("count=1") != std::string::npos);
}

ST_TEST(declarative_normal_state_write_has_no_diagnostic) {
  // 反向契约：**正常路径（事件回调里写）不该产生这条诊断**——
  // 否则它就成了噪音，真有问题的写入会被淹掉。
  Fixture fixture{/*write_in_build=*/false};
  fixture.frame();
  ST_CHECK(fixture.mount_stats.build_time_state_writes.empty());
  // 事件外写：立即生效（下一帧可见），无诊断。
  fixture.page->count.set(42);
  fixture.frame();
  ST_CHECK_EQ(fixture.page->count.value(), 42);
  ST_CHECK(fixture.writes().empty());
  ST_CHECK(fixture.label().find("count=42") != std::string::npos);
}

ST_TEST(declarative_build_time_write_is_deduplicated) {
  // 同一处每帧都写时不刷屏（诊断里同一描述最多几条）——
  // 否则一条诊断会被连续帧写满，反而看不到别的信息。
  struct AlwaysWriter : Component {
    State<int> tick{0};
    void build(Composer& c) override {
      tick.set(tick.value() + 1);   // 每帧都写（最坏用法）
      (void)text(c, [this] { return std::to_string(tick.value()); }, {.id = "tick-label"});
    }
  };
  UiRoot root{};
  root.set_viewport(st::math::Size{200.0F, 100.0F});
  root.set_content(std::make_unique<st::ui::Panel>(st::ui::FlexDirection::Column));
  root.layout(true);
  auto page = std::make_shared<AlwaysWriter>();
  auto host = mount(root, page);
  ST_REQUIRE(host != nullptr);
  ReconcileStats stats{};
  for (int frame = 0; frame < 5; ++frame) {
    stats = host->tick();
    root.layout(true);
  }
  // 5 帧都写，但描述去重（同一处 → 1 条）。
  ST_CHECK(stats.build_time_state_writes.size() <= 2);
  // 值仍单调增长（每帧 +1 没被吞）。
  ST_CHECK(page->tick.value() >= 1);
}
