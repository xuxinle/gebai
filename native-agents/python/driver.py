#!/usr/bin/env python3
"""Python 语言基础框架驱动（歌白多语言子代理协议 v1，见 native-agents/README.md）。

语言目录 native-agents/python/ 下共享：本驱动 + venv + requirements.txt；每个子代理项目
（语言目录下的二级目录，manifest agent.json 所在处）可选携带 tools.py 声明专属工具，
加载后与基础工具合并（同名覆盖基础工具）——一种语言派生任意多个子代理，实现语言对模型
透明（模型只看到工具与提示词）。

纯标准库实现（3.9+）：协议层不依赖任何 pip 包；AI 库（numpy/torch/…）只是
requirements.txt 层的可选依赖，由 python_pip 工具按需安装进语言目录 venv。

协议（stdin/stdout 各一行一个 JSON，UTF-8）：
  {"id":1,"op":"init"}                → {"id":1,"ok":true,"result":{"name":"python","protocol":2,...}}
  {"id":2,"op":"tools.list"}          → {"id":2,"ok":true,"result":[{name,description,parameters}...]}
  {"id":3,"op":"tool.call","tool":"python_run","args":{...},
                                       "ctx":{"sessionId":"a1b2","user":"admin","cwd":"…","env":{...},"sandboxed":false}}
                                      → {"id":3,"ok":true,"result":{"output":"...","data":{...}}}
约定：stdout 只写协议行（print 全部重定向捕获）；stderr 自由文本（宿主环形缓冲排障）；
stdin EOF → 立即退出（父进程已死，防孤儿）。

请求级 ctx（协议 v2 核心）：边车为进程单例跨会话共享，会话上下文随每次 tool.call 传递——
current_ctx()/ctx_env()/ctx_resolve() 读取；**禁止写入 os.environ**（并发请求不同会话会互踩，
进程全局态承载不了请求级数据）。

宿主注入的运行上下文（环境变量）：
  GEBAI_HOME      数据根（状态展示/兼容路径）
  GEBAI_AGENT_DIR 子代理项目目录（{agent_dir}/tools.py 存在时合并专属工具）
"""

import ast
import contextlib
import importlib.util
import io
import json
import os
import subprocess
import sys
import time
import traceback

PROTOCOL = 2

# 语言目录（本驱动所在目录）：venv 与 requirements.txt 的归属地
LANG_DIR = os.path.dirname(os.path.abspath(__file__))

VENVS = {}  # 每个 session 一个命名空间（常驻状态）

# ---------------- 请求级上下文（协议 v2） ----------------

# 当前请求 ctx（分发循环串行执行，处理 tool.call 前设置；None = 无请求上下文如启动阶段）。
# 边车为进程单例跨会话共享，会话 cwd/env/标识只能随请求传递。
_CURRENT_CTX = None


def set_current_ctx(ctx):
    """分发循环设置当前请求 ctx（None 清除）。"""
    global _CURRENT_CTX
    _CURRENT_CTX = ctx


def current_ctx():
    """当前请求 ctx（dict；无请求上下文时空 dict）。"""
    return _CURRENT_CTX or {}


def ctx_session_id():
    """当前请求会话标识（会话态隔离键，如 REPL 命名空间分桶；无请求 ctx 时 'default'）。"""
    return str(current_ctx().get("sessionId") or "default")


def ctx_env(key, default=None):
    """请求级环境变量：先查任务 env 覆盖，再回落进程 env。
    禁止写 os.environ（并发请求不同会话互踩，进程全局态承载不了请求级数据）。"""
    env = current_ctx().get("env") or {}
    if key in env:
        return env[key]
    return os.environ.get(key, default)


def ctx_resolve(path):
    """路径解析：绝对路径原样；相对路径基准=当前请求 ctx.cwd（会话工作区）；
    无请求 ctx 时回落进程工作目录。"""
    if not path:
        return path
    if os.path.isabs(path):
        return path
    base = str(current_ctx().get("cwd") or "") or (os.getcwd() if os.getcwd() else "")
    if not base:
        return path
    return os.path.join(base, path)


