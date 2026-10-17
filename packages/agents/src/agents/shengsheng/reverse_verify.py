#!/usr/bin/env python3
"""生生的**逆向验证**门：逐条回退 → 断言必须变红 → 把源码**验明**恢复原样。

为什么必须做（本项目 `CONVENTIONS.md` §7.1/§7.3，以及生生自身纪律「加了机制 ≠ 机制生效」）：
不做这一步很容易交付一个"恒绿的测试"——它看起来像护栏，实际什么都拦不住。
本仓已实测踩到三种假绿形态：
  ① 桩把差异抹平（§7.2）；
  ② 回退文本编译不过 ⇒ 跑的是上一份二进制（§7.3）；
  ③ **回退后修复与缺陷行为相同**（测例选得没判别力）——本轮上一单就栽在这。

本脚本的每条用例都：
  ① 用**能通过 tsc** 的最小改动回退（否则 Bun 会跑到旧的？不——Bun 不缓存，但语法错会让整文件
     加载失败，看起来像"全部通过"的假象，所以先确认 bun test 真跑起来了）；
  ② 恢复时**原子回填 + 回读逐字节校验**：不是「把备份拷回去就完事」——回填后再读一遍源码，
     与该用例开跑前的字节快照**逐字节**比较；不一致就告警（文件/偏移/行/重试次数）、重试，
     仍不一致则把该用例**计入失败**（源码状态不可信 ⇒ 它的红绿结论也不可信）；
  ③ 判据是「指定用例变红」+「用例数不为 0」（0 个跑起来 = 无效，不算通过）。

中断安全（SIGINT / SIGTERM）——本单补的第二道保险：
  · 信号处理器**只置标志**，不在处理器里做恢复。信号可能在任意字节码边界到达（包括
    `_write_atomic` 写到一半、`os.replace` 前后），在处理器里再发起写操作会**重入文件写入**，
    把源码写成半个文件——那正是本单要消灭的中间态。
  · 控制流由 `_check_interrupt()` 在**被保护区域之外沿**送出 `Interrupted`，于是该用例的
    `finally` 照常跑完（回填 + 校验），异常再穿出到 `main()` 收尾。
  · 退出码 SIGINT=130 / SIGTERM=143，并明确打印「已被中断，已恢复源码到原样」。
  · 用自定义的 `signal.signal` 处理器（而非默认的 KeyboardInterrupt）还有一个理由：
    默认处理器会在 `subprocess.run` 里抛 KeyboardInterrupt，**正在跑的 bun test 变孤儿**
    继续读源码，与 `finally` 的回填赛跑。自定义处理器让当前子进程先结束（或随进程组
    一起被 SIGINT 带走），回填时前台没有读者。

**这道保险在什么情况下会失效（诚实声明，别当万能）**：
  ① SIGKILL / 断电 / OOM killer：任何 Python 代码都不会执行，`finally` 不会跑。
     源码可能停在回退态，救援只剩磁盘上的 `.revbak`——下一轮起跑前的残留恢复段
     会把它搬回来并**显式报告**（那条报告就是说「上一轮没完成」）。
  ② 磁盘满 / 只读挂载 / 权限被改：回填本身失败。此时脚本会打印 `[恢复失败]`、
     把用例计入失败、并以 2 退出；但**工作树确实已被污染**，需要人工
     `git checkout -- <文件>`。脚本只能如实报告，不能创造磁盘空间。
  ③ 恢复后到退出之间的那一小段：若此时被 SIGKILL，源码是好的，但可能残留 `.revbak`
     （删除动作没执行完）——同样由下一轮的残留恢复兜住。
  ④ 本脚本只保护**它自己改的那一个文件**；若同时有别的进程/人在改同一棵树，
     字节比对会报「未回到原样」——那是真的有人改了，不是误报。并发验证由 `verify_guard` 在起跑前
     拦下（默认拒绝，`--force` 可越过）；但**改了名的**验证进程、容器/PID 命名空间里的进程、
     以及「锁文件说没人但真有人在写」的情形，本脚本看不见——判据是命令行文本，不是「谁在写这个文件」。
  ⑤ 信号在 `run_tests()` 的 600s 超时窗口内到达且只发给本进程（非进程组）时，
     当前那条 bun test 会跑完再中断——收尾会变慢，但源码仍会恢复（不抢跑是故意的）。

用法：python3 packages/agents/src/agents/shengsheng/reverse_verify.py [--list] [--force]

**并发防护（与演练脚本共用 `verify_guard.py` 一份实现）**——本单补的第三道保险：
  · 起跑前扫同机命令行：有别的 `*reverse_verify*.py` 或 `bun test` 在跑 ⇒ **默认拒绝并退出码 3**，
    打印「同树有并发验证在跑：<pid> <cmd>；如确认要跑请加 --force」。
    为什么必须默认拒绝：门禁临时把 `shengsheng.ts` 改成回退版，两个门禁同时跑会互相回退/回填同一份
    源码——09:39 就这样让一个 worker 报「莫名变红」（门的残留冒充真缺陷）。
  · `--force`：显式覆盖才跑，且**输出开头先写**「已按 --force 跳过并发检测，结果可能在竞态下不可信」。
  · 锁文件 `.reverse_verify.lock`（pid / boot_id / 时间 / 命令行），退出时删；陈旧锁按「pid 是否存活 +
    是否同一次开机」自清。
  · 排除自己/祖先/后代进程：`pgrep -af reverse_verify` 会命中它自己那条命令，不排除则「永远有人在跑」。

演练钩子（供外部演练脚本用，默认不生效）：
  REVERSE_VERIFY_TEST_PAUSE_AFTER_ROLLBACK=1  回退写入后暂停，等待外部信号
  REVERSE_VERIFY_TEST_PAUSE_SECONDS=60        暂停上限（秒，收到信号立即跳出）
"""
from __future__ import annotations

import argparse
import hashlib
import os
import pathlib
import re
import shutil
import signal
import subprocess
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))   # 同树同目录的 verify_guard.py
import verify_guard            # noqa: E402  （并发防护 + 信号收尾通道，与演练脚本共用一份实现）

