#!/usr/bin/env python3
"""生生自举闭环的**监督哨兵**：客观判定「河还在流吗」，供定时监督任务调用。

## 为什么需要它（判据必须可复跑，不能靠印象）

「优化效果」若靠读对话判断，会随上下文压缩而漂移——上一息说"跑了 2 次成功"，
这一息可能记成"一直很稳"。故把判据落到**磁盘与任务记录**上，每息重算一遍。

## 判据分四组（每组都给出「健康 / 退化」的明确界线）

A. **息在流**：两条演进线的心跳任务 runCount 递增、lastStatus 非 error，**且这一跳真有产出**——
   runCount 是「有动作」的计数，不是「有进展」的计数：任务记录里的 `skipped`（「上次执行尚未结束，
   本轮跳过」）同样让 runCount +1、同样不算 error。故 A 组读 `users/*/task-runs/<taskId>/*.json`
   把每跳分成 **success / skipped / error** 三类，`skipped` 按「连续 N 跳」与「窗口占比」两道上限判退化
   （跳过 = 该息没有产出新成果，与「呼吸未推进」同判）。取数与 `heartbeat_e2e.py` **同一份口径**
   （直接复用它的 `Repo.runs()`），两把尺子不再各算一套。
B. **平台在被改，且是心跳改的**：control.md 呼吸递增**且能归因到心跳会话**、台账有新增、种子被派/销账。
   呼吸递增但写入者是**非心跳会话** → **告警**（不算健康，也不算心跳停滯）：心跳未驱动、只有人在推。
   为什么必须问「谁改的」：同一项目根上可以同时有心跳会话与人驱动会话在调 `shengsheng_state`，
   人写的一次 note 同样让呼吸 +1——只比数字增量，「心跳真死了」与「心跳活着」在磁盘上长得一样。
   取证：本仓 09:44~09:52 呼吸 3→4→…→8 里，既有心跳会话 688a81da 写的，也有**人驱动会话
   7dd7b830 写的（呼吸 7）**；旧版判据把这 +1 一律计成「心跳在跑」，总判健康、退出码 0。
C. **闭环真跑**（这是本脚本的核心）：平台线种子里有 `source` 含「反哺」的条目，
   且该条目对应的方向**后来被派出去过**（不是只落种子躺着）
D. **修复没被回退**：跑关键测试子集（**两道门**：生生的 bun 测试 + 哨兵自己的 Python 自测）
   + 检查修复锚点仍在源码里。
   **前提**：门禁（`reverse_verify.py`）跑时会**故意**把源码回退成缺陷版——锚点消失、测试变红都是它
   造成的。故跑测试之前先探测 `.reverse_verify.lock` / `*.revbak` / 栈上 `*reverse_verify*` 进程，
   命中则 D 组判 **unknown**（「门禁正在回退源码，本息不判修复是否被回退」），一个字都不报 degraded。
   本仓实测：10:30 报「8 条测试红」、10:35 报「init 不跨线继承锚点缺失」，门禁跑完自行恢复（8/8 锚点）
   —— 这类假退化会诱人去找回一个**门禁正在改**的文件。

## 为什么归因靠**测试**守而不靠锚点

锚点只能是「某个字符串还在不在」，它与「归因逻辑真能分辨心跳/人工」是两件事。
本哨兵曾把归因锚点写成查自己的 `last_note` 字样——它恒绿、什么也不守（要弄红它得
同时删实现和这行断言）。已改为**由自测守归因**（`tools/test_shengsheng_sentinel.py`
里的归因用例），并把那道自测接进 D 组：把判据 B 退回“只看呼吸递增”的旧版，自测必红。

两个配套演练（可复跑，不是一次性手改）：
  python3 tools/sentinel_attrib_drill.py      # 把 judge_breath 改回旧版 → 自测必红 → 逐字节复原 → 再跑必绿
  python3 tools/sentinel_attrib_prodscene.py  # 在生产数据上换归因：心跳→ok／人工→告警／不明→unknown

## 用法
  python3 tools/shengsheng_sentinel.py                 # 人类可读报告
  python3 tools/shengsheng_sentinel.py --json          # 机器可读（供监督任务解析）
  python3 tools/shengsheng_sentinel.py --snapshot      # 只记录本息快照（不判退化）
  python3 tools/shengsheng_sentinel.py --state         # 看历史快照与趋势
  python3 tools/shengsheng_sentinel.py --root /tmp/x --home /tmp/x \
          --state-file /tmp/x/.evolve/sentinel-state.json   # 夹具/演练：不碰生产状态

退出码：0 = 全部健康；1 = 有退化项**或有告警项**（监督任务据此决定是否行动）；2 = 无法判定（缺坐标/读不到）。
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]  # → /home/x/code/gebai（本文件在 tools/ 下，parents[1]=仓库根）

# 跑哪条线、查哪份用户数据、快照落哪——一律可被 --root/--business/--home/--state 覆盖。
# 为什么要把它们做成可覆盖：判据 B 的归因要能**在夹具根上演练**（构造「只有人工 note」的场景），
# 而演练绝不能碰生产 `.evolve/control.md` 与生产快照历史。
CFG = {
    "root": REPO,                      # 平台线（生生自己）
    "business": REPO / "shuangtian",   # 业务线（霜天）
    "home": REPO,                      # 用户数据根：users/*/tasks.json、users/*/sessions/**/chat.json
    "state": REPO / ".evolve" / "sentinel-state.json",
    # 判据 D 的两条**代码树**坐标（与「跑哪条演进线」无关，故不随 --root 改；夹具用 CFG 覆盖它们）：
    # src = 会被门禁回退的那份源码所在目录（`*.revbak` 落在这里）；lock = 门禁锁（verify_guard.LOCK_NAME）。
    "src": REPO / "packages" / "agents" / "src" / "agents" / "shengsheng",
    "lock": REPO / ".reverse_verify.lock",
    # 进程探测的注入缝（缺省 None = 真扫 /proc）：与 --root 同一类夹具缝——
    # 自测要能断言「栈上有门禁」而**不依赖本机此刻真有人在跑门禁**（否则用例随机器状态飘）。
    "peers": None,
}

# ---- D 组：修复锚点（每条 = 一个曾被真撞出的缺陷，回退即退化）----
# 锚点是**代码特征串**而不是行号：行号会随重构漂移，特征串只在修复真被回退时消失。
ANCHORS = [
    ("并发写串行", "withPathLock", "packages/agents/src/agents/shengsheng/shengsheng.ts"),
    ("round 语义归位", "const round = (prevW?.round ?? 0) + 1", "packages/agents/src/agents/shengsheng/shengsheng.ts"),
    # 锚点从「运行态 ctx 判据」改成「ctx 的两个量分开」：前者的特征串（`ctxOver > WORKER_CTX_LIMIT`）
    # 正是被本单消灭的那个缺陷形状（把复用建议线当模型窗口用）——留着它等于逼后来人把 bug 写回去。
    ("ctx 两个量未合并", "window > 0 && t > window", "packages/agents/src/agents/shengsheng/shengsheng.ts"),
    ("派出去即销账", "cur.seeds.splice(seeded, 1)", "packages/agents/src/agents/shengsheng/shengsheng.ts"),
    ("init 不跨线继承", "const initialized = probe.text != null", "packages/agents/src/agents/shengsheng/shengsheng.ts"),
    ("跨层反哺 feed_to", "feedTo === root", "packages/agents/src/agents/shengsheng/shengsheng.ts"),
    ("布雨回显带根", "布雨完成（${root}）", "packages/agents/src/agents/shengsheng/shengsheng.ts"),
    ("逆向验证残留自愈", "残留备份", "packages/agents/src/agents/shengsheng/reverse_verify.py"),
]

# D 组要跑两道门：TS 侧（生生的工具行为）+ **哨兵自己的判定逻辑（Python 自测）**。
# 为什么后者必须在 D 组里跑：判据 B 的归因机制**没法用骗点守**——骗点检查只能是「某个字符串还在不在」，
# 而一个字符串在不在与「归因逻辑真能分辨心跳/人工」是两件事（本仓实测：曾把骗点写成查哨兵自己的
# 「last_note」字样，它恒绿、什么也不守）。故归因的正确性一律由可复现的**测试**守：
# 测试红 = D 组红，而把归因退回旧版正是那组测试变红的输入。
TEST_CMDS = [
    ["bun", "test", "packages/agents/src/agents/shengsheng/shengsheng.test.ts"],
    [sys.executable, "tools/test_shengsheng_sentinel.py"],
]

# ---- 判据 A 的「跳有三类」门槛（为什么是这两个数，见 judge_runs 的 docstring）----
# ① 连续被跳过 >= 3 跳：任务周期 3 分钟 → 9 分钟内**一条成果都没有**；
#    而一息正常耗时本仓实测 1.5~3 分钟，连跳 3 跳只能解释为「上一息永远没结束」。
SKIP_STREAK_DEGRADED = 3
# ② 窗口内跳过占比 >= 50%（且至少 4 跳、至多 12 跳）：过半的触发没干活 = 演进半停。
#    取「过半」而不是某个绝对数：不同周期的线（3m / 10m）跳数差别很大，占比才可比。
SKIP_RATIO_DEGRADED = 0.5
SKIP_WINDOW_MIN = 4
SKIP_WINDOW_MAX = 12
# ③ 连续被跳过 2 跳：还没到退化，但**也不能报健康**——单列告警（周期内可能自愈，但要有人看）。
SKIP_STREAK_WARN = 2
# 运行记录里的三种终态（与 heartbeat_e2e.py 的 `started` 口径一致：这三类是「真跑过」）。
RUN_DONE = ("success", "error", "timeout")
RUN_SKIPPED = "skipped"


# ---------------------------------------------------------------------------
# 判据 A 的取证：运行记录（与 heartbeat_e2e.py **同一份口径**）
# ---------------------------------------------------------------------------
def _e2e_repo():
    """借 `heartbeat_e2e.py` 的取证函数（它刚交付、已验证；这里只**复用**、不重写、不改它）。

    为何要复用：本单第 ③ 条缺陷就是「两把尺子各算一套」——e2e 说「3 skipped」、哨兵说「A 组健康」，
    两句话可以同时印在同一份报告里。取证逻辑（哪算一条 run、哪些字段是终态）只应该有一份实现。
    拿不到它（部署形态里脚本不在、或它 import 副作用失败）时返回 None，调用方退回本文件的直读。
    """
    try:
        import importlib.util as _ilu

        p = REPO / "packages" / "agents" / "scripts" / "heartbeat_e2e.py"
        if not p.exists():
            return None
        mod = sys.modules.get("heartbeat_e2e")
        if mod is None:
            spec = _ilu.spec_from_file_location("heartbeat_e2e", p)
            if spec is None or spec.loader is None:
                return None
            mod = _ilu.module_from_spec(spec)
            sys.modules["heartbeat_e2e"] = mod          # 先登记再执行：模块内的 dataclass/递归 import 会用到
            spec.loader.exec_module(mod)
        return mod.Repo(CFG["home"])
    except Exception:                                    # noqa: BLE001 —— 复用是优化，不是依赖
        return None


def _read_runs_direct(tid: str) -> list[dict]:
    """退回实现：自己读 `users/*/task-runs/<taskId>/*.json`（形状与 e2e 的 Repo.runs 保持一致）。"""
    out: list[dict] = []
    for f in sorted(CFG["home"].glob(f"users/*/task-runs/{tid}/*.json")):
        try:
            rec = json.loads(f.read_text(encoding="utf-8"))
        except Exception:
            continue
        if isinstance(rec, dict):
            rec.setdefault("_file", f.name)
            out.append(rec)
    return out


def read_task_runs(tid: str | None) -> list[dict]:
    """取一个任务的运行记录（按文件名排序 = 按时间升序）。

    取证路径：优先进 e2e 的 `Repo.runs()`（同一份口径），拿不到再退回直读。
    夹具只要把 `CFG["home"]` 指到临时 root，两条路径都只读夹具，**绝不碰生产 task-runs**。
    """
    if not tid:
        return []
    repo = _e2e_repo()
    if repo is not None:
        for user in repo.users():
            got = repo.runs(user, str(tid))
            if got:
                return got
        return []
    return _read_runs_direct(str(tid))


def classify_runs(runs: list[dict], limit: int = SKIP_WINDOW_MAX) -> dict:
    """把运行记录分成**成功 / 跳过 / 失败**三类，并算出「连续跳过」与「窗口占比」。

    为何必须分三类：`skipped` 是「上次执行尚未结束，本轮跳过」——它让 runCount +1、lastStatus 也可能
    仍写着 success，于是旧判据 A（只看 runCount 递增 + lastStatus 非 error）**全程绿**，
    而演进实际停滞。本仓真数据（`users/admin/task-runs/769433…/`）：29 条 = 26 success + 3 skipped。

    连续跳过要**从最近往回数**：中间只要有一次 success 就说明息又流起来了（计数归零）。
    """
    seq = [r for r in runs if isinstance(r, dict)]
    tail = seq[-limit:] if limit and len(seq) > limit else seq
    success = [r for r in seq if r.get("status") == "success"]
    skipped = [r for r in seq if r.get("status") == RUN_SKIPPED]
    failed = [r for r in seq if r.get("status") in ("error", "timeout")]
    streak = 0
    for r in reversed(seq):
        if r.get("status") == RUN_SKIPPED:
            streak += 1
        else:
            break
    win_skipped = [r for r in tail if r.get("status") == RUN_SKIPPED]
    return {
        "total": len(seq),
        "success": len(success),
        "skipped": len(skipped),
        "failed": len(failed),
        "streak": streak,                                  # 末尾连续跳过的跳数
        "window": len(tail),
        "window_skipped": len(win_skipped),
        "ratio": (len(win_skipped) / len(tail)) if tail else 0.0,
        "last_skip_reason": (skipped[-1].get("reason") or "") if skipped else "",
        "last_status": (seq[-1].get("status") if seq else None),
        "known": bool(seq),                                # 无记录 = 取证不可得（不是“没跳过”）
    }


# --------------------------------------------------------------------------
# 判据 D 的前提：门禁（reverse_verify.py）是否正在回退源码
# --------------------------------------------------------------------------
def _load_verify_guard(src: Path):
    """按**文件路径**加载源码目录里的 `verify_guard.py`（不往 sys.path 里塞、不用通用名 `verify_guard` 撞门禁自己的 import）。

    为何要复用它的 `detect_peers()`：判据「栈上有没有门禁」必须与门禁自己的并发检测**同一份实现**——
    它已排除自己/祖先/后代、并把 `pgrep … reverse_verify.py` 这类观察器挑出去。
    哨兵自己写一套就是本单第 ③ 条缺陷的同一亩地（两把尺子）。拿不到就返回 None（降级为锁+revbak 两道）。
    """
    p = src / "verify_guard.py"
    if not p.exists():
        return None
    try:
        mod = sys.modules.get("sentinel_verify_guard")
        if mod is None:
            import importlib.util as ilu

            spec = ilu.spec_from_file_location("sentinel_verify_guard", p)
            if spec is None or spec.loader is None:
                return None
            mod = ilu.module_from_spec(spec)
            sys.modules["sentinel_verify_guard"] = mod
            spec.loader.exec_module(mod)
        return mod
    except Exception:          # noqa: BLE001
        return None

# 会被当成「真在跑」的 argv[0]（其余一律视为 shell 包装器/字面量，不算证据）：
# 门禁链路真实形态本仓实测：`bash -c '… python3 reverse_verify.py'` → `timeout … python3 …` → `python3 …`。
# 前两者 argv0 是 shell，只是**命令行里提到**了另一个命令名；把它们当成“门禁在跑”就会误报。
RUNNER_ARGV0 = {"python", "python3", "python3.11", "python3.12", "bun", "bun.exe", "timeout", "node"}


def _pid_is_runner(pid: int) -> bool:
    """这个 pid 的 argv[0] 本身是个**执行器**吗（而不是一个只是提到命令名的 shell/grep）？

    为何要这一道：本仓实测 `bash -c '… for i in …; do pgrep …; done; … bun test …'`
    这类**等待循环**的命令行里就带着 `bun test` 字样，verify_guard 按命令行子串会把它算成“有 bun test 在跑”
    （它的观察器排除只看命中位置**之前**有没有 pgrep——而这类循环里 pgrep 在**前**、bun test 在**后**，判定就成了
    “不是观察器”）。在哨兵这里后果不对称：误报“门禁在跑”→ D 组永远 unknown ⇒ 真退化被洗白。
    读不到 /proc（非 Linux/已退出）时返回 True（拿不到证据就不排除，安全侧失败）。
    """
    try:
        raw = Path(f"/proc/{pid}/cmdline").read_bytes()
    except Exception:
        return True
    argv0 = raw.split(b"\0", 1)[0].decode("utf-8", "replace").strip()
    if not argv0:
        return True
    base = os.path.basename(argv0).lower()
    return base in RUNNER_ARGV0


def gate_probe() -> dict | None:
    """门禁在跑吗？三道判据（任一命中即算），全部只读、不写、不碰源码。

    为什么 D 组必须先问这句：`reverse_verify.py` 跑测试时会**故意**把 `shengsheng.ts` 临时回退成
    缺陷版（锚点串消失、测试变红），跑完再逐字节回填。这期间旧的 D 组必然报 degraded——
    本仓已两次实证误判（10:30「8 条测试红」、10:35「init 不跨线继承锚点缺失」，门禁跑完 8/8 锚点回来）。
    这类假退化最危险：它会叫人去「恢复」一个**门禁正在改**的文件，正好把门禁的回填撞掉。

    三道判据：
      ① `<tree>/.reverse_verify.lock` 存在（verify_guard 的登记；本脚本不判它是否陈旧——
         陈旧锁宁可 unknown 一息，也不拿“pid 已死”去赌源码此刻是好的）；
      ② 源码目录内有 `*.revbak`（崩溃残留：门禁被硬杀时源码**仍在回退态**）；
      ③ 栈上有 `*reverse_verify*.py` 或 `bun test` 进程（锁没写上、或跑在别处时兜底）。
    判据 ③ 用 `verify_guard.detect_peers()`——它已排除自己/祖先/后代与「只是拿名字当参数」的
    观察器（pgrep/grep/vim），不会因为哨兵自己命令里带这串就恒判「有门禁在跑」。
    非 Linux/无 /proc 时它返回空表（锁与 revbak 两道仍在），且会在报告里说明。

    返回 None = 无门禁在用源码；否则返回 {reasons: [...], peers: [...], notes: [...]}。
    `notes` 是**能力缺口**（如“没有 verify_guard.py，进程那道探测做不到”）——它**不算证据**：
    把能力缺口当成“门禁在跑”，会让 D 组在任何缺这个模块的部署形态里永远 unknown，那就是新的假绿。
    """
    reasons: list[str] = []      # **证据**（命中即「门禁可能正在改源码」）
    notes: list[str] = []        # **能力缺口**（探测做不到，本身不是证据）
    lock = Path(CFG["lock"])
    if lock.exists():
        info: dict[str, str] = {}
        try:
            for line in lock.read_text(errors="replace").splitlines():
                k, _, v = line.partition("=")
                if v:
                    info[k.strip()] = v.strip()
        except Exception:
            pass
        who = f"pid {info.get('pid', '?')}｜{info.get('time', '?')}"
        reasons.append(f"锁 {lock.name} 存在（{who}）")
    src = Path(CFG["src"])
    baks = sorted(src.glob("*.revbak")) if src.is_dir() else []
    if baks:
        reasons.append("残留备份 " + "、".join(b.name for b in baks))
    peers_cfg = CFG.get("peers")          # None = 真扫进程；夹具注列表（与 --root 同一类夹具缝）
    peers: list[dict] = []
    if peers_cfg is not None:
        peers = [dict(p) for p in peers_cfg]
        if peers:
            reasons.append("在跑：" + "；".join(f"pid {p.get('pid')}（{p.get('kind')}）" for p in peers))
    else:
        guard = _load_verify_guard(src)
        if guard is None:
            # ⚠ 这**不是证据**：找不到护栏模块只说明"进程那道探测做不到"，锁与 revbak 两道仍在。
            # 若把它当理由，D 组在任何缺这个模块的部署形态里会永远 unknown——那就是新的假绿。
            notes.append(f"进程探测不可用（{src} 下无 verify_guard.py），本息只凭锁/revbak 判")
        else:
            try:
                for p in guard.detect_peers(quiet=True):
                    if p.kind not in ("门禁脚本", "bun test"):
                        continue
                    if not _pid_is_runner(p.pid):
                        notes.append(f"pid {p.pid} 命令行含验证字样但 argv0 是包装器（{p.cmd[:60]}），不算证据")
                        continue
                    peers.append({"pid": p.pid, "kind": p.kind, "cmd": p.cmd[:120]})
                if peers:
                    reasons.append("在跑：" + "；".join(f"pid {p['pid']}（{p['kind']}）" for p in peers))
                if not guard.has_procfs():
                    notes.append("无 /proc（非 Linux）：进程那道探测降级，只剩锁/revbak")
            except Exception as e:        # noqa: BLE001 —— 探测失败不等于没门禁，但要如实说
                notes.append(f"进程探测不可用（{type(e).__name__}）")
    if not reasons:
        return None
    return {"reasons": reasons, "peers": peers, "notes": notes}


# ---------------------------------------------------------------------------
# 读态
# ---------------------------------------------------------------------------
def read_control(root: Path) -> dict | None:
    """解析 control.md 的关键计数与条目。缺失返回 None（=那条线还没 init）。"""
    f = root / ".evolve" / "control.md"
    if not f.exists():
        return None
    md = f.read_text(encoding="utf-8")

    def num(label: str) -> int:
        m = re.search(rf"- {label}：(\d+)", md)
        return int(m.group(1)) if m else 0

    def table(name: str) -> list[str]:
        m = re.search(rf"## {name}\n(.*?)(?=\n## |\Z)", md, re.S)
        if not m:
            return []
        return [ln for ln in m.group(1).splitlines() if ln.startswith("|") and not ln.startswith("| ---")]

    seeds = table("种子队列")[1:] if table("种子队列") else []   # 去掉表头
    # 写入者归因行（`- 最近一息：<sessionId> @ <ts>｜来源 heartbeat:<taskId> | manual`）：
    # 这一行是判据 B 的**唯一证人**——它由 shengsheng 的 note 落盘时写入，说的是「上一次呼吸是谁推的」。
    # 老量（既有 control.md）没有这一行：`last_note` 为 None = 归因不明，判 unknown 而不是猜健康。
    attribution = at_note(md)
    return {
        "breath": num("呼吸"),
        "seq": num("派工序号"),
        "workers": max(0, len(table("worker 池")) - 1),
        "inflight": max(0, len(table("在途")) - 1),
        "seeds": max(0, len(seeds)),
        "seed_rows": seeds,
        "fed_seeds": [r for r in seeds if "反哺" in r],
        "last_note": parse_attribution(attribution),
        "ledger_mtime": (root / ".evolve" / "ledger.md").stat().st_mtime
        if (root / ".evolve" / "ledger.md").exists()
        else 0,
        "ledger_size": (root / ".evolve" / "ledger.md").stat().st_size
        if (root / ".evolve" / "ledger.md").exists()
        else 0,
        "mtime": f.stat().st_mtime,
    }


# ---------------------------------------------------------------------------
# 写入者归因（判据 B 的证人）
# ---------------------------------------------------------------------------
AT_LABEL = "最近一息"


def at_note(md: str) -> str | None:
    """取 control.md 计数区里的「最近一息」行原文（没有则 None）。"""
    m = re.search(rf"^- {AT_LABEL}[：:]\s*(.+)$", md, re.M)
    return m.group(1).strip() if m else None


def parse_attribution(line: str | None) -> dict | None:
    """拆归因行 → {sessionId, at, source, taskId}。形状不对就整行不认（宁 unknown 不猜）。

    容忍两种形态：
      新：`688a81da… @ 1792201907347｜来源 heartbeat:52c42957…`
      旧/手写：`688a81da…｜来源 manual`（无时间戳也算数——会话 id 与来源才是判据要的）
    """
    if not line:
        return None
    m = re.search(
        r"(?P<sid>[0-9a-fA-F]{8,64})"
        r"(?:\s*@\s*(?P<at>\d+))?"
        r"\s*[｜|]\s*来源\s*[：:]?\s*(?P<src>heartbeat|manual)"
        r"(?:\s*[:：]\s*(?P<tid>[0-9a-zA-Z_-]{4,64}))?",
        line,
    )
    if not m:
        return None
    return {
        "sessionId": m.group("sid"),
        "at": int(m.group("at")) if m.group("at") else None,
        "source": m.group("src"),
        "taskId": m.group("tid"),
    }


def read_heartbeat_tasks() -> list[dict]:
    """从用户任务存储里取心跳任务（不依赖 task 工具，哨兵要能独立跑）。"""
    out: list[dict] = []
    for f in CFG["home"].glob("users/*/tasks.json"):
        try:
            data = json.loads(f.read_text(encoding="utf-8"))
        except Exception:
            continue
        items = data if isinstance(data, list) else data.get("tasks", [])
        if isinstance(items, dict):
            items = list(items.values())
        for t in items:
            if not isinstance(t, dict):
                continue
            if "SHENGSHENG_EVOLVE" in str(t.get("name", "")) or "SHENGSHENG_SUPERVISE" in str(t.get("name", "")):
                out.append(
                    {
                        "id": t.get("id"),
                        "name": t.get("name"),
                        "enabled": t.get("enabled"),
                        "runCount": t.get("runCount", 0),
                        "lastStatus": t.get("lastStatus"),
                        "lastError": t.get("lastError"),
                        "state": t.get("state"),
                        "lastRunAt": t.get("lastRunAt") or t.get("lastFinishedAt"),
                        "projectRoot": (t.get("env") or {}).get("SHENGSHENG_ROOT"),
                        "schedule": t.get("schedule"),
                        # ---- 归因要用：这个任务是在哪条会话里跑心跳的 ----
                        # stickySessionId 是定时任务复用的专用会话（心跳的“脉体”）；
                        # originSessionId 是**创建任务的源会话**——它同样会亲手调用note，
                        # 因此绝不能当心跳脉体（本栗 52c42957 的 originSessionId 正是人驱动会话 7dd7b830）。
                        "target": t.get("target"),
                        "stickySessionId": t.get("stickySessionId"),
                        "originSessionId": t.get("originSessionId"),
                        "boundSessionId": t.get("sessionId"),
                    }
                )
    return out


def heartbeat_drivers(tasks: list[dict], root: Path | str | None) -> dict:
    """本项目根的**心跳驱动者**：哪些会话写的 note 才算「心跳真在推」。

    归属规则（宁可多看不可漏看——认错方向只会多一次告警，不会多一次假健康）：
      1. 只认项目根相同的 `SHENGSHENG_EVOLVE` 任务（`SHENGSHENG_SUPERVISE` 只跑哨兵、不推呼吸；
         它常常绑在人会话上（本栗 917a0ac→7dd7b830），算作心跳脉体就等于把人工推进当心跳）；
      2. 脉体取 `stickySessionId`（定时任务复用的专用会话）；
      3. **`target=session` 的演进任务不算心跳**：它的脉体就是用户自己的会话——那就是人驱动；
      4. `originSessionId` **永远不入脉体**：那是“创建任务的会话”（人在用的那条），人亲手 note 很正常；
      5. 若同一根挂了多条演进任务（本栗真有两条： gebai 与 shuangtian），只取本项目根的那些；
         其中优先取「有过运行记录（runCount>0）」的。
    """
    root_s = str(root).rstrip("/") if root else ""
    mine = [
        t for t in tasks
        if "SHENGSHENG_EVOLVE" in str(t.get("name") or "")
        and str(t.get("projectRoot") or "").rstrip("/") == root_s
        and (t.get("target") or "ephemeral") in ("sticky", "ephemeral")
    ]
    sessions = {str(t.get("stickySessionId")) for t in mine if t.get("stickySessionId")}
    task_ids = {str(t.get("id")) for t in mine if t.get("id")}
    return {"sessions": sessions, "taskIds": task_ids,
            "tasks": [t for t in mine if t.get("runCount", 0) > 0] or mine}


NOTE_ECHO = re.compile(r"断点已落盘（呼吸 (\d+)）")

# 单次反查投入的封顶字节（默认 32MB）：会话记录会一直长，回查不能成为无限代价。
# 超预算的**最早**那几个文件直接跳过（宁可归因不明，不把哨兵拖成分钟级）。
SCAN_BUDGET = 32 * 1024 * 1024


def infer_note_writer(breath: int, since: float) -> dict | None:
    """**归因兜底**：control.md 没有「最近一息」行时（旧版写入者、手改、或那是归因上线前写的），
    去用户会话记录里找「哪个会话最近落了 breath=N 的断点」。

    为何要用会话记录做证人：`shengsheng_state action=note` 的回显里带「断点已落盘（呼吸 N）」，
    而每条 chat.json 都记着会话 id——这是磁盘上**唯一**不靠工具自觉就能反查写入者的证据。
    代价：要扫 json（会话文件可能很大），故只在“呼吸涨了但 control.md 无归因”这一小口子上扫，
    并用 mtime 先筛（只看 since 之后写过的文件）：本仓实测 11 个文件/13.6MB → 0.07s。
    超 SCAN_BUDGET 的文件按 mtime **旧的先跳**（新的更可能含这一息的证人）。
    找不到就返回 None（= 归因不明，宁可告警也不猜健康）。
    """
    cands: list[tuple[float, Path]] = []
    for f in CFG["home"].glob("users/*/sessions/*/*/*/chat.json"):
        try:
            mt = f.stat().st_mtime
        except OSError:
            continue
        if mt >= since:
            cands.append((mt, f))
    spent = 0
    best: dict | None = None
    for mt, f in sorted(cands, reverse=True):
        try:
            size = f.stat().st_size
        except OSError:
            continue
        if spent + size > SCAN_BUDGET:
            continue
        spent += size
        try:
            data = json.loads(f.read_text(encoding="utf-8"))
        except Exception:
            continue
        for m in data.get("messages") or []:
            if m.get("role") != "tool" or m.get("name") != "shengsheng_state":
                continue
            mm = NOTE_ECHO.search(str(m.get("content") or ""))
            if not mm or int(mm.group(1)) != breath:
                continue
            at = m.get("createdAt") or 0
            if best is None or at > (best.get("at") or 0):
                best = {"sessionId": f.parent.name, "at": at, "source": "session", "taskId": None}
    return best


# ---------------------------------------------------------------------------
# 采集（可注入夹具根）
# ---------------------------------------------------------------------------



def period_secs(schedule: str | None) -> int:
    """从 schedule 表达式估出周期秒数（只支持 @every Nm/h/s；认不出给 0=未知）。

    为何需要它："runCount 没递增"本身不是退化——**只在「已过一个周期」时才是**。
    本轮实测踩到：相隔 22 秒的两份快照（任务周期 3 分钟）被误判成停滞，
    若拿去行动就是自己制造动作（违反哨兵的第一纪律：健康时静默）。
    """
    m = re.match(r"@every\s+(\d+)\s*([smh])", (schedule or "").strip())
    if not m:
        return 0
    n, unit = int(m.group(1)), m.group(2)
    return n * {"s": 1, "m": 60, "h": 3600}[unit]


# ---- 「锚点会过期」：已被本仓的修复**消灭掉**的缺陷标记词 ----
#
# 一条锚点是「某个缺陷不会再回来」的钉子。但缺陷被消灭时，描述它的那个特征串往往**正是**
# 新修复要删掉的形状——于是「留着旧锚点」等于在源码里把缺陷本身钉住：要让它通过，就得把
# bug 写回去。本仓真案例（`1939b64f` 拆开 ctx 的两个量 → `e4181ef1` 改锚点）：锚点曾写作
#
#     ("运行态 ctx 判据", "const ctxOver = (live.ctxTokens ?? 0) > WORKER_CTX_LIMIT", …)
#
# 而「拿 `WORKER_CTX_LIMIT`（复用建议线）当模型窗口上限」正是那一单消灭的缺陷。留着它有两个
# 后果：① 锚点从此永久缺失 ⇒ D 组每息误报「修复被回退」；② 后来人为了让锚点通过，会把 bug
# 写回去。故把退役词放进这张表：锚点特征串里出现它们 ⇒ 提示「此锚点可能已过期」。
#
# 这是**提示**（见 stale_anchor_hints 的 docstring），不是判退化——判错了会让人去「修」一个
# 根本没坏的锚点，那比不报更贵。
RETIRED_SHAPES = [
    ("WORKER_CTX_LIMIT",
     "复用建议线曾被当成模型窗口上限用（1939b64f 把它拆成两个量）；ctx 判据里再出现它，"
     "就是把「肥到不值得复用」与「超过模型窗口」又判成一个量"),
    ("ctxOver",
     "同一缺陷的局部变量名：它把「过了建议线」与「超过窗口」判成一个布尔"),
]

# 行首这些前缀视为**注释行**（只认行首：行内 `//` 可能出现在字符串里，按行内判会误伤）。
_COMMENT_PREFIXES = ("//", "#", "*", "/*", "--", "<!--", '"""')


