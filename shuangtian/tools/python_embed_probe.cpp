// 最小闭环：内嵌 MicroPython 能跑一段 Python 并取回结果。
//
// ## 为什么先验这个
//
// 这是整条 bpy 链路的地基。若连"跑 `result = 1 + 1` 取回 2"都做不到，
// 后面的 bpy / bmesh / mathutils 全是空谈。
//
// ## 判据分四层（层层加码）
//
// 1. 语法与算术：`result = 1 + 2` → "3"
// 2. 数据结构：list/dict/for 循环
// 3. 函数与类：bpy 脚本里的常见形态
// 4. 异常：出错时必须给出**可读的原因**，而不是静默返回空
#include <cstdio>
#include <string>

#include "st/python/runtime.hpp"

namespace {

int failures = 0;

auto check(const char* label, const char* source, const char* expect_substr) -> void {
  auto& runtime = st::mp::Runtime::instance();
  const auto result = runtime.run(source, "<probe>");
  if (!result.has_value()) {
    std::printf("  [FAIL] %-30s 执行失败：%s\n", label, result.error().message.c_str());
    ++failures;
    return;
  }
  const bool ok = result->value.find(expect_substr) != std::string::npos;
  std::printf("  [%s] %-30s result=%s\n", ok ? "PASS" : "FAIL", label,
              result->value.c_str());
  if (!ok) ++failures;
}

}  // namespace

int main() {
  std::printf("MicroPython 可用：%s\n", st::mp::Runtime::available() ? "是" : "否");
  if (!st::mp::Runtime::available()) {
    std::printf("  原因：%s\n", st::mp::Runtime::unavailable_reason().c_str());
    return 1;
  }
  std::printf("\n");

  // ① 语法与算术
  check("算术", "result = 1 + 2", "3");
  check("浮点", "result = 3.0 / 4", "0.75");
  check("字符串", "result = 'turbo' + 'fan'", "turbofan");

  // ② 数据结构 + 循环（bpy 脚本的基本形态）
  check("list + sum", "xs = [i * 2 for i in range(5)]\nresult = sum(xs)", "20");
  check("dict", "d = {'teeth': 60}\nresult = d['teeth']", "60");
  check("for + 累加",
        "total = 0\nfor i in range(1, 11):\n    total += i\nresult = total", "55");

  // ③ 函数与类（bpy 里到处是 "定义一个造齿轮的函数"）
  check("函数",
        "def pitch(n):\n    return 6.283185 / n\nresult = round(pitch(60), 4)", "0.1047");
  check("类",
        "class Gear:\n"
        "    def __init__(self, teeth):\n"
        "        self.teeth = teeth\n"
        "    def label(self):\n"
        "        return 'z%d' % self.teeth\n"
        "result = Gear(42).label()", "z42");

  // ④ 内建能力：math 是 bpy 脚本重度依赖的
  check("math",
        "import math\nresult = round(math.sin(math.pi / 6), 4)", "0.5");

  // ⑤ 异常必须给出可读原因
  {
    auto& runtime = st::mp::Runtime::instance();
    const auto bad = runtime.run("result = 1 / 0");
    const bool ok = !bad.has_value() && !bad.error().message.empty();
    std::printf("  [%s] %-30s %s\n", ok ? "PASS" : "FAIL", "异常有可读原因",
                bad.has_value() ? "(没报错，不符合预期)" : bad.error().message.c_str());
    if (!ok) ++failures;
  }

  // ⑥ 命名空间隔离：上一次的 result 不该泄漏到下一次
  {
    auto& runtime = st::mp::Runtime::instance();
    (void)runtime.run("result = 'leaked'");
    const auto fresh = runtime.run("x = 1");
    const bool ok = fresh.has_value() && fresh->value.empty();
    std::printf("  [%s] %-30s result=%s\n", ok ? "PASS" : "FAIL", "命名空间隔离",
                fresh.has_value() ? (fresh->value.empty() ? "(空)" : fresh->value.c_str())
                                  : fresh.error().message.c_str());
    if (!ok) ++failures;
  }

  std::printf("\n");
  if (failures > 0) {
    std::printf("失败 %d 项\n", failures);
    return 1;
  }
  std::printf("最小闭环全部通过\n");
  return 0;
}
