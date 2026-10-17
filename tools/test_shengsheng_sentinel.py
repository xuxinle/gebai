#!/usr/bin/env python3
"""哨兵自身的回归测试：验证判据逻辑，不依赖真实磁盘状态。

## 为什么要有这个文件

哨兵是「监督优化效果」的唯一尺子。**尺子本身出错比没有尺子更坏**——
它会让人对着健康的系统反复动手，或对着退化的系统报健康。
本轮实测已踩到一次：相隔 22 秒的两份快照（心跳周期 3 分钟）被判成「停滞」，
若照它行动就是自己制造动作。故把判定逻辑钉进测试。

## 跑法

    python3 tools/test_shengsheng_sentinel.py       # 退出码 0=全过

只用标准库（不引入 pytest 依赖），与仓库其它 Python 工具一致。
"""
from __future__ import annotations

import json
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import shengsheng_sentinel as S  # noqa: E402

CASES: list[tuple[str, object]] = []


def case(fn):
    CASES.append((fn.__name__, fn))
    return fn


def mk_platform(breath=3, seeds=2, inflight=1, workers=1, ledger=1000, fed=True):
    """造一份平台线状态（只填 evaluate 用到的字段）。"""
    rows = ["| P1 | 某方向 | 自主探索 | — |"]
    if fed:
        rows.append("| P1 | 反哺方向 | 反哺 w-1@/x/biz（取证） | — |")
    return {
        "breath": breath, "seq": 1, "workers": workers, "inflight": inflight,
        "seeds": seeds, "seed_rows": rows, "fed_seeds": [r for r in rows if "反哺" in r],
        "ledger_size": ledger, "ledger_mtime": 0, "mtime": 0,
    }


def mk_task(name="HB", run=2, enabled=True, last="success", err=None, schedule="@every 3m"):
    return {"id": name, "name": name, "enabled": enabled, "runCount": run,
            "lastStatus": last, "lastError": err, "schedule": schedule, "projectRoot": "/x"}

def mk_cur(at=1000, platform=None, business=None, tasks=None, anchors_ok=True, tests_ok=True):
    return {
        "at": at,
        "platform": mk_platform() if platform is None else platform,
        "business": mk_platform() if business is None else business,
        "tasks": [mk_task()] if tasks is None else tasks,
        "anchors": [{"name": "a1", "anchor": "x", "file": "f", "present": anchors_ok}],
        "tests": {"ok": tests_ok, "passed": 63, "failed": 0 if tests_ok else 3, "rc": 0 if tests_ok else 1},
        "gate": None,
    }


def runs(*statuses) -> list[dict]:
    """造一串运行记录（一个 status 一条）——与 task-runs/*.json 的真实形状同构。"""
    out = []
    for i, s in enumerate(statuses):
        r = {"at": 1792200000000 + i * 180000, "status": s, "durationMs": 0}
        if s == "skipped":
            r["reason"] = "上次执行尚未结束，本轮跳过"
        out.append(r)
    return out


def runs_task(statuses, name="HB", run=None, schedule="@every 3m", last="success"):
    """带运行记录分类的任务行（= take_snapshot 会给任务挂上的形状）。"""
    t = mk_task(name=name, run=(len(statuses) if run is None else run), schedule=schedule, last=last)
    t["runs"] = S.classify_runs(runs(*statuses))
    return t


def lv(verdict, group):
    return [v for v in verdict if v["group"] == group]


def degraded(verdict):
    return [v["msg"] for v in verdict if v["level"] == "degraded"]


def warns(verdict):
    return [v["msg"] for v in verdict if v["level"] == "warn"]


def breaths(verdict):
    """判据 B 里关于「呼吸」的那些条（台账/种子条目不算）。"""
    return [v for v in lv(verdict, "B") if "呼吸" in v["msg"]]


# ---------------------------------------------------------------------------
# 夹具根：临时项目根 + 用户数据，**绝不碰生产** .evolve/control.md
# ---------------------------------------------------------------------------
HB_TASK = "52c429577cd547deb3502d3d76c34aa6"      # 心跳任务（sticky）
HB_SESS = "688a81da02d54113ada01c237e1c47ca"      # 心跳脉体会话
MAN_SESS = "7dd7b83059d9487db18f4fa69c2f3eb5"     # 人驱动会话（同时拿着 shengsheng 四工具）
ROOT = "/home/x/code/gebai"                        # control.md 里写的项目根（夹具里写同一个根，才算“同一项目根两个驱动者”）


def mk_task_full(tid, name, resp=None, origin=MAN_SESS, target="sticky", root=ROOT, run=3):
    """造一条 tasks.json 级别的任务（带归属与脉体会话字段——归因判据靠的就是这几个）。"""
    return {"id": tid, "name": name, "enabled": True, "runCount": run,
            "lastStatus": "success", "schedule": "@every 3m", "target": target,
            "stickySessionId": resp, "originSessionId": origin,
            "env": {"SHENGSHENG_ROOT": root}}


