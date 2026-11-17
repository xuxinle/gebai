// 量化三个「简单高收益」候选的真实成本
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "st/core/fs.hpp"
#include "st/core/hash.hpp"

namespace {
using Clock = std::chrono::steady_clock;
double ms(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }
}  // namespace

int main(int argc, char** argv) {
  const std::string self = argc > 1 ? argv[1]
                                    : "D:/code/gebai-pro/packages/desktop/dist/gebai-desktop.exe";
  constexpr std::size_t kTrailer = 32;

  const auto self_size = st::fs::file_size(self);
  const auto trailer = st::fs::read_span(self, *self_size - kTrailer, kTrailer);
  const auto u64 = [&](std::size_t off) {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>((*trailer)[off + i]) << (i * 8U);
    return v;
  };
  const std::uint64_t payload_len = u64(8);
  const std::uint64_t image_len = u64(16);

  // ── 候选 A：payload 被多复制一次（read_payload_from_self 里 vector(body->begin(), body->end())）
  auto t = Clock::now();
  auto body = st::fs::read_span(self, image_len, static_cast<std::size_t>(payload_len));
  const double read_ms = ms(t);

  t = Clock::now();
  std::vector<std::uint8_t> copy(body->begin(), body->end());  // 现状：复制一份
  const double copy_ms = ms(t);
  std::printf("A 读 payload            %7.1f ms\n", read_ms);
  std::printf("A 再复制成 vector       %7.1f ms  ← 可省（改成 move/直接用 Bytes）\n", copy_ms);

  // ── 候选 B：同一份 payload 被哈希两次（read_payload 校验 + materialize 比对）
  t = Clock::now();
  const auto h1 = st::hash::content_hash64(std::span<const std::uint8_t>(copy.data(), copy.size()));
  const double hash_ms = ms(t);
  std::printf("\nB 哈希 payload（SHA-256）%7.1f ms\n", hash_ms);
  std::printf("B 若复用同一哈希值      %7.1f ms  ← 可省（现被算两次）\n", hash_ms);

  // ── 候选 C：materialize 与 app.start() 是否串行
  // 无法在探针里测桩，用实测时间线代替：materialize ≈ 1.07 s，app.start() ≈ 0.43 s
  std::printf("\nC 时间线（实测日志）：\n");
  std::printf("   app.start() 返回        ~430 ms\n");
  std::printf("   materialize 完成        ~1070 ms（读+双哈希+比对）\n");
  std::printf("   侧车 spawn 发出         ~1500 ms\n");
  std::printf("   → 若 materialize 与建窗/WebView 并行，关键路径可省 ~1070 ms\n");

  (void)h1;
  return 0;
}