def gebai_home():
    return os.environ.get("GEBAI_HOME") or os.path.expanduser("~/.gebai")


def agent_dir():
    """子代理项目目录（manifest 所在处；驱动从语言目录共享，项目资产从这里取）。"""
    return os.environ.get("GEBAI_AGENT_DIR") or LANG_DIR


# 子代理项目名：manifest 所在目录名（宿主要求 init.name 与 manifest.name 一致；
# 项目 tools.py 可覆盖 AGENT_NAME——load_project_tools 在模块级后置处理）
_adir = agent_dir()
AGENT_NAME = os.path.basename(os.path.normpath(_adir)) if os.path.isfile(os.path.join(_adir, "agent.json")) else "python"


def venv_dir():
    """语言目录 venv（源码形态随仓库；服务部署形态宿主负责预置）。"""
    return os.path.join(LANG_DIR, "venv")


def venv_python():
    """venv 解释器路径（存在才返回）：Windows venv/Scripts/python.exe，其余 venv/bin/python。"""
    cand = (
        os.path.join(venv_dir(), "Scripts", "python.exe")
        if os.name == "nt"
        else os.path.join(venv_dir(), "bin", "python")
    )
    return cand if os.path.isfile(cand) else None


def requirements_path():
    """依赖清单：优先语言目录 requirements.txt（与 venv 同居），历史位置 {GEBAI_HOME}/requirements.txt
    存在且语言目录无时兼容使用（freeze 统一写回语言目录）。"""
    lang = os.path.join(LANG_DIR, "requirements.txt")
    if os.path.isfile(lang):
        return lang
    legacy = os.path.join(gebai_home(), "requirements.txt")
    if os.path.isfile(legacy):
        return legacy
    return lang


def split_last_expression(code):
    """REPL 语义：AST 末节点为独立表达式时切出（语句段 exec + 表达式段 eval）；
    语法错误返回 (code, None)——整段按语句执行，错误如实上报。"""
    try:
        tree = ast.parse(code)
    except SyntaxError:
        return code, None
    if not tree.body:
        return code, None
    last = tree.body[-1]
    if not isinstance(last, ast.Expr):
        return code, None
    lines = code.splitlines(True)
    stmt_src = "".join(lines[: last.lineno - 1])
    expr_src = "".join(lines[last.lineno - 1 : last.end_lineno]).strip()
    return stmt_src, expr_src


def ns_for(session):
    key = str(session or "default")
    if key not in VENVS:
        VENVS[key] = {"__name__": "python_repl", "__builtins__": __builtins__}
    return VENVS[key], key


# ---------------- 工具实现 ----------------


def tool_run(args):
    """python_run：常驻命名空间执行（REPL 语义）。"""
    code = str(args.get("code") or "")
    # 会话隔离缺省取请求级 ctx.sessionId（协议 v2）：边车进程单例跨会话共享，
    # 不同会话的 REPL 命名空间自然分桶；显式 session 参数仍可细粒度隔离
    session = args.get("session") or ctx_session_id()
    ns, key = ns_for(session)
    out, err = io.StringIO(), io.StringIO()
    t0 = time.monotonic()
    result_repr = None
    error = None
    stmt_src, expr_src = split_last_expression(code)
    try:
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            if stmt_src.strip():
                exec(compile(stmt_src, "<python_run>", "exec"), ns)
            if expr_src is not None:
                result_repr = repr(eval(compile(expr_src, "<python_run>", "eval"), ns))
    except BaseException:
        error = traceback.format_exc(limit=16)
    elapsed = int((time.monotonic() - t0) * 1000)
    parts = []
    if result_repr is not None:
        parts.append(result_repr)
    so, se = out.getvalue(), err.getvalue()
    if error is not None:
        parts.append(se + error if not se.endswith("\n") and se else error)
        output = "".join(parts)
        return {
            "output": output,
            "data": {
                "stdout": so[-100000:],
                "stderr": (se + error)[-100000:],
                "elapsedMs": elapsed,
                "session": key,
                "ok": False,
            },
        }
    output = (so + (("\n" if so and not so.endswith("\n") else "") + result_repr if result_repr is not None else "")) or ("（执行成功，无输出）")
    return {
        "output": output.rstrip("\n") or "（执行成功，无输出）",
        "data": {"stdout": so[-100000:], "stderr": se[-100000:], "elapsedMs": elapsed, "session": key, "ok": True},
    }


