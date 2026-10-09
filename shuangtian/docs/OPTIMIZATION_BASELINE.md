# 霜天结构优化 — 基线快照（改动前）

> 用途：优化前后对比的**唯一基准**。所有数字均为本轮实测，方法写在每项后面。
> 快照时间：基于 git `4b656d5`（工作树干净）。优化工作在分支 `optimize/structure-2026`。

## 1. 验证门禁（改动前后都必须保持）

| 门禁 | 基线值 | 复现命令 |
|---|---|---|
| 单元测试 | **700 passed / 0 failed / 17267 assertions** | `./build/bin/st test` |
| ASan/UBSan 档 | 0 报告（BACKLOG 记录） | `./build/bin/st test --san` |
| 禁令扫描 | 290 文件 / **0 违反** / 豁免 12 处 | `./build/bin/st lint` |
| 增量构建 | gallery：86 单元（重编 4 / 命中 82），编译 8.55s + 链接 0.52s = **9.48s** | `st build gallery --profile dev` |

## 2. 结构度量（改动的靶子）

| 指标 | 基线值 | 测量方法 |
|---|---|---|
| 自研代码行数（src + include） | 约 59,000 | `find src include -name '*.{cpp,hpp}' \| xargs wc -l` |
| 测试行数 | 23,445（74 文件） | 同上 `tests/` |
| `sizeof(Element)` | **432 B** | 探针编译 `sizeof()` |
| `sizeof(Text/Button/Input/Table/CodeEditor)` | 496 / 544 / 608 / 584 / **888** | 同上 |
| `Element` 成员数 / 虚函数数 | 50 / 24 | `include/st/ui/element.hpp` |
| 直接继承 `Element` 的组件数 | **42**（仅 `Heading : Text` 例外） | `grep 'class X : public Element'` |
| `raster::Surface` 纯虚函数数 | **34** | `grep '= 0;' include/st/raster/surface.hpp` |
| `dsl.cpp` 中组件类型身份的位置 | **4 套**（`type()` / `ST_DSL_TYPE` 40 条 / `make_element` 40+ if / `c.xxx()` 入口 dynamic_cast） | 见评审 A1 |
| `dynamic_cast` 总数 | 25 | `grep -rc dynamic_cast src include` |
| 传递闭包重编面 | `element.hpp` → **76** 个 .cpp；`surface.hpp` → **100** 个 .cpp | JS 传递包含闭包统计 |
| 公共头泄漏 nlohmann/json（25,526 行） | **8 个公共头** | `grep -rl 'st/ext/json.hpp' include/st` |

## 3. 巨型函数（>200 行）

| 位置 | 行数 | 说明 |
|---|---|---|
| `src/ui/components/code_editor.cpp:463 insert_with_pairs` | 1500+（含大段注释；同文件另有多个长函数） | 编辑命令族 |
| `src/control/server.cpp:639 handle` | **840**，23 个 `method ==` 分派 | 整个协议栈一个函数 |
| `src/text/highlight_builtin.cpp:39 builtin_languages` | **511**（占全文件 93%） | 40 种语言数据表 |
| `src/pkg/build.cpp:761 compile_units` | 345 | 编译/链接/缓存混合 |
| `src/pkg/lint.cpp:158 check_mutable_globals` | 343 | 文本级 lint |
| `src/shell/platform_win32.cpp:926 handle_message` | 305 | 消息循环 |
| `src/md/markdown.cpp:866 parse_blocks` | 270 | |
| `src/text/grid_fit.cpp:270 grid_fit` | 255 | |

## 4. 已知缺陷/纪律缺口（P1 靶子）

| # | 位置 | 问题 |
|---|---|---|
| ① | `src/ui/dsl.cpp:42` | `active_composers` 命名空间级 `unordered_set`，`on_state_write` 里**无锁遍历**；而同处其他并发路径（`inbox_mutex`、`post_to_main`）都有锁 → 潜在数据竞争/迭代器失效 UB |
| ② | `src/text/highlight.cpp:730` | `static LanguageRegistry registry`（进程级、函数内静态）静态析构顺序未定义 |
| ③ | `CONVENTIONS.md:315` vs `src/pkg/lint.cpp` | 文档宣称 lint 检查「禁用 include」（`<windows.h>` 仅限 `platform_*`），**实现里没有该规则**；`src/core/entry.cpp:12` 实际 include 了 `windows.h` |
| ④ | `src/pkg/lint.cpp:158` | L8 只在 `line_depth == 0` 判定 → **任何 namespace（匿名/具名）内的可变全局 + 函数内 static 全部漏网** |
| ⑤ | `src/ui/element.hpp:300-305` | `owner_` 为 `void*` + `static_cast` 逃生舱（依赖倒置做得不彻底，类型不安全） |
| ⑥ | 仓库根 | 45 个 core dump（约 1.5 GB）+ `_mesh_block.txt` 游离文件 |

## 5. 复现探针

对象尺寸探针保留在 `tools/structure_probe.cpp`（`sizeof` 实测；该文件已删——本轮结论已固化为下述数字，后需复测请重建同名探针），改动后重跑同一探针对比。
