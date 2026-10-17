#!/usr/bin/env python3
"""生生门禁的**共享护栏**：并发检测 + 陈旧锁 + 信号收尾通道。

为什么要有这个模块（真实事故，不是假想）：
  · 09:39 主门禁 `reverse_verify.py` 起跑时，同树另一个 `reverse_verify.py` 也在跑，
    两者互相回退同一份 `shengsheng.ts`，结果一个 worker 报「莫名变红」——**门的残留冒充真缺陷**。
  · 10:10 主门禁起跑时同树有 `pty_form_reverse_verify.py` 在跑，主门禁照样开跑；被手动 stop 后
    源码停在回退态并留下 `shengsheng.ts.revbak`，测试 62 pass / 1 fail，靠人工 cp 回去。

**为什么放在这里、且只有这一份实现**（反「同仓两套标准」）：
  1. 判据里的「同树」就是本文件所在目录——护栏与被保护的文件（`shengsheng.ts`、`.revbak`）
     同处一树，判据与被判对象不分离，换不来「我们俩说的同树不是同一棵树」这种裂缝。
  2. `reverse_verify.py` 是唯一的生产门禁，它的目录是天然归处；演练脚本是**消费方**，
     用 `sys.path` 指过来即可（一行），生产脚本的部署形态不变。
  3. 本仓已有 O(N) 份「自己扫 /proc 找并发」的私有实现（`reverse_verify.py` 的
     `warn_if_other_tests_running`、演练脚本的 `other_bun_tests`），彼此判据还不一样——
     这正是「两套标准」的土壤。现在两处都改成调用本模块，判据只有一份。

对外三件事：
  `detect_peers()`  —— 扫同树在跑的验证进程（**排除自己、祖先、后代**，否则会检测到自己）
  `preflight()`     —— 起跑前闸门：命中并发 → 拒绝并退出码 3；`force=True` 才放行并**在输出开头**写明警告；
                       起跑后写 `.reverse_verify.lock`（含 pid / 时间 / 命令行 / boot_id），退出时删；
                       陈旧锁按「pid 是否存活 + boot_id 是否同一次开机」判定自清。
    `Traps`           —— 信号收尾通道：第一次 SIGINT/SIGTERM 只置标志交给宿主 finally；
                      **之后的 SIGINT/SIGTERM 全部忽略**（这是 10:10 事故的根治点：第二次 SIGTERM
                       的默认处置会在回填/校验中途直接杀进程，finally 永远跑不完）。
                       退出码仍由宿主 `os._exit(130/143)` 显式给出（`exit_code_for()` 给同一口径），
                       不走「自送信号」——那要赌内核送达时机，而调用方（演练/上层任务）要的是确定的码。

用法（两个脚本共用；见各自调用点）：
    from verify_guard import preflight, Traps
    g = preflight(tree=SRC.parent, name="reverse_verify.py", force=args.force)
    if g.refused:
        return verify_guard.EXIT_CONCURRENT        # 3

**这个护栏在什么情况下会失效**（诚实声明，别当万能；报告与 DESIGN 同步）：
  ① 验证进程跑在**别的树**（例如从 `git worktree` 里起的）：本模块只看「同树」吗？——不，
     现在扫的是整机命令行（不分 cwd），所以反而**会**拦住跨树并发的本仓门禁；但反过来，
     若那个进程的命令行不含 `reverse_verify*.py`（改了名、被包成别的 exe、在容器/另一个
     PID 命名空间里），就看不见——**判据是命令行文本，不是「谁在写这个文件」**。
  ② 有进程用 `flock`/内核级独占打开？不——本模块的锁只是**marker 文件**，不是内核互斥；
     两个进程同时起跑（都读到「无锁」）的窗口靠「先写锁再干活」缩小，但**不是原子**的。
  ③ 锁文件与真实进程不同步：持有者被 SIGKILL/断电后锁留在盘上——按 pid 存活 + boot_id 自清
     （pid 被复用给无关进程时会误判为「有人持锁」→ 拒绝，安全侧失败，加 `--force` 可越过）。
  ④ 判据是**子串匹配**：`grep reverse_verify.py` 这类命令行也会被当成「有人跑验证」（假阳性，
     安全侧失败）；要躲开用 `--force` 或 `REVERSE_VERIFY_PEER_IGNORE=<正则>`。
  ⑤ 非 Linux / 无 `/proc`：无法枚举进程，只剩锁文件兜（本模块会显式打印这条降级）。
  ⑥ 本模块不消除**竞态本身**，只让竞态被**看到并被拒绝**；根治是把回退型门禁搬进独立
     `git worktree`（另一单，见 `.evolve/ledger.md` 的建议下一条）。
"""
from __future__ import annotations