class Fixture:
    """一个临时项目根 + 临时用户数据根，用完自毁；期间把哨兵的全局坐标改到它身上。

    为什么必须能改全局坐标：判据 B 的归因要**真读** control.md 与会话记录、真读 tasks.json，
    只把它当纯函数测就只能测一半。而真读就必定碰路径——故哨兵把路径收进了 CFG，这里只换 CFG。
    """

    def __init__(self, note_line=None, breath=4, note_meta=None, chat_echo=None):
        import tempfile
        self._tmp = tempfile.TemporaryDirectory(prefix="sentinel-fixture-")
        base = Path(self._tmp.name)
        self.root = base / "proj"
        self.home = base / "home"
        (self.root / ".evolve").mkdir(parents=True)
        (self.root / ".evolve" / "ledger.md").write_text("# 台账\n", encoding="utf-8")
        (self.home / "users" / "admin").mkdir(parents=True)
        self._old = dict(S.CFG)
        S.CFG["root"] = self.root
        S.CFG["business"] = self.root / "biz"
        S.CFG["home"] = self.home
        S.CFG["state"] = base / "state.json"
        # 心跳任务（sticky，项目根=夹具根）
        self.tasks = [self._task(HB_TASK, "SHENGSHENG_EVOLVE::proj-abc", resp=HB_SESS)]
        self._write_tasks()
        self.write_control(note_line=note_line, breath=breath, note_meta=note_meta)
        if chat_echo:
            sid, b = chat_echo
            self.write_chat(sid, b)

    def _task(self, tid, name, resp=None, origin=MAN_SESS, target="sticky", root=None, run=3):
        # `env.SHENGSHENG_ROOT` 必须与夹具根一致：哨兵的归属规则看的是「任务上的项目根」，
        # 不一致就会把本项目自己的心跳任务当成他项的（夹具写错直接造出假红——本轮实测踩过）。
        return mk_task_full(tid, name, resp=resp, origin=origin, target=target,
                            root=root or str(self.root), run=run)

    def _write_tasks(self, extra=None):
        data = list(self.tasks) + list(extra or [])
        (self.home / "users" / "admin" / "tasks.json").write_text(
            json.dumps(data, ensure_ascii=False), encoding="utf-8")

    def add_task(self, task):
        self.tasks.append(task)
        self._write_tasks()

    def write_control(self, note_line=None, breath=4, note_meta=None):
        lines = [
            "> 生生调度状态（`shengsheng_state` 托管，勿手改）。", "",
            "## 项目坐标", f"- 项目根：{ROOT}", "",
            "## 计数", f"- 呼吸：{breath}", "- 派工序号：2",
        ]
        if note_line is not None:
            lines.append(f"- 最近一息：{note_line}")
        lines += [
            "", "## worker 池", "| 别名 | 会话 | 领域 | 任务 | 派发时间 | 轮次 | 状态 |",
            "| --- | --- | --- | --- | --- | --- | --- |",
            "| w-1 | abcdef0123456789 | d | t | 1 | 1 | idle |",
            "", "## 在途", "| 别名 | 会话 | 方向 | 预期产出 | 派发时间 | 轮次 |",
            "| --- | --- | --- | --- | --- | --- |",
            "", "## 种子队列", "| 优先级 | 方向 | 来源 | 约束 |", "| --- | --- | --- | --- |",
            "| P1 | 反哺方向 | 反哺 w-1@/x/biz | — |",
            "", "## 收藏待办", "（空）", "", "## 断点", "（夹具）", "",
        ]
        (self.root / ".evolve" / "control.md").write_text("\n".join(lines), encoding="utf-8")
        # mtime = 现在（反查证人时按 mtime 筛文件）
        os.utime(self.root / ".evolve" / "control.md", None)

    def write_chat(self, sid, breath, at=None):
        """写一条「该会话落了 呼吸=N 的断点」的会话记录（供 infer_note_writer 反查）。"""
        d = self.home / "users" / "admin" / "sessions" / sid[:2] / sid[2:4] / sid
        d.mkdir(parents=True, exist_ok=True)
        msgs = [{"id": "m1", "role": "assistant", "content": "", "createdAt": int(time.time() * 1000),
                 "toolCalls": [{"id": "c1", "name": "shengsheng_state", "arguments": {"action": "note"}}]},
                {"id": "m2", "role": "tool", "name": "shengsheng_state", "toolCallId": "c1",
                 "content": f"断点已落盘（呼吸 {breath}）：在途 0、种子 1、池 1（1 idle）——夹具",
                 "createdAt": at if at is not None else int(time.time() * 1000)}]
        (d / "chat.json").write_text(json.dumps({"id": sid, "messages": msgs}, ensure_ascii=False),
                                     encoding="utf-8")

    def grew(self, delta=1, elapsed=600):
        """「呼吸涨了」的一对快照（prev/cur 都是真读夹具磁盘，不是手造字典）。"""
        prev = S.take_snapshot()
        self.write_control(breath=prev["platform"]["breath"] + delta,
                           note_line=S.at_note((self.root / ".evolve" / "control.md")
                                               .read_text(encoding="utf-8")))
        cur = S.take_snapshot()
        prev["at"] = cur["at"] - elapsed
        cur["anchors"] = [{"name": "a", "anchor": "x", "file": "f", "present": True}]
        cur["tests"] = {"ok": True, "passed": 1, "failed": 0, "rc": 0, "suites": 2}
        return cur, prev

    def judge(self, cur, prev):
        return S.evaluate(cur, prev)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        S.CFG.clear()
        S.CFG.update(self._old)
        self._tmp.cleanup()


def judge_fixture(fx, cur, prev):
    """直调判据 B（绕开 A/C/D 的其它噪声，只看呼吸那几条）。"""
    return S.judge_breath(cur, prev, prev["platform"], cur["platform"],
                          cur["tasks"], True, cur["at"] - prev["at"])


# ---------------------------------------------------------------------------
# A 组：周期内不动 ≠ 停滞（本轮实测踩到的假退化）
# ---------------------------------------------------------------------------
@case
def test_period_not_due_is_not_stagnation():
    """两份快照只隔 22 秒、周期 180 秒 → 不能报停滞（这是本轮真踩到的坑）。"""
    prev = {"at": 1000 - 22, "platform": mk_platform(), "business": mk_platform(),
            "tasks": [mk_task(run=2)]}
    cur = mk_cur(at=1000, tasks=[mk_task(run=2, schedule="@every 3m")])
    v = S.evaluate(cur, prev)
    assert "心跳停滞" not in " ".join(degraded(v)), degraded(v)
    assert any("周期未到" in x["msg"] for x in lv(v, "A")), lv(v, "A")


@case
def test_overdue_is_stagnation():
    """过了一个周期仍不递增 → 必须报停滞（否则真死掉也无人知）。"""
    prev = {"at": 1000 - 600, "platform": mk_platform(), "business": mk_platform(),
            "tasks": [mk_task(run=2)]}
    cur = mk_cur(at=1000, tasks=[mk_task(run=2, schedule="@every 3m")])
    v = S.evaluate(cur, prev)
    assert any("心跳停滞" in m for m in degraded(v)), degraded(v)


@case
def test_run_count_increase_is_ok():
    prev = {"at": 1000 - 600, "platform": mk_platform(), "business": mk_platform(),
            "tasks": [mk_task(run=2)]}
    cur = mk_cur(at=1000, tasks=[mk_task(run=5)])
    v = S.evaluate(cur, prev)
    assert "+3 次" in " ".join(x["msg"] for x in lv(v, "A")), lv(v, "A")
    assert not [m for m in degraded(v) if "停滞" in m]


@case
def test_disabled_task_is_degraded():
    cur = mk_cur(tasks=[mk_task(enabled=False)])
    assert any("停用" in m for m in degraded(S.evaluate(cur, None)))


@case
def test_running_after_error_is_not_degraded():
    """任务已重新执行中时，历史 error 不算当前退化——否则会让人去干预一个正在自我修复的事。

    本轮实测：服务重启打断一次执行，补跑已入队（state=running），哨兵仍报退化。
    """
    cur = mk_cur(tasks=[mk_task(last="error", err="服务重启，本次执行已中断")])
    assert any("出错" in m for m in degraded(S.evaluate(cur, None)))
    cur_running = mk_cur(tasks=[mk_task(last="error", err="服务重启，本次执行已中断")])
    cur_running["tasks"][0]["state"] = "running"
    assert not [m for m in degraded(S.evaluate(cur_running, None)) if "出错" in m], degraded(S.evaluate(cur_running, None))


@case
def test_task_error_is_degraded():
    cur = mk_cur(tasks=[mk_task(last="error", err="boom")])
    assert any("出错" in m for m in degraded(S.evaluate(cur, None)))