def run_pip(python_exe, pip_args, timeout_s):
    """以 venv 解释器跑 pip（子进程，输出合并捕获）。"""
    cmd = [python_exe, "-X", "utf8", "-m", "pip", *pip_args]
    env = dict(os.environ)
    env.setdefault("PYTHONUTF8", "1")
    try:
        cp = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout_s, env=env)
        return cp.returncode, (cp.stdout or "") + (("\n" if cp.stdout and not cp.stdout.endswith("\n") else "") + cp.stderr if cp.stderr else "")
    except subprocess.TimeoutExpired:
        return 124, f"pip 超时（{timeout_s}s）: {' '.join(pip_args)}"
    except OSError as exc:
        return 1, f"pip 启动失败: {exc}"


def tool_pip(args):
    """python_pip：install（无 venv 自动创建）/ freeze（写回 requirements.txt）。"""
    action = str(args.get("action") or "status")
    home = gebai_home()
    vdir = venv_dir()
    req = os.path.join(home, "requirements.txt")
    vexe = venv_python()

    if action == "status":
        lines = [f"venv: {vdir}（{'已创建' if os.path.isdir(vdir) else '未创建'}）"]
        lines.append(f"requirements: {req}（{'存在' if os.path.isfile(req) else '不存在'}）")
        if vexe:
            code, out = run_pip(vexe, ["list", "--format=freeze"], 120)
            if code == 0:
                pkgs = [l for l in out.splitlines() if l.strip()]
                lines.append(f"已装包（{len(pkgs)}）:")
                lines.extend("  " + p for p in pkgs[:80])
                if len(pkgs) > 80:
                    lines.append(f"  …（共 {len(pkgs)} 个）")
            else:
                lines.append(f"包列表获取失败:\n{out[-2000:]}")
        else:
            lines.append("venv 未创建——python_pip action=install 自动创建，或先手动: python -m venv " + vdir)
        return {"output": "\n".join(lines), "data": {"venvDir": vdir, "created": os.path.isdir(vdir)}}

    if action == "install":
        packages = args.get("packages")
        timeout_s = int(args.get("timeout") or 540)
        # 1) venv 不存在则创建（用户目录下，宿主已审批该工具调用）
        if not vexe:
            base = sys.executable
            cp = subprocess.run([base, "-m", "venv", vdir], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=300)
            vexe = venv_python()
            if cp.returncode != 0 or not vexe:
                return {"output": f"venv 创建失败（exit {cp.returncode}）:\n{(cp.stdout or '') + (cp.stderr or '')}", "data": {"ok": False}}
        # 2) 组装 pip 参数：packages 列表/字符串，或 -r requirements.txt（缺省且未指定包时）
        if packages:
            if isinstance(packages, str):
                packages = [p.strip() for p in packages.split() if p.strip()]
        elif os.path.isfile(req):
            packages = ["-r", req]
        else:
            return {"output": f"未指定 packages 且 {req} 不存在——传 packages 或先创建 requirements.txt", "data": {"ok": False}}
        code, out = run_pip(vexe, ["install", "--disable-pip-version-check", *packages], timeout_s)
        tail = out[-6000:]
        note = "\n注意：边车将以 venv 解释器重启以加载新依赖（下次调用自动生效）。" if code == 0 else ""
        # 3) 安装成功后本进程自杀重启：宿主崩溃自愈会拉起新进程——由 venv 优先解析逻辑接管
        return {"output": f"pip install {' '.join(packages)}（exit {code}）\n{tail}{note}", "data": {"ok": code == 0, "exitCode": code}, "_restart": code == 0}

    if action == "freeze":
        if not vexe:
            return {"output": "venv 未创建，无依赖可快照", "data": {"ok": False}}
        code, out = run_pip(vexe, ["freeze"], 120)
        if code != 0:
            return {"output": f"pip freeze 失败（exit {code}）:\n{out[-2000:]}", "data": {"ok": False}}
        req = os.path.join(LANG_DIR, "requirements.txt")  # 统一写回语言目录
        with open(req, "w", encoding="utf-8", newline="\n") as f:
            f.write(out)
        pkgs = [l for l in out.splitlines() if l.strip()]
        return {"output": f"已快照 {len(pkgs)} 个包到 {req}", "data": {"ok": True, "count": len(pkgs)}}

    return {"output": f"未知 action: {action}（支持 status/install/freeze）", "data": {"ok": False}}


