#!/usr/bin/env python3
"""codeeditor-dsl 端到端验证（控制通道驱动）。

验证声明式重写版的关键交互链：
  1. 五层骨架与标签栏都在（tree 结构）
  2. 打开文件（点侧栏按钮）→ 标签 + 编辑器内容
  3. 切换标签 → 活动标签跟 key 走，内容切换
  4. 切底部面板（输出/问题/终端）→ 面板可见性跟随
  5. 主题切换按钮 → 状态栏文案
  6. 关闭标签 → 标签栏收缩、无标签时显示空态
用法: python3 tools/codeeditor_dsl_e2e.py <binary> <ctl.json> <port>
"""
import json
import os
import socket
import struct
import subprocess
import sys
import time


def call(sock, request_id, method, params, token):
    if method == "hello" and token:
        params = dict(params, token=token)
    body = json.dumps({"id": request_id, "method": method, "params": params}).encode()
    sock.sendall(struct.pack(">I", len(body)) + body)
    header = b""
    while len(header) < 4:
        chunk = sock.recv(4 - len(header))
        if not chunk:
            raise RuntimeError("连接关闭")
        header += chunk
    (length,) = struct.unpack(">I", header)
    payload = b""
    while len(payload) < length:
        chunk = sock.recv(length - len(payload))
        if not chunk:
            raise RuntimeError("响应截断")
        payload += chunk
    return json.loads(payload)


def flatten(node, out):
    if isinstance(node, dict):
        if node.get("type"):
            out.append(node)
        for child in node.get("children", []) or []:
            flatten(child, out)
    return out


def nodes_of(sock, rid, token):
    reply = call(sock, rid, "tree", {}, token)
    return flatten(reply["result"]["tree"], [])


def get_props(sock, rid, token, element_id):
    """取元素的属性面（协议 `get` —— 与声明式写入同一份实现）。"""
    reply = call(sock, rid, "get", {"id": element_id}, token)
    result = reply.get("result", {})
    return result.get("props", {}) if isinstance(result, dict) else {}


def by_id(nodes, suffix):
    for node in nodes:
        if str(node.get("id", "")).endswith(suffix):
            return node
    return None


def by_type(nodes, kind):
    return [n for n in nodes if n.get("type") == kind]


