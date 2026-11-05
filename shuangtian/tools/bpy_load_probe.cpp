// 诊断：bpy 兼容层装载后 sys.modules 里到底有什么。
//
// 这是排查"装载无报错但 import 失败"的最小手段——直接跑 Python 看状态，
// 而不是靠猜。
#include <cstdio>
#include <string>

#include "st/python/runtime.hpp"

int main() {
  auto& runtime = st::mp::Runtime::instance();

  // ① sys.modules 里有什么
  const auto modules = runtime.run(
      "import sys\n"
      "keys = []\n"
      "for k in sys.modules.keys():\n"
      "    keys.append(str(k))\n"
      "keys.sort()\n"
      "result = ','.join(keys)\n");
  std::printf("① sys.modules = %s\n",
              modules.has_value() ? modules->value.c_str() : modules.error().message.c_str());

  // ② 能否往 sys.modules 写
  const auto write_test = runtime.run(
      "import sys\n"
      "sys.modules['zz'] = 42\n"
      "result = 'write-ok' if 'zz' in sys.modules else 'write-lost'\n");
  std::printf("② sys.modules 可写 = %s\n",
              write_test.has_value() ? write_test->value.c_str()
                                     : write_test.error().message.c_str());

  // ③ 直接 import bpy 看报什么
  const auto import_test = runtime.run(
      "import bpy\n"
      "result = 'ok'\n");
  std::printf("③ import bpy = %s\n",
              import_test.has_value() ? import_test->value.c_str()
                                      : import_test.error().message.c_str());

  // ④ 装载时的 globals 里有没有生成 _bpy 对象？
  //
  // 若 __main__ 的 globals 里搜不到，说明装载用的 dict 被丢了。
  const auto probe = runtime.run(
      "result = 'bpy' in dir()\n");
  std::printf("④ 当前 globals 里有 bpy = %s\n",
              probe.has_value() ? probe->value.c_str() : probe.error().message.c_str());
  return 0;
}