def tool_status(_args):
    """python_status：边车/解释器/venv 状态。"""
    vexe = venv_python()
    info = {
        "driverPython": sys.version.split()[0],
        "executable": sys.executable,
        "langDir": LANG_DIR,
        "agentDir": agent_dir(),
        "venvDir": venv_dir(),
        "venvActive": bool(vexe),
        "venvPython": vexe or "（未创建）",
        "sessions": sorted(VENVS.keys()),
        "platform": sys.platform,
    }
    lines = [f"{k}: {v}" for k, v in info.items()]
    return {"output": "\n".join(lines), "data": info}


# ---------------- 协议层 ----------------

TOOLS = [
    {
        "name": "run",
        "description": "在常驻 Python 边车进程的命名空间执行代码（REPL 语义）：同一 session 的全局状态跨调用保持——import/加载一次、多次复用（重 AI 库的秒级导入成本只付一次）。末行独立表达式自动求值并以 repr 回显；stdout/stderr 均捕获返回。安装依赖用 python_pip。timeout 秒（默认 300）超时杀进程重启（命名空间丢失）。",
        "parameters": {
            "type": "object",
            "properties": {
                "code": {"type": "string", "description": "Python 源码（多行；末行独立表达式= REPL 求值回显）"},
                "session": {"type": "string", "description": "命名空间键（缺省按会话隔离：同会话共享状态、跨会话互不可见；同会话内隔离实验可另起 session 名）"},
                "timeout": {"type": "number", "description": "超时秒（默认 300；AI 推理等长任务可调大）"},
            },
            "required": ["code"],
        },
    },
    {
        "name": "pip",
        "description": "Python 依赖管理（venv 与 requirements.txt 位于 Python 语言目录 native-agents/python/）。action=install 安装（packages 指定包名列表/空格分隔字符串；缺省则 -r requirements.txt；venv 不存在自动创建；装完边车自动重启加载新依赖）action=freeze 快照当前依赖写回 requirements.txt；action=status 查看 venv/已装包。",
        "parameters": {
            "type": "object",
            "properties": {
                "action": {"type": "string", "enum": ["install", "freeze", "status"], "description": "install/freeze/status"},
                "packages": {"description": "install 时可选：包名列表或空格分隔字符串（如 \"numpy pandas\"、\"torch --index-url https://download.pytorch.org/whl/cpu\"）；缺省用 requirements.txt"},
                "timeout": {"type": "number", "description": "install 超时秒（默认 540；大包如 torch 需数分钟）"},
            },
            "required": ["action"],
        },
    },
    {
        "name": "status",
        "description": "查看 Python 边车状态：驱动进程解释器版本/可执行路径、语言目录/venv 位置与激活状态、活跃命名空间列表。",
        "parameters": {"type": "object", "properties": {}},
    },
]

TOOL_IMPLS = {"run": tool_run, "pip": tool_pip, "status": tool_status}