def _anchor_hit_lines(text: str, needle: str) -> tuple[int, int]:
    """needle 在文本里命中几次、其中几次落在**代码行**（相对：落在注释行）。

    为什么代码行要单独数：锚点本来只能证明「某个字符串还在」——若那个字符串**只**在注释里，
    它守的就是文档而不是行为（有人把实现删了、注释留着，锚点恒绿而修复已经没了）。
    """
    if not needle:
        return 0, 0
    total = code = 0
    for line in text.splitlines():
        if needle not in line:
            continue
        n = line.count(needle)
        total += n
        if not line.lstrip().startswith(_COMMENT_PREFIXES):
            code += n
    return total, code


def stale_anchor_hints(rows: list[dict], read_text=None) -> list[dict]:
    """给锚点行挑出「可能已过期」的，返回 [{name, anchor, kind, why}]（没有就空表）。

    `rows` 的形状就是 `anchor_report()` 的产出（name / anchor / file / present）。两个判据：

      ① `retired`：锚点特征串含 `RETIRED_SHAPES` 里的退役词——它描述的形状已被本仓的修复消灭；
      ② `comment_only`：锚点在源码里**只在注释行**出现——守的是文档，不是行为。

    为什么是**提示**而不是判退化（level=warn/degraded）：这两个判据都可能误伤（退役词也可能
    合理地出现在新写法里、锚点也可能故意钉一句注释里的常量说明）。判错的代价是有人去「修」一个
    没坏的锚点、或为了让提示消失而改动好的代码——比不报更贵。收单的人看到提示，做一次判断即可。

    `read_text` 是夹具缝（缺省读 `REPO / rel`）：提示逻辑要能在临时夹具上演练，不碰生产源码。
    """
    reader = read_text or (lambda rel: (REPO / rel).read_text(encoding="utf-8", errors="replace"))
    out: list[dict] = []
    for r in rows:
        if not isinstance(r, dict):
            continue
        needle = str(r.get("anchor") or "")
        rel = str(r.get("file") or "")
        name = str(r.get("name") or "")
        for word, why in RETIRED_SHAPES:
            if word in needle:
                out.append({"name": name, "anchor": needle, "kind": "retired",
                            "why": f"含退役词 `{word}`：{why}"})
                break
        if not r.get("present"):
            continue          # 已缺失的锚点由 present 判，不再谈「过期」
        try:
            text = reader(rel)
        except Exception:      # noqa: BLE001 —— 读不到源码不是“过期证据”（与「0 命中/读不到」同纪律）
            continue
        total, code = _anchor_hit_lines(text, needle)
        if total and not code:
            out.append({"name": name, "anchor": needle, "kind": "comment_only",
                        "why": f"只在注释行命中 {total} 处——守的是文档不是行为"})
    return out


