#include "st/test/test.hpp"

#include <cstdio>
#include <string>
#include <string_view>

#include "st/pkg/semver.hpp"

namespace {

using st::pkg::Comparator;
using st::pkg::Version;
using st::pkg::VersionReq;

/// 约束文本是否匹配版本文本（任一解析失败即视为不匹配，测试里额外断言解析结果）。
[[nodiscard]] auto matches(std::string_view req_text, std::string_view version_text) -> bool {
  const auto req = VersionReq::parse(req_text);
  const auto version = Version::parse(version_text);
  if (!req || !version) return false;
  return req->matches(*version);
}

[[nodiscard]] auto version_text(std::string_view text) -> std::string {
  const auto version = Version::parse(text);
  return version ? version->to_string() : std::string("<parse-error>");
}

}  // namespace

ST_TEST(semver_parse_basic) {
  const auto full = Version::parse("1.2.3");
  ST_REQUIRE(full.has_value());
  ST_CHECK_EQ(full->major, 1U);
  ST_CHECK_EQ(full->minor, 2U);
  ST_CHECK_EQ(full->patch, 3U);
  ST_CHECK_EQ(full->to_string(), std::string("1.2.3"));
  ST_CHECK(!full->is_prerelease());

  const auto pre = Version::parse("1.2.3-beta.1+build.5");
  ST_REQUIRE(pre.has_value());
  ST_CHECK_EQ(pre->pre, std::string("beta.1"));
  ST_CHECK_EQ(pre->build, std::string("build.5"));
  ST_CHECK_EQ(pre->to_string(), std::string("1.2.3-beta.1+build.5"));
  ST_CHECK(pre->is_prerelease());
  ST_CHECK_EQ(pre->without_pre().to_string(), std::string("1.2.3+build.5"));

  // 缺省段补 0（清单里常见 `^1.2`）
  ST_CHECK_EQ(version_text("1"), std::string("1.0.0"));
  ST_CHECK_EQ(version_text("1.2"), std::string("1.2.0"));
  ST_CHECK_EQ(version_text(" 2.10.0 "), std::string("2.10.0"));
}

ST_TEST(semver_parse_errors) {
  ST_CHECK(!Version::parse("").has_value());
  ST_CHECK(!Version::parse("1.2.3.4").has_value());
  ST_CHECK(!Version::parse("1.2.x").has_value());
  ST_CHECK(!Version::parse("1.2.3-").has_value());
  ST_CHECK(!Version::parse("1.2.3+").has_value());
  ST_CHECK(!Version::parse("1.2.3-beta..1").has_value());
  ST_CHECK(!Version::parse("v1.2.3").has_value());
  ST_CHECK(!Version::parse("1.2.3 beta").has_value());
  const auto overflow = Version::parse("4294967296.0.0");
  ST_REQUIRE(!overflow.has_value());
  ST_CHECK_EQ(st::to_string(overflow.error().code), std::string("overflow"));
}

ST_TEST(semver_compare_prerelease_order) {
  // SemVer 规范的经典排序样例
  constexpr std::string_view chain[] = {"1.0.0-alpha",  "1.0.0-alpha.1", "1.0.0-alpha.beta",
                                        "1.0.0-beta",   "1.0.0-beta.2",  "1.0.0-beta.11",
                                        "1.0.0-rc.1",   "1.0.0",         "1.0.1"};
  for (std::size_t index = 0; index + 1 < std::size(chain); ++index) {
    const auto left = Version::parse(chain[index]);
    const auto right = Version::parse(chain[index + 1]);
    ST_REQUIRE(left.has_value());
    ST_REQUIRE(right.has_value());
    ST_CHECK_EQ(st::pkg::compare(*left, *right), -1);
    ST_CHECK_EQ(st::pkg::compare(*right, *left), 1);
  }
  const auto release = Version::parse("1.0.0");
  const auto alpha = Version::parse("1.0.0-alpha");
  ST_REQUIRE(release.has_value());
  ST_REQUIRE(alpha.has_value());
  ST_CHECK_EQ(st::pkg::compare(*release, *alpha), 1);  // 预发布 < 正式

  // 构建元数据不参与比较
  const auto with_build = Version::parse("1.0.0+linux");
  const auto without_build = Version::parse("1.0.0+windows");
  ST_REQUIRE(with_build.has_value());
  ST_REQUIRE(without_build.has_value());
  ST_CHECK_EQ(st::pkg::compare(*with_build, *without_build), 0);
}