def load_project_tools():
    """加载子代理项目专属工具（{agent_dir}/tools.py）：导出 AGENT_NAME + TOOLS + TOOL_IMPLS
    时与基础工具合并（同名覆盖）——同语言框架派生多个子代理的扩展点；加载失败记 stderr
    并回退基础工具集（失败安全：不影响基础工具可用）。"""
    global AGENT_NAME, TOOLS, TOOL_IMPLS
    path = os.path.join(agent_dir(), "tools.py")
    if not os.path.isfile(path):
        return
    try:
        spec = importlib.util.spec_from_file_location("project_tools", path)
        mod = importlib.util.module_from_spec(spec)
        # 本驱动可能以 __main__ 运行（非 import 名 driver）：注册进 sys.modules，
        # 项目 tools.py 内 `import driver` 拿到的即同一实例（ctx 助手/常驻状态共享，
        # 而非重新加载一份副本导致 _CURRENT_CTX 永远为空）
        sys.modules.setdefault("driver", sys.modules[__name__])
        spec.loader.exec_module(mod)
    except BaseException:
        sys.stderr.write("[driver] 项目 tools.py 加载失败，回退基础工具集:\n" + traceback.format_exc(limit=8) + "\n")
        return
    if hasattr(mod, "AGENT_NAME"):
        AGENT_NAME = str(mod.AGENT_NAME)
    tools_extra = getattr(mod, "TOOLS", None) or []
    impls_extra = getattr(mod, "TOOL_IMPLS", None) or {}
    for t in tools_extra:
        name = t.get("name")
        if name and name in impls_extra:
            TOOLS = [x for x in TOOLS if x["name"] != name] + [t]
            TOOL_IMPLS[name] = impls_extra[name]


load_project_tools()


def handle(op, args):
    if op == "init":
        return {
            "name": AGENT_NAME,
            "protocol": PROTOCOL,
            "python": sys.version.split()[0],
            "executable": sys.executable,
            "venvActive": bool(venv_python()),
        }
    if op == "tools.list":
        return TOOLS
    if op == "tool.call":
        tool = str(args.get("tool") or "")
        impl = TOOL_IMPLS.get(tool)
        if not impl:
            return {"__error__": f"未知工具: {tool}"}
        try:
            set_current_ctx(args.get("ctx") or None)
            return impl(args.get("args") or {})
        except BaseException:
            return {"__error__": traceback.format_exc(limit=12)}
        finally:
            set_current_ctx(None)
    return {"__error__": f"未知操作: {op}"}


def main():
    sys.stdout.reconfigure(encoding="utf-8", newline="\n")
    sys.stdin.reconfigure(encoding="utf-8")
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        restart = False
        try:
            req = json.loads(line)
        except json.JSONDecodeError as exc:
            sys.stdout.write(json.dumps({"id": None, "ok": False, "error": f"请求解析失败: {exc}"}) + "\n")
            sys.stdout.flush()
            continue
        try:
            # 协议 v2：tool.call 的 tool/args/ctx 为请求体顶级字段（与 op 平级），组装后交 handle
            op = req.get("op")
            hargs = req.get("args") or {}
            if op == "tool.call":
                hargs = {"tool": req.get("tool"), "args": req.get("args") or {}, "ctx": req.get("ctx")}
            result = handle(op, hargs)
        except SystemExit as exc:
            os._exit(int(exc.code or 0))
        except BaseException as exc:  # 宿主兜底：请求级意外不打死宿主
            sys.stdout.write(json.dumps({"id": req.get("id"), "ok": False, "error": f"宿主异常: {exc}"}) + "\n")
            sys.stdout.flush()
            continue
        if isinstance(result, dict) and "__error__" in result:
            resp = {"id": req.get("id"), "ok": False, "error": result["__error__"]}
        else:
            restart = isinstance(result, dict) and result.pop("_restart", False)
            resp = {"id": req.get("id"), "ok": True, "result": result}
        sys.stdout.write(json.dumps(resp, ensure_ascii=False) + "\n")
        sys.stdout.flush()
        if restart:
            # pip 安装成功：主动退出让宿主自动重启（venv 优先解析在下次启动生效）
            os._exit(0)
    os._exit(0)  # stdin EOF：父进程已退出


if __name__ == "__main__":
    main()