def anchor_report() -> list[dict]:
    """每个修复锚点是否仍在源码里。"""
    rows = []
    for name, needle, rel in ANCHORS:
        f = REPO / rel
        ok = f.exists() and needle in f.read_text(encoding="utf-8", errors="replace")
        rows.append({"name": name, "anchor": needle, "file": rel, "present": ok})
    return rows


def run_tests(gated: bool = False) -> dict:
    """跑 D 组的两道门，并把结果合并成一个结论（任一门红即 D 组红）。

    两门的口径不一样：TS 侧（bun）与 Python 自测（本文件旁）都是“0 fail 才算绿”，
    但输出里 pass/fail 行的标记不同（bun: `(pass)`/`(fail)`；Python 自测: `(pass)`/`(FAIL)`/`(ERROR)`）——
    故各认各的标记，统一累加。**只有一门能跑也不能算绿**：命令缺失/超时一律记为红（宁可报红不可报假绿）。

    `gated=True`（evaluate 走的口径）再加两道保险：
      ① 跑之前再探一次门禁——探测与跑之间门禁可能**刚起来**，此时测试红是门禁造成的，结论不可用，
         直接给 `gated: True` 让 D 组判 unknown（比“跑完发现白跑”更早、也不会把红当真）；
      ② 跑完再探一次——若中途门禁来过（本息测试期间源码被回退过），这一轮的红同样不可信。
    这两道只在**探到门禁**时生效；探不到就与旧行为完全一致（不引入新的 unknown 噪声）。
    """
    passed = failed = 0
    rcs: list[int] = []
    errs: list[str] = []
    gate_before = gate_probe() if gated else None
    if gate_before:
        # 探到门禁：不要去跑（源码正被回退）。跑出来的红必是假的，白烧 2 分钟还会给下一息留假回忆。
        return {"ok": False, "gated": True, "gate": gate_before,
                "passed": 0, "failed": 0, "rc": -1, "error": "门禁在跑，本轮不跑测试", "suites": len(TEST_CMDS)}
    for cmd in TEST_CMDS:
        try:
            p = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True, timeout=600)
        except Exception as e:  # noqa: BLE001
            failed += 1
            errs.append(f"{' '.join(cmd[:2])} 起不来：{e}")
            continue
        out = re.sub(r"\x1b\[[0-9;]*m", "", p.stdout + p.stderr)
        passed += len(re.findall(r"^\(pass\)", out, re.M))
        failed += len(re.findall(r"^\((fail|FAIL|ERROR)\)", out, re.M))
        rcs.append(p.returncode)
        if p.returncode != 0:
            # ⚠ **rc 非零但 failed=0** 是真实场景：文件级语法/加载错误时 bun 不输出 `(fail)` 行，
            # 只在底部给 `N fail / N errors` 与 `error: ...`。本仓实测（11:40）：`shengsheng.test.ts`
            # 第 5 行两条 import 被粘成一行 → 输出 `48 passed / 0 failed` 但 rc=1，看起来自相矛盾。
            # 故补从汇总行折算失败数，并把首条 error 带进说明——否则「0 失败却判红」不可解释。
            for pat in (r"^\s*(\d+)\s+errors?\s*$", r"^\s*(\d+)\s+fail\s*$"):
                m = re.search(pat, out, re.M)
                if m and int(m.group(1)) > failed:
                    failed = int(m.group(1))
            first_err = next((ln.strip() for ln in out.splitlines() if ln.strip().startswith("error:")), "")
            errs.append(f"{' '.join(cmd[:2])} rc={p.returncode}" + (f"｜{first_err[:120]}" if first_err else ""))
    res = {
        "ok": bool(rcs) and all(rc == 0 for rc in rcs) and failed == 0,
        "passed": passed,
        "failed": failed,
        "rc": max(rcs) if rcs else -1,
        "error": "；".join(errs),
        "suites": len(TEST_CMDS),
    }
    if gated:
        gate_after = gate_probe()
        if gate_after:
            # 中途门禁来过：本轮的 pass/fail 都不能拿去讲“修复是否被回退”。
            res.update({"ok": False, "gated": True, "gate": gate_after,
                        "error": (res.get("error") or "") + "；测试期间门禁起来过，读数不可信"})
    return res


