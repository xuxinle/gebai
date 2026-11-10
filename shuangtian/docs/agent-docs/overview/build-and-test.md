---
title: 构建与验证 · st build / test / lint
summary: 自研工具链 stpm；内容哈希增量判定；三档 profile；测试与 lint 的正确跑法与常见假绿。
---

# 构建与验证

## 一句话

一切经 `st`（自研工具链 `stpm`）：`st build <target> [--profile dev|quick|release|debug]`、
`st test [filter]`、`st lint`、`st check --toolchain=clang`。首次要先自举（`bootstrap.sh` / `bootstrap.ps1`）。

## profile 怎么选

| 档 | 用途 | 特征 |
|---|---|---|
| `dev` | **日常迭代主用** | 优化 + `-g1` 轻量调试信息；增量秒级 |
| `quick` | 最快迭代 / 临时验证 | `-O0 -g1`，全量最快 |
| `release` | 发行 | 无调试信息、最高优化 |
| `debug` | **回归测试默认档** | 无优化 + 完整调试信息；`tools/*_e2e.py` 的 `ST_PROFILE` 默认就是它 |
| `san` | ASan + UBSan | `st test --san`；**Windows 上没有**（本机是 Windows 时要在 Linux 上跑） |

> ⚠ 只重建 `dev` 而测试跑 `debug`，会得到"代码明明改了、测试还在用旧二进制"的假象。

## 增量判定只看**内容哈希**

`.meta` 是 `path<TAB>content-hash`——没有 mtime、没有 size。所以：

- `git checkout` / `stash pop` / `touch` / 编辑器原子保存**整批刷新 mtime 但内容没变** ⇒ 不会白等一轮；
- 反过来，"等长改写 + 保留时间戳"骗不过它；
- 看到 **`重编 0`** 不等于"我的改动没生效"——先做两件比 `--force` 便宜的取证：
  ① 查 `.meta` 里的哈希是不是当前文件内容的哈希；② 在产物二进制里搜一条**本次新加的字符串**。
  两项都否才真是没编。

三个"缓存"要分清（可以独立发生）：

| 名字 | 判据 | 意味着 |
|---|---|---|
| **对象复用**（`units_cached`） | 源内容哈希未变 | 不重编 `.o` |
| **链接跳过**（`linked`） | 产物比全部链接输入新 | 不重跑链接器（会给理由） |
| 内容哈希缓存 | 一次构建内去重 | 不可见 |

## 测试

- `st test`——**无条件重编 + 重链**再跑（所以它能自愈"旧测试二进制"问题）；
- `st test <filter>`——按用例名过滤；
- `st build <target>` **不链测试二进制**：改了共享头（尤其带尺寸的 `inline constexpr`）后只跑
  `st build`，旧测试二进制会按旧尺寸链接，运行期崩而编译期零告警。**改共享头后用 `st test` 验证。**

## lint

`st lint` 扫禁用特性（CONVENTIONS §2）与头文件纪律等硬红线。新增规则时可用 `--rules` 列出规则名
确认真跑的是新二进制（在 exe 里搜字符串**不能**证明新逻辑生效）。

## 跨编译器拓缺陷

GCC 不报的问题用 clang 再编一遍：`st check --toolchain=clang`（见 `docs/BUILD_CHECK.md`）。

## 基线（实测，注意口径）

全量 `st build gallery --profile dev` ≈ 75 s（83 个翻译单元，4 核配额）；改 1 个 `.cpp` ≈ 2.9 s；
改 1 个公共头（32 单元受影响）≈ 38 s；无改动 ≈ 0.6 s。换机器/换编译器这些数字都会变，
**引用时带口径**（见 `docs/ITERATION_SPEED.md`）。

## 更细

- 构建系统内部（求解/锁/构建图/按需链接/合成目标）：`DESIGN.md` §7
- 引导与共享对象缓存：`DESIGN.md` §7.7 · §7.11
- 跨平台构建约束（`--toolchain=mingw` 什么时候是空转）：`overview/cross-platform.md`
- 迭代速度基线：`docs/ITERATION_SPEED.md`
