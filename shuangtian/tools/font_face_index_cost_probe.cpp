// 定位 `FontStack::system_default()` 热调用为何不是“亚毫秒”。
//
// 假设（已由本次探针证实）：`load_face_cached` **失败不缓存**，而 `load_face` 对 CJK
// 集合字体原本循环 `index = 0..11` 逐个试 face —— 集合里没有的下标每次都**重新读盘失败**
// （`msyh.ttc` 20 MB，实测单次 9 ms），于是“缓存生效”被这些反复失败盖住。
//
// 用法：pwsh -File tools/build_probes.ps1 -Only font_face_index_cost_probe
//       build/probe/font_face_index_cost_probe.exe
//
// 只报数：逐 (路径, face_index) 给出 FontFace::load 的单次耗时与成功/失败。

#include <chrono>
#include <string>
#include <vector>

#include "st/core/font_platform.hpp"
#include "st/core/print.hpp"
#include "st/text/font.hpp"

namespace {

using Clock = std::chrono::steady_clock;

double once_ms(const std::string& path, int index, bool* ok) {
  const auto t0 = Clock::now();
  auto face = st::text::FontFace::load(path, index);
  const auto t1 = Clock::now();
  *ok = face.has_value();
  return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

}  // namespace

int main() {
  const auto text = st::platform::preferred_text_fonts();
  st::print("=== 逐候选 × face_index 的 FontFace::load 单次耗时 ===\n");
  double total_first = 0.0;            // 首次（缓存冷）总耗时
  double uncached_fail = 0.0;          // 其中的「失败」部分（每次调用都会重付）

  for (const auto& e : text) {
    if (!e.cjk) continue;   // 只有 prefer_cjk_face 才走下标探测
    st::print("\ncjk 候选: {}\n", e.path);
    for (int index = 0; index < 12; ++index) {
      bool ok = false;
      const double ms = once_ms(e.path, index, &ok);
      total_first += ms;
      if (!ok) uncached_fail += ms;
      st::print("   index={:2}  {}  {:>8.3f} ms\n", index, ok ? "OK  " : "FAIL", ms);
      if (!ok) break;   // 原实现遇失败即 break，后面的下标不会试
    }
  }

  st::print("\n=== 非 CJK 候选（只试 index 0）===\n");
  for (const auto& e : text) {
    if (e.cjk) continue;
    bool ok = false;
    const double ms = once_ms(e.path, 0, &ok);
    total_first += ms;
    st::print("   {}  {}  {:>8.3f} ms\n", ok ? "OK  " : "FAIL", e.path, ms);
  }

  st::print("\n合计（冷）           {:>8.3f} ms\n", total_first);
  st::print("其中「失败」部分      {:>8.3f} ms  ← 失败不进缓存 ⇒ 每次调用重付\n",
            uncached_fail);
  return 0;
}