# ---------------------------------------------------------------------------
# 快照与趋势
# ---------------------------------------------------------------------------
def load_state() -> dict:
    if CFG["state"].exists():
        try:
            return json.loads(CFG["state"].read_text(encoding="utf-8"))
        except Exception:
            pass
    return {"snapshots": []}


def save_state(st: dict) -> None:
    st["snapshots"] = st["snapshots"][-49:]  # 只留最近 50 条
    CFG["state"].parent.mkdir(parents=True, exist_ok=True)
    CFG["state"].write_text(json.dumps(st, ensure_ascii=False, indent=2), encoding="utf-8")


def take_snapshot() -> dict:
    tasks = read_heartbeat_tasks()
    # 每个心跳任务的**运行记录三分类**（成败/跳过）：挂到任务行上，随快照一起落盘。
    # 为何进快照：skip 率是**趋势**判据（同一任务前后比），只看本息也算得出来，但落盘后
    # 才能回答「这条线是从哪一息开始变成每轮所跳的」——而快照兼容必须靠**缺字段不判退化**：
    # 旧快照里的任务行没有 `runs` 字段（= None/缺失），evaluate 里一律当作「取证不可得」。
    for t in tasks:
        t["runs"] = classify_runs(read_task_runs(t.get("id")))
    return {
        "at": int(time.time()),
        "root": str(CFG["root"]),
        "platform": read_control(CFG["root"]),
        "business": read_control(CFG["business"]),
        "tasks": tasks,
    }


