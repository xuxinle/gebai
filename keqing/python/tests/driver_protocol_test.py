#!/usr/bin/env python3
"""客卿 Python 协议驱动（driver.py）的契约测试。

这是**纯标准库**路径：协议层（stdin/stdout 一行一个 JSON）不依赖任何 pip 包，
所以它必须始终可测——宿主与边车之间的握手失败会表现成「子代理装载不上/工具全不可用」，
而这类故障光看日志很难定位。

测试方式：把 driver.py 当真实子进程拉起，走一遍握手与列举（不改协议实现）。
"""

import json
import os
import subprocess
import sys

LANG_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DRIVER = os.path.join(LANG_DIR, "driver.py")


def _call(requests):
    """起一个 driver 子进程，按顺序收发每个请求，返回 (响应列表, stderr 文本)。"""
    payload = "".join(json.dumps(item, ensure_ascii=False) + "\n" for item in requests)
    proc = subprocess.run(
        [sys.executable, DRIVER],
        input=payload,
        capture_output=True,
        text=True,
        timeout=60,
        cwd=LANG_DIR,
    )
    responses = []
    for line in proc.stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        responses.append(json.loads(line))
    return responses, proc.stderr, proc.returncode


def test_init_handshake_contract():
    responses, stderr, code = _call([{"id": 1, "op": "init"}])
    assert code == 0, f"驱动异常退出: {code}\n{stderr}"
    assert len(responses) == 1, f"init 应恰好回一行，实际 {len(responses)} 行：{responses}"
    reply = responses[0]
    assert reply["id"] == 1 and reply["ok"] is True
    result = reply["result"]
    assert result["name"] == "python"
    assert isinstance(result["protocol"], int) and result["protocol"] >= 1
    # 宿主靠这两个字段判断解释器与运行态（venv 是否激活关系到依赖装到哪）
    assert result.get("python"), "init 应回 Python 版本（宿主诊断用）"
    assert result.get("executable"), "init 应回解释器路径（宿主据此定 venv）"
    assert isinstance(result.get("venvActive"), bool)


def test_tools_list_returns_base_tools_with_schema():
    responses, stderr, _ = _call([{"id": 1, "op": "init"}, {"id": 2, "op": "tools.list"}])
    assert len(responses) == 2, f"应答行数不对：{len(responses)}\n{stderr}"
    tools = responses[1]["result"]
    assert isinstance(tools, list) and tools, "基础工具集不能为空"
    names = {tool["name"] for tool in tools}
    # python_run / python_pip / python_status 是语言层基础工具（模型侧以 python_ 前缀可见）
    assert {"run", "pip", "status"} <= names, f"缺基础工具：{sorted(names)}"
    for tool in tools:
        assert tool["description"], f"{tool['name']} 缺描述（模型侧看不到用途）"
        assert isinstance(tool["parameters"], dict), f"{tool['name']} 缺参数 schema"


def test_unknown_op_reports_error_without_crashing():
    responses, stderr, code = _call([{"id": 7, "op": "no.such.op"}])
    assert code == 0, f"未知 op 不该让进程崩溃\n{stderr}"
    assert responses, "未知 op 也必须回一行（否则宿主永久等待）"
    assert responses[0]["id"] == 7
    assert responses[0]["ok"] is False


def test_stdin_eof_exits_process():
    """stdin EOF（父进程已死）必须立即退出——防孤儿边车进程常驻。"""
    proc = subprocess.run(
        [sys.executable, DRIVER],
        input="",
        capture_output=True,
        text=True,
        timeout=30,
        cwd=LANG_DIR,
    )
    assert proc.returncode == 0, f"EOF 后应以 0 退出，实际 {proc.returncode}\n{proc.stderr}"
