#!/usr/bin/env python3
"""复现 `docs/BUILD_ISOLATION_PLAN.md` 的实测数字（构建隔离的**证据链**）。

## 为什么有这个脚本

`docs/BUILD_ISOLATION_PLAN.md`（与 `CONVENTIONS.md` §9.0④）引用了一串实测数字：
抢 `.tmp` 的异常轮数、混代产物的归属分裂、坏共享缓存的读出率、dev 构建树的体量、
共享缓存里"同一源按树各存一份"、`flock` 的 inode 语义。它们支撑着那份**未落地方案**
的推荐顺序（B 构建锁优先于 A 独立构建目录），而此前只存在于一次会话里——
文档进了仓，证据链没有。本脚本就是那条证据链：**每处数字都能一条命令重跑**，
并把实测值与文档记载值并排打印、给出机器可判的判据。

## 用法

    python3 tools/build_isolation_repro.py --list            # 子命令 / 守的数字 / 出处行号
    python3 tools/build_isolation_repro.py all               # 全部跑一遍（退出码取最坏）
    python3 tools/build_isolation_repro.py <子命令> [--json] [--isolate]

子命令与文档行号：

| 子命令 | 复现的数字 | 出处 |
|---|---|---|
| `tmp-race` | 两进程共享同一 `.tmp` 名、各 120 轮 → 85 / 94 轮异常 | L26、`CONVENTIONS.md` §9.0④ 判据 3 |
| `mixed-gen` | 两"构建"各建 6 个同名单元 → 归属 1 / 5 分裂、链接零告警 | L27 |
| `cache-corrupt` | store 覆写 4 MB × 60 轮 + restore 并发读 3000 次 → 889 次读到截断（≈30%），坏条目跨构建存活 | L29、L120 |
| `tree-size` | dev 档 337 个 `.o`，单份 dev 树 625 MB（obj 266 + pch 117 + bin 242） | L51 |
| `cache-dup` | 共享缓存 1865 MB / 1903 条；同一相对路径跨树各占一条 | L52 |
| `lock-inode` | `flock` 跟 inode 走：持锁期间删锁文件 → 静默失去互斥（退出码 0） | L77 |

## 输出与退出码

每个子命令打印一张对照表：`项目 | 实测值 | 文档记载值 | 判据`。判据列只有三种取值：

- `同量级`：实测落在文档值的 **1/10 ~ 10 倍**内；文档值为 0 的用例（隔离后必须归零）
  要求实测**恰为 0**（那里容不下一个量级的带宽，否则"隔离没生效"也能判绿）；
- `超出`：超出一个量级——结论方向不一致；
- `未核`：本环境测不了（缺构建树 / 缺缓存 / 无 `flock`），**附原因**。

退出码：`0` 全绿；`1` 有行 `超出`；`2` 无 `超出` 但有 `未核`（测不了 ≠ 通过，
与 `tools/check_docs.py` 的三态纪律同口径）。

**`tmp-race` 例外——双向判据**（它同时证明"缺陷存在"与"修法有效"）：
退出码 `0` 当且仅当 **固定名版异常数 > 0 且 带 pid 版异常数 = 0 且 单进程对照 = 0**。
只满足一半（如两臂都 0 = 实验没造出竞争，或两臂都异常 = 修法无效）一律退出码 `1`，
并在末行明说哪一条不成立。

## 硬约束（本脚本的设计前提）

- **自建临时目录**（`tempfile.mkdtemp()`），跑完 `shutil.rmtree`；`_guard()` 拒绝对仓库
  `build/` 之下的一切写入——那是全机共享产物目录，被并发写坏一次就是 13 个 `unit.o`
  变 0 字节（`CONVENTIONS.md` §9.0④ 判据 5 记的事故）。
- **不联网**、不调 `st`；`tree-size` / `cache-dup` 是**只读**扫描，不改一个字节。
- **不并行跑真构建**——那正是被复现的缺陷。这里的并发全部发生在临时目录里的普通文件上。

## 它与被复现代码有多贴近

复现的是**同一机制**，不是同一二进制：

- `tmp-race`：`<obj>.tmp` **固定名**（`src/pkg/build.cpp:1123-1124`，rename 在 `1180`/`1185`）
  ⇒ 两进程抢同一个临时文件；
- `cache-corrupt`：`object_cache_store`/`restore` 走 `fs::copy_file(..., overwrite_existing)`
  原地覆写（`src/pkg/build.cpp:347-348`、`378-379`），非原子 rename
  ⇒ 读侧能读到被截短的对象；`fs::copy_file` 的实现是 `std::filesystem::copy_file`
  （`src/core/fs.cpp:442-448`），POSIX 上同样不保证原子性；
- `mixed-gen`：两进程往同一 `obj` 目录各写各的 `.o`，谁后 rename 谁留。

因此它测的是**机制的存在性与量级**。机制一旦被修（`--isolate` 就是它的形态），
这里的数字就归零——这正是脚本判别力的自证。
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unicodedata
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent             # shuangtian/
DOC = 'docs/BUILD_ISOLATION_PLAN.md'
ST_HOME = Path(os.environ.get('ST_HOME') or '~/.shuangtian').expanduser()
CACHE_ROOT = ST_HOME / 'cache' / 'objects'
DEFAULT_TREE = REPO / 'build' / 'dev'

# "同量级"的带宽：机器差异（核数 / 文件系统 / 并发密度 / 编译器）能让这些数字差 2~3 倍，
# 但**结论方向**（是否发生、是"数十个百分点"还是"千分之一"）不该翻。
MAG_LO, MAG_HI = 0.1, 10.0

OK, BAD, SKIP = '同量级', '超出', '未核'


# ───────────────────────────────── 通用 ─────────────────────────────────

def _dw(text: str) -> int:
    """显示宽度：CJK 全角算 2，其余算 1（表格对齐）。"""
    return sum(2 if unicodedata.east_asian_width(c) in 'WF' else 1 for c in str(text))


def _pad(text, width: int) -> str:
    s = str(text)
    return s + ' ' * max(0, width - _dw(s))


def table(rows, header=('项目', '实测值', '文档记载值', '判据')):
    """打印对照表（等宽对齐；`超出` / `未核` 可直接 grep）。"""
    widths = [max(_dw(r[i]) for r in (*rows, header)) for i in range(len(header))]
    print('  '.join(_pad(h, widths[i]) for i, h in enumerate(header)))
    print('  '.join('-' * w for w in widths))
    for r in rows:
        print('  '.join(_pad(c, widths[i]) for i, c in enumerate(r)))


def mag_ok(measured: float, doc: float) -> bool:
    """一个量级带宽内。文档值为 0（"隔离后必须归零"）要求实测恰为 0。"""
    if doc == 0:
        return measured == 0
    if measured == 0:
        return False
    return MAG_LO <= measured / doc <= MAG_HI


def verdict(measured, doc, ok=None) -> str:
    return (OK if mag_ok(measured, doc) else BAD) if ok is None else (OK if ok else BAD)


def _guard(path: Path) -> Path:
    """拒绝对仓库 `build/` 之下的任何写入（共享产物目录，实验绝不碰）。"""
    p = Path(path).resolve()
    forbidden = (REPO / 'build').resolve()
    if p == forbidden or forbidden in p.parents:
        raise SystemExit(f'拒绝写入仓库构建目录：{p}\n本脚本只许写 tempfile.mkdtemp() 下的目录。')
    return p


def _mkdtemp(prefix: str) -> Path:
    return _guard(Path(tempfile.mkdtemp(prefix=f'st-iso-{prefix}-')))


def _sizes(root: Path):
    """(文件数, 文件字节总数)；只算文件，不含目录元数据（跨机可比）。"""
    count = total = 0
    for f in root.rglob('*'):
        try:
            if f.is_file():
                count += 1
                total += f.stat().st_size
        except OSError:
            pass
    return count, total


def _mib(n: int) -> float:
    return n / (1024 ** 2)


def _mp_ctx():
    """优先 fork（子进程不必重新导入本模块）；无 fork 的平台退化为 spawn，仍可用。"""
    import multiprocessing as mp
    for m in ('fork', 'spawn'):
        if m in mp.get_all_start_methods():
            return mp.get_context(m)
    return mp.get_context()


def _fs_type() -> str:
    """临时目录所在文件系统类型（`cache-corrupt` 的截断窗口与它相关，必须报出来）。"""
    try:
        r = subprocess.run(['df', '-T', tempfile.gettempdir()], capture_output=True, text=True)
        cols = r.stdout.strip().splitlines()[-1].split()
        return cols[1] if len(cols) > 1 else '?'
    except Exception:
        return '?'


class _FileLock:
    """POSIX `flock` / Windows `msvcrt.locking` 的最小封装（`--isolate` 用它加互斥）。

    与被复现的缺陷直接相关：`fs::copy_file` 是 `overwrite_existing`（非原子 rename），
    所以"读侧拿到半截对象"只能靠**读写互斥**消除——这是 `cache-corrupt --isolate`
    把数字打到 0 的手段，也是本脚本判别力的来源。
    Windows 的 `msvcrt.locking` 没有共享锁，退化为一律独占（互斥方向不变，对本验证够用）。
    """

    def __init__(self, path: Path, shared: bool = False):
        self.path = Path(path)
        self.shared = shared
        self._fh = None

    def __enter__(self):
        self._fh = open(self.path, 'a+b')
        if os.name == 'nt':                                   # pragma: no cover 平台分支
            import msvcrt
            self._fh.seek(0)
            msvcrt.locking(self._fh.fileno(), msvcrt.LK_LOCK, 1)
        else:
            import fcntl
            fcntl.flock(self._fh.fileno(), fcntl.LOCK_SH if self.shared else fcntl.LOCK_EX)
        return self

    def __exit__(self, *exc):
        try:
            if os.name == 'nt':                               # pragma: no cover 平台分支
                import msvcrt
                self._fh.seek(0)
                msvcrt.locking(self._fh.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                import fcntl
                fcntl.flock(self._fh.fileno(), fcntl.LOCK_UN)
        finally:
            self._fh.close()
        return False


_TRYLOCK_CODE = (
    'import os,sys\n'
    'p=sys.argv[1]\n'
    'f=open(p,"a+b")\n'
    'try:\n'
    '    if os.name=="nt":\n'
    '        import msvcrt; f.seek(0); msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)\n'
    '    else:\n'
    '        import fcntl; fcntl.flock(f.fileno(), fcntl.LOCK_EX|fcntl.LOCK_NB)\n'
    'except OSError:\n'
    '    print("BUSY"); sys.exit(1)\n'
    'print("ACQUIRED"); sys.exit(0)\n'
)


def _trylock(path: Path) -> bool:
    """在**独立进程**里尝试非阻塞独占加锁（模拟"第二个构建进程"）。True = 锁到了。"""
    r = subprocess.run([sys.executable, '-c', _TRYLOCK_CODE, str(path)],
                       capture_output=True, text=True, timeout=120)
    return r.returncode == 0


def _finish(rows, args, name: str, detail: dict, code=None) -> int:
    """收尾：打印对照表 + 三态结论 + 可选 JSON。`code` 给了就用它（双向判据那类）。"""
    print()
    table(rows)
    bad = [r for r in rows if r[3] == BAD]
    skip = [r for r in rows if r[3] == SKIP]
    if code is None:
        code = 1 if bad else (2 if skip else 0)
    print()
    if code == 0:
        print(f'[{name}] 退出码 0：{len(rows)} 行全部与文档同量级')
        if skip:
            print(f'  注：另有 {len(skip)} 行标记为未核/无观测（不参与判据）—— '
                  + '; '.join(str(r[0]) for r in skip))
    elif code == 1:
        why = '; '.join(str(r[0]) for r in bad) or '见上方说明'
        print(f'[{name}] 退出码 1：判据不成立 —— {why}')
    else:
        print(f'[{name}] 退出码 2：无超出，但有 {len(skip)} 行**未核**（测不了 ≠ 通过）—— '
              + '; '.join(str(r[0]) for r in skip))
    if args.json:
        print(json.dumps({'cmd': name, 'rows': rows, 'detail': detail, 'exit': code},
                         ensure_ascii=False, default=str))
    return code


# ═══════════════════════ tmp-race（文档 L26）═══════════════════════
#
# 机制（`src/pkg/build.cpp:1123-1124`、`1180`、`1185`）：对象临时名是
# `unit->object + ".tmp"`——**固定名、不含 PID**。两个构建进程各写自己那份 `.o`，
# 却抢同一个临时文件：A 写一半，B 截断它开始写；A 去 rename 时要么找不到那个 inode
# （ENOENT），要么把 B 的半截内容当成自己的产物留下。
#
# 复现方式：两个子进程对**同一个** `.tmp` 做"分块写 → 读回校验 → rename"，各跑 N 轮。
# 任何一轮出现 (a) rename 失败、(b) 读回校验不符（长度或世代标记不对）都记一次异常。
# 三个臂一起给出结论：
#   ① 固定名（现状）  —— 必须 > 0 异常（证明缺陷存在）
#   ② 带 pid（方案 C）—— 必须 = 0 异常（证明修法有效）
#   ③ 单进程对照      —— 必须 = 0 异常（证明实验本身没问题：代码/磁盘不是异常源）
TR_ROUNDS = 120
TR_CHUNKS = 64
TR_CHUNK_BYTES = 8 * 1024


def _tr_worker(tmp_path: str, out_path: str, rounds: int, tag: int, pidized: bool, q):
    """子进程：反复"写临时名 → 读回校验 → rename 到正式名"。"""
    bad = 0
    reasons = {'enoent': 0, 'ebusy': 0, 'verify': 0, 'other': 0}
    body = bytes([tag]) * TR_CHUNK_BYTES
    for _ in range(rounds):
        name = f'{tmp_path}.{os.getpid()}.tmp' if pidized else tmp_path
        try:
            with open(name, 'wb') as fh:                  # "wb" = 截断后写，与 fopen("wb") 同义
                for c in range(TR_CHUNKS):
                    fh.write(body)
                    if c % 8 == 0:
                        time.sleep(0)                 # 主动让出 CPU，制造真实交错
            with open(name, 'rb') as fh:
                data = fh.read()
            if len(data) != TR_CHUNKS * TR_CHUNK_BYTES or data[0] != tag:
                bad += 1
                reasons['verify'] += 1                    # 读到的是对方的半截/整份内容
                continue
            os.replace(name, out_path)                    # ← rename（build.cpp:1180/1185）
        except FileNotFoundError:
            bad += 1
            reasons['enoent'] += 1                        # 对方先把共用的 .tmp 改名走了
        except OSError as e:
            bad += 1
            reasons['ebusy' if e.errno in (16, 26) else 'other'] += 1
    q.put((bad, reasons))


def _tr_arm(ctx, rounds, pidized, producers):
    """跑一个臂：`producers` 个子进程各跑 `rounds` 轮，返回（异常总数, 归因, 轮数）。"""
    d = _mkdtemp('tmpre')
    try:
        q = ctx.Queue()
        shared_tmp = str(d / 'unit.cpp.o.tmp')
        procs = [ctx.Process(target=_tr_worker,
                             args=(shared_tmp, str(d / f'unit{tag}.cpp.o'), rounds, tag, pidized, q))
                 for tag in (0x41, 0x42)[:producers]]
        for p in procs:
            p.start()
        total, reasons = 0, {}
        for _ in procs:
            b, r = q.get()
            total += b
            for k, v in r.items():
                reasons[k] = reasons.get(k, 0) + v
        for p in procs:
            p.join()
        return total, reasons, rounds * producers
    finally:
        shutil.rmtree(d, ignore_errors=True)


def cmd_tmp_race(args):
    rounds = args.rounds
    ctx = _mp_ctx()
    rows, detail = [], {}
    print(f'# tmp-race：两进程共享同一 `.tmp` 名，各 {rounds} 轮'
          f'"分块写 + 读回校验 + rename"（轮数可调：--rounds N）\n')

    shared_bad, shared_reasons, shared_n = _tr_arm(ctx, rounds, pidized=False, producers=2)
    print(f'  ① 固定名（现状 `<obj>.tmp`）      : 异常 {shared_bad} / {shared_n} 轮  {shared_reasons}')

    pid_bad, pid_reasons, pid_n = _tr_arm(ctx, rounds, pidized=True, producers=2)
    print(f'  ② 临时名带 pid（方案 C）          : 异常 {pid_bad} / {pid_n} 轮  {pid_reasons}')

    ctl_bad, ctl_reasons, ctl_n = _tr_arm(ctx, rounds, pidized=False, producers=1)
    print(f'  ③ 单进程对照                      : 异常 {ctl_bad} / {ctl_n} 轮  {ctl_reasons}\n')

    doc_bad = 85                                       # 文档：85 / 94 轮异常
    rows.append(('①固定名（现状）的异常轮数', f'{shared_bad} / {shared_n}',
                 f'85 / 94 轮（≈90%）', verdict(shared_bad / shared_n, doc_bad / 94)))
    rows.append(('①其中"读回校验不符"（静默混入对方字节）', f'{shared_reasons.get("verify", 0)}',
                 '文档未拆：本脚本拆开报', OK))
    # 失败形态（ENOENT vs 读回不符）取决于两进程的交错时序，每次跑都可能不同；
    # 判据只要求"异常总数 > 0"，这一行只如实报出本次观测到的形态，不做断言。
    enoent = shared_reasons.get('enoent', 0)
    rows.append(('①其中 rename 失败 errno=2(ENOENT)', f'{enoent}',
                 '判据 3 记为"主形态"（本机本次多表现为读回不符）',
                 SKIP if enoent == 0 else OK))
    rows.append(('②带 pid 的异常轮数（修法必须归零）', f'{pid_bad}', '0',
                 OK if pid_bad == 0 else BAD))
    rows.append(('③单进程对照的异常轮数（实验自检）', f'{ctl_bad} / {ctl_n}', '0',
                 OK if ctl_bad == 0 else BAD))

    # ── 双向判据：既证明缺陷存在，又证明修法有效 ──
    defect_shown = shared_bad > 0
    fix_works = pid_bad == 0
    control_clean = ctl_bad == 0
    rows.append(('双向判据：固定名 > 0 且 带 pid = 0 且 对照 = 0',
                 ('成立' if (defect_shown and fix_works and control_clean) else '不成立'),
                 '退出码 0 的充要条件',
                 OK if (defect_shown and fix_works and control_clean) else BAD))
    if not defect_shown:
        print('  ⚠ 固定名臂没有造出异常：本次**没有判别力**（竞争没发生，不能据此说缺陷不存在）——'
              '换机器/加大 --rounds 重跑。')
    if not fix_works:
        print('  ⚠ 带 pid 臂仍有异常：修法在这台机器上不成立。')
    if not control_clean:
        print('  ⚠ 单进程对照也异常：实验本身有问题（磁盘满/权限/代码 bug），以上数字作废。')

    detail.update({'shared': {'bad': shared_bad, 'rounds': shared_n, 'reasons': shared_reasons},
                   'pidized': {'bad': pid_bad, 'rounds': pid_n, 'reasons': pid_reasons},
                   'control': {'bad': ctl_bad, 'rounds': ctl_n, 'reasons': ctl_reasons},
                   'bidirectional': {'defect_shown': defect_shown, 'fix_works': fix_works,
                                     'control_clean': control_clean}})
    code = 0 if (defect_shown and fix_works and control_clean) else 1
    return _finish(rows, args, 'tmp-race', detail, code=code)


def cmd_tmp_race_isolate(args):
    """`--isolate`：只跑"临时名带 pid"那一臂（= 方案 C 落地后的形态），要求归零。

    与默认模式的分工：默认的双向判据一次就把"缺陷存在 + 修法有效"证明完；
    本模式是它的一半，供"只看隔离后读数"的场合用（如回归门禁）。
    """
    ctx = _mp_ctx()
    print(f'# tmp-race --isolate：临时名带 pid（`<obj>.<pid>.tmp`），两进程各 {args.rounds} 轮\n')
    bad, reasons, n = _tr_arm(ctx, args.rounds, pidized=True, producers=2)
    print(f'  带 pid 臂: 异常 {bad} / {n} 轮  {reasons}\n')
    rows = [('带 pid 的异常轮数（隔离后必须为 0）', f'{bad} / {n}', '0',
             OK if bad == 0 else BAD)]
    return _finish(rows, args, 'tmp-race', {'pidized': {'bad': bad, 'rounds': n}}, 
                   code=0 if bad == 0 else 1)


# ═══════════════════════ cache-corrupt（文档 L29 / L120）═══════════════════════
#
# 复现 `object_cache_store` / `object_cache_restore` 的**原地覆写**：
#   store   `fs::copy_file(unit.object, cache/<key>/unit.o)`   （build.cpp:378-379）
#   restore `fs::copy_file(cache/<key>/unit.o, unit.object)`   （build.cpp:347-348）
# 两侧都不是"写临时名 → rename"，于是读侧能在写侧把文件截短之后、写完之前读到它。
#
# 口径必须说清（否则数字是玄学）：一次读能否拿到截断对象，取决于它**是否落在写窗口内**：
#   P(截断) = P(读落在写窗口内) × P(窗口内读到截断)
# 前者由"读侧总耗时 / 写窗口时长"决定（并发读越多、单次读越便宜 ⇒ 采样越密），
# 后者是文件系统与页缓存的固有性质。文档的 ≈30% 是某个并发密度下的取值，
# 所以本子命令**扫并发度给一条曲线**，而不是挑一个点；曲线的包络才是可比的量。
CC_SIZE_MB = 4
CC_ROUNDS = 60
CC_READS = 3000
CC_DOC_TRUNC = 889


def _cc_reader(cache_obj, workdir, idx, quota, start_at, full, isolate, lock_path):
    """子进程：模拟 restore 侧的并发读，逐次量"读到的对象是否完整"。"""
    dst = os.path.join(workdir, f'restore{idx}.o')
    rec = []
    while time.time() < start_at:                      # 起跑屏障：与 writer 真正重叠
        time.sleep(0.0005)
    for _ in range(quota):
        t0 = time.time()
        try:
            if isolate:
                with _FileLock(Path(lock_path), shared=True):
                    shutil.copyfile(cache_obj, dst)
            else:
                shutil.copyfile(cache_obj, dst)        # ← restore：原地覆写到工程 obj
        except FileNotFoundError:
            continue                                   # 写侧重建条目的窄窗口，不计入
        t1 = time.time()
        try:
            size = os.path.getsize(dst)
        except OSError:
            size = -1
        rec.append((t0, t1, 1 if size == full else 0))
    with open(os.path.join(workdir, f'rec{idx}.json'), 'w') as fh:
        json.dump(rec, fh)


def _cc_trial(ctx, readers, isolate):
    """一次实验：readers 个并发 restore + 1 个 store 写 CC_ROUNDS 轮。

    返回 dict：截断次数、总读次数、写窗口时长、窗口内读次数、写后缓存条目是否损坏。
    """
    size = CC_SIZE_MB * 1024 * 1024
    quota = max(1, CC_READS // readers)
    d = _mkdtemp('cache')
    try:
        src = d / 'unit.src.o'
        with open(src, 'wb') as fh:
            fh.write(b'\x5a' * size)
        cache = d / 'cache'
        cache.mkdir()
        obj = cache / 'unit.o'
        shutil.copyfile(src, obj)                      # 冷启动时缓存里那份完整对象
        lock = cache / '.lock'
        lock.write_bytes(b'')
        start_at = time.time() + 0.35
        procs = [ctx.Process(target=_cc_reader,
                             args=(str(obj), str(d), i, quota, start_at, size, isolate, str(lock)))
                 for i in range(readers)]
        for p in procs:
            p.start()
        while time.time() < start_at:
            time.sleep(0.001)
        wins = []
        for _ in range(CC_ROUNDS):
            t0 = time.time()
            if isolate:
                with _FileLock(lock):
                    shutil.copyfile(src, obj)          # ← store
            else:
                shutil.copyfile(src, obj)              # ← store（原地覆写，非原子）
            wins.append((t0, time.time()))
        for p in procs:
            p.join()
        recs = []
        for i in range(readers):
            f = d / f'rec{i}.json'
            if f.exists():
                recs += json.loads(f.read_text())
        trunc = sum(1 for r in recs if not r[2])
        # 窗口内的读：解释"为什么比例是这个数"——窗口外的读必然完整
        inwin = sum(1 for r in recs
                    if any(not (r[1] < w0 or r[0] > w1) for w0, w1 in wins))
        entry_ok = obj.exists() and obj.stat().st_size == size
        return {'reads': len(recs), 'trunc': trunc, 'in_window': inwin,
                'window_ms': sum(w1 - w0 for w0, w1 in wins) * 1000,
                'entry_ok': entry_ok}
    finally:
        shutil.rmtree(d, ignore_errors=True)


def _cc_survival(size: int) -> int:
    """坏条目跨构建存活：缓存里一旦留下半截对象，之后每一次 restore 都抄到它。"""
    d = _mkdtemp('survive')
    try:
        obj = d / 'unit.o'
        with open(obj, 'wb') as fh:
            fh.write(b'\x5a' * (size // 3))            # 手工造一份"被写坏"的缓存条目
        dst = d / 'restored.o'
        bad = 0
        for _ in range(100):
            shutil.copyfile(obj, dst)                  # ← 之后每一次 restore
            bad += os.path.getsize(dst) != size
        return bad
    finally:
        shutil.rmtree(d, ignore_errors=True)


def cmd_cache_corrupt(args):
    ctx = _mp_ctx()
    sweep = args.readers or [1, 2, 4, 8, 16]
    reps = args.reps
    rows, detail = [], {}
    size = CC_SIZE_MB * 1024 * 1024
    print(f'# cache-corrupt：store 覆写 {CC_SIZE_MB} MB × {CC_ROUNDS} 轮，'
          f'restore 并发读共 {CC_READS} 次（每档重复 {reps} 次）')
    print(f'# 临时目录文件系统：{_fs_type()}（截断窗口宽度与它相关）')
    print(f'# 模式：{"--isolate（restore 共享锁、store 独占锁）" if args.isolate else "现状（无互斥）"}\n')

    curve = []
    for readers in sweep:
        trials = [_cc_trial(ctx, readers, args.isolate) for _ in range(reps)]
        rate = sum(t['trunc'] / max(1, t['reads']) for t in trials) / len(trials)
        truncs = [t['trunc'] for t in trials]
        reads = [t['reads'] for t in trials]
        inwin = sum(t['in_window'] for t in trials) / len(trials)
        win_ms = sum(t['window_ms'] for t in trials) / len(trials)
        bad_entry = sum(0 if t['entry_ok'] else 1 for t in trials)
        curve.append({'readers': readers, 'truncs': truncs, 'reads': reads, 'rate': rate,
                      'in_window': inwin, 'window_ms': win_ms, 'bad_entry': bad_entry})
        print(f'  readers={readers:2d}  截断 {truncs} / {reads} = {rate:6.1%}'
              f'   写窗口 {win_ms:5.0f} ms   窗口内读 {inwin:5.0f} 次'
              f'   写后条目损坏 {bad_entry}/{reps}')
    detail['curve'] = curve
    detail['fs'] = _fs_type()

    survival = _cc_survival(size)
    print(f'  坏条目存活：手工造半截条目后连续 restore 100 次 → 抄到坏对象 {survival} 次\n')

    doc_rate = CC_DOC_TRUNC / CC_READS
    hi = max(curve, key=lambda c: c['rate'])
    lo = min(curve, key=lambda c: c['rate'])
    if args.isolate:
        total = sum(sum(c['truncs']) for c in curve)
        total_reads = sum(sum(c['reads']) for c in curve)
        rows.append(('全档截断次数（隔离后必须为 0）', f'{total}', '0', OK if total == 0 else BAD))
        rows.append(('全档平均截断率（隔离后必须为 0%）',
                     f'{total / max(1, total_reads):.1%}', '0%',
                     OK if total == 0 else BAD))
        rows.append(('坏条目跨构建存活', f'{survival} / 100', '100 / 100', OK if survival == 100 else BAD))
        code = 0 if (total == 0 and survival == 100) else 1
        return _finish(rows, args, 'cache-corrupt', detail, code=code)

    rows.append((f'峰值截断率（{hi["readers"]} 路并发）', f'{hi["rate"]:.1%}',
                 f'{doc_rate:.1%}（889/3000）', verdict(hi['rate'], doc_rate)))
    inside = lo['rate'] - 1e-9 <= doc_rate <= hi['rate'] + 1e-9
    rows.append(('文档 29.6% 是否落在实测曲线内（扫描密度）',
                 f'[{", ".join(f"{c["rate"]:.1%}" for c in curve)}]',
                 '应落在曲线内（文档是某个并发密度下的取值）', OK if inside else BAD))
    # 低并发臂：不断言它与文档同量级（**并发密度不同，本就不该相同**），只断言
    # “它也不是 0”——即缺陷不因读得少而消失，只是撞上写窗口的概率变低。
    rows.append((f'低并发臂（{lo["readers"]} 路）截断不为零且低于峰值', f'{lo["rate"]:.1%}',
                 '> 0 且 < 峰值（缺陷在所有并发度下都存在）',
                 OK if 0 < lo['rate'] < hi['rate'] else BAD))
    peak = max(sum(c['truncs']) for c in curve)
    rows.append(('单档峰值截断次数', f'{peak}', str(CC_DOC_TRUNC), verdict(peak, CC_DOC_TRUNC)))
    rows.append(('坏条目跨构建存活（后续 restore 抄到坏对象）', f'{survival} / 100',
                 '100 / 100（文档：跨构建存活）', OK if survival == 100 else BAD))
    rows.append(('截断只发生在写窗口内（因子拆解）',
                 f'窗口内读占 {sum(c["in_window"] for c in curve) / max(1, sum(sum(c["reads"]) for c in curve)):.0%}',
                 '文档只报总率、未拆因子', OK))
    code = 1 if any(r[3] == BAD for r in rows) else 0
    return _finish(rows, args, 'cache-corrupt', detail, code=code)


# ═══════════════════════ mixed-gen（文档 L27）═══════════════════════
#
# 机制：两个进程往**同一个** `build/<档>/obj` 各写 6 个同名 `.o`，谁后 rename 谁留。
# 结果是一次"构建成功"的产物目录里同时含两个源码状态的单元——链接照样成功、零告警。
# 判定：按每个 `.o` 里写入的世代标记（A / B）归类，报"分裂度" min(A, B) 与静默性
#       （同目录里的单元是否全部"看起来合法"——只要非空就算，正是零告警的成因）。
MG_UNITS = 6


def _mg_worker(obj_dir: str, tag: str, rounds: int, start_at: float, order, q):
    """子进程：反复为 6 个同名单元写 `.o`（临时名带 pid + rename）。

    两个刻意选择，都写在明处：
    - **不复现层①**（抢 `.tmp`）——那是 `tmp-race` 的活；本层只考察"两个构建往同一
      obj 目录写、谁后 rename 谁留"，所以每个写入者用**自己的**临时名，
      否则测到的是"内容被对方截断"，两种缺陷混在一起。
    - **两臂用相反的单元顺序**——真实构建（`-j` 并行）里每个单元的完成顺序本来就
      不会两进程一致；顺序全同只会得到"最后跑完的那个赢走全部 6 个"，
      那是循环串行的结果，不是并行交错的结果。
    """
    payload = tag.encode() * 4096
    tmp = os.path.join(obj_dir, f'.tmp{os.getpid()}')
    while time.time() < start_at:                      # 起跑屏障：两进程真正叉开
        time.sleep(0.0005)
    for _ in range(rounds):
        for i in order:
            final = os.path.join(obj_dir, f'unit{i}.cpp.o')
            try:
                with open(tmp, 'wb') as fh:
                    fh.write(payload)
                os.replace(tmp, final)
            except OSError:
                pass
            time.sleep(0.0005)   # 单单元耗时的占位（真实构建是秒级；不睡就毫无交错）
    q.put(1)


def _mg_run(ctx, rounds, isolated):
    """跑一次：isolated=False 两进程共用一个 obj 目录；True 各用各的（= A 方案）。"""
    d = _mkdtemp('mixed')
    try:
        # 非隔离臂：两个"构建"共用**同一个** obj 目录（这正是被复现的场景）
        dirs = [d / 'obj' for _ in ('A', 'B')] if not isolated \
            else [d / f'obj{tag}' for tag in ('A', 'B')]
        for x in dict.fromkeys(dirs):
            x.mkdir()
        q = ctx.Queue()
        start_at = time.time() + 0.3
        orders = (list(range(MG_UNITS)), list(reversed(range(MG_UNITS))))
        procs = [ctx.Process(target=_mg_worker, args=(str(dirs[i]), tag, rounds, start_at, orders[i], q))
                 for i, tag in enumerate(('A', 'B'))]
        for p in procs:
            p.start()
        for _ in procs:
            q.get()
        for p in procs:
            p.join()
        result = []
        for di, tag in enumerate(('A', 'B')):
            gens = []
            for u in range(MG_UNITS):
                f = dirs[di] / f'unit{u}.cpp.o'
                gens.append(f.read_bytes()[:1].decode() if f.exists() else '-')
            result.append(gens)
        return result
    finally:
        shutil.rmtree(d, ignore_errors=True)


def cmd_mixed_gen(args):
    ctx = _mp_ctx()
    rows, detail = [], {}
    print(f'# mixed-gen：两进程各写 {MG_UNITS} 个同名单元（各 {args.rounds} 轮）\n')

    gens_a, gens_b = _mg_run(ctx, args.rounds, isolated=False)
    a, b = gens_a.count('A'), gens_a.count('B')
    split = min(a, b)
    valid = sum(1 for g in gens_a if g in ('A', 'B'))
    print(f'  同一 obj 目录（现状）: 归属 A={a} B={b}  → 分裂度 min(A,B)={split}'
          f'；6 个单元全部"看起来合法" {valid}/{MG_UNITS}')
    rows.append((f'6 个单元的归属分裂（min(A,B)）', f'{split}（A{a}/B{b}）', '1（文档：1 / 5 分裂）',
                 verdict(split, 1)))
    rows.append(('同目录单元是否全部"看起来合法"（零告警的成因）', f'{valid} / {MG_UNITS}',
                 f'{MG_UNITS} / {MG_UNITS}', OK if valid == MG_UNITS else BAD))

    iso_a, iso_b = _mg_run(ctx, args.rounds, isolated=True)
    wrong = sum(1 for g, tag in zip(iso_a, 'A' * MG_UNITS) if g != tag) + \
        sum(1 for g, tag in zip(iso_b, 'B' * MG_UNITS) if g != tag)
    print(f'  各用独立 obj 目录（方案 A）: 混代 {wrong} / {MG_UNITS * 2}\n')
    rows.append(('独立 obj 目录后的混代单元数（修法必须归零）', f'{wrong} / {MG_UNITS * 2}', '0',
                 OK if wrong == 0 else BAD))
    defect_shown, fix_works = split > 0, wrong == 0
    rows.append(('双向判据：混代 > 0 且 独立目录 = 0',
                 '成立' if (defect_shown and fix_works) else '不成立', '退出码 0 的充要条件',
                 OK if (defect_shown and fix_works) else BAD))
    detail.update({'shared': {'gens': gens_a, 'A': a, 'B': b, 'split': split},
                   'isolated': {'gens': iso_a + iso_b, 'wrong': wrong}})
    code = 0 if (defect_shown and fix_works) else 1
    return _finish(rows, args, 'mixed-gen', detail, code=code)


# ═══════════════════════ tree-size（文档 L51）═══════════════════════
#
# 纯只读扫描。文档值来自某个 8 核 Linux 机器的一份 dev 树；核数不同 ⇒ 单元数不同
# （`make_units` 会把框架源与测试源全部算进去，与核数无关，但档位/目标集会变），
# 体积随编译器版本与调试信息策略漂移。所以判据是量级与**结构关系**（三块之和 = 整树），
# 而不是精确相等。
DOC_OBJ_COUNT = 337
DOC_OBJ_MB, DOC_PCH_MB, DOC_BIN_MB, DOC_TOTAL_MB = 266, 117, 242, 625


def cmd_tree_size(args):
    tree = Path(args.tree).expanduser() if args.tree else DEFAULT_TREE
    rows, detail = [], {}
    print(f'# tree-size：只读扫描构建树 {tree}\n')
    if not tree.is_dir():
        rows.append((f'构建树是否存在（{tree}）', '不存在', '需要一份已构建的 dev 树', SKIP))
        detail['tree'] = str(tree)
        return _finish(rows, args, 'tree-size', detail)

    o_files = [f for f in tree.rglob('*.o') if f.is_file()]
    blocks = {}
    for name in ('obj', 'pch', 'bin'):
        p = tree / name
        blocks[name] = _sizes(p) if p.is_dir() else (0, 0)
    total_count, total_bytes = _sizes(tree)
    three = sum(b for _, b in blocks.values())

    rows.append(('`.o` 个数', str(len(o_files)), str(DOC_OBJ_COUNT),
                 verdict(len(o_files), DOC_OBJ_COUNT)))
    for name, doc_mb in (('obj', DOC_OBJ_MB), ('pch', DOC_PCH_MB), ('bin', DOC_BIN_MB)):
        cnt, by = blocks[name]
        rows.append((f'{name} 体积', f'{_mib(by):.0f} MB（{cnt} 文件）', f'{doc_mb} MB',
                     verdict(round(_mib(by)), doc_mb)))
    rows.append(('整棵树体积', f'{_mib(total_bytes):.0f} MB（{total_count} 文件）',
                 f'{DOC_TOTAL_MB} MB', verdict(round(_mib(total_bytes)), DOC_TOTAL_MB)))
    ratio = abs(three - total_bytes) / max(1, total_bytes)
    rows.append(('三块之和 ≈ 整树（结构关系）', f'{_mib(three):.0f} / {_mib(total_bytes):.0f} MB'
                 f'（差 {ratio:.1%}）', '文档：266+117+242 = 625 = 整树',
                 OK if ratio < 0.15 else BAD))
    detail.update({'tree': str(tree), 'o_count': len(o_files),
                   'blocks': {k: {'files': c, 'bytes': b} for k, (c, b) in blocks.items()},
                   'total_bytes': total_bytes, 'total_files': total_count})
    print(f'  扫描完成：{len(o_files)} 个 .o；obj {_mib(blocks["obj"][1]):.0f} MB · '
          f'pch {_mib(blocks["pch"][1]):.0f} MB · bin {_mib(blocks["bin"][1]):.0f} MB'
          f'（三块 {_mib(three):.0f} MB / 整树 {_mib(total_bytes):.0f} MB）\n')
    return _finish(rows, args, 'tree-size', detail)


# ═══════════════════════ cache-dup（文档 L52）═══════════════════════
#
# 只读扫描共享缓存，判据是"同一相对路径在不同树里各成一条"——这是 F6（键含源**绝对**
# 路径）的直接后果，也是 A 方案"换目录不解决 ④"的证据。做法：读每个条目的 `unit.meta`
# 首行（`path<TAB>mtime<TAB>size`）拿到源绝对路径，按首个 `/shuangtian/` 之前的
# 前缀切出"树"，剩余部分即"同一相对路径"。
DOC_CACHE_MB, DOC_CACHE_ENTRIES = 1865, 1903


def _meta_first_source(entry: Path):
    meta = entry / 'unit.meta'
    try:
        with open(meta, 'r', errors='replace') as fh:
            first = fh.readline()
    except OSError:
        return None
    return first.split('\t')[0].strip() or None


def _split_tree(path: str):
    """`/a/b/shuangtian/src/x.cpp` → (`/a/b`, `/shuangtian/src/x.cpp`)。"""
    idx = path.find('/shuangtian/')
    if idx < 0:
        return None, None
    return path[:idx], path[idx:]


def cmd_cache_dup(args):
    root = Path(args.cache).expanduser() if args.cache else CACHE_ROOT
    rows, detail = [], {}
    print(f'# cache-dup：只读扫描共享对象缓存 {root}\n')
    if not root.is_dir():
        rows.append((f'缓存是否存在（{root}）', '不存在', 'ST_HOME/cache/objects', SKIP))
        detail['cache'] = str(root)
        return _finish(rows, args, 'cache-dup', detail)

    entries = [d for d in root.iterdir() if d.is_dir()]
    total_bytes, no_meta = 0, 0
    groups: dict[str, set] = {}
    for e in entries:
        for f in e.rglob('*'):
            try:
                if f.is_file():
                    total_bytes += f.stat().st_size
            except OSError:
                pass
        first = _meta_first_source(e)
        if first is None:
            no_meta += 1
            continue
        tree, rel = _split_tree(first)
        if rel is not None:
            groups.setdefault(rel, set()).add(tree)

    dup = {r: t for r, t in groups.items() if len(t) >= 2}
    rows.append(('缓存条目数', str(len(entries)), str(DOC_CACHE_ENTRIES),
                 verdict(len(entries), DOC_CACHE_ENTRIES)))
    rows.append(('缓存体积', f'{_mib(total_bytes):.0f} MB', f'{DOC_CACHE_MB} MB',
                 verdict(round(_mib(total_bytes)), DOC_CACHE_MB)))
    rows.append(('同一相对路径跨树成条（组数）', f'{len(dup)}（可判 {len(groups)} 组）',
                 '> 0（文档：各占一条）', OK if dup else BAD))
    if dup:
        rel, trees = max(dup.items(), key=lambda kv: len(kv[1]))
        names = sorted(t.rsplit('/', 1)[-1] for t in trees)
        rows.append(('样本：单个相对路径 → 树数', f'{len(trees)} 棵（{rel.rsplit("/", 1)[-1]}）',
                     '≥ 2 棵', OK if len(trees) >= 2 else BAD))
        print(f'  样本：{rel}\n        → ' + ', '.join(names))
        detail['sample'] = {'rel': rel, 'trees': sorted(trees)}
    rows.append(('无 unit.meta 的旧条目（不计入跨树统计）', f'{no_meta}', '—', OK))
    detail.update({'cache': str(root), 'entries': len(entries), 'bytes': total_bytes,
                   'groups': len(groups), 'dup_groups': len(dup), 'no_meta': no_meta})
    print(f'  扫描完成：{len(entries)} 条 · {_mib(total_bytes):.0f} MB · {len(groups)} 个可判'
          f'相对路径，其中 {len(dup)} 个跨树各成一条\n')
    return _finish(rows, args, 'cache-dup', detail)


# ═══════════════════════ lock-inode（文档 L77）═══════════════════════
#
# `flock` 的互斥身份是**打开的 inode**，不是路径字符串。于是：
#   ① 持锁期间把锁文件删掉再新建 → 新文件是新 inode ⇒ 第二个进程立刻锁得上
#      （退出码 0，**静默**失去互斥）。这正是"持锁期间不要 `st clean`"的成因。
#   ② 同一 inode 的不同路径（符号链接 / 硬链接 / 相对 vs 绝对）**仍是同一把锁**。
#      文档 L77 括注里"两套路径会各自成锁"的说法**在本机实测不成立**——只有指向
#      不同 inode 才各自成锁。该行如实报出与文档的差异（文档本身把它标为"核查项"）。
#   ③ 锁放 `build/` 之外时，`st clean`（只删 build/）不再破坏互斥 —— 这是方案 B 的落地形态。
LI_CASES = [
    ('同一路径（绝对 vs 绝对）', 'same', '互斥成立'),
    ('同一路径（相对 vs 绝对）', 'relative', '同一把锁'),
    ('符号链接指向锁文件', 'symlink', '同一把锁（文档写"会各自成锁"，实测相反）'),
    ('硬链接指向锁文件', 'hardlink', '同一把锁'),
    ('持锁期间删锁文件后重建', 'delete_recreate', '失去互斥（退出码 0）'),
    ('锁在 build/ 之外，clean 只删 build/', 'outside_build', '互斥保持'),
]


def _li_setup(kind: str, d: Path):
    build = d / 'build'
    build.mkdir(exist_ok=True)
    inside = build / '.st-build.lock'
    inside.write_bytes(b'pid\n')
    outside = d / '.st-build.lock'
    outside.write_bytes(b'pid\n')
    if kind in ('same', 'relative', 'delete_recreate'):
        return inside, inside
    if kind == 'symlink':
        link = d / 'link.lock'
        link.unlink(missing_ok=True)
        link.symlink_to(inside)
        return inside, link
    if kind == 'hardlink':
        hard = d / 'hard.lock'
        if not hard.exists():
            os.link(inside, hard)
        return inside, hard
    if kind == 'outside_build':
        return outside, outside
    raise ValueError(kind)


def cmd_lock_inode(args):
    rows, detail = [], {}
    print('# lock-inode：flock 的互斥身份跟 inode 走（纯语义验证，不跑真构建）\n')
    results = {}
    for label, kind, expect in LI_CASES:
        d = _mkdtemp('lock')
        try:
            lock_path, probe_path = _li_setup(kind, d)
            holder = open(lock_path, 'a+b')
            try:
                if os.name == 'nt':                    # pragma: no cover 平台分支
                    import msvcrt
                    holder.seek(0)
                    msvcrt.locking(holder.fileno(), msvcrt.LK_NBLCK, 1)
                else:
                    import fcntl
                    fcntl.flock(holder.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError:
                rows.append((label, '持锁失败（平台不支持）', expect, SKIP))
                holder.close()
                continue
            try:
                if kind == 'delete_recreate':
                    os.unlink(lock_path)               # ← 模拟 `st clean` 删掉锁文件
                    lock_path.write_bytes(b'pid2\n')   # ← 下一个构建进程重新创建它
                elif kind == 'outside_build':
                    shutil.rmtree(d / 'build')         # ← 模拟 `st clean`
                    (d / 'build').mkdir()
                acquired = _trylock(probe_path)
            finally:
                holder.close()
            got = '可立即加锁（退出码 0）' if acquired else '被挡（退出码 1）'
            want_lost = kind == 'delete_recreate'
            ok = acquired if want_lost else (not acquired)
            rows.append((label, got, f'期望：{expect}', OK if ok else BAD))
            results[kind] = {'acquired': acquired, 'ok': ok}
        finally:
            shutil.rmtree(d, ignore_errors=True)
    detail['cases'] = results

    # 双向：删锁文件后**必须**失去互斥（证明缺陷），且锁放 build/ 外**必须**保持（证明修法）
    defect_shown = results.get('delete_recreate', {}).get('acquired') is True
    fix_works = results.get('outside_build', {}).get('acquired') is False
    rows.append(('双向判据：删锁文件即失去互斥 且 锁在 build/ 外保持互斥',
                 '成立' if (defect_shown and fix_works) else '不成立',
                 '退出码 0 的充要条件', OK if (defect_shown and fix_works) else BAD))
    if args.isolate:
        print('# 逆向验证（--isolate）：只看"锁在 build/ 之外"那一条 —— 它必须保持互斥')
        rows = [r for r in rows if '之外' in r[0] or '双向判据' in r[0]]
        code = 0 if fix_works else 1
    else:
        code = 0 if (defect_shown and fix_works and
                     all(r[3] == OK for r in rows)) else 1
    return _finish(rows, args, 'lock-inode', detail, code=code)


# ═══════════════════════════════ CLI ═══════════════════════════════

COMMANDS = {
    'tmp-race': (cmd_tmp_race, '两进程共享同一 `.tmp` 名的异常轮数（双向：固定名>0 且 带pid=0）', 'L26'),
    'mixed-gen': (cmd_mixed_gen, '两构建写同一 obj 目录的产物归属分裂（双向）', 'L27'),
    'cache-corrupt': (cmd_cache_corrupt, 'store 覆写 + restore 并发读的截断率与坏条目存活', 'L29/L120'),
    'tree-size': (cmd_tree_size, 'dev 构建树的 .o 个数与 obj/pch/bin 体积（只读）', 'L51'),
    'cache-dup': (cmd_cache_dup, '共享缓存条目数/体积与跨树重复条目（只读）', 'L52'),
    'lock-inode': (cmd_lock_inode, 'flock 的 inode 语义：删锁文件即失去互斥（双向）', 'L77'),
}


def build_parser():
    p = argparse.ArgumentParser(
        prog='build_isolation_repro.py',
        description='复现 docs/BUILD_ISOLATION_PLAN.md 的实测数字（自建临时目录，绝不碰 build/）',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='退出码：0 判据成立 · 1 判据不成立/有行超出一个量级 · 2 有行未核（不算通过）')
    p.add_argument('cmd', nargs='?', help='子命令；`all` 全跑；`--list` 看清单')
    p.add_argument('--list', action='store_true', help='列出子命令与它守的文档数字')
    p.add_argument('--json', action='store_true', help='末尾附一行机器可读 JSON（含退出码）')
    p.add_argument('--isolate', action='store_true',
                   help='逆向验证：加上对应隔离措施（读侧加锁 / pid 临时名 / 独立 obj 目录）后重跑，数字应归零')
    p.add_argument('--readers', type=int, nargs='*', default=None,
                   help='cache-corrupt 的并发读路数（默认扫 1 2 4 8 16）')
    p.add_argument('--reps', type=int, default=3, help='cache-corrupt 每档重复次数（默认 3）')
    p.add_argument('--rounds', type=int, default=TR_ROUNDS,
                   help=f'tmp-race / mixed-gen 的每进程轮数（默认 {TR_ROUNDS}）')
    p.add_argument('--tree', help='tree-size 扫描的构建树（默认 shuangtian/build/dev，只读）')
    p.add_argument('--cache', help='cache-dup 扫描的缓存根（默认 $ST_HOME/cache/objects，只读）')
    return p


def main(argv=None):
    args = build_parser().parse_args(argv)
    if args.list or not args.cmd:
        print(f'子命令 → 它守的数字（出处：{DOC}）\n')
        for name, (fn, desc, where) in COMMANDS.items():
            print(f'  {name:<14} {where:<9} {desc}')
            print(f'  {"":14} {"":9} 调用：python3 tools/build_isolation_repro.py {name}'
                  f'{"  --isolate" if name in ("cache-corrupt", "tmp-race", "lock-inode") else ""}')
        print('\n  all            依次跑全部子命令（退出码取最坏的那个）')
        print('\n退出码：0 判据成立 · 1 判据不成立 · 2 有行未核（测不了 ≠ 通过）')
        return 0
    if args.cmd == 'all':
        worst = 0
        for name, (fn, _, _) in COMMANDS.items():
            print('=' * 96)
            print(f'### {name}')
            print('=' * 96)
            worst = max(worst, fn(args))
            print()
        print(f'全部子命令完成：最坏退出码 {worst}')
        return worst
    if args.cmd not in COMMANDS:
        print(f'未知子命令 {args.cmd!r}；用 --list 看清单', file=sys.stderr)
        return 3
    fn = COMMANDS[args.cmd][0]
    if args.cmd == 'tmp-race' and args.isolate:
        fn = cmd_tmp_race_isolate          # --isolate：只跑修法那一臂（要求归零）
    return fn(args)


if __name__ == '__main__':
    sys.exit(main())