# ---------------------------------------------------------------------------
# B 组：平台在被改（区分「周期未到」与「只派不收」）
# ---------------------------------------------------------------------------
@case
def test_breath_flat_within_period_ok():
    """周期内呼吸不变不算病——否则每次快跑都报退化。"""
    prev = {"at": 1000 - 10, "platform": mk_platform(breath=3), "business": mk_platform(),
            "tasks": [mk_task()]}
    cur = mk_cur(at=1000, platform=mk_platform(breath=3))
    assert not [m for m in degraded(S.evaluate(cur, prev)) if "呼吸" in m]


@case
def test_breath_flat_overdue_degraded():
    prev = {"at": 1000 - 600, "platform": mk_platform(breath=3), "business": mk_platform(),
            "tasks": [mk_task()]}
    cur = mk_cur(at=1000, platform=mk_platform(breath=3))
    assert any("呼吸未推进" in m for m in degraded(S.evaluate(cur, prev)))


@case
def test_ledger_flat_overdue_means_only_dispatch_no_collect():
    """台账过一个周期不增长 = 只派不收 —— worker 的报告没进永久记忆。"""
    prev = {"at": 1000 - 600, "platform": mk_platform(ledger=1000), "business": mk_platform(),
            "tasks": [mk_task()]}
    cur = mk_cur(at=1000, platform=mk_platform(ledger=1000))
    assert any("只派不收" in m for m in degraded(S.evaluate(cur, prev)))


@case
def test_platform_not_initialized_is_degraded():
    cur = mk_cur(platform=None)
    cur["platform"] = None
    assert any("未 init" in m for m in degraded(S.evaluate(cur, None)))


# ---------------------------------------------------------------------------
# C 组：闭环真跑（本哨兵的核心使命）
# ---------------------------------------------------------------------------
@case
def test_no_fed_seed_breaks_loop():
    """平台线没有反哺种子 → 闭环断在最后一环。"""
    cur = mk_cur(platform=mk_platform(fed=False))
    assert any("没有反哺条目" in m for m in degraded(S.evaluate(cur, None)))


@case
def test_fed_seed_present_is_ok():
    cur = mk_cur(platform=mk_platform(fed=True))
    v = S.evaluate(cur, None)
    assert any("反哺种子" in x["msg"] and x["level"] == "ok" for x in lv(v, "C")), lv(v, "C")


@case
def test_fed_seed_but_no_inflight_is_degraded():
    """池空 + 在途空 + 种子堆积 = 只择不生 = 闭环不前进（这条最容易漏）。"""
    cur = mk_cur(platform=mk_platform(fed=True, seeds=2, inflight=0, workers=0))
    assert any("只择不生" in m for m in degraded(S.evaluate(cur, None)))


@case
def test_no_inflight_but_workers_present_is_ok():
    """刚收藏完的间隙：在途 0 但池里还有 worker（idle）——这是正常间隙，不得报退化。

    本轮实测：worker 刚被收藏、在途清空的那一刻，哨兵报「只择不生」，而它明明刚干完活。
    这类假退化会让人去催一个刚交完单的 worker。
    """
    cur = mk_cur(platform=mk_platform(fed=True, seeds=3, inflight=0, workers=2))
    v = S.evaluate(cur, None)
    assert not [m for m in degraded(v) if "只择不生" in m], degraded(v)
    assert any("正常间隙" in x["msg"] for x in lv(v, "C")), lv(v, "C")


# ---------------------------------------------------------------------------
# A' 组：**「有动作」不是「有进展」**——skipped 必须与 success / error 分开算
#
# 这组用例存在的理由（真事，不是假想）：`skipped`（「上次执行尚未结束，本轮跳过」）让 runCount +1、
# 而 lastStatus 往往仍是上一次真跑的 success——旧判据 A（只看 runCount 递增 + lastStatus 非 error）
# 就全程绿、退出码 0，而演进已经停在那里。把判据 A 退回「只看 runCount 递增」，下面第一、二条必红。
# 真数据：`users/admin/task-runs/769433…/` = 28 success + 3 skipped。
# ---------------------------------------------------------------------------
@case
def test_skipped_streak_degrades_even_though_run_count_grows():
    """连跳 3 跳（runCount 仍在涨、lastStatus 仍 success）→ 必须报退化。

    这正是旧版的盲区：runCount 递增→ ok；lastStatus=success → 不报错。两行都绿，而息早就断了。
    """
    prev = {"at": 1000 - 600, "platform": mk_platform(), "business": mk_platform(),
            "tasks": [runs_task(["success"] * 6)]}
    cur = mk_cur(at=1000, tasks=[runs_task(["success"] * 6 + ["skipped"] * 3, run=9, last="success")])
    v = S.evaluate(cur, prev)
    assert any("连跳 3 跳" in m for m in degraded(v)), degraded(v)
    assert not [x for x in lv(v, "A") if x["level"] == "ok" and "次（累计" in x["msg"]], "递增行不得洗白：" + str(lv(v, "A"))


@case
def test_all_skipped_is_degraded_not_healthy():
    """整窗全是被跳过（一成未干）→ 退化；且不得有任何一条 A 组 ok 去“洗白”它。"""
    cur = mk_cur(tasks=[runs_task(["skipped"] * 6, run=6)])
    v = S.evaluate(cur, None)
    assert any("连跳" in m or "跳过过半" in m for m in degraded(v)), degraded(v)
    assert not [x for x in lv(v, "A") if x["level"] == "ok"], lv(v, "A")


@case
def test_skip_ratio_over_half_is_degraded():
    """不是连跳，但窗口里跳过过半（如 成功/跳过 交错）→ 也是“有动作无进展”。"""
    cur = mk_cur(tasks=[runs_task(["skipped", "success"] * 4, run=8)])
    v = S.evaluate(cur, None)
    assert any("跳过过半" in m for m in degraded(v)), degraded(v)


@case
def test_two_skips_is_warn_not_ok_and_not_degraded():
    """连跳 2 跳：还没到退化阈值，但**绝不能报健康**——单列告警（本息确实没产出）。"""
    cur = mk_cur(tasks=[runs_task(["success"] * 5 + ["skipped"] * 2, run=7)])
    v = S.evaluate(cur, None)
    assert any("连跳 2 跳" in m for m in warns(v)), warns(v)
    assert not [m for m in degraded(v) if "连跳" in m or "跳过过半" in m], degraded(v)


@case
def test_occasional_skip_still_ok_but_counted():
    """偶发跳过（上一息稍慢，下一跳补上）是正常现象——判 ok，但必须把跳过数写出来（不许隐去）。"""
    cur = mk_cur(tasks=[runs_task(["success"] * 8 + ["skipped"] + ["success"] * 3, run=12)])
    v = lv(S.evaluate(cur, None), "A")
    assert not [x for x in v if x["level"] in ("degraded", "warn")], v
    assert any(x["level"] == "ok" and "1 跳过" in x["msg"] for x in v), v


