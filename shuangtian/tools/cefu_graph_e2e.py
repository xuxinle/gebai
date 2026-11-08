#!/usr/bin/env python3
"""册府知识图谱 e2e 核验（有头窗口 + 控制通道）。

## 它证明什么（以及为什么不能只靠单测）

`st/ui/graph_layout` 的布局/投影/拾取有单测，但**"窗口里到底画没画出来"**只能靠
真进程 + 截图：

| 断言 | 它防的具体缺陷 |
|---|---|
| 视口挂载成功 | 元素没进树（册府踩过：absolute 占位被布局成 0 尺寸而剔除） |
| 三维物体数 > 0 | 脚本装载失败（`load_script` 失败会**保持旧场景**，看起来"什么都没变"） |
| 标签实际画出 | 标签层被剔除/被盖住（"几何转了字不转"同源） |
| 相机距离合理 | 取景算错（DPI 被多算 → 画面只占中间一小块） |
| 截图里真的有东西 | 以上全绿但屏幕空白（视口剔除死锁的经典形态） |

## 用法

    python3 tools/cefu_graph_e2e.py [--exe <路径>] [--shots <目录>] [--keep]

退出码 0 = 全部断言通过。
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_EXE = ROOT / "build" / "dev" / "bin" / "cefu.exe"


# ── 控制通道最小客户端（与 tools/st_ctl.py 同一帧格式，不引第三方） ───────────


def _recv_exact(sock, length: int) -> bytes:
    payload = b""
    while len(payload) < length:
        chunk = sock.recv(length - len(payload))
        if not chunk:
            raise RuntimeError("连接被关闭")
        payload += chunk
    return payload


def _call(sock, request_id: int, method: str, params: dict) -> dict:
    body = json.dumps({"id": request_id, "method": method, "params": params}).encode()
    sock.sendall(struct.pack(">I", len(body)) + body)
    (length,) = struct.unpack(">I", _recv_exact(sock, 4))
    return json.loads(_recv_exact(sock, length))


class Control:
    def __init__(self, ctl_path: Path):
        import socket

        deadline = time.time() + 60
        data = None
        while time.time() < deadline:
            try:
                data = json.loads(ctl_path.read_text(encoding="utf-8"))
                break
            except Exception:  # noqa: BLE001 —— 文件还没写完
                time.sleep(0.2)
        if not data:
            raise RuntimeError(f"控制文件未就绪：{ctl_path}")
        self.sock = socket.create_connection(("127.0.0.1", data["port"]), timeout=120)
        # 单次请求的读超时：**重建三维后端**（GPU 设备预热 + swapchain）实测可到
        # 一分多钟（冷启动的驱动初始化），而 socket 默认超时会把它误判为挂死。
        self.sock.settimeout(300)
        self._next = 1
        hello = self.call("hello", {"token": data["token"]})
        # 控制通道的响应是 `{id, ok, result}` / `{id, ok:false, error}` —— `ok` 在**顶层**，
        # 不在 `result` 里（第一版写成 `result.ok`，于是每次握手都被误判为失败）。
        if not hello.get("ok", False):
            raise RuntimeError(f"hello 失败：{hello}")
        self.hello = hello.get("result", {})

    def call(self, method: str, params: dict) -> dict:
        request_id = self._next
        self._next += 1
        return _call(self.sock, request_id, method, params)

    def evaluate(self, code: str):
        """跑一段 JS（走 `script` 方法的 code 分支——桥函数挂在全局 `app` 下）。

        ## 返回值拆包（踩过）

        响应是 `{ok, result: {result: <桥函数返回值>, ops, memory}}`：**两层 result**——
        外层是控制通道的 `script` 方法，内层是桥函数自己的返回。只拆一层会让所有
        断言都变成 `None`（看上去像“功能全没实现”，实际是解包错了）。
        """
        response = self.call("script", {"code": code})
        if not response.get("ok", False):
            raise RuntimeError(f"script 失败：{response.get('error')}")
        payload = response.get("result")
        if isinstance(payload, dict) and "result" in payload:
            return payload["result"]
        return payload

    def close(self) -> None:
        try:
            self.sock.close()
        except Exception:  # noqa: BLE001
            pass


# ── 断言 ─────────────────────────────────────────────────────────────────────


class Checks:
    def __init__(self) -> None:
        self.failures: list[str] = []
        self.passed = 0

    def check(self, name: str, ok: bool, detail: str = "") -> bool:
        if ok:
            self.passed += 1
            print(f"  PASS {name}" + (f"  [{detail}]" if detail else ""))
        else:
            self.failures.append(f"{name}{('：' + detail) if detail else ''}")
            print(f"  FAIL {name}" + (f"  [{detail}]" if detail else ""))
        return ok

    def report(self) -> int:
        print()
        if self.failures:
            print(f"失败（{len(self.failures)}）：")
            for item in self.failures:
                print(f"  - {item}")
        print(f"{self.passed} 通过 · {len(self.failures)} 失败")
        return 1 if self.failures else 0


def png_unique_colors(path: Path, target_samples: int = 20000) -> int:
    """抽样统计 PNG 里有多少种颜色（不回引 PIL，只用 zlib）。

    ## 为何不拿文件大小当判据（实测教训）

    第一版用“> 30 KB”判“有内容”。而一张 **3533×1921 的纯白** PNG 也有 171 KB
    ——14 个字节宽、缩了几万倍也压不小，于是一个**什么都没画的空画布**
    能轻松“通过”内容检查。而真实图谱的抽样颜色数是 100+、空画布是 1。

    只支持本仓 `codec::png` 产出的形式（8 位、非隔行、RGB/RGBA）。
    """
    import struct
    import zlib

    if not path.is_file():
        return 0
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return 0
    pos = 8
    width = height = 0
    color_type = 0
    idat = bytearray()
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        chunk_type = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if chunk_type == b"IHDR":
            width, height, _depth, color_type = struct.unpack(">IIBB", body[:10])
        elif chunk_type == b"IDAT":
            idat += body
        elif chunk_type == b"IEND":
            break
    channels = {0: 1, 2: 3, 4: 2, 6: 4}.get(color_type)
    if channels is None or width == 0 or height == 0:
        return 0
    try:
        raw = zlib.decompress(bytes(idat))
    except Exception:  # noqa: BLE001 —— 解不开就当无内容（如实保守）
        return 0
    stride = width * channels
    prev = bytearray(stride)
    offset = 0
    colors: set[bytes] = set()
    step = max(1, (width * height) // max(target_samples, 1))
    for y in range(height):
        filter_type = raw[offset]
        offset += 1
        line = bytearray(raw[offset:offset + stride])
        offset += stride
        if filter_type == 1:
            for x in range(channels, stride):
                line[x] = (line[x] + line[x - channels]) & 0xFF
        elif filter_type == 2:
            for x in range(stride):
                line[x] = (line[x] + prev[x]) & 0xFF
        elif filter_type == 3:
            for x in range(stride):
                left = line[x - channels] if x >= channels else 0
                line[x] = (line[x] + ((left + prev[x]) >> 1)) & 0xFF
        elif filter_type == 4:
            for x in range(stride):
                a = line[x - channels] if x >= channels else 0
                b = prev[x]
                c = prev[x - channels] if x >= channels else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pred) & 0xFF
        if y % max(1, step // width + 1) == 0:
            for x in range(0, width, max(1, step)):
                off = x * channels
                colors.add(bytes(line[off:off + min(channels, 3)]))
        prev = line
    return len(colors)


def main() -> int:
    parser = argparse.ArgumentParser(description="册府知识图谱 e2e 核验")
    parser.add_argument("--exe", default=str(DEFAULT_EXE))
    parser.add_argument("--shots", default="", help="截图目录（缺省临时目录）")
    parser.add_argument("--keep", action="store_true", help="保留临时数据目录（排查用）")
    parser.add_argument("--seed-entries", type=int, default=0,
                        help="先用 put 灌入 N 条示例词条（0 = 用内置种子）")
    args = parser.parse_args()

    exe = Path(args.exe)
    if not exe.is_file():
        print(f"[错误] 找不到可执行文件：{exe}\n  先跑 st build cefu", file=sys.stderr)
        return 2

    work = Path(tempfile.mkdtemp(prefix="cefu-graph-e2e-"))
    shots = Path(args.shots) if args.shots else work / "shots"
    shots.mkdir(parents=True, exist_ok=True)
    ctl_path = work / "ctl.json"
    db_path = work / "cefu.db"

    print(f"工作目录：{work}")
    print(f"截图目录：{shots}")

    proc = subprocess.Popen(  # noqa: S603 —— 被测程序是仓内构建产物
        [str(exe), "--db", str(db_path), "--control-port", "0",
         "--control-file", str(ctl_path), "--shots", str(shots)],
        cwd=str(ROOT / "apps" / "cefu"),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )

    checks = Checks()
    control = None
    try:
        control = Control(ctl_path)
        checks.check("控制通道握手", True)

        # 灌数据：内置 seed.md 只在空库首次启动时导入。这里显式 put，
        # 免得"种子有没有生效"变成核验的隐含前提。
        #
        # ⚠ 两个接口契约（第一版都踩了，后果都是**进程直接 abort**）：
        # ① `scope` 是 **JSON 字符串**（不是对象）——它进库时原样存成文本；
        # ② 必须给 `source_ref`（没出处的知识不该进库）。
        entries = [
            ("写入延迟升高", "观测", "p99 从 20ms 涨到 400ms", ["阈值=5x"], ["慢查询定位"]),
            ("写入队列积压", "观测", "queue_depth 持续 > 1万", ["端口=8428"], []),
            ("TSDB 端口冲突", "部署", "进程起不来，端口被占", ["端口=8428", "路径=/etc/tsdb.conf"], []),
            ("配额调整流程", "流程", "找运维张三批", ["阈值=5x"], []),
            ("日志轮转策略", "运维", "logrotate 每天切", ["路径=/var/log/app"], []),
            ("慢查询定位", "观测", "先开 EXPLAIN ANALYZE", [], ["写入延迟升高"]),
        ]
        put_ok = 0
        for title, category, symptom, facts, links in entries:
            payload = json.dumps({"title": title, "category": category,
                                  "symptom": symptom, "facts": facts, "links": links,
                                  "scope": json.dumps({"kind": "global"}),
                                  "source_kind": "person",
                                  "source_ref": "tools/cefu_graph_e2e.py"},
                                 ensure_ascii=False)
            result = control.evaluate(f"app.cefu.put({payload})")
            if isinstance(result, dict) and result.get("id"):
                put_ok += 1
            else:
                print(f"  [warn] put 失败：{title} -> {result}")
        checks.check("数据灌注", put_ok == len(entries), f"{put_ok}/{len(entries)} 条")

        # 切到图谱模式（C++ 侧挂载 + 建图 + 取景）
        info = control.evaluate("app.cefu.graph_toggle(true)")
        checks.check("graph_toggle 返回统计", isinstance(info, dict), str(info)[:160])
        if isinstance(info, dict):
            checks.check("建图成功", bool(info.get("ok")), f"error={info.get('error')!r}")
            nodes = int(info.get("nodes") or 0)
            edges = int(info.get("edges") or 0)
            checks.check("图非空", nodes > 0 and edges > 0, f"{nodes} 节点 / {edges} 边")
            # 共享硬事实（端口=8428 出现在两条词条里）→ 至少一条 shared_fact 边
            checks.check("共享硬事实建边", edges >= len(entries), f"{edges} 边")

        # 等首帧 + 三维后端就绪。
        #
        # ⚠ 为何要等（实测 2026-11-08）：GPU 腿是**后台异步升级**的——先以软件画布
        #   跑，设备预热完再切 GPU（新建 swapchain）。`capture` 走主画布，若在升级
        #   刚完成时抓，拿到的是**新缓冲的一帧空内容**：整张图只有一种颜色
        #   （实测采样 2 万个像素 → 唯一颜色数 = 1），而文件仍有 171KB。
        #   对照：同一进程多等 2 秒再抓 → 47~68 种颜色，图谱正常。
        #
        # 这里的等法是**轮询直到有内容**（而不是拍一个魔法秒数）：升级耗时依设备而异，
        # 拍少了会假红、拍多了每轮都白等。上限 20 秒（超时就如实报“一直是空白”）。
        shot_path = (shots / "cefu-graph.png").resolve()
        settled = False
        colors = 0
        for _attempt in range(8):
            time.sleep(1.0)
            resp = control.call("capture", {"encode": "file", "path": str(shot_path)})
            if not resp.get("ok", False):
                print(f"  [warn] capture 失败：{str(resp.get('error'))[:160]}")
                continue
            colors = png_unique_colors(shot_path)
            if colors >= 8:
                settled = True
                break
        checks.check("画面已稳定出图（等 GPU 升级完成）", settled, f"{colors} 种颜色")

        debug = control.evaluate("app.cefu.graph_debug()")
        checks.check("graph_debug 可读", isinstance(debug, dict), str(debug)[:200])
        if isinstance(debug, dict):
            checks.check("视口已挂载", bool(debug.get("mounted")), str(debug.get("mounted")))
            objects = int(debug.get("objects") or 0)
            checks.check("三维场景有物体", objects > 0, f"objects={objects}")
            labels = int(debug.get("labels") or 0)
            checks.check("标签已实际绘制", labels > 0, f"labels={labels}")
            visible = int(debug.get("visible_nodes") or 0)
            checks.check("节点在视锥内", visible > 0, f"visible_nodes={visible}")
            checks.check("无装载错误", not debug.get("error"), f"error={debug.get('error')!r}")

        camera = control.evaluate("app.cefu.graph_camera()")
        if isinstance(camera, dict):
            distance = float(camera.get("distance") or 0.0)
            # 取景合理区间：册府布局 scale=170、包围半径约 170，fov 46° ⇒ 约 400~800。
            # 太近会穿进图里、太远图缩成一点（`frame_scene` 那个 DPI 坑给出过 600+ 的错误值，
            # 所以这里也守"不是被放大过"）。
            checks.check("相机距离合理", 200.0 < distance < 1600.0, f"distance={distance:.1f}")

        # 真截图：控制通道的 capture 方法
        #
        # ⚠ `capture` 的落盘路径有**白名单**（截图是「让应用进程写文件」的原语，
        #   不限路径 = 本地越权写）。白名单含应用自己的 shots 目录与系统临时目录——
        #   所以这里必须用 `--shots` 那个目录并 **resolve 成绝对路径**。
        #   白名单比较按**路径身份**（大小写不敏感 + 长名展开），所以短名/长名不会互误。
        capture = control.call("capture", {"encode": "file", "path": str(shot_path)})
        ok_capture = bool(capture.get("ok", False))
        checks.check("截图成功", ok_capture, str(capture.get("error", ""))[:160])
        if ok_capture:
            # 内容判据：**抽样颜色数**。空画布 = 1 种颜色（纯色），
            # 真图谱 = 40+（文字抗锯齿 + 多色连线 + 背景）。
            # 不给“文件大于 30KB”那种阈值——实测一张 3533×1921 的**纯白** PNG
            # 也有 171KB，那种判据会被空画面轻松“通过”。
            colors = png_unique_colors(shot_path)
            checks.check("截图里有内容（非纯色）", colors >= 8, f"{colors} 种颜色")

        # 反向验证：隐藏真的生效（防"隐藏没生效"这类静默失效）
        #
        # 断言对象是桥面回报的 `visible`，而不是 `visible_nodes`：后者由绘制路径重算，
        # 元素不可见后**不再绘制也就不再更新**，它会停在上一帧的值。
        # （第一版就是这么写的——永远不会绿，而且是把自己测错了当成代码错。）
        hidden = control.evaluate("app.cefu.graph_toggle(false)")
        checks.check("隐藏返回可见位为 false",
                     isinstance(hidden, dict) and hidden.get("visible") is False,
                     str(hidden)[:120])
        # 再打开一次：证明开关是双向的（只能关不能开 = 半残），也证明
        # **重建路径**能跑通（重建三维场景比首次装载更容易漏东西）。
        again = control.evaluate("app.cefu.graph_toggle(true)")
        checks.check("重新开启成功", isinstance(again, dict) and bool(again.get("ok")),
                     str(again)[:160])
        if isinstance(again, dict):
            # `graph_toggle` 的返回是**统计摘要**（节点/边/可见数），`labels` 在
            # `graph_debug` 里——两个桥函数的字段面不同，别拿一个的字段断言另一个。
            checks.check("重新开启后图仍有内容",
                         int(again.get("nodes") or 0) > 0
                         and int(again.get("visible") or 0) > 0,
                         f"nodes={again.get('nodes')} visible={again.get('visible')}")
        return checks.report()
    finally:
        if control is not None:
            control.close()
        try:
            proc.terminate()
            proc.wait(timeout=10)
        except Exception:  # noqa: BLE001
            proc.kill()
        output = ""
        try:
            output = proc.stdout.read() if proc.stdout else ""
        except Exception:  # noqa: BLE001
            pass
        if output and "错误" in output:
            print("--- 进程输出（含错误）---")
            print(output[-3000:])
        if not args.keep:
            shutil.rmtree(work, ignore_errors=True)
        else:
            print(f"保留工作目录：{work}")


if __name__ == "__main__":
    sys.exit(main())