import datetime as _dt
import os
import pathlib
import re
import signal
import sys

DEFAULT_TREE = pathlib.Path(__file__).resolve().parent   # 模块所在目录 = 门禁与其被保护文件所在的树
LOCK_NAME = ".reverse_verify.lock"
EXIT_CONCURRENT = 3                                      # 区别于「验证本身失败」（1/2）
INT_EXIT = {signal.SIGINT: 130, signal.SIGTERM: 143}

# ── 判据（三条，缺一不可解释）─────────────────────────────────────────────────
# ① 本仓所有回退型门禁都叫 `*reverse_verify*.py`（reverse_verify.py /
#    reverse_verify_kill_drill.py / shuangtian/tools/pty_form_reverse_verify.py …）。
#    要求「以 .py 结尾的路径词」——这样 `grep reverse_verify`（不带 .py）不会误中。
VERIFY_PY_RE = re.compile(r"(?:^|[\s/='\"(])([^\s/='\"()]*reverse_verify[^\s'\"()]*\.py)(?=$|[\s'\"()])")
# ② 任何 `bun test …`（门禁自己跑测试时，别人跑 bun test 会读到回退版源码 → 假红）。
BUN_TEST_RE = re.compile(r"(?:^|[\s/='\"(])bun(?:\.exe)?[\s]+test(?:[\s'\"(]|$)")
# ③ 逃生口：额外判据，**逗号分隔**的正则（别的门禁脚本改了名时用）。
EXTRA_PATTERNS_ENV = "REVERSE_VERIFY_PEER_PATTERNS"
# ④ 逃生口：命中即排除，**逗号分隔**的正则（自检/演练时屏蔽已知噪声；含空格的判据靠它）。
IGNORE_ENV = "REVERSE_VERIFY_PEER_IGNORE"
# ⑤ 逃生口：按 **pid 精确排除**（逗号分隔整数）。给自检/演练的“正面控制”用：
#    “场上确实有别人在跑”时，要验“无并发⇒放行”就只能精确屏蔽那几个已知进程，
#    而不是糙糙地写一条宽正则（宽正则会把判据整段旁路化，那也等于没有判据）。
IGNORE_PIDS_ENV = "REVERSE_VERIFY_PEER_IGNORE_PIDS"


def _signame(signum: int) -> str:
    try:
        return signal.Signals(signum).name
    except Exception:
        return f"signal {signum}"


def _boot_id() -> str:
    try:
        return pathlib.Path("/proc/sys/kernel/random/boot_id").read_text().strip()
    except Exception:
        return ""


def has_procfs() -> bool:
    return pathlib.Path("/proc/self/cmdline").exists()


def _pid_alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True          # 存在但不属我（仍算活着）
    except Exception:
        return False
    # 僵尸也算「死」：它不会再改写任何文件
    try:
        stat = pathlib.Path(f"/proc/{pid}/stat").read_text()
        return stat.rsplit(")", 1)[1].split()[0] != "Z"
    except Exception:
        return True


def _ppid_map() -> dict[int, int]:
    out: dict[int, int] = {}
    if not has_procfs():
        return out
    for entry in pathlib.Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            stat = (entry / "stat").read_text()
            out[int(entry.name)] = int(stat.rsplit(")", 1)[1].split()[1])
        except Exception:
            continue
    return out