ROOT = pathlib.Path(__file__).resolve().parents[5]  # → /home/x/code/gebai（[0]=本目录 … [3]=packages/agents … [5]=仓库根）
SRC = ROOT / "packages/agents/src/agents/shengsheng/shengsheng.ts"
# 第二个被保护的目标：提交范围判据（宽范围暂存的纪律与检测）。
# 为何单列一份而不是把它的实现搬进 shengsheng.ts：判据（git 输出解析、路径覆盖）是纯函数，
# 独立成模块才能**单独回退它**并看用例变红——搬进 3000 行的主文件会让逆向验证的回退锚点
# 与别处的代码相互干扰（
# `commit-scope.ts` 头部说明了同型边界）。
SRC_SCOPE = ROOT / "packages/agents/src/agents/shengsheng/commit-scope.ts"
# 第三个被保护的目标：`pgrep -f` 自匹配活锁的判据（review 的自动检测 + 派工模板里的等待纪律同源）。
SRC_PGREP = ROOT / "packages/agents/src/agents/shengsheng/pgrep-lock.ts"
# 所有被保护件的「起跑前」快照（收尾自证要对**每一个**都比字节，不只比主文件）
PROTECTED = (SRC, SRC_SCOPE, SRC_PGREP)
TEST_DIR = ROOT / "packages/agents/src/agents/shengsheng"
TSC = ROOT / "packages/agents/node_modules/.bin/tsc"

ANSI = re.compile("\x1b\\[[0-9;]*m")

ROLLBACK_SUFFIX = ".revbak"   # 崩溃兜底：进程被硬杀后唯一还能救源码的东西
TMP_SUFFIX = ".revtmp"        # 原子写的临时文件（不含有效内容）
RESTORE_ATTEMPTS = 3          # 回填校验失败的重试次数
INT_EXIT = verify_guard.INT_EXIT          # {SIGINT:130, SIGTERM:143}——口径只一份（在 verify_guard）
LOCK = verify_guard.LOCK_NAME             # .reverse_verify.lock
EXIT_CONCURRENT = verify_guard.EXIT_CONCURRENT   # 3：被并发检测拒绝

PAUSE_ENV = "REVERSE_VERIFY_TEST_PAUSE_AFTER_ROLLBACK"
PAUSE_SECS_ENV = "REVERSE_VERIFY_TEST_PAUSE_SECONDS"
# 演练钩子②：把回填拉慢，给“收尾中途再打一次信号”制造一个确定的窗口（默认不生效）
SLOW_RESTORE_ENV = "REVERSE_VERIFY_TEST_SLOW_RESTORE_MS"
# 演练钩子③：把信号通道**降级**成「第一次信号后就恢复默认处置」——用来反向证明
# “收尾中的第二次信号确实能把进程打死、并把源码留在回退态”（否则演练断言可能是空转的）。
TRAP_DEGRADE_ENV = "REVERSE_VERIFY_TEST_TRAP_DEGRADE"


class Case:
    def __init__(self, name: str, old: str, new: str, expect_fail: list[str], note: str = "",
                 src: pathlib.Path | None = None):
        self.name = name
        self.old = old
        self.new = new
        self.expect_fail = expect_fail
        self.note = note
        # 回退哪一份文件（缺省主文件）：判据分散在多个模块时，「回退哪一处」必须是每条用例自带的
        # 信息，否则会把“另一个文件里的锚点”当成“代码已变，请更新脚本”跳过（那是最坏的一种空转）。
        self.src = src or SRC
    @property
    def name_of_file(self) -> str:
        """被回退的文件名（日志用）。"""
        return self.src.name


