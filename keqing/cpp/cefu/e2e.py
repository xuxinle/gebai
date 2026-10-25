
import json, os, subprocess, sys, tempfile, pathlib

import pathlib as _pl
BASE = _pl.Path(r"C:\Users\Administrator\code\gebai\keqing\cpp\cefu")
DRIVER = str(BASE / "driver.exe")
tmp = pathlib.Path(tempfile.mkdtemp())
env = dict(os.environ, CEFU_DB=str(tmp / "cefu.json"))

if sys.stdout.encoding and sys.stdout.encoding.lower() not in ("utf-8", "utf8"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

p = subprocess.Popen([DRIVER], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=subprocess.DEVNULL, env=env, text=True, encoding="utf-8", bufsize=1)

def call(req):
    p.stdin.write(json.dumps(req, ensure_ascii=False) + "\n")
    p.stdin.flush()
    return json.loads(p.stdout.readline())

print("init:", call({"id": 1, "op": "init"})["result"]["name"])
tools = call({"id": 2, "op": "tools.list"})["result"]
print("工具:", [t["name"] for t in tools])

def run(name, **args):
    r = call({"id": 99, "op": "tool.call", "tool": name, "args": args})
    return r

# ① 写三条
w1 = run("put", title="TSDB 写入延迟升高", category="存储/TSDB",
         source_ref="runbook.md:12",
         aliases=["写入慢", "写入延迟", "p99 高"],
         symptom="写入 p99 明显上涨，磁盘 IO 可能打满",
         causes=["批量写放大", "compaction 落后"],
         steps=["查批量写放大倍数（阈值 5x）", "查 compaction 进度", "确认磁盘 IO 是否打满"],
         commands=["curl /metrics | grep tsdb_write_amplification"],
         facts=["写放大阈值 5x"], owner="张三")
print("\n[写1]", w1["result"]["output"].split("\n")[0])

w2 = run("put", title="配额问题找谁", category="人/职责", source_ref="群消息 oc_abc",
         aliases=["配额", "联系谁", "审批"], symptom="需要调整配额",
         facts=["算法组张三负责配额审批 ou_abc"], owner="张三")
print("[写2]", w2["result"]["output"].split("\n")[0])

w3 = run("put", title="训练任务长期 pending", category="调度",
         source_ref="会话 a79", aliases=["排队", "pending 很久"],
         symptom="任务提交后长时间 pending",
         steps=["查该用户队列排队数", "查可用节点资源", "确认调度器正常"],
         commands=["kubectl get queue -o wide"])
print("[写3]", w3["result"]["output"].split("\n")[0])

# ② 查重拒绝
dup = run("put", title="TSDB 写入延迟升高", category="存储/TSDB", source_ref="x",
          aliases=["写入慢"])
print(f"\n[查重] 拒绝={not dup['ok']} {(dup.get('error') or '')[:60]}")

# ③ 缺 source 拒绝
nos = run("put", title="随便", category="临时")
print(f"[缺出处] 拒绝={not nos['ok']} {(nos.get('error') or '')[:46]}")

# ④ 检索（中文短句，不拆词）
for q in ["写入延迟高", "配额找谁", "任务一直 pending", "磁盘IO满了"]:
    r = run("search", query=q, limit=3)
    hits = r["result"]["data"]
    first = hits[0]["title"] if hits else "（无命中）"
    why = hits[0].get("why") if hits else []
    print(f"[检索] 「{q}」→ {len(hits)} 条，首条：{first} | why={why}")

# ⑤ 取全文 + 计命中
g = run("get", id=w1["result"]["data"]["id"])
print(f"\n[全文] {g['result']['output'][:160]}...")
g2 = run("get", id=w1["result"]["data"]["id"])
print(f"[命中计数] 第二次取全文后 hits={g2['result']['data']['hits']}")

# ⑥ 目录与统计
print("\n[目录]", run("list")["result"]["output"][:120].replace("\n", " | "))
print("[统计]", run("stats")["result"]["output"].replace("\n", " | "))

# ⑦ 巡检
rev = run("review")
print(f"\n[巡检] 项数={len(rev['result']['data'])}")
print("  ", rev["result"]["output"][:200].replace("\n", " "))

# ⑧ 确认后置信度变 verified
c = run("confirm", id=w2["result"]["data"]["id"])
print(f"\n[确认] {c['result']['data']['confidence']}")

assert dup["ok"] is False and "similarity" not in dup, "dedup not enforced"
assert nos["ok"] is False and "source_ref" in (nos.get("error") or ""), "missing source not rejected"
assert g2["result"]["data"]["hits"] == 2, "命中计数不对"
assert c["result"]["data"]["confidence"] == "verified", "确认未生效"

p.stdin.close()
p.wait(timeout=5)
print("\n=== 全部通过 ===")