def ancestors(pid: int | None = None, ppid_map: dict[int, int] | None = None) -> list[int]:
    """自己 + 祖先链（到 1 为止）。"""
    pmap = ppid_map if ppid_map is not None else _ppid_map()
    pid = os.getpid() if pid is None else pid
    chain = [pid]
    cur = pid
    for _ in range(64):
        nxt = pmap.get(cur, 0)
        if nxt <= 1 or nxt in chain:
            break
        chain.append(nxt)
        cur = nxt
    return chain


def descendants(pid: int, ppid_map: dict[int, int] | None = None) -> set[int]:
    """自己的后代（我们亲手起的子进程都在这儿——它们是我们的一部分，不是「别人」）。"""
    pmap = ppid_map if ppid_map is not None else _ppid_map()
    out: set[int] = set()
    frontier = [pid]
    while frontier:
        cur = frontier.pop()
        for child, parent in pmap.items():
            if parent == cur and child not in out:
                out.add(child)
                frontier.append(child)
    return out


class Peer:
    """一个被判定为「正在做验证」的外部进程。"""

    def __init__(self, pid: int, cmd: str, kind: str, cwd: str = ""):
        self.pid = pid
        self.cmd = cmd
        self.kind = kind
        self.cwd = cwd

    def same_tree(self, tree: pathlib.Path) -> bool:
        """它的工作目录是否就在这棵树里（树内 / 树外靠这个分辨）。"""
        if not self.cwd:
            return False
        try:
            return pathlib.Path(self.cwd).resolve().is_relative_to(tree.resolve())
        except Exception:
            return False

    def line(self, tree: pathlib.Path | None = None) -> str:
        where = ""
        if self.cwd:
            where = f"｜cwd {self.cwd}"
            if tree is not None:
                where += "（同树）" if self.same_tree(tree) else "（他树）"
        return f"pid {self.pid}（{self.kind}）：{self.cmd}{where}"

    def __repr__(self) -> str:      # pragma: no cover - 调试用
        return f"Peer({self.pid}, {self.kind!r}, {self.cmd!r})"


def _cwd_of(pid: int) -> str:
    try:
        return os.readlink(f"/proc/{pid}/cwd")
    except Exception:
        return ""        # 已退出 / 无权 / 非 Linux（readlink 失败不影响判据，只是少一个坐标）


def _flat_cmdline(pid: int) -> str:
    try:
        raw = pathlib.Path(f"/proc/{pid}/cmdline").read_bytes()
    except Exception:
        return ""
    return raw.replace(b"\0", b" ").decode("utf-8", "replace").strip()


def _compile_env_patterns(value: str) -> re.Pattern | None:
    """把环境变量里的多个正则编成一个（**逗号分隔**）。

    ⚠ 为什么是逗号而不是空格：判据里天然带空格（`bun test`、`tsc --noEmit`）——
    按空格拆会把它们拆成两半，于是 ignore 静默失效（本单实测踩到：屏蔽了却仍然拒绝）。
    """
    value = (value or "").strip()
    if not value:
        return None
    parts = [p.strip() for p in value.split(",") if p.strip()]
    try:
        return re.compile("|".join(f"(?:{p})" for p in parts))
    except re.error:
        return None


def _classify(flat: str, include_bun_test: bool, extra: re.Pattern | None) -> str:
    if VERIFY_PY_RE.search(flat):
        return "门禁脚本"
    if include_bun_test and BUN_TEST_RE.search(flat):
        return "bun test"
    if extra is not None and extra.search(flat):
        return "额外判据"
    return ""


