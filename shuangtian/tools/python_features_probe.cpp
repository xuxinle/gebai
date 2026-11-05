// 探测 MicroPython（MINIMUM 档）支持哪些 Python 语法特性。
//
// ## 为什么需要
//
// 写 bpy 兼容层时用了 `__slots__` / `property` / `@staticmethod` /
// 推导式 / `*args` 等特性，而 MINIMUM 档可能剪掉了其中一部分。
//
// 失败信息只有 `NameError: name not defined`——**没说是哪个名字**，
// 因为 MINIMUM 档剪掉了异常参数的格式化能力。所以只能一条条试。
#include <cstdio>
#include <string>

#include "st/python/runtime.hpp"

namespace {

struct Case {
  const char* label;
  const char* source;
};

}  // namespace

int main() {
  auto& runtime = st::mp::Runtime::instance();
  const Case cases[] = {
      {"基础类", "class A:\n    def __init__(self):\n        self.x = 1\nresult = A().x"},
      {"__slots__", "class A:\n    __slots__ = ('x',)\n    def __init__(self):\n        self.x = 1\nresult = A().x"},
      {"property", "class A:\n    def __init__(self):\n        self._x = 2\n    @property\n    def x(self):\n        return self._x\nresult = A().x"},
      {"staticmethod", "class A:\n    @staticmethod\n    def f():\n        return 7\nresult = A.f()"},
      {"列表推导式", "result = sum([i * 2 for i in range(3)])"},
      {"生成器表达式", "result = sum(i * 2 for i in range(3))"},
      {"*args", "def f(*a):\n    return len(a)\nresult = f(1, 2)"},
      {"**kwargs", "def f(**k):\n    return len(k)\nresult = f(a=1)"},
      {"类属性", "class A:\n    K = 5\nresult = A.K"},
      {"继承", "class A:\n    def f(self):\n        return 1\nclass B(A):\n    pass\nresult = B().f()"},
      {"__len__", "class A:\n    def __len__(self):\n        return 3\nresult = len(A())"},
      {"__iter__", "class A:\n    def __iter__(self):\n        return iter([1,2])\nresult = sum(A())"},
      {"__getitem__", "class A:\n    def __getitem__(self, i):\n        return i * 2\nresult = A()[3]"},
      {"字符串 join", "result = ','.join(['a', 'b'])"},
      {"f-string", "x = 3\nresult = f'v={x}'"},
      {"% 格式化", "result = '%d-%s' % (3, 'a')"},
      {"sum 生成器带条件", "result = sum(i for i in range(6) if i % 2 == 0)"},
      {"dict keys", "d = {'a': 1}\nresult = ','.join(d.keys())"},
      {"异常捕获", "try:\n    1 / 0\nexcept Exception as e:\n    result = 'caught'"},
      {"类内双下划线属性", "class A:\n    def __init__(self, n):\n        self.__name__ = n\nresult = A('x').__name__"},
      // ## 内建函数：MINIMUM 档剪掉的比直觉多得多
      //
      // 实测踩到 `max` 缺失：一个 400 行的齿轮脚本因此整个建不出来，
      // 而报错只有 `NameError: name not defined`（没有名字）。
      // 所以把 bpy 语料里常见的内建全列出来逐条验。
      {"max", "result = max(1, 2)"},
      {"min", "result = min(1, 2)"},
      {"abs", "result = abs(-3)"},
      {"sum", "result = sum([1, 2, 3])"},
      {"round", "result = round(1.6)"},
      {"pow", "result = pow(2, 3)"},
      {"divmod", "result = divmod(7, 2)[0]"},
      {"sorted", "result = sorted([3, 1, 2])[0]"},
      {"reversed", "result = list(reversed([1, 2]))[0]"},
      {"enumerate", "result = list(enumerate([7]))[0][0]"},
      {"zip", "result = list(zip([1], [2]))[0][0]"},
      {"map", "result = list(map(str, [1]))[0]"},
      {"filter", "result = list(filter(None, [1]))[0]"},
      {"any", "result = any([0, 1])"},
      {"all", "result = all([1, 1])"},
      {"isinstance", "result = isinstance(1, int)"},
      {"hasattr", "result = hasattr([], 'append')"},
      {"set", "result = len(set([1, 1, 2]))"},
      {"tuple", "result = len(tuple([1, 2]))"},
      {"frozenset", "result = len(frozenset([1, 1]))"},
      {"super()", "class A:\n    def f(self):\n        return 1\nclass B(A):\n    def f(self):\n        return super().f() + 1\nresult = B().f()"},
  };
  int failed = 0;
  for (const auto& item : cases) {
    const auto result = runtime.run(item.source);
    if (result.has_value()) {
      std::printf("  [OK  ] %-20s result=%s\n", item.label, result->value.c_str());
    } else {
      std::printf("  [FAIL] %-20s %s\n", item.label, result.error().message.c_str());
      ++failed;
    }
  }
  std::printf("\n失败 %d / %d\n", failed, static_cast<int>(sizeof(cases) / sizeof(cases[0])));
  return 0;
}
