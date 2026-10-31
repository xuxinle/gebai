// channel_swap_probe.cpp —— 逐像素通道重排的几种写法，量到底能快多少。
//
// 背景：软件腿上屏前要把画布像素 `0xRRGGBBAA` 转成 DIB 要的 `0xAARRGGBB`
// （内存字节 R,G,B,A → B,R,G,A，即**第 0/2 字节互换、第 3 字节不动**）。
// 现有实现是逐像素拆 4 个通道再拼回去，3533×1921 实测 **~3.1 ms**，
// 而同一块数据的 `memcpy` 只要 **~1.5 ms**——说明有一半花在"拆位重组"上。
//
// 本探针先排除过一条路：`BI_BITFIELDS` 让 GDI 自己按掩码解读（`dib_bitfields_probe.cpp`）——
// 通道正确，但它的 `BitBlt` 要 **14.4 ms**（现路径 0.245 ms），GDI 在 blit 内部
// 逐像素重打包，省下的全还回去还倒赔。所以只能在 CPU 侧把这一趟做快。
//
// 候选写法（都不改数据布局、不改接口，只是同一语义的更快实现）：
//   A 现状：逐通道拆掩码再拼        —— 4 次移位 + 3 次掩码 + 3 次移位 + 3 次或
//   B 装进 64 位一次处理 2 像素      —— 用掩码移位把两个像素的第 0/2 字节并发互换
//   C B + 每轮 8 像素展开             —— 减少循环开销
//   D 编译器自动向量化（简单 for）    —— 作为"什么都不做"的对照
//
// 判定标准不是"快一点"，而是**逼近 memcpy 的 1.5 ms**：那才是这趟搬运的下限。
//
// 编译：g++ -std=c++17 -O2 channel_swap_probe.cpp -o channel_swap_probe.exe

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

struct Stats {
  double min_ms = 0.0;
  double med_ms = 0.0;
};

Stats summarize(std::vector<double> samples) {
  Stats s;
  if (samples.empty()) return s;
  std::sort(samples.begin(), samples.end());
  s.min_ms = samples.front();
  s.med_ms = samples[samples.size() / 2];
  return s;
}

// A 现状（与 platform_win32.cpp 的 blit() 逐字相同）
void swap_baseline(const std::uint32_t* src, std::uint32_t* dst, std::size_t count) {
  for (std::size_t index = 0; index < count; ++index) {
    const std::uint32_t pixel = src[index];
    const std::uint32_t red = (pixel >> 24U) & 0xFFU;
    const std::uint32_t green = (pixel >> 16U) & 0xFFU;
    const std::uint32_t blue = (pixel >> 8U) & 0xFFU;
    const std::uint32_t alpha = pixel & 0xFFU;
    dst[index] = (alpha << 24U) | (red << 16U) | (green << 8U) | blue;
  }
}

// B 装进 64 位，一次处理 2 像素。
//
// 推导（前两版都写错过，正确形式在这里写清楚）：
//   单个像素 `p = R<<24 | G<<16 | B<<8 | A`，变成 `A<<24 | R<<16 | G<<8 | B`。
//   把四个字节依次看：(R,G,B,A) → (A,R,G,B)——这正好是**循环右移一个字节**：
//     `(p >> 8) | (p << 24)`
//   （不是 16 位交换 `(p<<16)|(p>>16)`：那得到 (B,A,R,G)，实测得 0x33441122 ≠ 0x44112233。）
//   两个像素拼成 64 位后，低半位的 `<<24` 会溢出到高半位，所以**必须先分半再用掩码拼回去**。
void swap_pairwise(const std::uint32_t* src, std::uint32_t* dst, std::size_t count) {
  constexpr std::uint64_t kLo = 0x00000000FFFFFFFFULL;
  constexpr std::uint64_t kHi = 0xFFFFFFFF00000000ULL;
  std::size_t index = 0;
  for (; index + 1 < count; index += 2) {
    std::uint64_t two = 0;
    std::memcpy(&two, src + index, sizeof(two));
    const std::uint64_t lo = ((two & kLo) >> 8U) | ((two & kLo) << 24U);
    const std::uint64_t hi = ((two & kHi) >> 8U) | ((two & kHi) << 24U);
    two = (lo & kLo) | (hi & kHi);
    std::memcpy(dst + index, &two, sizeof(two));
  }
  if (index < count) {
    const std::uint32_t pixel = src[index];
    dst[index] = (pixel >> 8U) | (pixel << 24U);
  }
}

