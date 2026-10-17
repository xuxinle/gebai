#!/usr/bin/env python3
"""逆向验证：「锚点会过期」这条检测真能抓住东西吗（可复跑，不是一次性手改）。

## 为什么需要这个脚本

`tools/shengsheng_sentinel.py` 新增了 `stale_anchor_hints()`——它要回答的问题是
「**这条修复锚点还配不配钉在那儿**」：特征串里含已被本仓修复消灭的缺陷标记词（`RETIRED_SHAPES`），
或锚点只在**注释行**命中（守的是文档不是行为）。这类检测的失败形态很特别：**它永远返回空表也照样绿**。
所以判据不能是「跑一遍没报错」，而是「把它退化成永远不报 ⇒ 用例必须变红」+「把真的过期锚点喂进去
⇒ 用例必须变红」。这正是本仓 §7.1 的口径：**要一条会红的反证**。

## 三个子场景（缺一不可解释）

  A. **退化杀得死**：把 `stale_anchor_hints` 改成第一行 `return []`（=永远不报）→ 自测**必红**。
     红不了 ⇒ 这条检测没有判别力，等于没写。
  B. **真案例杀得死**：把本仓那个**真实旧锚点**（`运行态 ctx 判据` / `… > WORKER_CTX_LIMIT`）
     塞回临时副本的 `ANCHORS` 表 → 自测**必红**（那条「本仓锚点表里不该有过期锚点」的断言会拔响）。
     这一条用的是 1939b64f→e4181ef1 的真实形状，不是编出来的。
  C. **正向对照**：未改的副本 → 自测**必绿**。没有它，A/B 的红可能只是「副本本身就坏」。

## 为什么全程只在临时副本里动手（与其它 reverse_verify 脚本不同）

`tools/shengsheng_sentinel.py` 的工作树里**同时摊着另一条演进线的未提交改动**（同一文件、764 行）。
真去改生产文件再回填，最坏后果是撞掉别人正在验的那份回退态——那是本仓已经付过代价的教训。
这里不需要那个风险：被验的是**判据**，判据只依赖 `shengsheng_sentinel.py` + 它自己的自测，
两者拷进临时目录就是完整闭环（且不需要门禁锁 `.reverse_verify.lock`：我们从不写生产树）。
拷贝用 `copy2` + **每次先删后写**回填（`copy2` 会把旧 mtime 一起搬回来，那是另一种假绿）。

## 跑法

    python3 tools/sentinel_anchor_stale_drill.py     # 退出码 0 = 四个场景都如期

需要 `bun` 以外的一切（只用 python3 + 本仓的两个文件）。
"""
from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
SENTINEL = REPO / "tools" / "shengsheng_sentinel.py"
SELFTEST = REPO / "tools" / "test_shengsheng_sentinel.py"

# 本仓真实的旧锚点（1939b64f 之前的样子）——它是「缺陷形状」本身，正是本单要禁的那类。
OLD_ANCHOR_LINE = (
    '    ("运行态 ctx 判据", "const ctxOver = (live.ctxTokens ?? 0) > WORKER_CTX_LIMIT",'
    ' "packages/agents/src/agents/shengsheng/shengsheng.ts"),\n'
)
# 退化手法：让检测永远不报（第一行直接返回空表）。必须落在函数体第一行之后，否则语法坏掉。
DEGRADE_OLD = '    out: list[dict] = []\n    for r in rows:'
DEGRADE_NEW = '    out: list[dict] = []\n    return []          # ← 演练用的退化：永远不报任何提示\n    for r in rows:'


def run_selftest(workdir: Path) -> tuple[int, str]:
    """在 workdir 里跑自测（两条命令：直接跑与 `python3 脚本` 等价，这里用同一解释器）。"""
    p = subprocess.run([sys.executable, str(workdir / "test_shengsheng_sentinel.py")],
                       cwd=workdir, capture_output=True, text=True, timeout=900)
    out = (p.stdout or "") + (p.stderr or "")
    tail = "\n".join(out.strip().splitlines()[-6:])
    return p.returncode, tail