CASES = [
    Case(
        "并发写：去掉路径锁",
        # 回退：让锁变成"直接执行"（保留 withPathLock 名字，最小改动、能过 tsc）
        """function withPathLock<T>(path: string, fn: () => Promise<T>): Promise<T> {
  const prev = pathLocks.get(path) ?? Promise.resolve()""",
        """function withPathLock<T>(path: string, fn: () => Promise<T>): Promise<T> {
  void pathLocks
  return fn()
  const prev = pathLocks.get(path) ?? Promise.resolve()""",
        ["并发 seed_add", "并发 note", "并发 dispatch"],
        "锁一旦退化为直通，三处并行丢写立现（这是本轮真往返撞出来的原缺陷）",
    ),
    Case(
        "上下文水位：两个量合并回一个（建议线冒充模型窗口）",
        # 原用例是「退回裸数字」——那个缺陷已被更严格的本条取代：现在真正致命的退化是
        # **合并两个量**（window := advisory），即把「建议线」当「模型窗口」用（ctx 224K 于是又被报成“超上限”）。
        # 回退写法只改赋值源、不改类型（`window` 仍是 number），故能过 tsc——
        # 回退版编译不过时 Bun 会加载失败，看起来像“全部通过”（CONVENTIONS §7.3 的假绿形态之一）。
        "  const { window, advisory } = scale\n",
        "  const advisory = scale.advisory\n  const window = advisory\n",
        ["两个量分开报", "ctx 224K 报", "读态与收藏的口径"],
        "两个量一合并，ctx 224K 这个现场就会被判成“过模型窗口 → 必须退役”——就是本单要消灭的那条真误报。",
    ),
    Case(
        "跨层反哺：把“送到 feed_to 指定的根”退回“只送本项目”",
        r"""            return { next: dst, result: { added: true, total: dst.seeds.length, dup: null } }""",
        r"""            void dst; return { result: { added: false as const, total: 0, dup: null } }""",
        ["跨层反哺"],
        "不把种子写进 feed_to 那条线时，调度层问题永远回不到平台自身——自举闭环断在最后一环。",
    ),
    Case(
        "init 不跨线继承：把“以目标根为准”退回“走回落链”",
        # 回退：把 initialized 的来源从「读目标根本身」换成「loadState 回落链」
        r"""      const initialized = probe.text != null""",
        r"""      const fallback = await loadState(ctx, targetRoot)
      const initialized = fallback.initialized""",
        ["init 不跨线继承"],
        "走回落链时，env 指 A 而 init B → 读到 A 的状态作回显，却把坐标写进 B（跨线污染）。",
    ),
    Case(
        "派出去即销账：去掉种子消费",
        """          if (seeded >= 0) cur.seeds.splice(seeded, 1)""",
        """          void seeded""",
        ["派出去即销账"],
        "不销账时同一条方向会留在种子队列，下一息读态把它当待派方向再派一次（真重复烧命）。",
    ),
    Case(
        "运行态 ctx 判据：运行中不说 ctx（只剩“勿扰，下息再看”）",
        # 回退点：把「运行中也要看 ctx」那一支（过建议线的处置）关掉，只剩勿扰。
        # 写法用 `false as boolean`（而不是删整支）：最小改动、能过 tsc——回退版编译不过时
        # Bun 会加载失败，看起来像“全部通过”（CONVENTIONS §7.3 的假绿形态之一）。
        "        else if (ctxV?.level === \"over-advisory\")",
        "        else if ((false as boolean) && ctxV?.level === \"over-advisory\")",
        ["运行中也要报上下文水位"],
        "只留「勿扰」时，跑到 ctx 171K 的 worker 会得到「下息再看」这条唯一建议（它还在涨，续派就亏）。",
    ),
    Case(
        "轮次语义：round 退回全局序号",
        """          const round = (prevW?.round ?? 0) + 1""",
        """          const round = seq""",
        ["轮次语义：全新会话首派", "轮次语义：同会话复用"],
        "round 用全局 seq 时，第二个全新会话首派会写成 2（复核判别力）",
    ),
    Case(
        "独占资源租约：把“占用时顺延”退回“照旧开跑”",
        # 回退点：claimLease 里「被他人持着 → 不派、方向退回种子」那一支。
        # `req.minutes < 0` 永不成立（leaseMinutes 保证 ≥ 1）——于是每次申请都走“发租约”一支，
        # 即**后来的持者直接顶掉前一位**：两人同时开跑，正是 load 峰值 55 却仍在跑门禁那次事故的原形。
        # 用表达式而不是字面量 `false`：字面量可能有 unreachable-code 诊断，使回退版本“编译不过”，
        # 那样跑到的就可能是旧/坏代码（CONVENTIONS §7.3 的假绿形态之一）。
        """    const holder = leaseOf(cur, key)
    if (holder && !leaseHeldBy(holder, req.sessionId, req.alias)) {""",
        """    const holder = leaseOf(cur, key)
    if (holder && !leaseHeldBy(holder, req.sessionId, req.alias) && req.minutes < 0) {""",
        [
            "独占租约：同一批两个工单都申请",
            "独占租约：A 持租约期间 B 派工不得开跑",
            "独占租约：同一息两次并发 dispatch 只能一位拿到",
        ],
        "回退后冲突单照旧开跑：同一资源会同时出现两位持者，两套负载敏感测量又互相污染。",
    ),
    Case(
        "独占资源租约：把“超期回收”退回“不回收”",
        # 回退点：让回收理由函数一律返回空串 → 三条判据（到期/会话不存在/idle）全数失效，
        # 死租约永久留在盘上。判据挂在 `REVERSE_VERIFY_NEVER_RECLAIM` 环境变量上（不在 env 里 ⇒
        # 永远不成立）；返回值用 `String()` 而不是 `""`，是为了**避免嵌套引号**——回退文本一旦
        # 拼错就编译不过，而“编译不过 ⇒ 跑的是旧/坏代码”正是 CONVENTIONS §7.3 的假绿形态。
        # ⚠ 必须用**恒真**的环境（`typeof nowMs === "number"`：形参，TS 不做常量折叠）——
        # 本用例第一版写成 `process.env.X` 而该 env 从未设置 ⇒ 回退后行为与修复版相同，
        # 逆向验证当场报「假绿」（这正是这一步存在的理由：它在真数据上抓住了自己的错）。
        """      const recycleReasonOf = (l: Lease, liveIdx: Record<string, Live>, nowMs = Date.now()): string => {""",
        """      const recycleReasonOf = (l: Lease, liveIdx: Record<string, Live>, nowMs = Date.now()): string => {
        if (typeof nowMs === "number") return String()""",
        ["独占租约：超期回收"],
        "不回收时，持有者已转 idle / 会话已删除 / 已到期的死租约会永久堵住院子——恰好堵住“要独占才敢做”的那类活。",
    ),
    Case(
        "独占资源租约：去掉启动宽限（刚派的单被同一息撕掉自己的租约）",
        # 回退点：空闲判据去掉启动宽限（见 LEASE_START_GRACE_MS）。
        # 为何这是真缺陷：`dispatch` 返回后引擎拉起会话有延迟，而 dispatch 末尾与下一息开头都会立刻读态——
        # “未运行即回收”会让**自己刚领的租约在同一息被自己撕掉**，紧接着的第二单就进来了（又两套测量互污染）。
        """        if (isIdle && nowMs - (l.start || 0) > LEASE_START_GRACE_MS) {""",
        """        if (isIdle) {""",
        ["独占租约：刚派出的单不得被同一息的读态回收"],
        "去掉宽限后，同一个派工调用之后的读态就会把刚领的租约回收掉（自伤）。",
    ),
    # ── 提交范围：宽范围暂存带走了别人的东西（2026-10-17 本仓真事故 856ba9bb）────────────
    Case(
        "宽范围暂存：把「不在工单里就报」退回「永远不报」",
        # 回退点：范围核对的分类支——把「不在工单里」的东西也归进 `inScope`，于是 `outOfScope`
        # 永远为空 ⇒ 宽范围暂存事故**完全隐形**，而这正是本条纪律存在的唯一理由。
        # ⚠ 回退写在 `commit-scope.ts` 而不是主文件的 `if (!audit.outOfScope.length)`：
        # 后者回退成 `if (true)` 后接 `else` 会让 `else` 分支不可达，`tsc` 直接报错
        # ⇒ 逆向验证报「[无效] 类型检查失败」（本轮实测踩到一次：那条用例算白跑）。
        # 回退写法保留 `else` 且两边类型一致（都是 `CommitFile[]`），因此能过 tsc。
        """    if (mentions.some((m) => mentionCovers(m, f.path))) inScope.push(f)
    else outOfScope.push(f)""",
        """    if (mentions.some((m) => mentionCovers(m, f.path))) inScope.push(f)
    else inScope.push(f)""",
        ["宽范围暂存：别人的未跟踪新文件被一起提交"],
        "不报时，`856ba9bb` 那种事故（另一个 worker 的 proc-tree.ts +596 / 用例 +573 被一起提交）在 review 里没有任何信号。",
        src=SRC_SCOPE,
    ),
    Case(
        "宽范围暂存：把「逐个列路径」退回「整棵树」（护栏与事故同形）",
        # 回退点：把「自己列的路径」丢掉、换成 `git add -A`——这不是装饰性改动，它是**事故本形**。
        # 用例里用 `stagingCommands()` 生成真实命令并执行：生成值变成 `add -A` ⇒ 别人的 `??` 文件
        # 会被一起提交 ⇒ 反向用例（`not.toContain("src/other-worker.ts")`）当场变红。
        # 这条件把「纪律退回允许 add -A」与「测试变红」接在同一根线上。
        """  const paths = own.map((p) => p.trim()).filter(Boolean)
  return [
    paths.length ? `git add -- ${paths.join(" ")}` : "git add -- <逐个列出你这一单要提交的文件>",""",
        """  const paths = own.map((p) => p.trim()).filter(Boolean)
  return [
    paths.length || !paths.length ? "git add -A" : "",""",
        ["反向用例：别人的"],
        "一旦生成的命令是 `git add -A`，别人的 `??` 文件就会进提交——这条用例直接把「宽范围暂存」当被测对象。",
        src=SRC_SCOPE,
    ),
    # ── `pgrep -f` 自匹配活锁（2026-10-17 实测两次，一次空转 12 分钟零产出）──────────────
    Case(
        "自匹配活锁：把「自匹配判据」退回「永远不报」",
        # 回退点：`isSelfMatch` 恒假——于是文本那一路对任何命令都沉默。
        # 这正是本单要防的退化：纪律写了但判据不生效（“加了机制 ≠ 机制生效”）。
        # ⚠ 回退写法**必须真的改变行为**：恒假条件有几个坑，本单三个都踩/验过——
        #   · `if (pattern.length < 0) return false` 是**空操作**（形参长量永不 <0），回退后全套仍绿；
        #     逆向验证当场报「[假绿] 回退后 0 条红」——这正是脚本存在的理由：它抓的是「回退没改到东西」，
        #     而不是「测试没判别力」。别把空操作当成「测试不行」（§7.3 的另一种假信号）。
        #   · 字面 `false` 或被 `const` 收窄的条件可能触发 unreachable-code 诊断 ⇒ 回退版编译不过，
        #     跑的是旧代码（§7.3 的头号假绿）。
        #   · `typeof command === "string"` 会把 `command` 收窄成 `never`，下一行 `command.includes` 编译不过。
        # 用 `pattern.length >= 0`（形参长量，TS 不做常量折叠；恒真 ⇒ 永远走到 `return false`）
        # ——实测：tsc rc=0、bun test 3 条变红且命中两条期望（纯函数 / 端到端）。
        """export function isSelfMatch(command: string, pattern: string): boolean {
  if (!pattern) return false""",
        """export function isSelfMatch(command: string, pattern: string): boolean {
  if (!pattern) return false
  if (pattern.length >= 0) return false""",
        ["纯函数：自匹配判据", "端到端：worker 连续 5 次"],
        "判据恒假时，「12 分钟零产出」那现场（等待循环里的 pgrep 命中自己）在 review 里没有任何信号。",
        src=SRC_PGREP,
    ),
    Case(
        "自匹配活锁：把「gate 现已无进程」那一路退回「永不产生」",
        # 回退点：`staleGates` 永不产生——只靠“自匹配”那一路。
        # 为何必须单列：端到端那条命令**自己**含 pattern，自匹配那一路就会命——
        # 「活锁惯性」那一半因此可以被完全回退而不变红（本单实测：退回后全套仍 97 pass）。
        # 回退写法带 `typeof waits === "string"` 保证能过 tsc（`waits` 是 number，恒假）。
        "    if (alive.length === 0) block.staleGates.push({ pattern, waits })",
        "    if (alive.length < 0 || typeof waits === \"string\") block.staleGates.push({ pattern, waits })",
        ["活锁惯性那一路单独可报"],
        "不产生 staleGates 时，「连续等 N 次、而 gate 早已无进程」这种**不自匹配**的活锁又变成隐形了。",
        src=SRC_PGREP,
    ),
    Case(
        "自匹配活锁：把「派工消息带等待纪律」退回「不带」",
        # 回退点：纪律不再注入派工消息。worker 只看得见这条消息，纪律不写进去它就只会用自己习惯的写法
        # （而那个写法恰好就是恒真的那个）。
        # 回退用恒假条件（`order.task.length > 99999`）而不是删行：删行会让后面代码移位，
        # 且可能引出 tsc 的 unreachable 诊断。
        "  lines.push(`等待纪律：${waitProtocolLine()}`)",
        "  if (typeof order.task === \"string\" && order.task.length > 99999) lines.push(`等待纪律：${waitProtocolLine()}`)",
        ["派工消息必带等待纪律"],
        "派工消息不带纪律时，worker 根本不知道 `pgrep -f` 会匹配到它自己——纪律与检测必须两头都在。",
    ),
]