// C 同 B，但每轮 4 对（8 像素）展开。
//
// ⚠ 第一版这里写成了沿用 B 的错误掩码，且「一次 memcpy 4 个 uint64 再逐个换、
// 再 memcpy 回去」引入两次 32 字节临时拷贝，实测反而比现状慢 3 倍（17.5 ms）。
// 本版直接读写 dst，避开临时数组。
void swap_pairwise_unrolled(const std::uint32_t* src, std::uint32_t* dst, std::size_t count) {
  constexpr std::uint64_t kLo = 0x00000000FFFFFFFFULL;
  constexpr std::uint64_t kHi = 0xFFFFFFFF00000000ULL;
  auto one = [](std::uint64_t two) {
    const std::uint64_t lo = ((two & kLo) >> 8U) | ((two & kLo) << 24U);
    const std::uint64_t hi = ((two & kHi) >> 8U) | ((two & kHi) << 24U);
    return (lo & kLo) | (hi & kHi);
  };
  std::size_t index = 0;
  for (; index + 8 <= count; index += 8) {
    std::uint64_t a = 0;
    std::uint64_t b = 0;
    std::uint64_t c = 0;
    std::uint64_t d = 0;
    std::memcpy(&a, src + index, sizeof(a));
    std::memcpy(&b, src + index + 2, sizeof(b));
    std::memcpy(&c, src + index + 4, sizeof(c));
    std::memcpy(&d, src + index + 6, sizeof(d));
    a = one(a);
    b = one(b);
    c = one(c);
    d = one(d);
    std::memcpy(dst + index, &a, sizeof(a));
    std::memcpy(dst + index + 2, &b, sizeof(b));
    std::memcpy(dst + index + 4, &c, sizeof(c));
    std::memcpy(dst + index + 6, &d, sizeof(d));
  }
  swap_pairwise(src + index, dst + index, count - index);
}

}  // namespace

namespace {

// D 直接按字节读写，一次 4 像素（16 字节）。
//
// 思路：变换是「每像素字节循环右移一位」= 目标第 0..3 字节取自源的 1,2,3,0 字节。
// 不装进整数、不做掩码，直接用字节下标重排——编译器对这类定长展开
// 往往能合成 SSE/AVX 的 shuffle，比位运算更快。
void swap_bytes(const std::uint32_t* src, std::uint32_t* dst, std::size_t count) {
  const auto* s = reinterpret_cast<const std::uint8_t*>(src);
  auto* d = reinterpret_cast<std::uint8_t*>(dst);
  std::size_t index = 0;
  for (; index + 4 <= count; index += 4) {
    const std::uint8_t* p = s + index * 4;
    std::uint8_t* q = d + index * 4;
    q[0] = p[1];
    q[1] = p[2];
    q[2] = p[3];
    q[3] = p[0];
    q[4] = p[5];
    q[5] = p[6];
    q[6] = p[7];
    q[7] = p[4];
    q[8] = p[9];
    q[9] = p[10];
    q[10] = p[11];
    q[11] = p[8];
    q[12] = p[13];
    q[13] = p[14];
    q[14] = p[15];
    q[15] = p[12];
  }
  swap_pairwise(src + index, dst + index, count - index);
}

}  // namespace

