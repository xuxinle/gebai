"""docqa 子代理项目专属工具（经 driver.py 框架加载合并）：本地文档问答。

典型场景——本地知识库检索问答（RAG 雏形）：索引目录下的文本/markdown 文档，
BM25 词法检索 + 中文二元分词（纯标准库），索引持久化（{agent_dir}/index.json），
常驻进程零启动开销、重复查询零重索引成本。

演示 Python 基础框架的项目扩展点：本文件与 manifest 同目录，driver.py 启动时自动
加载——导出 AGENT_NAME 与 TOOLS/TOOL_IMPLS（与基础工具合并，同名覆盖）。
协议/REPL/pip 等基础能力由语言目录共享的 driver.py 提供。
"""

import difflib
import hashlib
import json
import math
import os
import re
import time
from datetime import datetime

AGENT_NAME = "docqa"

# ---------------- 分词 ----------------

_WORD_RE = re.compile(r"[A-Za-z0-9_]+")
_CJK_RE = re.compile(r"[\u4e00-\u9fff]")


def tokenize(text):
    """检索分词：ASCII 词元原样 + 中文按二元（bigram）——无词典依赖的稳健中文词法切分。"""
    tokens = []
    for w in _WORD_RE.findall(text):
        tokens.append(w.lower())
    # 中文串滑窗成二元
    buf = []
    for ch in text:
        if _CJK_RE.match(ch):
            buf.append(ch)
        else:
            if len(buf) >= 2:
                tokens.extend(buf[i] + buf[i + 1] for i in range(len(buf) - 1))
            elif buf:
                tokens.append(buf[0])
            buf = []
    if len(buf) >= 2:
        tokens.extend(buf[i] + buf[i + 1] for i in range(len(buf) - 1))
    elif buf:
        tokens.append(buf[0])
    return tokens


# ---------------- 索引 ----------------

INDEX_VERSION = 1
MAX_FILE_BYTES = 2 * 1024 * 1024  # 单文件上限 2MB（防巨型文件拖垮）
SUPPORTED_EXT = (".md", ".txt", ".log", ".csv")


def agent_dir():
    return os.environ.get("GEBAI_AGENT_DIR") or os.path.dirname(os.path.abspath(__file__))


def index_path_for(source_dir):
    """索引持久化位置：{GEBAI_HOME}/native-agent-data/docqa/（运行时数据归数据根，
    不进源码树——dist 复制不带、热加载签名不受素引更新扰动）。"""
    home = os.environ.get("GEBAI_HOME") or os.path.expanduser("~/.gebai")
    data_dir = os.path.join(home, "native-agent-data", "docqa")
    os.makedirs(data_dir, exist_ok=True)
    key = hashlib.sha1(source_dir.encode("utf-8", "replace")).hexdigest()[:16]
    return os.path.join(data_dir, f"index-{key}.json")


def iter_docs(root):
    """递归收集支持的文本文件（跳过 venv/node_modules/.git 等噪声目录）。"""
    skip = {"venv", "node_modules", ".git", "__pycache__", "dist", "build", "target", ".venv"}
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in skip and not d.startswith(".")]
        for name in filenames:
            if name.lower().endswith(SUPPORTED_EXT):
                yield os.path.join(dirpath, name)


def build_index(root, incremental=True):
    """构建 BM25 索引：{docs:[{path,mtime,size,terms:{token:tf}}], df, avgdl, source, builtAt}。
    增量模式：mtime/size 未变的文档复用旧词条（重命名/删除对账剔除）。"""
    old = load_index(root) if incremental else None
    old_docs = {d["path"]: d for d in (old or {}).get("docs", [])}
    docs = []
    errors = []
    for path in iter_docs(root):
        try:
            st = os.stat(path)
            prev = old_docs.get(path)
            if prev and prev.get("mtime") == int(st.st_mtime) and prev.get("size") == st.st_size:
                docs.append(prev)  # 未变：复用（跳过重分词）
                continue
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                text = f.read(MAX_FILE_BYTES)
            terms = {}
            for t in tokenize(text):
                terms[t] = terms.get(t, 0) + 1
            docs.append({"path": path, "mtime": int(st.st_mtime), "size": st.st_size, "terms": terms})
        except OSError as exc:
            errors.append(f"{path}: {exc}")
    df = {}
    for d in docs:
        for t in d["terms"]:
            df[t] = df.get(t, 0) + 1
    avgdl = (sum(sum(d["terms"].values()) for d in docs) / len(docs)) if docs else 0.0
    return {"version": INDEX_VERSION, "source": root, "docs": docs, "df": df, "avgdl": avgdl,
            "builtAt": datetime.now().isoformat(timespec="seconds"), "errors": errors}