@case
def test_run_count_growth_from_skip_is_degraded_not_ok():
    """runCount 递增那行**不得**再说一句 ok：涨幅来自跳过时，那是“有动作无产出”。"""
    prev = {"at": 1000 - 600, "platform": mk_platform(), "business": mk_platform(),
            "tasks": [runs_task(["success"] * 4)]}
    cur = mk_cur(at=1000, tasks=[runs_task(["success"] * 4 + ["skipped"] * 3, run=7)])
    v = lv(S.evaluate(cur, prev), "A")
    assert any(x["level"] == "degraded" and "runCount 递增但无产出" in x["msg"] for x in v), v
    assert not [x for x in v if x["level"] == "ok" and "次（累计" in x["msg"]], v


@case
def test_no_run_records_is_unknown_never_ok():
    """取不到运行记录（新任务/该目录不可读）→ unknown，不假装“没跳过”。"""
    t = mk_task()
    t["runs"] = {"known": False, "total": 0}
    v = lv(S.evaluate(mk_cur(tasks=[t]), None), "A")
    assert any(x["level"] == "unknown" and "取不到运行记录" in x["msg"] for x in v), v
    assert not [x for x in v if x["level"] == "ok"], v


@case
def test_old_snapshot_without_runs_field_is_not_degraded():
    """**快照兼容**：旧快照/旧任务行里没有 `runs` 字段 → 不得当成退化（只能 unknown/按旧行为）。

    为何单列：`sentinel-state.json` 里已有的快照都是本单之前落的，它们没有 runs 字段。
    若把“缺字段”当成“跳过很多”，升级后第一次跑就会全局误报退化。
    """
    old_prev = {"at": 1000 - 600, "platform": mk_platform(), "business": mk_platform(),
                "tasks": [mk_task(run=2)]}                      # 旧形状：无 runs
    old_cur = mk_cur(at=1000, tasks=[mk_task(run=3)])            # 本次快照也没 runs（夹具直造）
    v = S.evaluate(old_cur, old_prev)
    assert not [m for m in degraded(v) if "跳过" in m or "连跳" in m], degraded(v)
    assert not [m for m in warns(v) if "连跳" in m], warns(v)


@case
def test_classify_runs_three_classes():
    """取证函数本身：三类分开、连跳从**最近往回数**、窗口占比只看最近 12 跳。"""
    r = S.classify_runs(runs("success", "success", "skipped", "skipped", "success", "skipped"))
    assert (r["total"], r["success"], r["skipped"], r["failed"], r["streak"]) == (6, 3, 3, 0, 1), r
    assert r["last_skip_reason"] == "上次执行尚未结束，本轮跳过", r
    # 连跳从末尾数：中间一旦有 success 就归零（那是“息又流起来了”）
    assert S.classify_runs(runs("skipped", "skipped", "success"))["streak"] == 0
    # 早期跳过被挤出窗口后不再计入占比（窗口只取最近 12 跳）
    r2 = S.classify_runs(runs(*(["skipped"] * 6 + ["success"] * 12)))
    assert r2["total"] == 18 and r2["window"] == 12 and r2["window_skipped"] == 0, r2
    # 真实数据形状：失败也算“真跑过”，不算跳过
    r3 = S.classify_runs(runs("success", "error", "timeout"))
    assert (r3["failed"], r3["skipped"], r3["streak"]) == (2, 0, 0), r3


@case
def test_task_runs_read_from_fixture_home_only():
    """取证只读 `CFG["home"]` 下的 task-runs（夹具纪律：绝不碰生产 users/admin/task-runs）。"""
    import tempfile
    old = dict(S.CFG)
    with tempfile.TemporaryDirectory(prefix="sentinel-runs-") as td:
        home = Path(td)
        d = home / "users" / "admin" / "task-runs" / "tid-fix"
        d.mkdir(parents=True)
        (d / "2026-10-17T01-00-00.000Z.json").write_text(
            json.dumps({"status": "skipped", "reason": "上次执行尚未结束，本轮跳过"}), encoding="utf-8")
        (d / "2026-10-17T01-03-00.000Z.json").write_text(json.dumps({"status": "success"}), encoding="utf-8")
        try:
            S.CFG["home"] = home
            got = S.read_task_runs("tid-fix")
            assert [r.get("status") for r in got] == ["skipped", "success"], got
            assert S.read_task_runs("nope") == []
        finally:
            S.CFG.clear()
            S.CFG.update(old)
    assert dict(S.CFG) == old, S.CFG


# ---------------------------------------------------------------------------
# D' 组：**门禁在跑 ≠ 修复被回退**
#
# 这组用例存在的理由（真事，两次）：reverse_verify.py 跑时会**故意**把 shengsheng.ts 回退成缺陷版
# （锚点串消失、测试变红），旧 D 组此时必然报 degraded——10:30 报「8 条测试红」、
# 10:35 报「init 不跨线继承锚点缺失」，门禁跑完 8/8 锚点自行回来。把「恒认为没门禁在跑」退回去，
# 下面第一、二、三条必红。
# ---------------------------------------------------------------------------
@case
def test_gate_lock_means_unknown_not_degraded():
    """`.reverse_verify.lock` 存在 + 锚点缺失 + 测试红 → D 组必须 unknown，不得报任何 degraded。"""
    cur = mk_cur(anchors_ok=False, tests_ok=False)
    cur["gate"] = {"reasons": ["锁 .reverse_verify.lock 存在（pid 4242｜2026-10-17T10:30:12）"], "peers": []}
    v = lv(S.evaluate(cur, None), "D")
    assert any(x["level"] == "unknown" and "门禁正在回退源码" in x["msg"] for x in v), v
    assert not [x for x in v if x["level"] == "degraded"], f"门禁在跑时不得报 degraded：{v}"


@case
def test_gate_revbak_and_peer_are_detected():
    """三道判据各自能单独命中：锁 / revbak / 栈上进程（夹具注，不依赖本机真有人在跑）。"""
    import tempfile
    old = dict(S.CFG)
    with tempfile.TemporaryDirectory(prefix="sentinel-gate-") as td:
        base = Path(td)
        src = base / "src"
        src.mkdir()
        try:
            S.CFG["src"] = src
            S.CFG["lock"] = base / ".reverse_verify.lock"
            # ① 什么都没有 → None（不得自己造出一个门禁）
            S.CFG["peers"] = []
            assert S.gate_probe() is None, S.gate_probe()
            # ② 栈上有门禁进程
            S.CFG["peers"] = [{"pid": 4242, "kind": "门禁脚本", "cmd": "python3 reverse_verify.py"}]
            g = S.gate_probe()
            assert g and any("在跑" in r for r in g["reasons"]), g
            # ③ 只剩 *.revbak（锁已删、进程已退：崩溃残留，源码**仍在回退态**）
            S.CFG["peers"] = []
            (src / "shengsheng.ts.revbak").write_text("x", encoding="utf-8")
            g2 = S.gate_probe()
            assert g2 and any("残留备份" in r and "revbak" in r for r in g2["reasons"]), g2
            # ④ 锁文件存在
            (src / "shengsheng.ts.revbak").unlink()
            S.CFG["lock"].write_text("pid=4242\nboot_id=abc\ntime=2026-10-17T10:30:12\n", encoding="utf-8")
            g3 = S.gate_probe()
            assert g3 and any("锁" in r and "4242" in r for r in g3["reasons"]), g3
        finally:
            S.CFG.clear()
            S.CFG.update(old)
    assert dict(S.CFG) == old, S.CFG