BUN = shutil.which("bun") or "bun"

# ── 中断状态 ────────────────────────────────────────────────────────────────
_SIG = {"signum": 0, "count": 0}
# 当前正在被改写/回填的用例现场（给意外异常路径做最后一次兜底恢复用）
_ACTIVE: dict[str, object] = {"snapshot": None, "backup": None, "case": ""}
# 汇总用（finally 里不能 return，只能记账）
_RESULT = {"restore_failures": [], "tree_dirty": False}
# 信号通道（verify_guard.Traps）：第一次信号交给我们收尾，之后的信号**全部忽略**——
# 默认处置下第二次 kill 会在回填/校验中途直接杀进程，finally 永远跑不完（10:10 事故就是这么来的）。
TRAPS = verify_guard.Traps()


class Interrupted(Exception):
    """信号置位后由 `_check_interrupt()` 抛出；它穿过 finally 时恢复已经跑完。"""

    def __init__(self, signum: int):
        try:
            name = signal.Signals(signum).name
        except Exception:
            name = f"signal {signum}"
        super().__init__(name)
        self.signum = signum

    @property
    def exit_code(self) -> int:
        return INT_EXIT.get(self.signum, 1)


def _on_signal(signum, count):
    """信号回调：**只置标志 + 一行提示**（不做恢复，理由见模块 docstring）。

    由 `verify_guard.Traps` 在第 1 次信号时调用；第 2 次起 Traps 自己负责忽略并提示。
    """
    _SIG["count"] = count
    _SIG["signum"] = signum
    try:
        name = signal.Signals(signum).name
    except Exception:
        name = f"signal {signum}"
    print(f"\n⚠ 收到 {name}：正在收尾——当前用例的 finally 会先把源码恢复原样再退出（请勿再按；再按会被忽略）",
          flush=True)
    if os.environ.get(TRAP_DEGRADE_ENV) == "1":
        # 【演练钩子·降级】把处置交回内核：下一次信号会直接杀死本进程，finally 跑不完。
        # 只为反向验明“收尾中的信号确实能留下半回退态”（演练用例 S6 用）。
        for s in (signal.SIGINT, signal.SIGTERM):
            try:
                TRAPS._cb = None
                signal.signal(s, signal.SIG_DFL)
            except Exception:
                pass
        print(f"[演练钩子·降级] {TRAP_DEGRADE_ENV}=1：信号处置已恢复默认（下一次信号将直接杀本进程）",
              flush=True)