def save_index(root, index):
    path = index_path_for(root)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(index, f, ensure_ascii=False)
    return path


def load_index(root):
    path = index_path_for(root)
    try:
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
        if data.get("version") == INDEX_VERSION:
            return data
    except (OSError, ValueError):
        pass
    return None


def bm25(query_tokens, doc, df, n_docs, avgdl, k1=1.5, b=0.75):
    score = 0.0
    dl = sum(doc["terms"].values())
    for qt in query_tokens:
        tf = doc["terms"].get(qt, 0)
        if tf == 0:
            continue
        n_df = df.get(qt, 0)
        idf = math.log(1 + (n_docs - n_df + 0.5) / (n_df + 0.5))
        score += idf * (tf * (k1 + 1)) / (tf + k1 * (1 - b + b * (dl / avgdl if avgdl else 1)))
    return score


def highlight(text, query_tokens, width=160):
    """命中文本片段截取：优先含查询词元的片段，命中词元高亮【】。"""
    lowered = text.lower()
    hits = []
    for qt in query_tokens:
        pos = lowered.find(qt)
        if pos >= 0:
            hits.append((pos, qt))
    if not hits:
        return text[:width].replace("\n", " ") + ("…" if len(text) > width else "")
    pos, qt = min(hits)
    start = max(0, pos - width // 3)
    frag = text[start : start + width].replace("\n", " ")
    for _, t in sorted(hits, key=lambda x: -len(x[1])):
        frag = frag.replace(t, f"【{t}】")
    return frag + ("…" if start + width < len(text) else "")


# ---------------- 工具实现 ----------------

def _tool_index(args):
    root = str(args.get("dir") or "").strip()
    if not root:
        return {"output": "缺少 dir 参数（要索引的文档目录）", "data": {"ok": False}}
    root = os.path.abspath(os.path.expanduser(root))
    if not os.path.isdir(root):
        return {"output": f"目录不存在: {root}", "data": {"ok": False, "dir": root}}
    incremental = bool(args.get("incremental", True))
    t0 = time.time()
    index = build_index(root, incremental)
    n_terms = len(index["df"])
    lines = [
        f"索引完成: {len(index['docs'])} 个文档（词表 {n_terms}，均长 {index['avgdl']:.0f}，耗时 {time.time() - t0:.2f}s）",
        f"源目录: {root}",
    ]
    if index["errors"]:
        lines.append(f"跳过 {len(index['errors'])} 个不可读文件（详见 data.errors）")
    data = {"ok": True, "docs": len(index["docs"]), "terms": n_terms, "source": root,
            "incremental": incremental, "errors": index["errors"][:10]}
    path = save_index(root, index)
    lines.append(f"索引落盘: {path}")
    return {"output": "\n".join(lines), "data": data}


def _tool_query(args):
    q = str(args.get("query") or "").strip()
    if not q:
        return {"output": "缺少 query 参数", "data": {"ok": False}}
    root = str(args.get("dir") or "").strip()
    if root:
        root = os.path.abspath(os.path.expanduser(root))
    index = load_index(root) if root else _latest_index()
    if not index:
        return {"output": "尚未建索引——先调 docqa_index 建索引", "data": {"ok": False}}
    top_k = int(args.get("top_k") or 5)
    qt = tokenize(q)
    scored = []
    for d in index["docs"]:
        s = bm25(qt, d, index["df"], len(index["docs"]), index["avgdl"])
        if s > 0:
            scored.append((s, d))
    scored.sort(key=lambda x: -x[0])
    results = []
    lines = [f"查询「{q}」命中 {len(scored)} 篇（索引 {len(index['docs'])} 篇，建自 {index['builtAt']}）:"]
    for rank, (s, d) in enumerate(scored[:top_k], 1):
        snippet = ""
        try:
            with open(d["path"], "r", encoding="utf-8", errors="replace") as f:
                text = f.read(65536)
            snippet = highlight(text, qt)
        except OSError:
            snippet = "(文件已不可读——建议重建索引)"
        results.append({"path": d["path"], "score": round(s, 3), "snippet": snippet})
        lines.append(f"{rank}. [{s:.2f}] {d['path']}")
        lines.append(f"   {snippet}")
    if not results:
        lines.append("（无命中文档）")
    return {"output": "\n".join(lines), "data": {"ok": True, "count": len(scored), "results": results,
            "indexedFrom": index["source"], "builtAt": index["builtAt"]}}


def _latest_index():
    """未指定 dir 时取最近更新的索引（多知识库场景免重复指定）。"""
    for p in _list_index_files():
        try:
            with open(p, "r", encoding="utf-8") as f:
                data = json.load(f)
            if data.get("version") == INDEX_VERSION:
                return data
        except (OSError, ValueError):
            continue
    return None


def _list_index_files():
    """数据根下全部索引文件（mtime 降序）。"""
    home = os.environ.get("GEBAI_HOME") or os.path.expanduser("~/.gebai")
    data_dir = os.path.join(home, "native-agent-data", "docqa")
    try:
        files = [os.path.join(data_dir, n) for n in os.listdir(data_dir)
                 if n.startswith("index-") and n.endswith(".json")]
    except OSError:
        return []
    return sorted(files, key=lambda p: os.stat(p).st_mtime if os.path.exists(p) else 0, reverse=True)


def _tool_status(args):
    indexes = []
    for p in _list_index_files():
        try:
            with open(p, "r", encoding="utf-8") as f:
                data = json.load(f)
            indexes.append({"file": os.path.basename(p), "source": data.get("source"),
                            "docs": len(data.get("docs", [])), "builtAt": data.get("builtAt")})
        except (OSError, ValueError):
            indexes.append({"file": os.path.basename(p), "broken": True})
    lines = ["docqa 索引状态:"]
    if not indexes:
        lines.append("（无索引——先调 docqa_index）")
    for it in indexes:
        if it.get("broken"):
            lines.append(f"  {it['file']}: 损坏")
        else:
            lines.append(f"  {it['source']}: {it['docs']} 篇（建自 {it['builtAt']}，{it['file']}）")
    import sys
    lines.append(f"解释器: {sys.executable}")
    return {"output": "\n".join(lines),
            "data": {"ok": True, "indexes": indexes, "executable": sys.executable}}


_TOOLS = [
    {
        "name": "index",
        "description": "为文档目录建 BM25 索引（增量：mtime/size 未变的文档复用旧词条）。支持 md/txt/log/csv，"
        "中文二元分词 + 英文词元，单文件上限 2MB。索引持久化落盘，进程常驻重查零成本。",
        "parameters": {
            "type": "object",
            "properties": {
                "dir": {"type": "string", "description": "要索引的文档目录（绝对或 ~ 路径）"},
                "incremental": {"type": "boolean", "description": "增量索引（默认 true：未变文档复用）"},
            },
            "required": ["dir"],
        },
    },
    {
        "name": "query",
        "description": "知识库检索问答：对已建索引的文档 BM25 检索，返回 top_k 命中文档与含【高亮】词元的"
        "片段——RAG 检索阶段即插即用。缺省 dir 时用最近更新的索引。",
        "parameters": {
            "type": "object",
            "properties": {
                "query": {"type": "string", "description": "查询文本（自然语言或关键词，中英混合）"},
                "dir": {"type": "string", "description": "知识库目录（缺省用最近索引）"},
                "top_k": {"type": "number", "description": "返回篇数（默认 5）"},
            },
            "required": ["query"],
        },
    },
    {
        "name": "status",
        "description": "查看索引状态：全部已建索引（源目录/文档数/建立时间）与解释器信息。",
        "parameters": {"type": "object", "properties": {}},
    },
]

TOOLS = _TOOLS
TOOL_IMPLS = {"index": _tool_index, "query": _tool_query, "status": _tool_status}