@case
def test_end_to_end_gate_lock_stops_tests_and_makes_D_unknown():
    """**全链路**：真探到锁 → `run_tests(gated=True)` **根本不去跑** → D 组整组 unknown。

    为何非要这条（而不只靠上面直接注 `cur["gate"]` 的两条）：直接注 gate 只测 `evaluate` 分支，
    测不到“谁去探门禁、探到后要不要拦下测试”。把 `gate_probe` 退回 `return None`，那两条照样绿
    （它们根本没走 probe），只有这条会红——这就是本单硬要求②的兜底证据。

    安全地跑 `run_tests`：把 `TEST_CMDS` 临时换成“往文件里写一行”的桩命令——
    否则回退情形下它会去跑**真实**的两道门（其中包含本自测自己 ⇒ 无限递归）。
    """
    import tempfile
    old_cfg = dict(S.CFG)
    old_cmds = list(S.TEST_CMDS)
    with tempfile.TemporaryDirectory(prefix="sentinel-e2e-gate-") as td:
        base = Path(td)
        src = base / "src"
        src.mkdir()
        mark = base / "tests-ran.marker"
        try:
            S.CFG["src"] = src
            S.CFG["lock"] = base / ".reverse_verify.lock"
            S.CFG["peers"] = []                       # 不依赖本机此刻真有人在跑门禁
            S.CFG["lock"].write_text("pid=4242\ntime=2026-10-17T10:30:12\n", encoding="utf-8")
            S.TEST_CMDS = [[sys.executable, "-c",
                            f"open({str(mark)!r},'a').write('(pass) stub\\n')"]]
            gate = S.gate_probe()
            assert gate is not None, "锁文件在场却没探到门禁"
            tests = S.run_tests(gated=True)
            assert tests.get("gated") is True, f"探到门禁后不得真跑测试：{tests}"
            assert not mark.exists(), "门禁在场时仍然去跑了测试（白烧 2 分钟，且红必是假的）"
            cur = mk_cur(anchors_ok=False)              # 模拟门禁回退期间的读态：锚点已消失
            cur["gate"] = gate
            cur["tests"] = tests
            v = lv(S.evaluate(cur, None), "D")
            assert any(x["level"] == "unknown" and "门禁正在回退源码" in x["msg"] for x in v), v
            assert not [x for x in v if x["level"] == "degraded"], f"门禁在跑时不得报 degraded：{v}"
        finally:
            S.CFG.clear()
            S.CFG.update(old_cfg)
            S.TEST_CMDS = old_cmds
    assert dict(S.CFG) == old_cfg, S.CFG


@case
def test_missing_guard_module_is_not_evidence_of_a_gate():
    """**能力缺口不是证据**：源码目录里没有 `verify_guard.py` 时，gate_probe 不得因此报“门禁在跑”。

    本仓实测踩到（真跑时）：把源码目录指到一个没有护栏模块的地方，旧草稿把「进程探测不可用」
    写进了 `reasons`，于是 D 组整组 unknown——而树其实是干净的。那就是拿**探测能力**冒充**探测结果**，
    换一个部署形态就变成新的假绿（永远不判退化）。判据：没锁、没 revbak、没进程 ⇒ None。
    """
    import tempfile
    old = dict(S.CFG)
    with tempfile.TemporaryDirectory(prefix="sentinel-noguard-") as td:
        base = Path(td)
        src = base / "src"
        src.mkdir()                                    # 空目录：既没 verify_guard.py，也没 revbak
        try:
            S.CFG["src"] = src
            S.CFG["lock"] = base / ".reverse_verify.lock"
            S.CFG["peers"] = None                      # 真走进程探测分支（验证降级路径）
            assert S.gate_probe() is None, f"能力缺口被当成了证据：{S.gate_probe()}"
            assert not [x for x in lv(S.evaluate(mk_cur(), None), "D") if x["level"] == "unknown"], \
                "干净树 + 缺护栏模块不得让 D 组变 unknown"
        finally:
            S.CFG.clear()
            S.CFG.update(old)
    assert dict(S.CFG) == old, S.CFG


@case
def test_shell_wrapper_mentioning_bun_test_is_not_evidence():
    """**包装器不是证据**：`bash -c '… bun test …'` 这类等待循环（只是命令行里提到）不得算「bun test 在跑」。

    本仓实测（真事）：另一条线的等待循环 `bash -c '…; timeout 250 bun test …'` 一直挂在栈上，
    `verify_guard.detect_peers()` 把它归为「bun test」（它的观察器排除只看命中位置**之前**有没 pgrep，
    而这类循环里 pgrep 在前、bun test 在后）。在哨兵这里后果不对称：
      误报「门禁在跑」⇒ D 组永远 unknown ⇒ **真退化被永远洗白**。
    故只认 argv[0] 本身是执行器的进程（python3 / bun / timeout）。

    ⚠ 必须**等子进程真的 exec 完**再断言：`Popen` 返回时子进程可能还停在 fork 后的 python 副本上
    （/proc/pid/cmdline 还是 python3）——不等就会时红时绿（本轮实测：单独跑绿、演练里连跑时红）。
    """
    import subprocess as sp

    def argv0(pid: int) -> str:
        try:
            raw = Path(f"/proc/{pid}/cmdline").read_bytes()
        except Exception:
            return ""
        return raw.split(b"\0", 1)[0].decode("utf-8", "replace").strip()

    # ① argv0 是 bash、命令行里带 `bun test` 字样 → 不算执行器
    proc = sp.Popen(["/bin/bash", "-c", "sleep 3; echo bun test"], start_new_session=True)
    try:
        for _ in range(200):                       # 最多等 ~2s 让它 exec 成 bash
            if argv0(proc.pid).endswith("bash"):
                break
            time.sleep(0.01)
        assert argv0(proc.pid).endswith("bash"), f"子进程没 exec 成 bash：{argv0(proc.pid)!r}"
        assert not S._pid_is_runner(proc.pid), "bash 包装器被当成了执行器（会误报门禁在跑）"
    finally:
        proc.kill()
        proc.wait()
    # ② 真的执行器（本进程是 python3）→ 算
    assert S._pid_is_runner(os.getpid()), "python3 自己都不算执行器？判据反了"
    # ③ 读不到 /proc（进程已退出/非 Linux）→ 宁可保留（安全侧失败，不把可能真在跑的门禁排掉）
    assert S._pid_is_runner(proc.pid) is True, "拿不到 argv0 时必须保留该进程（不能当成包装器排掉）"