# 「只看不跑」的命令：它们把 `reverse_verify.py` 当**参数**而不是当成自己要跑的程序。
# 本仓真有这类进程（实测：另一条线的等待循环里写着 `pgrep -f "reverse_verify.py"`，
# 被当成「有人在跑验证」→ 假阻）。
#
# **为什么排除它们不会漏掉真的门禁**：门禁自己是独立进程，它的 cmdline 就是
# `python3 …/reverse_verify.py`（argv[0] 不是观察器）；拉起它的 `timeout` 包装器也在（同样命中）。
# 所以“排除观察器”不会把真门禁一起丢掉——这是可证明的，不是拍脑袋。
OBSERVER_PROGS = {
    "pgrep", "pgrep.exe", "ps", "top", "htop", "grep", "egrep", "fgrep", "rg", "ag",
    "cat", "head", "tail", "less", "more", "wc", "ls", "stat", "md5sum", "sha256sum",
    "vim", "nvim", "vi", "nano", "diff", "cmp", "file", "tree", "journalctl",
}


def _argv0_base(flat: str) -> str:
    first = flat.strip().split()[0] if flat.strip() else ""
    return os.path.basename(first).lower()


def _is_observer(flat: str, match_start: int) -> bool:
    """该进程只是「在看/在搜」这个词，不是真的要跑它。"""
    if _argv0_base(flat) in OBSERVER_PROGS:
        return True
    # shell 包装器：`bash -c '… pgrep -f "reverse_verify.py" …'`
    # 判据：在命中位置**之前**已经出现了观察命令（pgrep/ps/grep/rg）。
    pre = flat[:match_start]
    return any(re.search(rf"(?:^|[\s;&|(])(?:{p})\s", pre) for p in
               ("pgrep", "ps", "grep", "egrep", "fgrep", "rg"))


def detect_peers(*, extra_patterns: str = "", include_bun_test: bool = True,
                 quiet: bool = False) -> list[Peer]:
    """扫同机命令行，找出**别人**正在做的验证。

    ⚠ 必须先排除自己 / 祖先 / 后代：`pgrep -af reverse_verify` 会命中它自己那条命令
    （命令行里就含这个串），也会命中拉它的 shell——不排除的话「永远有人在跑」是必然。
    """
    if not has_procfs():
        if not quiet:
            print("⚠ 没有 /proc（非 Linux）：并发检测不可用，只剩锁文件兜（见 verify_guard 模块 docstring ⑥）")
        return []
    me = os.getpid()
    pmap = _ppid_map()
    skip = set(ancestors(me, pmap)) | descendants(me, pmap)
    extra = _compile_env_patterns(extra_patterns or os.environ.get(EXTRA_PATTERNS_ENV, ""))
    ignore = _compile_env_patterns(os.environ.get(IGNORE_ENV, ""))
    ignore_pids = {int(x) for x in (os.environ.get(IGNORE_PIDS_ENV, "").replace(" ", "") or "").split(",")
                   if x.strip().isdigit()}
    peers: list[Peer] = []
    for entry in pathlib.Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        pid = int(entry.name)
        if pid in skip or pid == me or pid in ignore_pids:
            continue
        flat = _flat_cmdline(pid)
        if not flat:
            continue                       # 内核线程 / 已退出 / 无权读
        kind = _classify(flat, include_bun_test, extra)
        if not kind:
            continue
        m = VERIFY_PY_RE.search(flat)
        if kind == "门禁脚本" and m is not None and _is_observer(flat, m.start()):
            continue                       # `pgrep`/`grep`/`vim` 把名字当参数：不算“在跑验证”
        if ignore is not None and ignore.search(flat):
            continue
        peers.append(Peer(pid, flat[:160], kind, _cwd_of(pid)))
    peers.sort(key=lambda p: p.pid)
    return peers