# ---------------------------------------------------------------------------
# 判据 B 的呼吸判读（带归因）
# ---------------------------------------------------------------------------
def judge_breath(cur: dict, prev: dict, pp: dict, cp: dict,
                 tasks: list[dict], due_yet: bool, elapsed_b: int) -> list[dict]:
    """**呼吸涨了不等于心跳活着**。四类结论，界限写死：

    | 情形                                    | 结论 |
    |---------------------------------------|------|
    | 呼吸递增，且最近一息是**心跳脉体**写的          | ok |
    | 呼吸递增，但来自**非心跳会话**（人驱动）         | **告警**（单列一类：不算健康、也不算心跳停滞） |
    | 呼吸递增，但**归因不明**（旧版 note / 无证人）      | unknown（宁可报不明，不猜） |
    | 呼吸不增，且已过周期（或更早的快照已在追它）         | degraded（旧版行为不变） |
    | 呼吸不增，但周期未到                              | ok（旧版行为不变） |

    “更早的快照已在追它”——为什么还需要这条：
    人驱动会话一直写 note 时，呼吸**每快照都在涨**，于是“不增”分支永远不被触发，
    而每一次都只报一句告警、永远不升级——监督任务看到的是“一直告警的”，容易当成噪声。
    故在快照里记下“**从哪一息开始只能看到人工/不明推进**”（`manualRun.from`）：
    持续超过一个心跳周期就**升级为退化**；心跳重新接手（写回一条心跳归因）后归零。
    但周期未到的短间隙仍只告警——否则一次人工补记就能让哨兵误报“心跳死”。
    """
    out: list[dict] = []
    if cp["breath"] <= pp["breath"]:
        # ---- 不增：旧版行为**完全保留**（周期内 ok / 过周期 degraded）----
        run = cur.get("manualRun") or {}
        if due_yet:
            msg = (f"平台线呼吸未推进（停在 {cp['breath']}，已过 {elapsed_b // 60} 分钟）"
                   "——息没落到 note，或心跳未真跑")
            if run.get("from") is not None:
                msg += f"（且自呼吸 {run['from']} 起只见人工/不明推进）"
                cur["manualRun"] = None
            out.append(dict(level="degraded", group="B", msg=msg))
        else:
            out.append(dict(level="ok", group="B",
                            msg=f"平台线呼吸未变（距上快照 {elapsed_b}s，未到心跳周期）——不算停滞"))
        return out

    # ---- 增了：必须问「谁改的」----
    drivers = heartbeat_drivers(tasks, CFG["root"])
    att = cp.get("last_note")
    delta = cp["breath"] - pp["breath"]
    head = f"平台线呼吸 +{delta}（{pp['breath']} → {cp['breath']}）"

    # ① control.md 自己写了归因 → 直接用它；
    # ② 没写 → 去会话记录里反查证人（兼容归因上线前的老数据），但**必须先拿得到 mtime**：
    #    mtime 是“这次改动就发生在这一小段时间”的唯一错位。没有它就只能扫全史，而**呼吸号会被
    #    不同驱动者重复使用**（A 写 12、B 又写 12）——一口气就能把另一条会话当成写入者，
    #    那就从“不猜健康”倒回了“猜错人”。故 mt 缺失时直接归因不明。
    # ③ 两者都没有 → 归因不明。
    via = "control.md"
    if not att:
        mt = cp.get("mtime") or 0
        if mt:
            att = infer_note_writer(cp["breath"], mt - 600)
            via = "会话记录反查" if att else "无"
        else:
            via = "无（control.md 无 mtime，不敢反查——不猜）"

    if att is None:
        out.append(dict(level="unknown", group="B",
                        msg=f"{head}，但**归因不明**（{via}）——不武断判健康："
                            "旧版写入者/手改都会造成这种情形，需看是谁在推"))
        return out

    sid = str(att.get("sessionId") or "")
    src = att.get("source")
    is_hb = sid in drivers["sessions"] or (att.get("taskId") in drivers["taskIds"] and src == "heartbeat")
    who = f"{sid[:12]}（{('heartbeat:' + str(att['taskId'])[:8]) if src == 'heartbeat' and att.get('taskId') else src}，经{via}）"

    if is_hb:
        if cur.get("manualRun"):
            out.append(dict(level="ok", group="B",
                            msg=f"心跳已重新接手：{head}，写入者 {who}（此前的人工推进已结束："
                                f"呼吸 {cur['manualRun'].get('from')} → {cur['manualRun'].get('to')}）"))
        else:
            out.append(dict(level="ok", group="B", msg=f"{head}，写入者 {who}"))
        cur["manualRun"] = None
        return out

    # ---- 呼吸涨了，但不是心跳写的 ----
    run = dict(cur.get("manualRun") or {})
    if not run.get("from"):
        run.update({"from": pp["breath"], "to": cp["breath"], "at": cur["at"],
                    "sessions": [sid] if sid else []})
    else:
        run["to"] = cp["breath"]
        if sid and sid not in (run.get("sessions") or []):
            run["sessions"] = (run.get("sessions") or []) + [sid]
    cur["manualRun"] = run
    span = cur["at"] - (run.get("at") or cur["at"])
    longest = max([p for p in (period_secs(t.get("schedule")) for t in tasks) if p], default=0)
    who_all = "、".join(s[:12] for s in (run.get("sessions") or [])) or "（未知会话）"
    detail = (f"呼吸 {pp['breath']} → {cp['breath']} 是 **{sid[:12] or '未知会话'}** 写的"
              f"（{src}{(':' + str(att['taskId'])[:8]) if att.get('taskId') else ''}，经{via}），"
              f"不是本项目心跳任务 {sorted(drivers['taskIds'])[:2] or '（无）'} 的脉体会话"
              f"{sorted(drivers['sessions']) or '（无）'}")
    advice = "——心跳未被驱动，只有人在推：查心跳任务（schedule/enabled/会话是否卡住）与 sticky 会话是否存在"
    if longest and span > longest:
        out.append(dict(level="degraded", group="B",
                        msg=f"**心跳未驱动、仅人工推进（持续 {span // 60} 分钟 > 一个心跳周期）**：{detail}；"
                            f"自呼吸 {run['from']} 起累计人工推进 {run['from']} → {run['to']}，驱动者 {who_all}{advice}"))
    else:
        out.append(dict(level="warn", group="B",
                        msg=f"**心跳未驱动、仅人工推进**：{detail}{advice}"))
    return out