@case
def test_gated_tests_are_unknown_not_ok_not_red():
    """测试记录本身标了 gated（本息跑测试期间门禁起来过）→ 测试那条 unknown，绝不报红。

    注意：这类“软命中”**不应**把整组 D 抹成 unknown——锚点是跑测试**之前**读的，
    那时树还没被动过，它的绿是真读数；被污染的是测试结果。故这里只断言测试那条。
    """
    cur = mk_cur(tests_ok=True)
    cur["tests"] = {"ok": False, "gated": True, "passed": 0, "failed": 0, "rc": -1,
                    "gate": {"reasons": ["锁 .reverse_verify.lock 存在（pid 4242）"]}}
    v = lv(S.evaluate(cur, None), "D")
    assert any(x["level"] == "unknown" and "门禁在跑" in x["msg"] for x in v), v
    assert not [x for x in v if x["level"] == "degraded"], v
    # 软命中下若锚点也缺：不得报 degraded（可能是门禁回退的那一阵被读到）
    cur2 = mk_cur(anchors_ok=False, tests_ok=True)
    cur2["tests"] = {"ok": False, "gated": True, "passed": 0, "failed": 0, "rc": -1,
                     "gate": {"reasons": ["在跑：pid 4242（门禁脚本）"]}}
    v2 = lv(S.evaluate(cur2, None), "D")
    assert not [x for x in v2 if x["level"] == "degraded"], f"软命中下锚点缺失不得判红：{v2}"
    assert any("门禁起过" in x["msg"] for x in v2), v2


@case
def test_clean_tree_still_judges_anchors_and_tests():
    """没有门禁时 D 组行为**完全不变**：锚点缺→红、测试红→红、全好→绿。

    这道是上面三条的对照组：如果门禁探测把「没门禁」也当成有，D 组会永远 unknown，就成了新的假绿。
    """
    ok = lv(S.evaluate(mk_cur(), None), "D")
    assert [x["level"] for x in ok] == ["ok", "ok"], ok
    miss = lv(S.evaluate(mk_cur(anchors_ok=False), None), "D")
    assert any("修复锚点缺失" in x["msg"] and x["level"] == "degraded" for x in miss), miss
    red = lv(S.evaluate(mk_cur(tests_ok=False), None), "D")
    assert any("测试未绿" in x["msg"] and x["level"] == "degraded" for x in red), red


# ---------------------------------------------------------------------------
# D 组：修复没被回退（锚点 + 测试）
# ---------------------------------------------------------------------------
@case
def test_missing_anchor_is_degraded():
    cur = mk_cur(anchors_ok=False)
    assert any("修复锚点缺失" in m for m in degraded(S.evaluate(cur, None)))


@case
def test_red_tests_is_degraded():
    cur = mk_cur(tests_ok=False)
    assert any("测试未绿" in m for m in degraded(S.evaluate(cur, None)))


@case
def test_skipped_tests_is_unknown_not_ok():
    """--no-tests 时不得报「测试 OK」——行为未验是待核，不是健康（本轮实测：曾输出 None passed）。"""
    cur = mk_cur()
    cur["tests"] = {"ok": True, "skipped": True}
    v = lv(S.evaluate(cur, None), "D")
    assert any(x["level"] == "unknown" and "未跑" in x["msg"] for x in v), v
    assert not any("None" in x["msg"] for x in v), v


# ---------------------------------------------------------------------------
# D 组：锚点会不会「过期」（旧修复的特征串正是新修复要消灭的缺陷形状）
# ---------------------------------------------------------------------------
# 真实案例（本仓已入仓）：锚点曾写作
#     ("运行态 ctx 判据", "const ctxOver = (live.ctxTokens ?? 0) > WORKER_CTX_LIMIT", …)
# 而「拿复用建议线当模型窗口上限」正是 1939b64f 消灭的缺陷。e4181ef1 改掉它——
# 那一单的原话：「留着它等于逼后来人把 bug 写回去」。下面两条把这些判据钉住。
@case
def test_stale_anchor_retired_word_is_hinted():
    """锚点特征串含退役词（`WORKER_CTX_LIMIT` / `ctxOver`）⇒ 报「可能已过期」，点名是哪个词、为何。

    这道用例的输入就是本仓那个真实旧锚点（bug 形状本身）——它必须在**不碰源码**的前提下被认出来。
    """
    rows = [
        {"name": "运行态 ctx 判据",
         "anchor": "const ctxOver = (live.ctxTokens ?? 0) > WORKER_CTX_LIMIT",
         "file": "packages/agents/src/agents/shengsheng/shengsheng.ts", "present": True},
        {"name": "ctx 两个量未合并", "anchor": "window > 0 && t > window",
         "file": "packages/agents/src/agents/shengsheng/shengsheng.ts", "present": True},
    ]
    # 全部锚点都“在位”（退役词确实在源码里能找到）—— 过期提示不能靠 present 判，必须自己认字。
    hints = S.stale_anchor_hints(rows, read_text=lambda rel: "\n".join(r["anchor"] for r in rows))
    assert [(h["name"], h["kind"]) for h in hints] == [("运行态 ctx 判据", "retired")], hints
    assert any("WORKER_CTX_LIMIT" in h["why"] for h in hints), hints
    # 对照组：已改口的锚点（两个量分开）**不得**被误报——否则提示一多就没人看了。
    assert not [h for h in hints if h["name"] == "ctx 两个量未合并"], hints


@case
def test_comment_only_anchor_is_hinted():
    """锚点在源码里**只在注释行**命中 ⇒ 提示「守的是文档不是行为」。

    为何要这道：`WORKER_CTX_LIMIT` 正是这个形态——实现里的用法已被消灭，只剩 DESIGN/注释在讲那段历史。
    若锚点串恰好落在这种注释上，它会恒绿，而修复早就没了。
    """
    rows = [{"name": "只在注释里的锚点", "anchor": "NEEDLE_XYZ", "file": "fake.ts", "present": True}]
    code_only = "const a = 1\nconst b = 'NEEDLE_XYZ'\n"
    assert S.stale_anchor_hints(rows, read_text=lambda rel: code_only) == []
    comment_only = "// 历史：NEEDLE_XYZ 曾在这里\nconst a = 1\n"
    hints = S.stale_anchor_hints(rows, read_text=lambda rel: comment_only)
    assert [h["kind"] for h in hints] == ["comment_only"], hints
    # 读不到源码（文件没了/读失败）不是“过期证据”——与「0 命中与读不到必须是两个数」同纪律
    def boom(rel):
        raise OSError("读不到")
    assert S.stale_anchor_hints(rows, read_text=boom) == []