def stage(tmp: Path) -> Path:
    """把哨兵 + 自测拷进临时目录（自测靠同目录 `sys.path` 找哨兵，故两者必须同级）。"""
    work = tmp / "work"
    work.mkdir(parents=True, exist_ok=True)
    for src in (SENTINEL, SELFTEST):
        dst = work / src.name
        if dst.exists():
            dst.unlink()          # 先删旧文件再写（copy2 搬 mtime = 假绿的经典成因）
        shutil.copy2(src, dst)
    return work


def put(path: Path, text: str) -> None:
    """**删旧再写**（不保留旧文件句柄/mtime 语义），并落盘后回读确认。"""
    path.unlink()
    path.write_text(text, encoding="utf-8")
    assert path.read_text(encoding="utf-8") == text, f"回填没生效：{path}"


def main() -> int:
    if not SENTINEL.exists() or not SELFTEST.exists():
        print(f"✗ 缺少文件：{SENTINEL} / {SELFTEST}")
        return 2
    sent_before = SENTINEL.read_bytes()          # 生产文件的指纹（演练不许动它）
    test_before = SELFTEST.read_bytes()
    body = SENTINEL.read_text(encoding="utf-8")
    if DEGRADE_OLD not in body:
        # 演练自己也会过期：`stale_anchor_hints` 被重构后这里必须**大声失败**，
        # 而不是“没找到就跳过 → 全绿”（那正是我们在别处反复抓到的假绿）。
        print("✗ 退化锚点（DEGRADE_OLD）在 shengsheng_sentinel.py 里找不到——"
              "stale_anchor_hints 可能被重构了，请同步本演练脚本（本轮不产出任何结论）")
        return 2

    results: list[tuple[str, bool, str]] = []
    with tempfile.TemporaryDirectory(prefix="sentinel-anchor-drill-") as td:
        tmp = Path(td)

        # ---- C. 正向对照：未改的副本必须绿（先跑它：A/B 的红才有意义）----
        work = stage(tmp)
        rc, tail = run_selftest(work)
        results.append(("C 正向对照（未改副本）→ 必须绿", rc == 0, tail))
        if rc != 0:
            print("✗ 正向对照就红了，后面两个场景的红不可解释——先修副本/环境")
            for name, ok, t in results:
                print(f"[{'OK ' if ok else '红  '}] {name}\n{t}\n")
            return 1

        # ---- A. 把检测退化成“永远不报”→ 自测必红 ----
        work = stage(tmp)
        put(work / "shengsheng_sentinel.py", body.replace(DEGRADE_OLD, DEGRADE_NEW, 1))
        rc, tail = run_selftest(work)
        results.append(("A 检测退化成「永远不报」→ 必须红", rc != 0, tail))

        # ---- B. 把真实旧锚点塞回去 → 自测必红（本仓锚点表里不该有过期锚点）----
        work = stage(tmp)
        lines = body.splitlines(keepends=True)
        at = next((i for i, ln in enumerate(lines) if ln.startswith("ANCHORS = [")), None)
        if at is None:
            print("✗ ANCHORS 表找不到（结构变了），本轮不产出任何结论")
            return 2
        seeded = "".join(lines[: at + 1]) + OLD_ANCHOR_LINE + "".join(lines[at + 1:])
        put(work / "shengsheng_sentinel.py", seeded)
        rc, tail = run_selftest(work)
        results.append(("B 塞回真实旧锚点（含 WORKER_CTX_LIMIT）→ 必须红", rc != 0, tail))

    # ---- 复原核对：生产文件一个字节都没变（演练只在副本上动手）----
    same = SENTINEL.read_bytes() == sent_before and SELFTEST.read_bytes() == test_before
    results.append(("复原核对：生产文件逐字节未变", same, f"{SENTINEL.name} / {SELFTEST.name}"))

    ok_all = all(ok for _, ok, _ in results)
    for name, ok, tail in results:
        print(f"[{'OK ' if ok else '红  '}] {name}")
        print("\n".join("      " + ln for ln in tail.splitlines()[-4:]))
    print(f"\n{'全部如期（A/B 红是为了证明判据有判别力）' if ok_all else '有场景不符合预期'}")
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
