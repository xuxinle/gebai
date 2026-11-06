// mp_runtime_smoke_probe —— `st_mp_init` → `st_mp_run` 的最小闭合检查。
//
// 为什么单独做这一支：`mp_reinit_probe` 在**主线程第一次 `run()`** 就崩了（输出停在
// 步骤①之后），这比“跨线程”更近——说明 `st_mp_init` 之后的**第一次解释执行**就不成立。
// 先用最小面把这件事钉死，再谈线程。
//
// 三档，逐步加码：
//   A. 只 `st_mp_init()` → 看返回值
//   B. `init` + 跑一句 `result = 1` → 看返回值
//   C. `init` + 跑 `import bpy` 那句（真实用例的最小面）
//
// 每步都 fflush：崩溃时 stdio 缓冲会丢，不刷就什么都看不到。

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

#include "st/python/runtime.hpp"

namespace {

void say(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  std::vfprintf(stdout, fmt, args);
  va_end(args);
  std::fflush(stdout);
}

/// 直接调 st_mp_* 的最小封装（绕过 `Runtime` 的所有纪律，只看"桥本身通不通"）。
extern "C" {
int st_mp_init(char* err, size_t err_cap);
int st_mp_run(const char* source, size_t source_len, const char* filename, size_t filename_len,
              char* out, size_t out_cap, char* err, size_t err_cap);
void st_mp_deinit(void);
}

}  // namespace

int main() {
  std::vector<char> out(256 * 1024, '\0');
  std::vector<char> err(256 * 1024, '\0');

  say("[A] st_mp_init\n");
  const int init_rc = st_mp_init(err.data(), err.size());
  say("    rc=%d err='%s'\n", init_rc, err.data());
  if (init_rc != 0) return 1;

  const struct {
    const char* label;
    const char* source;
  } cases[] = {
      {"B(简单赋值)", "result = 1 + 1\n"},
      {"C(import bpy)", "import bpy\nresult = 'ok'\n"},
  };
  for (const auto& item : cases) {
    err.assign(err.size(), '\0');
    out.assign(out.size(), '\0');
    say("[%s] st_mp_run\n", item.label);
    const int rc = st_mp_run(item.source, std::strlen(item.source), "<probe>", 7, out.data(),
                             out.size(), err.data(), err.size());
    say("    rc=%d out='%s' err='%s'\n", rc, out.data(), err.data());
  }

  say("[D] st_mp_deinit\n");
  st_mp_deinit();
  say("==== 全程未崩 ====\n");
  return 0;
}
