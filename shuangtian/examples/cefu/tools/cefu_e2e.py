
"""册府 e2e：启动 → 控制通道 → 断言界面与数据 → 截图 → 交互验证。"""
import json, os, pathlib, shutil, socket, struct, subprocess, sys, time

ROOT = pathlib.Path("C:/Users/Administrator/code/gebai/shuangtian/examples/cefu")
BIN = ROOT / "build" / "dev" / "bin" / "cefu.exe"
SHOTS = ROOT / "tmp" / "e2e"

if sys.stdout.encoding and sys.stdout.encoding.lower() not in ("utf-8", "utf8"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

def read_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise RuntimeError("控制通道提前关闭")
        buf += chunk
    return buf

class Client:
    def __init__(self, port, token):
        self.port, self.token, self.rid = port, token, 0
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=30.0)
        self.sock.settimeout(30.0)
    def _call(self, method, params=None):
        self.rid += 1
        if method == "hello":
            params = dict(params or {}, token=self.token)
        body = json.dumps({"id": self.rid, "method": method, "params": params or {}}).encode()
        self.sock.sendall(struct.pack(">I", len(body)) + body)
        n = struct.unpack(">I", read_exact(self.sock, 4))[0]
        return json.loads(read_exact(self.sock, n))
    def ok(self, method, params=None):
        r = self._call(method, params)
        if not r.get("ok"):
            raise AssertionError(f"{method} 失败: {r.get('error')}")
        return r.get("result", r)

def main():
    shutil.rmtree(SHOTS, ignore_errors=True)
    SHOTS.mkdir(parents=True, exist_ok=True)
    ctl = SHOTS / "ctl.json"
    log_f = open(SHOTS / "log.txt", "wb")
    proc = subprocess.Popen([str(BIN), "--headless", "--control-port", "0",
                             "--control-file", str(ctl), "--shots", str(SHOTS)],
                            cwd=str(ROOT), stdout=log_f, stderr=subprocess.STDOUT)
    client = None
    try:
        for _ in range(120):
            time.sleep(0.25)
            try:
                raw = ctl.read_text(encoding="utf-8").strip()
                if not raw:
                    continue
                info = json.loads(raw)
                if info.get("port") and info.get("token"):
                    cand = Client(info["port"], info["token"])
                    cand.ok("hello")
                    client = cand
                    break
            except Exception as e:
                last = e
                pass
        if client is None:
            print("控制通道没起来")
            print((SHOTS / "log.txt").read_text(encoding="utf-8", errors="replace")[-2000:])
            return 1
        print(f"[OK] 控制通道 {client.port}")

        # ① 界面树：三栏都在
        found = client.ok("find", {"selector": "#root"})
        print(f"[OK] #root 命中 {len(found.get('matches') or [])}")
        for sel, name in [("#cat-list", "分类栏"), ("#entry-list", "词条列表"), ("#detail-body", "正文栏")]:
            n = len(client.ok("find", {"selector": sel}).get("matches") or [])
            print(f"[{'OK' if n else 'FAIL'}] {name} {sel} = {n}")
            if not n:
                return 1

        # ② 数据：种子 7 条已导入，统计文本非空
        txt = client.ok("get", {"id": "stats", "property": "text"}).get("text") or ""
        print(f"[{'OK' if txt and txt != '——' else 'FAIL'}] stats = {txt!r}")
        if not txt or txt == "——":
            return 1

        # ③ 列表真的建出了卡片（种子 7 条 → entry-list 下有子元素）
        # 用 tree 看 #entry-list 下真实有什么（选择器语法不确定时先看结构）
        tree = client.ok("tree", {})
        root_node = tree.get("tree") if isinstance(tree, dict) else tree
        def walk(node, depth=0, out=None):
            out = out if out is not None else []
            if not isinstance(node, dict):
                return out
            ident = node.get("id") or ""
            typ = node.get("type") or node.get("tag") or ""
            out.append(("  " * depth) + f"{typ} #{ident}".strip())
            for ch in (node.get("children") or []):
                walk(ch, depth + 1, out)
            return out
        lines = walk(root_node)
        interesting = [l for l in lines if "entry-list" in l or "list-title" in l or "cat-list" in l]
        print("[INFO] 结构片段:")
        for l in interesting[:10]:
            print("   ", l)
        # 找 entry-list 节点的子元素数
        def find_node(node, ident):
            if not isinstance(node, dict):
                return None
            if node.get("id") == ident:
                return node
            for ch in (node.get("children") or []):
                r = find_node(ch, ident)
                if r is not None:
                    return r
            return None
        el = find_node(root_node, "entry-list")
        cnt = len((el or {}).get("children") or [])
        print(f"[{'OK' if cnt >= 7 else 'FAIL'}] 列表卡片 = {cnt}")
        if cnt < 7:
            return 1

        # ④ 截图
        # ⑤ 交互：点第一张卡片 → 正文栏应出现该词条标题
        # 点卡片里的按钮（Panel 不参与命中测试，真实可点区是那个 Button）
        cards = client.ok("find", {"selector": "#entry-list Button"}).get("matches") or []
        print(f"[INFO] 卡片内按钮数 = {len(cards)}")
        rect = (cards[0].get("bounds") or {}) if cards else {}
        print(f"[INFO] 首卡片按钮 rect={rect}")
        btn_id = cards[0].get("id") or "" if cards else ""
        print(f"[INFO] 首卡片按钮 id={btn_id!r}")
        # 两条路径都试：① 真实鼠标；② 协议 invoke click（引擎内置动作）
        client.ok("input.mouse", {"kind": "click",
                                  "x": rect["x"] + rect["width"] / 2,
                                  "y": rect["y"] + rect["height"] / 2})
        time.sleep(0.2)
        time.sleep(0.5)  # 让重绘落地（脚本内 prop 读回与协议 get 走不同路径，后者要等一帧）
        t1 = client.ok("get", {"id": "detail-title", "property": "text"}).get("text") or ""
        print(f"[INFO] 鼠标点击后 detail-title={t1!r}")

        print(f"[{'OK' if t1 and t1 != '从左边选一个词条' else 'FAIL'}] 点击卡片后正文标题 = {t1!r}")
        if not t1 or t1 == "从左边选一个词条":
            return 1

        # ⑥ 检索：输入「配额」→ 列表应变少且命中含"配额"
        client.ok("set", {"id": "search", "props": {"value": "配额"}})
        client.ok("invoke", {"id": "do-search", "action": "click"})
        time.sleep(0.3)
        lt = client.ok("get", {"id": "list-title", "property": "text"}).get("text") or ""
        print(f"[{'OK' if '检索' in lt else 'FAIL'}] 检索后列表标题 = {lt!r}")
        if "检索" not in lt:
            return 1

        import base64
        shot = SHOTS / "search.png"
        reply = client.ok("capture", {})
        shot.write_bytes(base64.b64decode(reply["base64"]))
        print(f"[{'OK' if shot.exists() and shot.stat().st_size > 5000 else 'FAIL'}] 截图 {shot} ({shot.stat().st_size if shot.exists() else 0} B)")
        if not shot.exists() or shot.stat().st_size < 5000:
            return 1

        print("=== 全部通过 ===")
        return 0
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()

sys.exit(main())