# ── 锁文件 ──────────────────────────────────────────────────────────────────
class Lock:
    """`<tree>/.reverse_verify.lock`：marker 文件（pid / 时间 / 命令行 / boot_id）。

    它是**辅助**判据，不是内核互斥：并发检测看命令行，锁看「登记」——两者互补，
    命令行看不见（改了名/跨命名空间）时锁还能兜住；锁陈旧时按 pid 存活 + boot_id 自清。
    """

    def __init__(self, path: pathlib.Path):
        self.path = path

    # ---- 读 ----
    def info(self) -> dict[str, str]:
        try:
            text = self.path.read_text(errors="replace")
        except Exception:
            return {}
        out: dict[str, str] = {}
        for line in text.splitlines():
            if "=" in line:
                key, _, val = line.partition("=")
                out[key.strip()] = val.strip()
        return out

    def state(self) -> tuple[str, dict[str, str]]:
        """('absent'|'stale'|'active', info)。boot_id 不同（换了一次开机）一律算陈旧。"""
        if not self.path.exists():
            return "absent", {}
        info = self.info()
        try:
            pid = int(info.get("pid", "0"))
        except ValueError:
            pid = 0
        if pid <= 0:
            return "stale", info
        boot = info.get("boot_id", "")
        if boot and _boot_id() and boot != _boot_id():
            return "stale", info
        return ("active" if _pid_alive(pid) else "stale"), info

    def purge(self, *, why: str) -> None:
        print(f"清理{why}：{self.path.name}"
              f"（pid {self.info().get('pid', '?')}，{self.info().get('time', '?')}）")
        try:
            self.path.unlink(missing_ok=True)
        except Exception as exc:
            print(f"  ⚠ 删不掉：{type(exc).__name__}: {exc}（不影响本轮，但下轮还会看到它）")

    # ---- 写 ----
    def take(self, *, label: str) -> bool:
        flat = _flat_cmdline(os.getpid()) or " ".join(sys.argv)
        body = "\n".join([
            f"pid={os.getpid()}",
            f"boot_id={_boot_id()}",
            f"time={_dt.datetime.now().isoformat(timespec='seconds')}",
            f"label={label}",
            f"cmd={flat[:400]}",
            "",
        ])
        tmp = self.path.with_name(self.path.name + ".tmp")
        try:
            tmp.write_text(body)
            os.replace(tmp, self.path)
            return True
        except Exception as exc:
            print(f"⚠ 锁文件写不进去（{type(exc).__name__}: {exc}）——本轮无锁保护，并发检测仍按命令行判据生效")
            return False

    def release(self) -> None:
        info = self.info()
        try:
            if info.get("pid") == str(os.getpid()):
                self.path.unlink(missing_ok=True)
        except Exception:
            pass


# ── 起跑前闸门 ──────────────────────────────────────────────────────────────
class Preflight:
    """`preflight()` 的返回值：宿主据此决定「拒绝」还是「跑」，并在收尾时复核。"""

    def __init__(self, *, tree: pathlib.Path, name: str, forced: bool,
                 peers: list[Peer], lock: Lock | None, lock_held_by: str):
        self.tree = tree
        self.name = name
        self.forced = forced
        self.peers = peers
        self.lock = lock
        self.lock_held_by = lock_held_by    # "" | "other" | "ancestor"
        self.refused = False
        self.start_count = len(peers)

    # ---- 收尾复核 ----
    def report_end(self) -> None:
        """收尾再扫一次：起跑时有人（--force 下的已知噪声）与中途有人，是两回事。"""
        later = detect_peers(quiet=True)
        if not self.forced and not later:
            return
        print("并发复核（收尾）：起跑时 "
              f"{self.start_count} 个 → 现在 {len(later)} 个"
              + ("；仍在跑：" + "；".join(p.line(self.tree) for p in later) if later else "（树已静）"))

    def release(self) -> None:
        if self.lock is not None:
            self.lock.release()
            self.lock = None