def _slow_restore_seconds() -> float:
    """演练钩子②：回填前的人为延时（秒），默认 0。"""
    try:
        return max(0.0, float(os.environ.get(SLOW_RESTORE_ENV, "0")) / 1000.0)
    except ValueError:
        return 0.0


def install_signal_traps() -> None:
    TRAPS.install(_on_signal)


def _check_interrupt() -> None:
    """在**被保护区域之外沿**把信号变成控制流（不在 finally 内部调用）。"""
    if _SIG["signum"]:
        raise Interrupted(_SIG["signum"])


# ── 文件读写与比对 ──────────────────────────────────────────────────────────
def _sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()[:16]


def _write_atomic(path: pathlib.Path, data: bytes) -> None:
    """同目录临时文件 + `os.replace` 的原子替换，落盘后打新 mtime。

    为什么不用 unlink+copy：那有「文件不存在」与「只写了一半」两个窗口，此刻任何并发的
    `bun test` / `tsc` 都会读到半个模块（假红假绿都可能）。rename 之后读者要么看到旧内容、
    要么看到新内容，没有中间态。（`copy2` 还会把旧 mtime 搬回来，故统一显式 utime。）
    """
    tmp = path.with_name(path.name + TMP_SUFFIX)
    with open(tmp, "wb") as fh:
        fh.write(data)
        fh.flush()
        os.fsync(fh.fileno())
    os.replace(tmp, path)
    stamp = time.time()
    os.utime(path, (stamp, stamp))


def describe_mismatch(expected: bytes, actual: bytes) -> str:
    """把两个字节串的差异讲成人话：长度、首个差异偏移、所在行、两版行内容。"""
    if expected == actual:
        return "逐字节一致"
    delta = len(actual) - len(expected)
    parts = [f"长度 {len(expected)} → {len(actual)} 字节（{delta:+d}）",
             f"sha {_sha(expected)} → {_sha(actual)}"]
    limit = min(len(expected), len(actual))
    off = next((i for i in range(limit) if expected[i] != actual[i]), limit)
    line_no = expected.count(b"\n", 0, off) + 1
    parts.append(f"首个差异 @ 字节 {off}（第 {line_no} 行）")
    try:
        exp_lines = expected.split(b"\n")
        act_lines = actual.split(b"\n")
        if line_no - 1 < len(exp_lines):
            parts.append(f"    期望(开跑前快照) L{line_no}: {exp_lines[line_no - 1][:240].decode('utf-8', 'replace')!r}")
        if line_no - 1 < len(act_lines):
            parts.append(f"    实际(回读)       L{line_no}: {act_lines[line_no - 1][:240].decode('utf-8', 'replace')!r}")
    except Exception:
        pass
    return "\n".join(parts)


def restore_source(snapshot: bytes, backup: pathlib.Path | None, *, case_name: str,
                   target: pathlib.Path | None = None) -> tuple[bool, str]:
    """把源码回填成 `snapshot` 并**回读逐字节校验**，失败重试 `RESTORE_ATTEMPTS` 次。

    返回 (是否回到原样, 人话说明)。**绝不抛异常**——它跑在 finally 里，抛出去会把真正的原因
    （中断 / 断言失败）盖掉，而源码状态才是第一优先信息。
    """
    detail = ""
    tgt = target or SRC
    pad = _slow_restore_seconds()
    if pad:
        print(f"[演练钩子] 回填前暂停 {pad:g}s（{SLOW_RESTORE_ENV}）——给收尾中途的第二次信号制造窗口",
              flush=True)
        time.sleep(pad)
    for attempt in range(1, RESTORE_ATTEMPTS + 1):
        try:
            _write_atomic(tgt, snapshot)
            got = tgt.read_bytes()
        except Exception as exc:  # 磁盘满 / 只读 / 权限 / IO 错
            detail = f"第 {attempt} 次回填抛异常：{type(exc).__name__}: {exc}"
            print(f"⚠ [回填异常] {case_name}：{detail}", flush=True)
            time.sleep(0.2 * attempt)
            continue
        if got == snapshot:
            note = ""
            if backup is not None:
                # 校验通过**之后**才删崩溃兜底：删早了，一次 SIGKILL 就再没有救援文件
                try:
                    backup.unlink(missing_ok=True)
                except Exception as exc:
                    note = f"（⚠ 但 {backup.name} 删除失败：{type(exc).__name__}: {exc}；下一轮起跑会当残留处理）"
            return True, f"回填后回读逐字节校验通过（{len(snapshot)} 字节，sha {_sha(snapshot)}）{note}"
        detail = describe_mismatch(snapshot, got)
        print(f"⚠ [回填未生效] {case_name}：第 {attempt}/{RESTORE_ATTEMPTS} 次回填后校验不一致 ——\n{detail}",
              flush=True)
        time.sleep(0.2 * attempt)
    return False, detail


