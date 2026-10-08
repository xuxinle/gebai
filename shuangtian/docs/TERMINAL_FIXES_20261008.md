# 终端能力修复轮（2026-10-08）——光标错位 / 输入回显 / 多会话串台

> 范围：`st::ui::Terminal`（组件层）、`st::text::AnsiScreen`（屏幕模型）、
> `st::process::PtySession`（PTY 平台层）三层全面体检与修复。
> 方法论：逐层探针验证（PTY 层用裸 C 探针、组件层用独立 cpp 探针），
> 每项修复配套回归测试；全部结论以实测为准（见下「根因表」）。

## 根因表（用户可见症状 → 代码根因）

| 症状 | 根因 | 修复 |
|---|---|---|
| **光标逐行漂移**（输出几行后错位） | `arrange` 用「字号×0.6」估格宽上报 PTY 列数，`paint_content` 却用 `measure_width("M")` 实测格宽渲染——shell 认为的换行列与实际渲染的换行列**错开** | `refresh_cell_metrics()` 统一两处口径（缓存到成员，字体/缩放变化时刷新） |
| **中文行光标错位** | 同上（宽字符按 2 列推进，但列宽两套数） | 同上 |
| **多标签输出串台**（后台标签的内容喂进前台屏幕） | `pty_pending_` 是组件级**单缓冲**，各会话读线程都往里塞；`pump` 只喂「当前会话」 | 缓冲下沉到 `TerminalSession`（每会话一份），`pump` 逐会话喂；PTY 退出收尾也从「只收 current」改为逐会话 |
| **标签标题错标** | OSC 标题变化回调硬发 `active_`（当前标签号）而不是数据归属的会话 | 按会话指针反查容器序号再回调 |
| **vim 里看不到光标** | 渲染时 `!in_alt_screen()` 把备用屏光标禁了——vim/htop 的插入光标正是屏幕状态 | 备用屏照画（回看滚动时才不画） |
| **写满一行时光标多跳一格** | `pending_wrap` 挂起态时光标应停在最后一格，渲染却画在 `cols`（越界+视觉跳格） | 挂起态回退一列再画 |
| **滚出屏幕的内容看不见** | `AnsiScreen` 维护了 4000 行 scrollback 但组件从不渲染 | 滚轮事件驱动 `scrollback_offset`，`paint_content` 把回看行画在屏幕上方；贴底跟随（新输出到达自动回底，用户上翻时不打扰） |
| **Alt+b/f、Ctrl+←→、Shift+方向 全失灵** | 键盘映射只认裸键——Alt 组合、修饰方向键直接丢弃 | xterm 标准编码补齐：Alt+字母=`ESC`前缀、Ctrl/Shift/Alt+方向=`CSI 1;n{ABCDEF}`、Insert |
| **大段粘贴可能冻 UI** | master 端阻塞写（头文件自己点名的高频错） | `O_NONBLOCK` + `poll` 有限轮询读 + 写侧 EAGAIN 限时重试（40×5ms） |
| **中文行删字符后整行错位** | DCH（`CSI P`）按纯格数删——宽字符被劈成两半，行里留孤儿 continuation | 宽字符感知删除：删除点在主格时整个宽字算一个单位；落在右半时回退到主格 |
| **vim 设光标色后序列错位** | OSC 的 `ESC` 后非 `\` 字节被无条件吞一个（`ESC[` 的 `[` 被吃） | 只在 `ESC \`（ST）时结束 OSC；其余回 OSC 状态继续收集 |
| **表格横线画不出** | `CSI b`（REP）未实现 | 记录上个写入字符 `last_graphic_`，REP 重复之 |
| **缩窄切开宽字符留半张脸** | resize 截断判据写反 | 按「被截掉的首格是 continuation」判劈开，界内左半补空；防御性清孤儿 |
| **`login_tty` 编译失败（Linux/部分 musl）** | 声明在 `<utmp.h>` 且需 `_GNU_SOURCE`；老 glibc 没有 | 文件内定义 `_GNU_SOURCE` + `pty_login_tty` 内联包装（glibc≥2.28 用库的，否则三步退回） |
| **会话切换后行模式文本偶发不刷新** | `last_output_` 缓存跨会话未失效 | 切换/关闭会话时清空 |

## 平台层（`platform_pty.cpp` POSIX 分支）

- master 设 `O_NONBLOCK`；`read` 改 `poll(100ms)` 有限轮询 + `done` 标志检查——
  保证 `terminate()` 关 fd 后读**确定**返回（之前无限期 poll 对被关 fd 的行为不可靠）。
- `terminate()` 先置 `done` 再 close（读线程 100ms 内必观察到）。
- `write` EAGAIN/EINTR 限时重试（40×5ms≈200ms），超限返回已写量——调用方
  （`Terminal::send_bytes`）的补齐循环会继续，彻底卡住不如部分写入。

## 测试修复（测试专用 shell 策略）

- **仅测试**用固定 shell：POSIX = `/bin/bash --noprofile --norc -i` + `PS1=st-test# `
  （提示符完全确定、不受 dotfiles/CI 重定向影响）；Windows = PowerShell。
  **产品代码不动**——`default_shell()` 仍按系统默认（`GEBAI_TERMINAL_SHELL` >
  `SHELL` > `/bin/sh`）。
- 环境坑（实测记录）：测试框架把 stdout 重定向到文件时，dash 判定
  「stdin 是 tty 但 stdout 不是」→ **不打印提示符**，等任何提示符特征都超时；
  zsh 无 tty 时 dotfiles 吐错刷屏。bash + 固定 PS1 一劳永逸。
- `pty_child_sees_a_real_terminal` 等命令**输出**（`pts`）而非回显（`tty`）——
  回显先到、输出后到，等回显会在输出前断言（时序竞态）。
- `pty_resize` 的 POSIX 分支等 `stty size` 的输出（`40 120`）而非 `resize-ok`
  （那字符串只在 Windows 分支出现）。
- 新增回归测试：REP、OSC 转义健壮性（`ESC \` 才收尾）、DCH 宽字符语义（主格/右半
  两种删除点）、resize 截断孤儿清理。

## 验证

- 全量测试：**936 通过 / 0 失败**，`st lint` 0 违规。
- PTY 层探针（组件同款 jthread 读模式）连跑 5 次稳定：提示符、回显、命令执行全通。

## 追加轮（2026-10-08 续）：初判"环境预存"的 3 个失败——实际两个是真 bug

首轮把 3 个失败归为"环境预存"（靠 git stash 基线对比得出）。用户要求修复，
逐条挖下去发现**其中两个是真缺陷**——而 stash 基线会同样失败，恰恰因为是
**存量 bug 而非环境依赖**（基线对比只能排除"本轮引入"，不能证明"不是 bug"）。

| 用例 | 真因 | 修复 |
|---|---|---|
| `channel_local_runs_command_and_reports_exit_code` | **`args` 里重复插了 `program`**：`LocalProcessChannel::build_argv` 先 `push_back(spec.program)`，而 `StreamHandle::open` 又插一次 → 实际执行 `/bin/sh /bin/sh -c "…"`（sh 把第二个 `/bin/sh` 当**脚本文件**解析 → 退 2、无输出）。**影响面**：`Terminal::run_program`（IDE 的构建/lint/测试按钮）传的就是 `args` + 空 `command`——所有带参集成命令全部跑不起来 | 删掉 `build_argv` 里那句（`args` 契约就是"程序参数"，不含 program） |
| `line_layout_band_never_cuts_off_the_text` | 容差 `0.01px` 小于**设计上已知的字体面差异**：`layout_line` 刻意用固定参考样本 `"Ag(|)"` 保证逐行基线一致，而汉字回退到 **CJK 面**（`ink_metrics` 的 `above` 12.73 vs 参考 11.48）→ 紧行距（×1.0）下中文墨迹顶溢出带顶 **0.042px**（亚像素） | 判据改为「溢出 ≤ **本行字体面的墨迹高度差**」——用 `ink_metrics(本行文本).above − ink_metrics(参考).above` 量化上界（不是拍脑袋的容差）。⚠ 实测踩坑：用 `shaped_ascent` 量会得 0（它是**基线**口径，与墨迹顶不是同一个量） |
| `font_default_text_uses_one_family_for_latin_and_han` | **测试漏了平台守卫**：文件头已声明"非 Windows 跳过①③"（其他三个用例都加了守卫），只有这条没加。该契约（中英文同用微软雅黑）**只在 Windows 成立**——非 Windows 系统链里拉丁走 DejaVu、汉字走 Noto CJK，本就是两个面，那是平台正常取向而**不是缺陷** | 加 `#if !defined(_WIN32) return;` 守卫（与文件头声明的口径对齐） |

**方法论教训**：「stash 基线同样失败」只能证明"不是本轮引入"，**不能**证明"不是 bug"。
把两个存量真缺陷误判为"环境问题"差点让它们留在库里——`run_program` 那条尤其严重
（IDE 集成命令全废，而不只是测试红）。

**新增回归**：`channel_local_passes_args_without_repeating_the_program`
（钉死 `args` 不得重复 `program` 的契约）。