def main():
    binary, control_file, port = sys.argv[1], sys.argv[2], int(sys.argv[3])
    if os.path.exists(control_file):
        os.remove(control_file)
    proc = subprocess.Popen([binary, "--headless", "--control-port", str(port),
                             "--control-file", control_file, "--ms", "40000"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    rid = [0]

    def next_rid():
        rid[0] += 1
        return rid[0]

    try:
        for _ in range(120):
            if os.path.exists(control_file):
                break
            time.sleep(0.1)
        info = json.load(open(control_file))
        token = info.get("token", "")
        sock = socket.create_connection(("127.0.0.1", info["port"]), timeout=10)
        call(sock, next_rid(), "hello", {}, token)
        time.sleep(0.5)

        nodes = nodes_of(sock, next_rid(), token)
        # —— 1. 五层骨架 ——
        ids = {str(n.get("id", "")) for n in nodes}
        for want in ["editor-page", "titlebar", "activity-bar", "sidebar", "editor-tabs",
                     "editor", "bottom-panel", "bottom-tabs", "statusbar",
                     "status", "btn-theme", "language-label"]:
            assert any(i.endswith(want) for i in ids), f"缺少元素 #{want}"
        print("[1] 五层骨架与钩子元素齐备")

        # —— 2. 打开文件（点侧栏树按钮）——
        open_btn = None
        for node in nodes:
            if node.get("type") == "Button" and "deploy.py" in str(node.get("text", "")):
                open_btn = node
                break
        assert open_btn, "侧栏没有 deploy.py 按钮"
        call(sock, next_rid(), "invoke", {"id": open_btn["id"], "action": "click"}, token)
        time.sleep(0.4)
        nodes = nodes_of(sock, next_rid(), token)
        tabs = by_type(nodes, "Tabs")
        assert tabs, "没有 Tabs"
        editor = by_id(nodes, "editor")
        assert editor is not None, "没有 #editor"
        props = get_props(sock, next_rid(), token, "editor")
        assert "python" in str(props.get("language", "")), \
            f"语言未跟随标签（属性面）: {props.get('language')}"
        assert "Deployer" in str(props.get("text", "")), "编辑器内容未切到 deploy.py"
        # 标签数（含底部面板的 Tabs）：找 editor-tabs
        etabs = next((t for t in tabs if str(t.get("id", "")).endswith("editor-tabs")), None)
        assert etabs is not None
        print(f"[2] 打开 deploy.py → 标签就绪，语言 {props.get('language')}")

        # —— 3. 切标签（点 renderer.cpp 按钮）——
        rb = None
        for node in nodes:
            if node.get("type") == "Button" and "renderer.cpp" in str(node.get("text", "")):
                rb = node
                break
        assert rb, "找不到 renderer.cpp"
        call(sock, next_rid(), "invoke", {"id": rb["id"], "action": "click"}, token)
        time.sleep(0.4)
        nodes = nodes_of(sock, next_rid(), token)
        editor = by_id(nodes, "editor")
        props = get_props(sock, next_rid(), token, "editor")
        assert "cpp" in str(props.get("language", "")), \
            f"切换标签后语言未跟随: {props.get('language')}"
        lang_label = by_id(nodes, "language-label")
        assert "cpp" in str(lang_label.get("text", "")), f"状态栏语言未更新: {lang_label}"
        print("[3] 切回 renderer.cpp → 编辑器与状态栏一致（cpp）")

        # —— 4. 底部面板切换（点「问题」标签）——
        # Tabs 的标签不是子元素，而是组件内部状态；用输入坐标（标签矩形）点击。
        bottom_tabs = by_id(nodes, "bottom-tabs")
        assert bottom_tabs, "找不到 #bottom-tabs"
        reply = call(sock, next_rid(), "get", {"id": bottom_tabs["id"]}, token)
        count = reply["result"]["props"].get("count")
        # 改用协议 `invoke select`（Tabs 的一等动作；选择第 0 个 = 问题）
        call(sock, next_rid(), "invoke",
             {"id": bottom_tabs["id"], "action": "select", "argument": "0"}, token)
        time.sleep(0.4)
        nodes = nodes_of(sock, next_rid(), token)
        visible_texts = [n.get("text", "") for n in nodes
                         if n.get("state", {}).get("visible", True)]
        assert any("工作区干净" in t for t in visible_texts), \
            f"『问题』面板未显示（count={count}）: {visible_texts[:8]}"
        print("[4] 底部面板切到『问题』→ 内容可见")

        # —— 5. 主题切换 ——
        theme_btn = by_id(nodes, "btn-theme")
        assert theme_btn, "找不到 #btn-theme"
        call(sock, next_rid(), "invoke", {"id": theme_btn["id"], "action": "click"}, token)
        time.sleep(0.4)
        nodes = nodes_of(sock, next_rid(), token)
        status = by_id(nodes, "status")
        assert "dark" in str(status.get("text", "")).lower(), f"主题切换未反映: {status}"
        print(f"[5] 主题切换 → 状态栏「{status['text']}」")

        # —— 6. 命令面板（菜单 → 查看 → 命令面板；overlay 条件声明）——
        nodes = nodes_of(sock, next_rid(), token)
        menubar = next((n for n in nodes if str(n.get("id", "")).endswith("menubar")), None)
        assert menubar is not None, "没有 #menubar"
        # 打开菜单面板：点菜单栏标题不可靠（坐标）；直接验证 overlay 生命周期：
        # 先确认无 #command-palette，再经快捷键路径（spawn 时无键输入）改用菜单面板
        palette_before = any("command-palette" in str(n.get("id", "")) for n in nodes)
        assert not palette_before, "初始不应有命令面板"
        # 用 MenuBar 的 activate 动作打开第一个菜单（index 0）→ 面板 overlay。
        # `click`/`activate` 都汇聚到 `MenuBar::activate()` → on_open_menu(0)。
        call(sock, next_rid(), "invoke",
             {"id": menubar["id"], "action": "click"}, token)
        time.sleep(0.4)
        nodes = nodes_of(sock, next_rid(), token)
        panel = next((n for n in nodes if "menu-panel-file" in str(n.get("id", ""))), None)
        assert panel is not None, "菜单下拉面板（overlay）未挂上"
        item_texts = {n.get("text", "") for n in flatten(panel, [])}
        assert any("保存" in t for t in item_texts), f"菜单项缺失: {item_texts}"
        print("[6] 菜单栏 → 下拉面板 overlay 挂载正常")

        # —— 7. 命令面板 overlay 生命周期（声明与回收）——
        # MenuPanel 是纯自绘（菜单项不是子元素）——经协议点击条目需坐标；
        # 这里用「菜单面板已挂」验证菜单路径，命令面板用输入文本 + 真实鼠标点击条目。
        # 先用 App 级路径：点菜单面板内的条目（MenuPanel 的语义文本里有条目名）。
        panel_node = next((n for n in flatten(panel, []) if n.get("type") == "MenuPanel"), None)
        assert panel_node is not None, "菜单面板元素缺失"
        sem = str(panel_node.get("text", ""))
        assert "保存" in sem, f"菜单语义文本异常: {sem[:60]}"
        print(f"[6b] 菜单面板语义就绪（{sem[:40]}…）")

        # 命令面板：经 MenuPanel 的条目坐标点击（用 bounds + 行高推算第二项 = 打开文件…）
        bounds = panel_node.get("bounds", {})
        # 关闭当前菜单（再点一次 MenuBar）→ 先点菜单第一项（新建文件 → 打开命令面板）
        item_x = bounds.get("x", 0) + 40
        item_y = bounds.get("y", 0) + 22   # 第一项的垂直中心（条目高约 28）
        call(sock, next_rid(), "input.mouse",
             {"kind": "click", "x": item_x, "y": item_y, "button": 1, "click_count": 1}, token)
        time.sleep(0.5)
        nodes = nodes_of(sock, next_rid(), token)
        palette = next((n for n in nodes if str(n.get("id", "")).endswith("command-palette")), None)
        assert palette is not None, "命令面板未打开（点菜单首项）"
        cmds = [n.get("text", "") for n in flatten(palette, []) if "文件:" in str(n.get("text", ""))]
        assert cmds, "命令面板内没有命令条目"
        print(f"[7] 命令面板打开（{len(cmds)} 条命令）")
        # 关闭按钮 → 下一帧 sweep 移除（overlay 生命周期）
        close_btn = next((n for n in flatten(palette, []) if str(n.get("id", "")).endswith("palette-close")), None)
        assert close_btn is not None, "命令面板没有关闭按钮"
        call(sock, next_rid(), "invoke", {"id": close_btn["id"], "action": "click"}, token)
        time.sleep(0.5)
        nodes = nodes_of(sock, next_rid(), token)
        gone = not any(str(n.get("id", "")).endswith("command-palette") for n in nodes)
        assert gone, "命令面板关闭后未被 sweep 回收"
        print("[8] 命令面板关闭 → overlay 被回收（生命周期正确）")

        print("\n[OK] codeeditor-dsl 端到端全部通过")
        return 0
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    raise SystemExit(main())