def recover_stray_backups() -> None:
    """起跑前恢复上一轮残留（= 上一轮未完成的回退），并把话**说明白**。

    ⚠ 报告口径：既然发生了「上一轮未完成回退」，那么工作树在本轮开跑前的状态就**不是**
    任何人有意留下的状态，而是上下文的残骸。本轮据此得出的任何「原本如何」的判断都不成立。
    """
    temps = sorted(SRC.parent.glob(f"*{TMP_SUFFIX}"))
    for tmp in temps:
        print(f"清理上一轮遗留的临时文件 {tmp.name}（原子写未完成，不含有效内容）")
        try:
            tmp.unlink(missing_ok=True)
        except Exception as exc:
            print(f"  ⚠ 删除失败：{type(exc).__name__}: {exc}")

    strays = sorted(SRC.parent.glob(f"*{ROLLBACK_SUFFIX}"))
    if not strays:
        return

    print("=" * 86)
    print("⚠ 上一轮未完成回退：发现残留备份（残留备份即未完成的回退，判据见本文件源注释）")
    for bak in strays:
        target = bak.with_name(bak.name[: -len(ROLLBACK_SUFFIX)])
        try:
            data = bak.read_bytes()
        except Exception as exc:
            print(f"  ✗ {bak.name} 读不出来（{type(exc).__name__}: {exc}）——无法自动恢复，请人工 git checkout")
            _RESULT["tree_dirty"] = True
            continue
        before = target.read_bytes() if target.exists() else b""
        ok, detail = restore_source(data, bak, case_name=f"残留恢复 {target.name}")
        mark = "✓" if ok else "✗"
        print(f"  {mark} {bak.name} → {target.name}：{detail}")
        if ok:
            print(f"    当前源码 {len(before)} 字节 sha {_sha(before) or '(缺失)'} → "
                  f"恢复为备份内容 {len(data)} 字节 sha {_sha(data)}")
        else:
            _RESULT["tree_dirty"] = True
    print("  已按上一轮的备份恢复。")
    print("  ⚠ **本轮结论不能用于判定工作树原本状态**：这里的「基线」取自上一轮的备份文件，")
    print("     不是 git 里的提交；「这个缺陷本来在不在 / 修没修」一类问题不能拿本轮回答。")
    print("     要判工作树原本状态，先看实物：git status / git diff。")
    print("=" * 86)
    sys.stdout.flush()


# ── bun test / tsc ─────────────────────────────────────────────────────────
def run_tests() -> tuple[int, int, list[str]]:
    """跑 shengsheng 单测，返回 (pass, fail, 失败用例名列表)。

    ⚠ **必须传测试文件路径，不能传目录**：`bun test <目录>` 只打汇总
    （`57 pass`），没有逐条 `(pass)/(fail)` 行——而本脚本靠逐条行做归因，
    传目录会让“基线一条都没跑起来”恒成立（本轮实测踩到）。
    """
    proc = subprocess.run([BUN, "test", str(TEST_DIR / "shengsheng.test.ts")],
                          capture_output=True, text=True, timeout=600)
    out = ANSI.sub("", proc.stdout + proc.stderr)
    passed = failed = 0
    names: list[str] = []
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("(pass)"):
            passed += 1
        elif line.startswith("(fail)"):
            failed += 1
            names.append(line[7:].strip())
    return passed, failed, names


def typecheck() -> bool:
    if not TSC.exists():
        return True  # 没有 tsc 就跳过（Bun 会做转译，但不查类型）
    proc = subprocess.run([str(TSC), "--noEmit"], cwd=ROOT / "packages/agents",
                          capture_output=True, text=True, timeout=600)
    return proc.returncode == 0


def warn_if_other_tests_running() -> None:
    """**旧版提示性检测**——已被 `verify_guard.detect_peers()` 取代（保留函数只因下面有它的调用点）。

    历史：这一版只打提示、不拦截，而且是**私有实现**（判据只有 `bun`+`test`+`reverse_verify` 不命中），
    与演练脚本里那份 `other_bun_tests()` 还不是一套判据——那正是「同仓两套标准」的土壤。
    现在两处都改成 `verify_guard`，本节仅留作“其它 bun test 也在跑”的补充提醒（供 --force 后用）。
    """
    peers = [p for p in verify_guard.detect_peers() if p.kind == "bun test"]
    if peers:
        print("⚠ 检测到别的 bun test 进程正在跑（它们此刻可能读到本脚本的回退版源码 → 假红）：")
        for p in peers[:5]:
            print(f"    · {p.line()}")
        print("    本脚本照常执行；若那些测试红了，先确认不是本脚本的临时改动。\n", flush=True)


def maybe_pause_after_rollback() -> None:
    """演练钩子：把进程停在「源码已是回退态 + .revbak 已就位」的那一刻，等外部信号。

    默认不生效（环境变量未设）。它刻意放在回退写入之后、typecheck 之前——那正是事故里
    「源码已回退、备份未恢复」的中间态。
    """
    if os.environ.get(PAUSE_ENV) != "1":
        return
    try:
        secs = float(os.environ.get(PAUSE_SECS_ENV, "60"))
    except ValueError:
        secs = 60.0
    print(f"[演练钩子] 源码已进入回退态（{_ACTIVE.get('target') or SRC} + {ROLLBACK_SUFFIX} 已就位），暂停 ≤{secs:g}s 等信号"
          f"（{PAUSE_ENV}=1，仅外部演练脚本使用）", flush=True)
    deadline = time.monotonic() + secs
    while time.monotonic() < deadline and not _SIG["signum"]:
        time.sleep(0.05)