def preflight(*, tree: pathlib.Path = DEFAULT_TREE, name: str = "reverse_verify.py",
              force: bool = False, extra_patterns: str = "",
              include_bun_test: bool = True) -> Preflight:
    """起跑前的唯一闸门。**拒绝时自己把话说全**，宿主只需 `if g.refused: return 3`。

    命中判据（任一）：
      · 命令行里有 `*reverse_verify*.py`（别人在跑门禁）
      · 命令行里有 `bun test`（别人跑测试会读到我们临时改的回退版源码 → 假给红/绿）
      · 锁文件登记着一个**活着的**、不属我祖先的 pid
    """
    peers = detect_peers(extra_patterns=extra_patterns, include_bun_test=include_bun_test)
    lock = Lock(tree / LOCK_NAME)
    state, info = lock.state()
    lock_held_by = ""
    # 显式 pid 白名单（REVERSE_VERIFY_PEER_IGNORE_PIDS）对**锁**也生效：
    # 同一个“已知无关进程”，命令行与锁两条判据必须口径一致，否则自检/演练想验“无并发⇒放行”
    # 会被“命令行屏蔽了、锁还挂着”搞成永远拒（本单实测踩到）。
    ignored_pids = {int(x) for x in (os.environ.get(IGNORE_PIDS_ENV, "").replace(" ", "") or "").split(",")
                    if x.strip().isdigit()}
    if state == "stale":
        lock.purge(why="陈旧锁（持有者已不存在）")
    elif state == "active":
        holder = int(info.get("pid", "0") or 0)
        mine = set(ancestors()) | descendants(os.getpid())
        if holder in mine:
            lock_held_by = "ancestor"      # 演练脚本持锁、它起的门禁是子进程：不算并发
        elif holder in ignored_pids:
            lock_held_by = "ignored"       # 显式白名单（自检用）
        else:
            lock_held_by = "other"
            if not any(p.pid == holder for p in peers):
                peers.append(Peer(holder, info.get("cmd", "")[:160]
                                  or info.get("label", "?"), "锁文件持有者"))

    g = Preflight(tree=tree, name=name, forced=force, peers=peers, lock=lock,
                  lock_held_by=lock_held_by)
    concurrent = bool(peers) or lock_held_by == "other"

    if concurrent:
        if force:
            # 警告必须在**输出最开头**：读日志的人先看到它，再看到后面的红绿
            print("=" * 92)
            print("⚠ 已按 --force 跳过并发检测，结果可能在竞态下不可信")
            print("  （别的验证在跑：两个门禁会互相回退同一份源码，源码字节、红绿结论、"
                  "甚至「恢复成功」都可能不可信；即使源码不重叠，重负载也会让时间敏感用例假红）")
            for p in peers:
                print(f"    · {p.line(tree)}")
            print("=" * 92)
        else:
            same = [p for p in peers if p.same_tree(tree)]
            print("=" * 92)
            print(f"✗ {name} 拒绝起跑：同树有并发验证在跑"
                  + (f"（其中 {len(same)} 个工作目录就在本树）" if same else
                     "（本树内没有，但同机有别的验证在跑——重负载/互相回退都会使结论不可信）"))
            for p in peers:
                print(f"    · {p.line(tree)}")
            print("  为什么拒绝：门禁会临时把 shengsheng.ts 改成回退版；两个门禁同时跑会互相回退/回填")
            print("  同一份源码，结果是「莫名变红」或「源码停在回退态」——本仓已为此付过两次代价。")
            print("  如确认要跑请加 --force（结果可能在竞态下不可信）。")
            print("=" * 92)
            sys.stdout.flush()
            g.refused = True
            return g
    elif force:
        pass    # 树上没人：--force 没什么可跳过的，不喊（避免噪声变成「狼来了」）

    if lock_held_by == "ancestor":
        print(f"（锁文件 {LOCK_NAME} 由祖先进程持有——演练脚本持锁起门禁属正常，本轮不自持锁）")
    elif lock_held_by == "ignored":
        print(f"（锁文件 {LOCK_NAME} 持有者在显式 pid 白名单里——按自检口径不自持锁）")
    elif lock_held_by == "other":
        print(f"⚠ 锁文件 {LOCK_NAME} 由别的进程持有（--force 下继续），本轮不自持锁")
    else:
        lock.take(label=name)
        g.lock = lock
    sys.stdout.flush()
    return g