# ---------------------------------------------------------------------------
# 判据 A：一跳到底是「干了活」还是「又被跳过」
# ---------------------------------------------------------------------------
def judge_runs(t: dict) -> list[dict]:
    """把「runCount 递增」拆成**真有产出**与**只是被跳过**。

    背景（真事）：
      · `skipped` = 「上次执行尚未结束，本轮跳过」——它让 runCount +1，且 lastStatus 往往仍是 success（
        上一次真跑的结果），于是旧版 A 组全程绿、退出码 0，而演进已经停在那里；
      · 本仓真数据：`task-runs/769433…` = 26 success + 3 skipped，且这 3 跳连着落在中间；
      · 同一份数据 `heartbeat_e2e.py` 会打印「3 skipped」，而哨兵说「A 组健康」——两把尺子自相矛盾。

    判据（阈值是常量，可一处调）：
      · 末尾连续跳过 >= SKIP_STREAK_DEGRADED      → degraded（「连跳 N 跳 = N×周期内零成果」）
       · 窗口（最近至多 12 跳）跳过占比 >= 50% 且窗口 >= 4 → degraded（过半触发没干活）
       · 末尾连续跳过 == SKIP_STREAK_WARN          → warn（还不能断定死，但**不得当成健康**）
       · 有跳过但未过阈                         → ok，但消息里把「M 跳里有 K 跳被跳过」写出来
       · 无运行记录（取证不可得）                  → unknown（不假装看过）
    """
    r = t.get("runs")
    if not isinstance(r, dict) or not r.get("known"):
        # 旧快照没这个字段 / 新任务刚建还没有记录——一律 unknown，不归退化（快照兼容纪律）。
        return [dict(level="unknown", group="A",
                     msg=f"{t.get('name')}：取不到运行记录（task-runs 空或不可读）——"
                         "runCount 只能证明「有动作」，不能证明「有产出」")]
    out: list[dict] = []
    name = t.get("name")
    why = f"（最近一次：{r.get('last_skip_reason') or '未记因'}）"
    if r["streak"] >= SKIP_STREAK_DEGRADED:
        out.append(dict(level="degraded", group="A",
                        msg=f"**连跳 {r['streak']} 跳 = 零成果**：{name} 最近 {r['streak']} 次触发全被跳过{why}"
                            "——runCount 在涨但息没流，查上一次执行是否卡死/未结束"))
    elif r["window"] >= SKIP_WINDOW_MIN and r["ratio"] >= SKIP_RATIO_DEGRADED:
        out.append(dict(level="degraded", group="A",
                        msg=f"**跳过过半**：{name} 最近 {r['window']} 跳里 {r['window_skipped']} 跳被跳过"
                            f"（{r['ratio']:.0%}）{why}——有动作无进展"))
    elif r["streak"] >= SKIP_STREAK_WARN:
        out.append(dict(level="warn", group="A",
                        msg=f"{name} 连跳 {r['streak']} 跳{why}——未到退化阈值，但本息不算有成果"))
    else:
        out.append(dict(level="ok", group="A",
                        msg=f"{name}：运行 {r['total']} 跳 = {r['success']} 成功 / {r['skipped']} 跳过 / {r['failed']} 失败"
                            + (f"（连续跳过 {r['streak']}）" if r["streak"] else "")))
    return out