# ── 主流程 ──────────────────────────────────────────────────────────────────
def _execute(run_start_all: dict[pathlib.Path, bytes]) -> int:
    """stray 恢复之后的主流程；`run_start_all` 是进程级「起跑前」字节快照（每份被保护件一份）。"""
    _check_interrupt()
    base_pass, base_fail, _ = run_tests()
    _check_interrupt()
    if base_fail > 0:
        print(f"基线就有 {base_fail} 条失败——先修好再跑逆向验证（否则无法归因）")
        return 2
    if base_pass == 0:
        print("基线一条用例都没跑起来——测试入口坏了，先修它")
        return 2
    print(f"基线：{base_pass} passed / 0 failed（类型检查 {'OK' if typecheck() else '失败'}）\n")
    _check_interrupt()

    for case in CASES:
        _check_interrupt()  # 上一轮没收完就不开下一轮

        tgt = case.src   # 该用例回退**哪一份文件**（判据分散在多个模块时由用例自带）
        # ① 该用例开跑前的**字节快照**（恢复的唯一真值，不是"备份拷回去"）
        snapshot = tgt.read_bytes()
        _ACTIVE.update(snapshot=snapshot, case=case.name, backup=None, target=tgt)
        source = snapshot.decode("utf-8")
        if case.old not in source:
            print(f"[跳过] {case.name}：回退锚点未命中 {tgt.name}（代码已变，请更新脚本）")
            _RESULT["restore_failures"].append(case.name)
            _ACTIVE.update(snapshot=None, target=None)
            continue

        backup = tgt.with_name(tgt.name + ROLLBACK_SUFFIX)
        _ACTIVE["backup"] = backup
        restored_ok = True
        try:
            # ② 崩溃兜底：进程被 SIGKILL 时，救援只剩这个文件
            _write_atomic(backup, snapshot)
            _write_atomic(tgt, source.replace(case.old, case.new, 1).encode("utf-8"))
            maybe_pause_after_rollback()   # 演练钩子（默认不生效）
            _check_interrupt()

            if not typecheck():
                _check_interrupt()
                print(f"[无效] {case.name}：回退版本类型检查失败（不许当通过——那会跑旧/坏代码）")
                _RESULT["restore_failures"].append(case.name)
                continue
            _check_interrupt()
            passed, failed, names = run_tests()
            _check_interrupt()             # 跑完就查，别让中断再等下一条用例
            if passed == 0 and failed == 0:
                print(f"[无效] {case.name}：一条用例都没跑起来（不算通过）")
                _RESULT["restore_failures"].append(case.name)
                continue
            hit = [n for n in names if any(k in n for k in case.expect_fail)]
            if hit:
                print(f"[通过] {case.name}：回退后 {failed} 条变红，其中命中期望着 {len(hit)} 条 —— 测试真能抓住（{case.note}）")
            else:
                print(f"[假绿] {case.name}：回退后 {failed} 条红，但**没命中**期望用例：{names} —— 测试拦不住这个缺陷")
                _RESULT["restore_failures"].append(case.name)
        finally:
            # ③ 回填 + **回读逐字节校验**（中断/异常路径也会走到这里）
            ok, detail = restore_source(snapshot, backup, case_name=case.name, target=tgt)
            _ACTIVE.update(snapshot=None, backup=None, case="", target=None)
            restored_ok = ok
            if ok:
                print(f"[恢复] {case.name}：{tgt.name} 已{detail}")
            else:
                _RESULT["tree_dirty"] = True
                print("\n" + "!" * 86)
                print(f"❌ [恢复失败] {case.name}：{tgt.name} **没能**回到开跑前（已重试 {RESTORE_ATTEMPTS} 次）")
                print(f"   文件：{tgt}")
                print(f"   差在哪：{detail}")
                print(f"   该用例**不计为通过**（源码状态不可信 ⇒ 它的红绿也不可信）。")
                print(f"   人工修：cd {ROOT} && git checkout -- {tgt.relative_to(ROOT)}")
                print("!" * 86 + "\n")
                _RESULT["restore_failures"].append(f"{case.name}（源码未回到原样）")
        sys.stdout.flush()

    # ④ 收尾：不许留 .revbak（它是「回退未完成」的唯一信号），源码必须等于进程级快照
    leftovers = sorted(SRC.parent.glob(f"*{ROLLBACK_SUFFIX}"))
    if leftovers:
        print(f"⚠ 收尾发现残留备份 {[p.name for p in leftovers]} —— 按残留恢复口径处理")
        recover_stray_backups()
    _check_interrupt()

    # 被保护的**每一份**文件都得逐字节回到起跑前（不只主文件：判据已拆成多个模块）
    for path, start in run_start_all.items():
        try:
            now = path.read_bytes()
        except Exception as exc:
            print(f"\n{path.name} 读不出来：{type(exc).__name__}: {exc}")
            return 2
        if now != start:
            print(f"\n{path.name} 与「起跑前」不一致（本脚本自身的收尾校验）：")
            print(describe_mismatch(start, now))
            print("—— 工作树可能已损坏，请检查 git status")
            return 2

    # 恢复后必须回到基线（绿 + 类型通过）
    passed, failed, _ = run_tests()
    _check_interrupt()
    if failed > 0 or passed != base_pass:
        print(f"\n恢复后未回到基线（{passed} passed / {failed} failed）——工作树可能已损坏，请检查 git status")
        return 2
    if not typecheck():
        print("\n恢复后类型检查失败——工作树可能已损坏")
        return 2
    if _RESULT["restore_failures"]:
        print(f"\n{len(_RESULT['restore_failures'])} 条未通过逆向验证：{', '.join(_RESULT['restore_failures'])}")
        return 2 if _RESULT["tree_dirty"] else 1
    print(f"\n全部 {len(CASES)} 条逆向验证通过；已恢复基线（{passed} passed / 0 failed）")
    return 0