@case
def test_stale_hint_never_judges_degrade():
    """过期提示**不得**进 evaluate 结论（它是提示，不是退化）。

    代价不对称：把提示当退化，会让人去“修”一个没坏的锚点，或为了让告警消失而改动好代码。
    """
    cur = mk_cur()
    cur["anchors"] = [{"name": "a", "anchor": "x", "file": "f", "present": True,
                       "stale": {"kind": "retired", "why": "含退役词"}}]
    assert degraded(S.evaluate(cur, None)) == [], degraded(S.evaluate(cur, None))


@case
def test_anchor_check_alone_reports_missing_without_touching_state():
    """`--anchors`（收单前那道）：只读锚点，写不写 state、跑不跑测试都与它无关。

    这道守两件事：① 它能在**不产生副作用**的前提下跑（收单时回快照会污染趋势判据）；
    ② 它的退出码口径是「任一缺失 = 拒收」。这里用真 ANCHORS 跑一次：本仓当前应 8/8 在位。
    """
    rows = S.anchor_report()
    assert len(rows) == len(S.ANCHORS) and all(isinstance(r["present"], bool) for r in rows), rows
    assert S.stale_anchor_hints(rows) == [], f"本仓锚点表里不该有过期锚点：{S.stale_anchor_hints(rows)}"


# ---------------------------------------------------------------------------
# 辅助函数
# ---------------------------------------------------------------------------
@case
def test_period_secs_parsing():
    assert S.period_secs("@every 3m") == 180
    assert S.period_secs("@every 30s") == 30
    assert S.period_secs("@every 2h") == 7200
    assert S.period_secs("0 9 * * *") == 0        # cron 不估（未知→0，不猜）
    assert S.period_secs(None) == 0


@case
def test_all_healthy_is_ok():
    """全健康时不得有退化项——否则哨兵会天天喊狼。"""
    prev = {"at": 1000 - 600, "platform": mk_platform(breath=2, ledger=900),
            "business": mk_platform(), "tasks": [mk_task(run=1)]}
    cur = mk_cur(at=1000, platform=mk_platform(breath=4, ledger=1200), tasks=[mk_task(run=3)])
    assert degraded(S.evaluate(cur, prev)) == [], degraded(S.evaluate(cur, prev))


# ---------------------------------------------------------------------------
# B' 组：判据 B 的**归因**（呼吸涨了，到底是谁改的）
#
# 这组用例存在的理由（本仓取证）：旧版判据 B 只比 `cp["breath"] > pp["breath"]`，
# 于是「心跳真死了、只剩人驱动会话写 note」在磁盘上长得与「心跳活着」一模一样。
# 这一组就是那个盲区的**可复现反证**：把它退回旧版，下面的第一、四、五条必红。
# ---------------------------------------------------------------------------
@case
def test_breath_up_by_manual_session_is_warn_not_ok():
    """呼吸递增，但归因行指向**非心跳会话** → 必须告警：既不算 ok，也不算心跳停滯。

    这正是本单要堵的盲区：心跳没跑，人在推，旧版哨兵会输出一句「平台线呼吸 +1 → 健康」。
    """
    with Fixture(note_line=f"{MAN_SESS} @ 1792201840343｜来源 manual", breath=4) as fx:
        cur, prev = fx.grew(delta=1)
        v = judge_fixture(fx, cur, prev)
        assert not [x for x in breaths(v) if x["level"] == "ok"], f"人工推进不得判 ok：{breaths(v)}"
        assert any(x["level"] == "warn" and "心跳未驱动、仅人工推进" in x["msg"] for x in breaths(v)), breaths(v)
        assert "心跳停滞" not in " ".join(warns(v)), warns(v)


@case
def test_breath_up_by_heartbeat_session_is_ok():
    """呼吸递增，且归因行指向**心跳脉体会话** → ok（且不得报人工推进）。"""
    with Fixture(note_line=f"{HB_SESS} @ 1792201907347｜来源 heartbeat:{HB_TASK}", breath=4) as fx:
        cur, prev = fx.grew(delta=1)
        v = judge_fixture(fx, cur, prev)
        assert any(x["level"] == "ok" and "写入者" in x["msg"] for x in breaths(v)), breaths(v)
        assert not warns(v), warns(v)


@case
def test_no_attribution_line_is_unknown_never_ok():
    """control.md 无「最近一息」行（老状态/手改）→ unknown，**不得猜成 ok**。"""
    with Fixture(note_line=None, breath=4) as fx:
        cur, prev = fx.grew(delta=1)
        v = judge_fixture(fx, cur, prev)
        assert not [x for x in breaths(v) if x["level"] == "ok"], f"无证人不得判 ok：{breaths(v)}"
        assert any(x["level"] == "unknown" and "归因不明" in x["msg"] for x in breaths(v)), breaths(v)


@case
def test_malformed_attribution_is_not_trusted():
    """归因行形状非法（垃圾串/缺来源）→ 宁 unknown，不得误认成心跳或人工。"""
    for bad in ["看不出来是谁写的",
                "@ 1792201840343",
                f"{HB_SESS} @ 1792201907347",              # 缺「｜来源」
                f"{HB_SESS}｜来源 其它"]:
        with Fixture(note_line=bad, breath=4) as fx:
            cur, prev = fx.grew(delta=1)
            v = judge_fixture(fx, cur, prev)
            assert not [x for x in breaths(v) if x["level"] == "ok"], f"非法归因不得判 ok：{bad!r} → {breaths(v)}"
            assert any(x["level"] == "unknown" for x in breaths(v)), f"非法归因应 unknown：{bad!r} → {breaths(v)}"
            assert not warns(v), f"不得把非法归因当成人工推进：{bad!r} → {warns(v)}"


@case
def test_origin_session_is_not_heartbeat():
    """任务的 **originSessionId（创建任务的源会话）绝不能算心跳脉体**——本栗真是同一条会话。

    真值：心跳任务 52c42957 的 originSessionId = 7dd7b830（人驱动会话），若把它当脉体，
    “人驱动推进”就会被永久误认成“心跳在跑”——盲区原样搬进归因里。
    """
    with Fixture(note_line=f"{MAN_SESS} @ 1792201840343｜来源 manual", breath=4) as fx:
        # 夹具里心跳任务的 originSessionId 就是 MAN_SESS（与生产一致）——它不得进脉体集
        drivers = S.heartbeat_drivers(S.read_heartbeat_tasks(), fx.root)
        assert drivers["sessions"] == {HB_SESS}, drivers
        assert MAN_SESS not in drivers["sessions"], f"源会话被当成心跳脉体了：{drivers}"
        cur, prev = fx.grew(delta=1)
        v = judge_fixture(fx, cur, prev)
        assert any(x["level"] == "warn" for x in breaths(v)), f"源会话不得算心跳：{breaths(v)}"