int main(int argc, char** argv) {
  int width = 3533;
  int height = 1921;
  if (argc > 2) {
    width = std::atoi(argv[1]);
    height = std::atoi(argv[2]);
  }
  const std::size_t count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  std::printf("画布 %dx%d（%.1f Mpx）· %zu 字节\n\n", width, height, count / 1e6,
              count * sizeof(std::uint32_t));

  std::vector<std::uint32_t> source(count);
  for (std::size_t i = 0; i < count; ++i) {
    const std::uint32_t v = static_cast<std::uint32_t>(i * 2654435761U);
    source[i] = (v & 0xFFFFFF00U) | 0x000000FFU;  // 每像素第 3 字节 = alpha = 0xFF
  }
  std::vector<std::uint32_t> dst(count, 0);

  // ── 正确性：四种写法结果必须逐字节相同（否则"快"毫无意义）──
  std::vector<std::uint32_t> ref(count, 0);
  swap_baseline(source.data(), ref.data(), count);
  bool all_equal = true;
  auto verify = [&](const char* name, void (*fn)(const std::uint32_t*, std::uint32_t*, std::size_t)) {
    std::fill(dst.begin(), dst.end(), 0U);
    fn(source.data(), dst.data(), count);
    const bool same = std::memcmp(dst.data(), ref.data(), count * sizeof(std::uint32_t)) == 0;
    if (!same) all_equal = false;
    std::printf("  校验 %-24s %s\n", name, same ? "逐字节一致" : "**不一致**");
  };
  std::printf("[正确性]\n");
  verify("A 基线", swap_baseline);
  verify("B 64位成对", swap_pairwise);
  verify("C 64位展开", swap_pairwise_unrolled);
  verify("D 字节重排", swap_bytes);
  std::printf("\n");

  constexpr int kIterations = 60;
  std::vector<double> memcpy_t;
  std::vector<double> a_t, b_t, c_t, d_t;
  for (int i = 0; i < kIterations; ++i) {
    auto t0 = Clock::now();
    std::memcpy(dst.data(), source.data(), count * sizeof(std::uint32_t));
    memcpy_t.push_back(ms_since(t0));

    t0 = Clock::now();
    swap_baseline(source.data(), dst.data(), count);
    a_t.push_back(ms_since(t0));

    t0 = Clock::now();
    swap_pairwise(source.data(), dst.data(), count);
    b_t.push_back(ms_since(t0));

    t0 = Clock::now();
    swap_pairwise_unrolled(source.data(), dst.data(), count);
    c_t.push_back(ms_since(t0));

    t0 = Clock::now();
    swap_bytes(source.data(), dst.data(), count);
    d_t.push_back(ms_since(t0));
  }
  const Stats sMem = summarize(memcpy_t);
  const Stats sA = summarize(a_t);
  const Stats sB = summarize(b_t);
  const Stats sC = summarize(c_t);
  const Stats sD = summarize(d_t);

  auto line = [&](const char* name, const Stats& s) {
    std::printf("  %-30s min %6.3f  median %6.3f  (%.2f ns/px)\n", name, s.min_ms, s.med_ms,
                s.med_ms * 1e6 / static_cast<double>(count));
  };
  // 排除一个混淆项：同一段代码写「普通堆内存」vs「DIB 位图内存」是否差很多？
  // 若差很多，则探针之间不可直接比绝对值（只能各自比相对值）。
  std::vector<double> heap_t;
  std::vector<std::uint32_t> heap(count, 0);
  for (int i = 0; i < kIterations; ++i) {
    auto t0 = Clock::now();
    swap_pairwise_unrolled(source.data(), heap.data(), count);
    heap_t.push_back(ms_since(t0));
  }
  const Stats sHeap = summarize(heap_t);
  std::printf("[耗时]（median，ms）\n");
  line("下限 memcpy", sMem);
  line("A 现状（逐通道拆拼）", sA);
  line("B 64位成对互换", sB);
  line("C 64位展开 ×8", sC);
  line("D 字节重排 ×4", sD);
  line("C 写到堆内存（对照）", sHeap);

  std::printf("\n[相对现状]\n");
  std::printf("  B 比 A 快 %.3f ms（%.0f%%），距 memcpy 下限还差 %.3f ms\n", sA.med_ms - sB.med_ms,
              (sA.med_ms - sB.med_ms) / sA.med_ms * 100.0, sB.med_ms - sMem.med_ms);
  std::printf("  C 比 A 快 %.3f ms（%.0f%%），距 memcpy 下限还差 %.3f ms\n", sA.med_ms - sC.med_ms,
              (sA.med_ms - sC.med_ms) / sA.med_ms * 100.0, sC.med_ms - sMem.med_ms);
  std::printf("  D 比 A 快 %.3f ms（%.0f%%），距 memcpy 下限还差 %.3f ms\n", sA.med_ms - sD.med_ms,
              (sA.med_ms - sD.med_ms) / sA.med_ms * 100.0, sD.med_ms - sMem.med_ms);

  std::printf("\n结论: %s\n", all_equal ? "全部写法逐字节等价（可安全替换）" : "有写法结果不一致（不可用）");
  return all_equal ? 0 : 1;
}