def _verify_tree(run_start_all: dict[pathlib.Path, bytes]) -> tuple[bool, str]:
    """退出前的最后一道自证：每份被保护件字节 == 起跑前快照，且无 *revbak 残留。"""
    lines: list[str] = []
    ok = True
    for path, start in run_start_all.items():
        try:
            now = path.read_bytes()
        except Exception as exc:
            return False, f"{path.name} 读不出来：{type(exc).__name__}: {exc}"
        if now == start:
            lines.append(f"{path.name} 已回到起跑前原样（{len(start)} 字节，sha {_sha(start)}）")
        else:
            ok = False
            lines.append(f"{path.name} **未**回到起跑前：\n" + describe_mismatch(start, now))
    leftovers = sorted(SRC.parent.glob(f"*{ROLLBACK_SUFFIX}")) + sorted(SRC.parent.glob(f"*{TMP_SUFFIX}"))
    if leftovers:
        ok = False
        lines.append(f"残留文件：{[p.name for p in leftovers]}")
    else:
        lines.append(f"无 *{ROLLBACK_SUFFIX} / *{TMP_SUFFIX} 残留")
    return ok, "\n   ".join(lines)


def _finish_interrupted(exc: Interrupted, run_start_all: dict[pathlib.Path, bytes]) -> int:
    """中断收尾：恢复已在 finally 里跑完，这里只做自证 + 明确的话与退出码。"""
    # 万一异常从被保护区里穿出（snapshot 还挂着），最后兜一次
    snapshot = _ACTIVE.get("snapshot")
    if snapshot:  # type: ignore[truthy-bool]
        try:
            restore_source(snapshot, _ACTIVE.get("backup"), case_name=str(_ACTIVE.get("case") or "中断收尾"),
                           target=_ACTIVE.get("target"))  # type: ignore[arg-type]
        except Exception:
            pass

    ok, detail = _verify_tree(run_start_all)
    print("\n" + "=" * 86)
    print(f"⚠ 已被中断（{exc}）：已恢复源码到原样" if ok else f"❌ 已被中断（{exc}）：源码未能确认恢复原样")
    print(f"   {detail}")
    print(f"   本轮逆向验证**未完成**，结论不可用；退出码 {exc.exit_code}")
    if not ok:
        print(f"   人工修：cd {ROOT} && git checkout -- {SRC.relative_to(ROOT)}"
              f"（或删掉残留后重跑本脚本，起跑前会自动恢复）")
    print("=" * 86)
    sys.stdout.flush()
    return exc.exit_code


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--force", action="store_true",
                       help="显式跳过并发检测（同树已有验证在跑时也能跑；结果可能在竞态下不可信）")
    parser.add_argument("--check-only", action="store_true",
                       help="只跑并发闸门检查（不动源码、不跑用例）：退出码 3 = 现在不该起跑")
    args = parser.parse_args()
    if args.list:
        for c in CASES:
            print(f"{c.name:26} → 期望红：{c.expect_fail}")
        return 0

    install_signal_traps()   # 第一件事：让中断从这一刻起就被接住

    # ② 并发防护（与演练脚本共用 verify_guard）：起跑前扫同树是否已有验证在跑。
    # 这一步放在残留恢复**之前**：否则两个并发进程会各自“恢复”对方正在回退的源码。
    guard = verify_guard.preflight(tree=SRC.parent, name=pathlib.Path(__file__).name, force=args.force)
    if guard.refused:
        return EXIT_CONCURRENT
    try:
        if args.check_only:
            lock_state, lock_info = verify_guard.Lock(SRC.parent / LOCK).state()
            print(f"✓ 并发闸门检查通过（--check-only）：{('已按 --force 越过并发检测' if guard.forced and guard.peers else '同树没有别的验证在跑')}，"
                  f"可以起跑。现在同机有 {len(guard.peers)} 个验证进程在跑；锁：{lock_state}；自己的 pid {os.getpid()}")
            if lock_info.get("pid"):
                print(f"   锁持有者 pid {lock_info.get('pid')}：{lock_info.get('cmd', '')[:110]}")
            return 0
        return _main_body()
    finally:
        try:
            guard.report_end()
        except Exception:
            pass
        guard.release()


def _main_body() -> int:
    # ⚠ **残留备份即未完成的回退**：上一轮被中断（Ctrl-C / kill）时，源码会停在回退态而
    # `.revbak` 就是原版——**必须先恢复再开跑**，否则会把“半回退源码”当基线，
    # 得出“基线就红”，或更阴的“以为修好了其实只修了一半”。
    # （本轮实测：一次中断留下 `round = seq`，随后跑测试莫名其妙变红、查三轮才发现是脚本残留。）
    recover_stray_backups()
    if _RESULT["tree_dirty"]:
        print("残留备份无法全部恢复——先人工确认工作树再跑（本轮不产出任何结论）")
        return 2

    run_start_all = {p: p.read_bytes() for p in PROTECTED}   # 进程级快照：中断路径与收尾都用它自证
    _check_interrupt()
    warn_if_other_tests_running()
    try:
        return _execute(run_start_all)
    except Interrupted as exc:
        return _finish_interrupted(exc, run_start_all)
    except KeyboardInterrupt:      # 信号处理器被绕过/替换时的兜底
        return _finish_interrupted(Interrupted(signal.SIGINT), run_start_all)
    except BaseException:
        # 意外异常也必须先交代源码状态，再让异常继续冒泡（保留 traceback 供归因）
        snapshot = _ACTIVE.get("snapshot")
        if snapshot:  # type: ignore[truthy-bool]
            try:
                restore_source(snapshot, _ACTIVE.get("backup"), case_name="异常收尾",
                               target=_ACTIVE.get("target"))  # type: ignore[arg-type]
            except Exception:
                pass
        ok, detail = _verify_tree(run_start_all)
        print("\n" + "=" * 86)
        print("❌ 逆向验证因未预期异常中止")
        print(f"   {detail}")
        if not ok:
            print(f"   人工修：cd {ROOT} && git checkout -- {SRC.relative_to(ROOT)}")
        print("=" * 86)
        sys.stdout.flush()
        raise


if __name__ == "__main__":
    _code = 1
    try:
        _code = main()
    finally:
        try:
            sys.stdout.flush()
            sys.stderr.flush()
        except Exception:
            pass
    # 用 os._exit 固定退出码：不让任何 atexit / 缓冲 / 未收尾线程改写「我们留下了什么状态」
    os._exit(_code if isinstance(_code, int) else 1)