# ---------------------------------------------------------------------------
# 判定
# ---------------------------------------------------------------------------
def evaluate(cur: dict, prev: dict | None) -> list[dict]:
    """每条 = {level, group, msg}。level: ok / degraded / unknown。"""
    verdict: list[dict] = []

    # ---- A. 息在流 ----
    tasks = cur["tasks"]
    if not tasks:
        verdict.append(dict(level="degraded", group="A",
                            msg="没有心跳任务——演进线不会被驱动（用 shengsheng_state action=task op=ensure 建）"))
    else:
        prev_by_id = {t["id"]: t for t in (prev["tasks"] if prev else [])}
        for t in tasks:
            p = prev_by_id.get(t["id"])
            if not t.get("enabled"):
                verdict.append(dict(level="degraded", group="A", msg=f"心跳任务停用：{t['name']}"))
                continue
            if t.get("lastStatus") == "error" or t.get("lastError"):
                # ⚠ 区分「历史错误」与「现在坏了」：任务已在运行中（state=running）说明
                # 它正在自我修复，此时报退化会让人去手动干预一个已经在做的事。
                # 本轮实测：服务重启打断了一次执行，补跑已入队，哨兵仍报退化。
                if t.get("state") == "running" or t.get("running"):
                    verdict.append(dict(level="ok", group="A",
                                        msg=f"{t['name']}：上次出错（{str(t.get('lastError'))[:80]}），已在重新执行中"))
                else:
                    verdict.append(dict(level="degraded", group="A",
                                        msg=f"心跳最近一次出错：{t['name']} — {str(t.get('lastError'))[:160]}"))
            # ---- 「这跳真的干了活吗」：runCount 递增只是“有动作”，不是“有进展” ----
            # 必须先判这一条：若最近一味 skipped，那就不管 runCount 涨不涨都不是“息在流”。
            runs_v = judge_runs(t)
            verdict.extend(runs_v)
            runs_bad = any(v["level"] == "degraded" for v in runs_v)
            if p is not None:
                if t.get("runCount", 0) <= p.get("runCount", 0):
                    # 没递增：**只有在“已过一个周期”时才是退化**。否则（周期未到）不是问题——
                    # 本轮实测踩到：相隔 22 秒的两份快照（周期 3 分钟）被误判成停滞。
                    period = period_secs(t.get("schedule"))
                    elapsed = cur["at"] - (prev.get("at") or cur["at"])
                    if period and elapsed > period * 1.5:
                        verdict.append(dict(level="degraded", group="A",
                                            msg=f"**心跳停滞**：{t['name']} 已过 {elapsed // 60} 分钟无新增（周期 {period // 60} 分钟，累计 {t.get('runCount')}）——查 lastError 与会话是否卡住"))
                    elif period:
                        verdict.append(dict(level="ok", group="A",
                                            msg=f"{t['name']}：周期未到（{elapsed}s / {period}s），不算停滞"))
                    else:
                        verdict.append(dict(level="unknown", group="A",
                                            msg=f"runCount 未递增：{t['name']}（{p.get('runCount')} → {t.get('runCount')}）——周期未知，无法判定"))
                else:
                    # 递增了：但「涨在哪里」已由 judge_runs 分开报（成功/跳过）。不要让本行再说一句
                    # 「+N 次 → ok」——那就回到「把有动作当有进展」的老错上（两个 ok 并列会让人只看得出“绿”）。
                    if runs_bad:
                        verdict.append(dict(level="degraded", group="A",
                                            msg=f"**runCount 递增但无产出**：{t['name']} +{t.get('runCount', 0) - p.get('runCount', 0)} 次"
                                                f"（累计 {t.get('runCount')}）——涨幅来自跳过，不代表息在流"))
                    else:
                        verdict.append(dict(level="ok", group="A",
                                            msg=f"{t['name']}：+{t.get('runCount', 0) - p.get('runCount', 0)} 次（累计 {t.get('runCount')}），"
                                                "且其中含真执行（见上行分类）"))

    # ---- B. 平台在被改，**且是心跳改的**（归因）----
    pp, cp = (prev["platform"] if prev else None), cur["platform"]
    # 同为“周期内不动不算病”：平台线也在心跳周期上推进，离得太近的快照不能当证据。
    # 取所有心跳任务里最短周期的 1.5 倍作为“该动了”的门槛。
    shortest = min((period_secs(t.get("schedule")) for t in tasks if period_secs(t.get("schedule"))), default=0)
    elapsed_b = cur["at"] - prev["at"] if prev else 0
    due_yet = bool(shortest) and elapsed_b > shortest * 1.5
    if cp is None:
        verdict.append(dict(level="degraded", group="B", msg="平台线未 init（无 .evolve/control.md）——自举闭环缺平台端"))
    elif pp is not None:
        verdict.extend(judge_breath(cur, prev, pp, cp, tasks, due_yet, elapsed_b))
        if cp["ledger_size"] > pp["ledger_size"]:
            verdict.append(dict(level="ok", group="B", msg=f"台账增长 {pp['ledger_size']} → {cp['ledger_size']} 字节（有东西被收藏）"))
        elif due_yet:
            verdict.append(dict(level="degraded", group="B",
                                msg=f"台账已过一个周期无增长（{cp['ledger_size']} 字节）——只派不收：worker 的报告没进永久记忆"))
        else:
            verdict.append(dict(level="ok", group="B", msg=f"台账未变（{cp['ledger_size']} 字节，周期未到）"))

    # ---- C. 闭环真跑（本哨兵的核心）----
    if cp is not None:
        fed = cp["fed_seeds"]
        if not fed:
            verdict.append(dict(level="degraded", group="C",
                                msg="平台线种子里**没有反哺条目**——业务线的调度层问题没回流，闭环断在最后一环"))
        else:
            verdict.append(dict(level="ok", group="C", msg=f"平台线有 {len(fed)} 条反哺种子（闭环有入口）"))
            # 反哺种子若长期不被派，就是"只落种子躺着"——那也是断链。
            # ⚠ 门槛要宽一点：**只要还有 worker 在池（不管 idle 还是 running），就不算“不前进”**——
            # idle 的 worker 是刚交完一单的，下一息就会再派；而“在途 0 但池里有 idle”正是
            # 刚收藏完的正常间隙。真正的“只择不生”是：**池空 + 在途空 + 种子堆积**。
            # 本轮实测：worker 刚被收藏、在途清空的那一刻，哨兵报“只择不生”，而它明明刚干完活。
            if cp["inflight"] == 0 and cp["workers"] == 0 and cp["seeds"] > 0:
                verdict.append(dict(level="degraded", group="C",
                                    msg=f"**只择不生**：种子 {cp['seeds']} 条堆积，但池空且在途 0——没人干活，闭环不前进"))
            elif cp["inflight"] == 0 and cp["workers"] > 0:
                verdict.append(dict(level="ok", group="C",
                                    msg=f"池里还有 {cp['workers']} 个 worker（无在途）——刚收藏完的正常间隙"))
        # 业务线的反哺来源必须可溯源
        bp = cur["business"]
        if bp is not None and cp["workers"] == 0 and bp["workers"] == 0:
            verdict.append(dict(level="unknown", group="C", msg="两线都没有 worker——当前无正在推进的工作"))

    # ---- D. 修复没被回退 ----
    # ⚠ 第一步不是查锚点，是问「**门禁在不在跑**」：reverse_verify 跑测试时会**故意**把源码回退成
    # 缺陷版，这时锚点必缺、测试必红。旧版 D 组没有这一问，已两次在生产上误报
    # （10:30「8 条测试红」、10:35「init 不跨线继承锚点缺失」），最坏后果是人去“恢复”一个门禁正在改的
    # 文件，正好把回填撞掉。故门禁在场时 D 组**整组判 unknown**，且一个字都不报 degraded。
    gate = cur.get("gate")
    t = cur["tests"]
    gated_soft = bool(t.get("gated"))          # 门禁在**本息窗口内**起过（起跑后探测到）：读数已被污染
    if gate:
        why = "；".join(gate.get("reasons") or [])
        verdict.append(dict(level="unknown", group="D",
                            msg=f"**门禁正在回退源码，本息不判修复是否被回退**（{why}）"
                                "——等门禁跑完（锁消失/无 revbak）再验；此时锚点缺失与测试红皆为中间态"))
        return verdict
    missing = [a for a in cur["anchors"] if not a["present"]]
    if missing and gated_soft:
        verdict.append(dict(level="unknown", group="D",
                            msg=f"锚点缺失（{', '.join(a['name'] for a in missing)}）但**本息测试期间门禁起过**"
                                "——哪个时候读的算哪个时候的账，不据此判“修复被回退”"))
    elif missing:
        verdict.append(dict(level="degraded", group="D",
                            msg=f"修复锚点缺失（疑被回退）：{', '.join(a['name'] for a in missing)}"))
    elif gated_soft:
        verdict.append(dict(level="ok", group="D",
                            msg=f"{len(cur['anchors'])} 个修复锚点全在（读数取自本息起跑前；门禁在本息窗口内起过）"))
    else:
        verdict.append(dict(level="ok", group="D", msg=f"{len(cur['anchors'])} 个修复锚点全在"))
    if t.get("gated"):
        why = "；".join((t.get("gate") or {}).get("reasons") or [])
        verdict.append(dict(level="unknown", group="D",
                            msg=f"测试本息未判（门禁在跑：{why or '探测命中'}）——此时的红/绿都不是「修复被回退」的证据"))
    elif t.get("skipped"):
        verdict.append(dict(level="unknown", group="D", msg="测试本息未跑（--no-tests 快速模式）——锚点在但行为未验"))
    elif t.get("ok"):
        verdict.append(dict(level="ok", group="D", msg=f"测试 {t.get('passed')} passed / 0 failed"))
    else:
        verdict.append(dict(level="degraded", group="D",
                            msg=f"测试未绿：{t.get('passed')} passed / {t.get('failed')} failed（rc={t.get('rc')}）{t.get('error', '')}"))

    return verdict


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--snapshot", action="store_true")
    ap.add_argument("--state", action="store_true")
    ap.add_argument("--no-tests", action="store_true", help="跳过测试（哨兵快速模式）")
    ap.add_argument("--anchors", action="store_true",
                    help="只跑修复锚点检查（收单前那道：任一 present=false 即拒收；退出码 1）")
    # 夹具/演练开关：把“跑哪条线、查哪份用户数据、快照落哪”全换掉——判据 B 的归因必须能在
    # 临时 root 上演练（构造「只有人工 note」的场景），而**绝不能碰生产** .evolve/control.md 与快照历史。
    ap.add_argument("--root", help="平台线项目根（缺省=本仓）")
    ap.add_argument("--business", help="业务线项目根（缺省=<root>/shuangtian）")
    ap.add_argument("--home", help="用户数据根（users/ 所在目录；缺省=本仓）")
    ap.add_argument("--state-file", help="快照文件路径（缺省=<root>/.evolve/sentinel-state.json）")
    # 门禁探测的两个坐标（演练用）：把锁/源码目录指到别处，就能在**真实树**上演示「门禁在场」
    # 而不去碰生产 `.reverse_verify.lock`（真写它会把真正要跑的门禁拦在门外）。
    ap.add_argument("--src", help="会被门禁回退的源码目录（缺省=<本仓>/packages/agents/src/agents/shengsheng）")
    ap.add_argument("--lock", help="门禁锁文件路径（缺省=<本仓>/.reverse_verify.lock）")
    args = ap.parse_args()

    if args.anchors:
        # 收单前的**那一道**：只看锚点，不写快照、不跑测试、不判 ABCD 四组。
        # 为什么必须能单跑：收单是人对人做的事（比如 review 一个 worker 的提交），此时要的是
        # 「我盯着的这棵树上，8 条修复锚点还都在吗」一个答案——不该为了问它而等两轮测试、
        # 也不该顺手把一息快照写进生产 state（那会给下一息的趋势判据塞一条假读数）。
        # 退出码口径：1 = 有锚点缺失（**拒收**），0 = 全在（stale_hints 只是提示，不影响退出码）。
        rows = anchor_report()
        miss = [r for r in rows if not r["present"]]
        hints = stale_anchor_hints(rows)
        print(json.dumps({"anchors": rows,
                          "ok": not miss,
                          "missing": [r["name"] for r in miss],
                          "stale_hints": hints}, ensure_ascii=False, indent=2))
        return 0 if not miss else 1

    if args.root:
        CFG["root"] = Path(args.root).resolve()
        CFG["business"] = CFG["root"] / "shuangtian"
        CFG["state"] = CFG["root"] / ".evolve" / "sentinel-state.json"
    if args.business:
        CFG["business"] = Path(args.business).resolve()
    if args.home:
        CFG["home"] = Path(args.home).resolve()
    if args.state_file:
        CFG["state"] = Path(args.state_file).resolve()
    if args.src:
        CFG["src"] = Path(args.src).resolve()
    if args.lock:
        CFG["lock"] = Path(args.lock).resolve()

    st = load_state()
    if args.state:
        print(json.dumps({"snapshots": st["snapshots"][-10:], "count": len(st["snapshots"])},
                         ensure_ascii=False, indent=2))
        return 0

    cur = take_snapshot()
    cur["anchors"] = anchor_report()
    # 门禁探测在**跑测试之前**（也让锚点结果能被正确解释：门禁回退时锚点缺失是中间态）。
    cur["gate"] = gate_probe()
    if args.no_tests:
        cur["tests"] = {"ok": True, "skipped": True}
    else:
        cur["tests"] = run_tests(gated=True)
    prev = st["snapshots"][-1] if st["snapshots"] else None
    # 人工推进计数（manualRun）**从上一份快照继承**：它记的是「自哪一息开始只见人工/不明推进」，
    # 跨次运行才有意义（单次跑完就丢，就永远升不到「持续超一个周期」那条退化）。
    # 先接上再判，判完把（可能被更新的）结果写回快照。
    if prev and prev.get("manualRun"):
        cur["manualRun"] = prev["manualRun"]

    if args.snapshot:
        st["snapshots"].append({k: cur[k] for k in ("at", "platform", "business", "tasks", "manualRun") if k in cur})
        save_state(st)
        print(json.dumps({"snapped": True, "at": cur["at"]}, ensure_ascii=False))
        return 0

    verdict = evaluate(cur, prev)
    # 快照在**判完之后**落盘：judge_breath 会改写 cur["manualRun"]（开新窗口/归零），那才是下一份该继承的态。
    snap = {k: cur[k] for k in ("at", "platform", "business", "tasks")}
    snap["manualRun"] = cur.get("manualRun")
    # 快照里的 platform 不必把整行归因全文留下（只留解析结果，体积小、可直接复用）
    st["snapshots"].append(snap)
    save_state(st)

    degraded = [v for v in verdict if v["level"] == "degraded"]
    warned = [v for v in verdict if v["level"] == "warn"]
    result = {
        "at": cur["at"],
        "health": "degraded" if degraded else ("warn" if warned else "ok"),
        "degraded_count": len(degraded),
        "warn_count": len(warned),
        "verdict": verdict,
        "platform": {k: (cur["platform"] or {}).get(k) for k in ("breath", "seq", "workers", "inflight", "seeds")},
        "platform_last_note": (cur["platform"] or {}).get("last_note"),
        "manual_run": cur.get("manualRun"),
        "business": {k: (cur["business"] or {}).get(k) for k in ("breath", "seq", "workers", "inflight", "seeds")},
        "tasks": cur["tasks"],
        "tests": cur["tests"],
        "gate": cur["gate"],
    }
    if args.json:
        print(json.dumps(result, ensure_ascii=False, indent=2))
    else:
        icon = {"ok": "OK  ", "degraded": "退化", "warn": "告警", "unknown": "待核"}
        head = (f"退化（{len(degraded)} 项）" if degraded
                else f"告警（{len(warned)} 项）" if warned else "健康")
        print(f"生生自举闭环监督 · {time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(cur['at']))}")
        print(f"总判：{head}\n")
        for v in verdict:
            print(f"[{icon[v['level']]}] {v['group']} {v['msg']}")
        for t in cur["tasks"]:
            r = t.get("runs") or {}
            if r.get("known"):
                print(f"      · 运行记录 {str(t.get('id'))[:8]}：{r['total']} 跳 = {r['success']} success /"
                      f" {r['skipped']} skipped / {r['failed']} failed（末尾连跳 {r['streak']}）")
    # 告警也是「不可当成健康」：监督任务据退出码行动，告警必须让它行动（否则人驱动推进又能蒙混过去）。
    return 1 if (degraded or warned) else 0


if __name__ == "__main__":
    sys.exit(main())