@case
def test_cross_root_and_supervise_tasks_are_not_heartbeat():
    """同根多任务时归属要准：他项的演进任务、与绑在人会话上的监督任务都不算本项目心跳。"""
    with Fixture(note_line=f"{MAN_SESS} @ 1792201840343｜来源 manual", breath=4) as fx:
        # ① 另一项目根的演进任务（脉体是另一个人会话）
        fx.add_task(mk_task_full("9" * 32, "SHENGSHENG_EVOLVE::other", resp=MAN_SESS, root="/other/proj"))
        # ② 同根但 target=session（脉体就是用户自己的会话——那就是人驱动）
        fx.add_task(mk_task_full("8" * 32, "SHENGSHENG_EVOLVE::proj-user", resp=MAN_SESS, target="session"))
        # ③ 同根的监督哨兵任务（它只跑哨兵、不推呼吸，且常绑在人会话上）
        fx.add_task(mk_task_full("7" * 32, "SHENGSHENG_SUPERVISE::proj", resp=MAN_SESS))
        drivers = S.heartbeat_drivers(S.read_heartbeat_tasks(), fx.root)
        assert drivers["sessions"] == {HB_SESS}, drivers
        assert MAN_SESS not in drivers["sessions"], drivers
        cur, prev = fx.grew(delta=1)
        v = judge_fixture(fx, cur, prev)
        assert any(x["level"] == "warn" for x in breaths(v)), breaths(v)


@case
def test_manual_push_escalates_to_degraded_after_a_period():
    """人一直推：单次只告警，**持续超一个心跳周期**必须升为退化——否则告警会被当噪声。"""
    with Fixture(note_line=f"{MAN_SESS} @ 1｜来源 manual", breath=4) as fx:
        cur, prev = fx.grew(delta=1, elapsed=60)
        v1 = judge_fixture(fx, cur, prev)
        assert any(x["level"] == "warn" for x in breaths(v1)), breaths(v1)
        assert cur.get("manualRun", {}).get("from") is not None, cur.get("manualRun")
        # 第二次：同一人工窗口再推一息，且已过 2 个心跳周期
        cur2 = S.take_snapshot()
        cur2["at"] = cur["at"] + 400
        cur2["manualRun"] = cur["manualRun"]
        fx.write_control(note_line=f"{MAN_SESS} @ 2｜来源 manual", breath=cur["platform"]["breath"] + 1)
        cur2["platform"] = S.read_control(fx.root)
        prev2 = dict(cur, at=cur["at"])
        v2 = judge_fixture(fx, cur2, prev2)
        assert any(x["level"] == "degraded" and "仅人工推进（持续" in x["msg"] for x in breaths(v2)), breaths(v2)


@case
def test_heartbeat_takes_over_resets_manual_window():
    """心跳重新接手 → ok，且人工窗口归零（否则会永远停在“持续人工推进”里污名化）。"""
    with Fixture(note_line=f"{HB_SESS} @ 1792201907347｜来源 heartbeat:{HB_TASK}", breath=9) as fx:
        cur, prev = fx.grew(delta=1)
        cur["manualRun"] = {"from": 5, "to": 8, "at": cur["at"] - 900, "sessions": [MAN_SESS]}
        v = judge_fixture(fx, cur, prev)
        assert any(x["level"] == "ok" and "心跳已重新接手" in x["msg"] for x in breaths(v)), breaths(v)
        assert cur["manualRun"] is None, cur["manualRun"]


@case
def test_attribution_falls_back_to_session_records():
    """老 control.md 没有归因行，但会话记录里有「断点已落盘（呼吸 N）」 → 反查出写入者。

    这是归因上线后的**历史数据兼容**：不给反查，所有老仓在下一息都只能报 unknown（噪声）。
    """
    with Fixture(note_line=None, breath=4) as fx:
        cur, prev = fx.grew(delta=1)                           # 4 → 5，且 control.md 仍无归因行
        fx.write_chat(MAN_SESS, cur["platform"]["breath"])     # 会话记录里落了这一息的断点
        v = judge_fixture(fx, cur, prev)
        assert any("会话记录反查" in x["msg"] and x["level"] == "warn" for x in breaths(v)), breaths(v)


@case
def test_fallback_refused_without_mtime():
    """拿不到 control.md 的 mtime 时**不得反查**：否则会把“另一条会话的同一呼吸号”当成写入者。

    本轮实测：在生产数据演练里把 mtime 置 0，反查就扫全史，把心跳会话当成了一次“人工推进”的
    写入者（呼吸号会被不同驱动者重复使用：A 写 12、B 又写 12）。从“不猜健康”倒回“猜错人”。
    """
    with Fixture(note_line=None, breath=4) as fx:
        cur, prev = fx.grew(delta=1)
        fx.write_chat(HB_SESS, cur["platform"]["breath"])      # 会话记录里确实有一个“证人”
        cur["platform"] = dict(cur["platform"], mtime=0)       # 但 mtime 不可得
        v = judge_fixture(fx, cur, prev)
        assert any(x["level"] == "unknown" and "不敢反查" in x["msg"] for x in breaths(v)), breaths(v)
        assert not [x for x in breaths(v) if x["level"] == "ok"], breaths(v)


@case
def test_attribution_parse_shapes():
    """归因行解析：两种形态都认，噪音不认。"""
    a = S.parse_attribution(f"{HB_SESS} @ 1792201907347｜来源 heartbeat:{HB_TASK}")
    assert a == {"sessionId": HB_SESS, "at": 1792201907347, "source": "heartbeat", "taskId": HB_TASK}, a
    b = S.parse_attribution(f"{MAN_SESS}｜来源 manual")
    assert b["sessionId"] == MAN_SESS and b["source"] == "manual" and b["at"] is None and b["taskId"] is None, b
    for bad in [None, "", "（无）", "写着玩", f"{HB_SESS} 没写来源"]:
        assert S.parse_attribution(bad) is None, bad


@case
def test_fixture_never_touches_production_control():
    """夹具纪律：跑完必须把哨兵坐标还回去——否则后续用例/真跑会去读临时目录。"""
    before = dict(S.CFG)
    with Fixture(note_line=None) as fx:
        assert S.CFG["root"] == fx.root
    assert dict(S.CFG) == before, S.CFG


def main() -> int:
    failed = 0
    for name, fn in CASES:
        try:
            fn()
            print(f"(pass) {name}")
        except AssertionError as e:
            failed += 1
            print(f"(FAIL) {name}: {e}")
        except Exception as e:  # noqa: BLE001
            failed += 1
            print(f"(ERROR) {name}: {type(e).__name__}: {e}")
    print(f"\n{len(CASES) - failed} pass / {failed} fail （共 {len(CASES)}）")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
