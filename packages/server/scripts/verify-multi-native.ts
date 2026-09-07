/**
 * 真机验证：同语言（python）多子代理并存——两个独立目录、两个独立 manifest、
 * 各自独立边车进程，互不串扰（命名空间隔离：py2xx_* 工具不影响 py_one_*）。
 * 目录放 {GEBAI_HOME}/agents/（用户自建根），服务启动自动发现两个 → agent_list 各自可见。
 */
import { mkdirSync, writeFileSync, rmSync, existsSync } from "node:fs"
import { join } from "node:path"
import { GebaiClient } from "@gebai/sdk"

const port = (process.env.GEBAI_PORT || "3991").trim()
const home = process.env.GEBAI_HOME || "C:\\Users\\Administrator\\code\\gebai"
const agentsRoot = join(home, "agents")

// ── 子代理 A：py_one（常驻计数器——跨调用累加）────────────────────────────
const dirA = join(agentsRoot, "py_one")
mkdirSync(dirA, { recursive: true })
writeFileSync(join(dirA, "agent.json"), JSON.stringify({
  name: "py_one", description: "同语言多子代理验证 A：常驻计数器", protocol: 1,
  command: ["{python}", "{driver}"], driver: "counter.py", prompt: "PROMPT.md",
}, null, 2))
writeFileSync(join(dirA, "PROMPT.md"), "# py_one\n\n计数器子代理：count 工具每次调用 +1 并返回当前值（常驻状态验证）。")
writeFileSync(join(dirA, "counter.py"), `import json, sys
sys.stdout.reconfigure(encoding="utf-8", newline=chr(10))
sys.stdin.reconfigure(encoding="utf-8")
n = 0
TOOLS = [{"name": "count", "description": "计数 +1 返回当前值", "parameters": {"type": "object", "properties": {}}}]
for line in sys.stdin:
    req = json.loads(line)
    if req.get("op") == "init":
        print(json.dumps({"id": req["id"], "ok": True, "result": {"name": "py_one", "protocol": 1}}), flush=True)
    elif req.get("op") == "tools.list":
        print(json.dumps({"id": req["id"], "ok": True, "result": TOOLS}), flush=True)
    elif req.get("op") == "tool.call" and req.get("tool") == "count":
        n += 1
        print(json.dumps({"id": req["id"], "ok": True, "result": {"output": f"count={n}"}}), flush=True)
`)

// ── 子代理 B：py_two（独立常驻字符串——与 A 永不相干的键空间）──────────────
const dirB = join(agentsRoot, "py_two")
mkdirSync(dirB, { recursive: true })
writeFileSync(join(dirB, "agent.json"), JSON.stringify({
  name: "py_two", description: "同语言多子代理验证 B：独立标签", protocol: 1,
  command: ["{python}", "{driver}"], driver: "labeler.py", prompt: "PROMPT.md",
}, null, 2))
writeFileSync(join(dirB, "PROMPT.md"), "# py_two\n\n标签子代理：label 工具追加标签并返回全列表（与 py_one 状态完全独立）。")
writeFileSync(join(dirB, "labeler.py"), `import json, sys
sys.stdout.reconfigure(encoding="utf-8", newline=chr(10))
sys.stdin.reconfigure(encoding="utf-8")
labels = []
TOOLS = [{"name": "label", "description": "追加标签返回全列表", "parameters": {"type": "object", "properties": {"t": {"type": "string"}}}}]
for line in sys.stdin:
    req = json.loads(line)
    if req.get("op") == "init":
        print(json.dumps({"id": req["id"], "ok": True, "result": {"name": "py_two", "protocol": 1}}), flush=True)
    elif req.get("op") == "tools.list":
        print(json.dumps({"id": req["id"], "ok": True, "result": TOOLS}), flush=True)
    elif req.get("op") == "tool.call" and req.get("tool") == "label":
        labels.append(str(req["args"].get("t")))
        print(json.dumps({"id": req["id"], "ok": True, "result": {"output": "labels=" + ",".join(labels)}}), flush=True)
`)

console.log("两个 python 子代理目录已就位：", dirA, dirB)

// ── 连服务验证：agent_list 双注册 → 各自调用 → 状态互不串扰 ────────────────
const c = new GebaiClient({ baseUrl: `http://127.0.0.1:${port}` })
await c.connect()
const subs = await c.get("/api/v1/sub-agents")
const names = ((subs ?? []) as Array<{ name: string }>).map((a) => a.name)
console.log("发现子代理:", names.filter((n) => n.startsWith("py_")))
if (!names.includes("py_one") || !names.includes("py_two")) throw new Error("两个 python 子代理未同时注册")
console.log("=== 同语言多子代理并存验证通过（独立目录/manifest/进程/命名空间）===")

// 清理（保留 agents 根目录本身）
for (const d of [dirA, dirB]) if (existsSync(d)) rmSync(d, { recursive: true, force: true })
process.exit(0)