ST_TEST(semver_req_parse_and_print) {
  const auto caret = VersionReq::parse("^1.2.3");
  ST_REQUIRE(caret.has_value());
  ST_CHECK_EQ(caret->comparators.size(), std::size_t{1});
  ST_CHECK(caret->comparators[0].op == Comparator::Op::Caret);
  ST_CHECK_EQ(caret->to_string(), std::string("^1.2.3"));

  const auto range = VersionReq::parse(">=1.0.0 <2.0.0");
  ST_REQUIRE(range.has_value());
  ST_CHECK_EQ(range->comparators.size(), std::size_t{2});
  ST_CHECK_EQ(range->to_string(), std::string(">=1.0.0 <2.0.0"));

  // 逗号等价于空白（AND 语义）
  const auto comma = VersionReq::parse(">=1.0.0, <2.0.0");
  ST_REQUIRE(comma.has_value());
  ST_CHECK_EQ(comma->to_string(), range->to_string());

  const auto any = VersionReq::parse("*");
  ST_REQUIRE(any.has_value());
  ST_CHECK_EQ(any->to_string(), std::string("*"));
  ST_CHECK(VersionReq::any().matches(*Version::parse("9.9.9")));

  ST_CHECK(!VersionReq::parse("").has_value());
  ST_CHECK(!VersionReq::parse("^").has_value());
  ST_CHECK(!VersionReq::parse("^x.y.z").has_value());
}

ST_TEST(semver_req_match_boundaries) {
  // `^`：兼容到下一个非零段
  ST_CHECK(matches("^1.2.3", "1.2.3"));
  ST_CHECK(matches("^1.2.3", "1.2.4"));
  ST_CHECK(matches("^1.2.3", "1.9.0"));
  ST_CHECK(!matches("^1.2.3", "2.0.0"));
  ST_CHECK(!matches("^1.2.3", "1.2.2"));
  ST_CHECK(matches("^0.2.3", "0.2.9"));
  ST_CHECK(!matches("^0.2.3", "0.3.0"));
  ST_CHECK(matches("^0.0.3", "0.0.3"));
  ST_CHECK(!matches("^0.0.3", "0.0.4"));
  ST_CHECK(matches("^1.2", "1.5.0"));  // 缺省段补 0

  // `~`：同 minor 内
  ST_CHECK(matches("~1.2.3", "1.2.3"));
  ST_CHECK(matches("~1.2.3", "1.2.9"));
  ST_CHECK(!matches("~1.2.3", "1.3.0"));
  ST_CHECK(!matches("~1.2.3", "1.2.2"));

  // `=` / 裸版本号：精确
  ST_CHECK(matches("=1.2.3", "1.2.3"));
  ST_CHECK(!matches("=1.2.3", "1.2.4"));
  ST_CHECK(matches("1.2.3", "1.2.3"));

  // 区间
  ST_CHECK(matches(">=1.0.0 <2.0.0", "1.9.9"));
  ST_CHECK(!matches(">=1.0.0 <2.0.0", "2.0.0"));
  ST_CHECK(matches(">1.0.0", "1.0.1"));
  ST_CHECK(!matches(">1.0.0", "1.0.0"));
  ST_CHECK(matches("<=1.0.0", "1.0.0"));
  ST_CHECK(!matches("<=1.0.0", "1.0.1"));
  ST_CHECK(matches("*", "0.0.1"));
}

ST_TEST(semver_req_prerelease_gate) {
  // npm 口径：约束未点名预发布时，预发布候选不匹配（避免误选 alpha）
  ST_CHECK(!matches("^1.2.3", "1.3.0-beta"));
  ST_CHECK(!matches(">=1.2.3", "1.5.0-rc.1"));
  ST_CHECK(!matches("~1.2.3", "1.2.4-alpha"));
  // 点明了预发布则按语义序比较
  ST_CHECK(matches(">=1.0.0-alpha", "1.0.0-beta"));
  ST_CHECK(matches(">=1.0.0-alpha", "1.0.0"));
  ST_CHECK(matches("^1.2.3-beta.1", "1.2.3-beta.2"));
  ST_CHECK(!matches("^1.2.3-beta.1", "1.2.3-alpha.1"));
  // `*` 例外：任意版本（含预发布）
  ST_CHECK(matches("*", "1.0.0-alpha"));
}