# ── 信号收尾通道 ────────────────────────────────────────────────────────────
class Traps:
    """SIGINT/SIGTERM 的**收尾通道**（宿主的恢复逻辑仍在自己的 `finally` 里，这里只保管控制流）。

    与「默认行为」的差别（10:10 事故的根治点）：
      · 第一次信号：只回调宿主（置标志 + 一行提示），**不在处理器里做恢复**——信号可能落在
        `os.replace` 前后，在处理器里再发起写会重入文件写入，把源码写成半个文件。
      · 之后的信号：**全部忽略**。默认处置下第二次 `kill`（SIGTERM）会当场终止进程，
        `finally` 里的回填/回读校验永远跑不完 ⇒ 源码停在回退态 + 留 `.revbak`。这条路现在被堵死。
      · 收尾完成后 `terminate_self()`：恢复默认处置并自送同一信号，让退出码仍是 130/143
        （被信号终止，shell 视角 128+sig）——而不是被改写成一个普通退出码。
    """

    def __init__(self):
        self.signum = 0
        self.count = 0
        self.deferred = False
        self.installed = False
        self._cb = None

    def install(self, on_signal=None, sigs=(signal.SIGINT, signal.SIGTERM)) -> None:
        self._cb = on_signal
        for sig in sigs:
            try:
                signal.signal(sig, self._handle)
            except (ValueError, OSError):     # 非主线程等异常环境：退化为默认行为
                pass
        self.installed = True

    def _handle(self, signum, _frame) -> None:
        self.count += 1
        first = self.count == 1
        if first:
            self.signum = signum
            self.deferred = True              # 先立牌：从这一刻起后续信号不再打断收尾
            try:
                if self._cb is not None:
                    self._cb(signum, self.count)
                else:
                    print(f"\n⚠ 收到 {_signame(signum)}：正在收尾…", flush=True)
            except Exception:                 # 处理器里绝不冒异常
                pass
        else:
            try:
                print(f"⚠ 再次收到 {_signame(signum)}（第 {self.count} 次）：已在收尾，已忽略——"
                      f"它不再能打断回填；硬杀只能用 SIGKILL（那会留下 *.revbak，下一轮起跑自愈）",
                      flush=True)
            except Exception:
                pass

    def exit_code_for(self, signum: int | None = None) -> int:
        """该信号对应的退出码（SIGINT→130，SIGTERM→143；与 shell 的 128+sig 一致）。"""
        return INT_EXIT.get(signum or self.signum or signal.SIGTERM, 1)

    def note_ignored(self, signum: int) -> None:
        """宿主在收尾里做长动作（回填重试）时，可以借它把「已忽略」喊出来。"""
        if self.count > 1:
            print(f"（本次共收到 {self.count} 次 {_signame(signum)}，除第一次外均已忽略——收尾不再被打断）",
                  flush=True)


# ── 自检入口（人工排查 / 演练取证用；不参与生产流程）──────────────────────────
def _main() -> int:
    import argparse
    ap = argparse.ArgumentParser(description="器：打印本模块此刻看到什么（只读）")
    ap.add_argument("--probe", action="store_true", help="打印并发检测结果与锁状态")
    ap.add_argument("--tree", default=str(DEFAULT_TREE))
    args = ap.parse_args()
    if not args.probe:
        ap.print_help()
        return 0
    tree = pathlib.Path(args.tree)
    print(f"树：{tree}")
    peers = detect_peers()
    print(f"并发验证进程：{len(peers)} 个")
    for p in peers:
        print(f"  · {p.line()}")
    lock = Lock(tree / LOCK_NAME)
    state, info = lock.state()
    print(f"锁 {LOCK_NAME}：{state} {info or ''}")
    print(f"自己的祖先链：[{', '.join(str(p) for p in ancestors())}]")
    return 0


if __name__ == "__main__":
    sys.exit(_main())
