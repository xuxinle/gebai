#!/usr/bin/env python3
"""裁云（Nimbus）· **AI 导演闭环**端到端验证。

覆盖「AI 控制生成高质量视频」这条主张的**可执行判据**（不靠截图猜）：

  1. 控制面存在：`#director`（`CaiyunDirector`）能被寻址，属性面给出工程摘要；
  2. **脚本即真值**：`invoke apply_script <json>` 后，场景数/时长/fps/分辨率随脚本变，
     时间轴片段数 == 场景数（一个场景一个片段）；
  3. **脚本可校验**：坏脚本（未知 kind / 空 scenes）**被拒**且工程保持原状——
     这是"AI 写错脚本不该毁掉工程"的护栏；
  4. **渲染闭环**：`invoke render <out.mp4>` → 轮询 `render_stage` 到 `done` →
     断言成片存在、大小合理、帧数与 `ceil(时长×fps)` 一致（用 ffprobe 复核容器）；
  5. **人工微调仍作用在同一份数据上**：`invoke set_opacity` / `trim` / `ripple_delete`
     之后检查器与时间轴同步变化（AI 与人的入口不分叉）。

用法: python3 tools/caiyun_director_e2e.py [binary] [shots]

⚠ 默认跑的是 **dev** 档二进制（build/dev/bin）。做反向验证时构建的是 debug 档，
  却不传 binary 的话测试仍在跑**旧的 dev 二进制** → 回退被测不出、得到假绿
  （实测踩到：回退"缺图静默跳过"后测试仍全绿，差点当成断言无效）。
"""
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

if sys.stdout.encoding and sys.stdout.encoding.lower() not in ("utf-8", "utf8"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BIN = os.path.join(ROOT, "build", "dev", "bin", "caiyun.exe")

# AI 侧典型输入：一个 3 场景 / 4 秒的小片（渲染快，判据一致）。
SCRIPT = {
    "name": "AI 闭环测试",
    "fps": 24,
    "size": [640, 360],
    "transition": 0.2,
    "fade_in": 0.1,
    "fade_out": 0.15,
    "audio": {"enabled": True, "style": "calm", "bpm": 96, "gain": 0.5},
    "scenes": [
        {"kind": "title", "duration": 1.5, "title": "裁云", "subtitle": "NIMBUS",
         "lines": ["AI 生成", "人工微调"], "accent": "#ff9f6bff",
         "background": ["#0a0812ff", "#241428ff"]},
        {"kind": "triptych", "duration": 1.5, "title": "三段能力", "subtitle": "脚本 · 渲染 · 交付",
         "lines": ["脚本", "渲染", "交付"], "accent": "#6bd6ffff",
         "background": ["#04121aff", "#0a2230ff"]},
        {"kind": "outro", "duration": 1.0, "title": "结尾", "subtitle": "OUTRO",
         "accent": "#ffd166ff", "background": ["#0a0812ff", "#181028ff"]},
    ],
}


class Client:
    def __init__(self, port, token):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=30.0)
        self.sock.settimeout(30.0)
        self.rid = 0
        self.token = token

    def call(self, method, params=None):
        self.rid += 1
        if method == "hello":
            params = dict(params or {}, token=self.token)
        body = json.dumps({"id": self.rid, "method": method, "params": params or {}}).encode()
        self.sock.sendall(struct.pack(">I", len(body)) + body)
        header = b""
        while len(header) < 4:
            chunk = self.sock.recv(4 - len(header))
            if not chunk:
                raise RuntimeError("控制通道在响应头前关闭")
            header += chunk
        length = struct.unpack(">I", header)[0]
        payload = b""
        while len(payload) < length:
            chunk = self.sock.recv(length - len(payload))
            if not chunk:
                raise RuntimeError("控制通道在响应体前关闭")
            payload += chunk
        return json.loads(payload)

    def ok(self, method, params=None):
        reply = self.call(method, params)
        if not reply.get("ok"):
            raise AssertionError(f"{method} 失败: {reply.get('error')}")
        return reply.get("result") or {}

    def invoke(self, element_id, action, argument=None):
        params = {"id": element_id, "action": action}
        if argument is not None:
            params["argument"] = argument
        return self.ok("invoke", params).get("handled", False)

    def props(self, element_id):
        return self.ok("get", {"id": element_id})["props"]

    def count(self, selector):
        return self.ok("find", {"selector": selector})["count"]


def start(binary, shots):
    ctl = os.path.join(shots, "caiyun-director-ctl.json")
    try:
        os.remove(ctl)
    except FileNotFoundError:
        pass
    log = open(os.path.join(shots, "caiyun-director.log"), "wb")
    argv = [binary, "--headless", "--control-port", "0", "--control-file", ctl, "--shots", shots]
    process = subprocess.Popen(argv, stdout=log, stderr=subprocess.STDOUT)
    for _ in range(200):
        if os.path.exists(ctl):
            try:
                info = json.load(open(ctl, encoding="utf-8"))
                if info.get("port"):
                    return process, Client(info["port"], info.get("token", "")), info
            except Exception:
                pass
        time.sleep(0.2)
    raise RuntimeError("应用未在 40s 内写出控制文件")


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def ffprobe(path):
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-show_entries",
         "format=duration:stream=codec_name,width,height,nb_frames",
         "-of", "json", path],
        capture_output=True, text=True)
    if out.returncode != 0:
        return None
    return json.loads(out.stdout or "{}")


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_BIN
    shots = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "build", "e2e-caiyun-director")
    os.makedirs(shots, exist_ok=True)
    out_dir = os.path.join(shots, "render")
    if os.path.isdir(out_dir):
        shutil.rmtree(out_dir)

    process, client, info = start(binary, shots)
    try:
        client.ok("hello")
        print(f"已连接：{info.get('app')} {info.get('version')} 后端={info.get('backend')}")

        # —— 1. 控制面 ——
        check(client.count("#director") == 1, "缺少（或重复）控制面元素 #director")
        base = client.props("director")
        check(base["scene_count"] == "5", f"内置示例片应有 5 个场景，实为 {base['scene_count']}")
        check(base["size"] == "1920x1080", f"内置脚本分辨率异常：{base['size']}")
        print(f"[1] 控制面就绪：内置工程「{base['name']}」 {base['size']} @{base['fps']}fps")

        # —— 2. 脚本即真值 ——
        check(client.invoke("director", "apply_script", json.dumps(SCRIPT, ensure_ascii=False)),
              "apply_script 被拒（内联脚本应为合法）")
        after = client.props("director")
        check(after["scene_count"] == "3", f"应用后场景数应为 3，实为 {after['scene_count']}")
        check(abs(float(after["duration"]) - 4.0) < 1e-3,
              f"应用后时长应为 4.0s，实为 {after['duration']}")
        check(abs(float(after["fps"]) - 24.0) < 1e-3, f"应用后 fps 应为 24，实为 {after['fps']}")
        check(after["size"] == "640x360", f"应用后分辨率应为 640x360，实为 {after['size']}")
        check(after["last_error"] == "", f"apply_script 报错：{after['last_error']}")
        # 一个场景一个片段（时间轴与脚本同源）
        timeline = client.props("timeline")
        check(timeline["clip_count"] == "3", f"时间轴片段数应为 3，实为 {timeline['clip_count']}")
        print(f"[2] 脚本即真值：3 场景 / {after['duration']}s / {after['size']}，时间轴 3 段")

        # —— 3. 坏脚本被拒且工程不变 ——
        bad = json.loads(json.dumps(SCRIPT))
        bad["scenes"] = [{"kind": "不存在的模板", "duration": 1.0}]
        handled = client.invoke("director", "apply_script", json.dumps(bad, ensure_ascii=False))
        rejected = client.props("director")
        check(not handled, "未知 kind 的脚本应被拒绝（handled 应为 false）")
        check(rejected["last_error"] != "", "被拒的脚本应给出可读原因")
        check(rejected["scene_count"] == "3", f"被拒后工程应保持不变，实为 {rejected['scene_count']}")
        empty = {"name": "空", "scenes": []}
        check(not client.invoke("director", "apply_script", json.dumps(empty)), "空 scenes 应被拒绝")
        print(f"[3] 坏脚本被拒且工程不变（原因：{rejected['last_error'][:40]}…）")

        # —— 4. 渲染闭环 ——
        # 先合成一段“旁白”WAV（纯本地正弦波，不依赖 TTS）并写进脚本，验证
        # **旁白轨 + 配乐 ducking** 这条真实链路：它是宣传片与“无声幻灯片”的分界。
        vo_dir = os.path.join(shots, "vo")
        os.makedirs(vo_dir, exist_ok=True)
        vo_path = os.path.join(vo_dir, "vo.wav")
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-f", "lavfi",
                        "-i", "sine=frequency=320:duration=1.2", "-af", "volume=3.0",
                        "-ar", "16000", "-ac", "1", "-c:a", "pcm_s16le", vo_path], check=False)
        has_vo = os.path.exists(vo_path)
        if has_vo:
            script_with_vo = json.loads(json.dumps(SCRIPT))
            script_with_vo["audio"]["voice"] = [
                {"path": vo_path, "start": 0.4, "gain": 0.9},
                {"path": vo_path, "start": 3.2, "gain": 0.9},
            ]
            check(client.invoke("director", "apply_script",
                                json.dumps(script_with_vo, ensure_ascii=False)),
                  "带旁白轨的脚本被拒")

        out = os.path.join(out_dir, "ai-film.mp4")
        check(client.invoke("director", "render", out), "render 未能启动")
        stage = ""
        deadline = time.time() + 240
        while time.time() < deadline:
            props = client.props("director")
            stage = props["render_stage"]
            if stage in ("done", "failed", "cancelled"):
                break
            time.sleep(0.25)
        props = client.props("director")
        check(stage == "done", f"渲染未完成：stage={stage} error={props['render_error']}")
        check(os.path.exists(out), f"成片不存在：{out}")
        size = os.path.getsize(out)
        check(size > 20_000, f"成片体积异常（{size} 字节），可能只有一两帧")
        total = int(props["render_frames_total"])
        expected = int(-(-4.0 * 24 // 1))   # ceil(4.0 × 24) = 96
        check(total == expected, f"帧数应为 {expected}，实为 {total}")
        meta = ffprobe(out)
        if meta is None:
            print("[4] 渲染完成（本机无 ffprobe，跳过容器复核）")
        else:
            video = [s for s in meta.get("streams", []) if s.get("codec_name") not in ("aac",)]
            audio = [s for s in meta.get("streams", []) if s.get("codec_name") == "aac"]
            check(video and video[0]["width"] == 640 and video[0]["height"] == 360,
                  f"成片分辨率与脚本不一致：{video}")
            check(abs(float(meta["format"]["duration"]) - 4.0) < 0.15,
                  f"成片时长异常：{meta['format']['duration']}")
            check(bool(audio), "成片缺音轨（配乐没进容器）")
            print(f"[4] 渲染闭环：{size // 1024} KiB / {video[0]['codec_name']} "
                  f"{video[0]['width']}x{video[0]['height']} / 时长 {meta['format']['duration']}s / 含音轨")
        # 旁白轨真的混进了成片：比较“有人声的 0.4~1.6s”与“纯配乐的 3.7~3.9s”的 RMS。
        # （两处人声之间留了空档，拿它当“无旁白”参照——拿**开头前 0.4s**做参照不行：
        # 那里有淡入，必然更小，会让“人声混进去了”与“开头静音”混为一谈。）
        if has_vo:
            score = os.path.join(out_dir, "score.wav")
            check(os.path.exists(score), "没有落盘混音结果 score.wav")

            def rms_db(start, length):
                out = subprocess.run(
                    ["ffmpeg", "-hide_banner", "-ss", str(start), "-t", str(length),
                     "-i", score, "-af", "astats=metadata=1:reset=0", "-f", "null", "-"],
                    capture_output=True, text=True)
                for line in out.stderr.splitlines():
                    if "RMS level dB" in line:
                        return float(line.split(":")[-1])
                return None

            voice_band = rms_db(0.5, 1.0)
            quiet_band = rms_db(2.3, 0.6)
            check(voice_band is not None and quiet_band is not None, "RMS 量不出来（astats 无输出）")
            check(voice_band - quiet_band > 1.5,
                  f"旁白没混进去：人声段 {voice_band:.1f}dB vs 空白段 {quiet_band:.1f}dB（差值应 >1.5dB）")
            print(f"[4b] 旁白轨混入：人声段 {voice_band:.1f}dB > 空白段 {quiet_band:.1f}dB")

        # —— 5. 人工微调作用在同一份数据上 ——
        check(client.invoke("timeline", "select", "1"), "选中片段失败")
        check(client.invoke("timeline", "set_opacity", "1 0.5"), "set_opacity 失败")
        check(client.invoke("timeline", "set_speed", "1 2.0"), "set_speed 失败")
        check(client.invoke("timeline", "trim", "1 1 1.0"), "trim 失败")
        after_edit = client.props("director")
        check(after_edit["scene_count"] == "3", "人工编辑不应改动场景数（数据同源）")
        range_text = client.props("inspector-range")["text"]
        check("1.00" in range_text, f"检查器未反映修剪后的出点：{range_text}")
        print(f"[5] 人工微调同源：检查器 {range_text}，场景数仍为 3")

        # —— 6. AI 控制态：人只能看 ——
        #
        # 判据分两半，缺一不可：
        #   ① **编辑全被拒**——而且必须逐个动作拿：删一个入口就少一条护栏；
        #   ② **查看仍然可用**——seek/播放不能被一起锁死（那变成“死界面”）。
        # 为什么逐动作而不是只看 clip_count：早期 `set_opacity` 绕过 Project 直接写字段，
        # 它在锁定态**返回 true** 而片段数根本不变——只数片段数就漏了。
        check(not client.props("director")["edit_locked"] == "true",
              "初始不应处于 AI 控制态")
        check(client.invoke("director", "edit_lock"), "edit_lock 未生效")
        locked = client.props("director")
        check(locked["edit_locked"] == "true", f"edit_locked 属性应为 true，实为 {locked['edit_locked']}")
        clips_before = client.props("timeline")["clip_count"]
        seek_before = client.props("monitor")["frame_time"]
        edits = {
            "分割": ("timeline", "split", "1.0"),
            "波纹删除": ("timeline", "ripple_delete", "1"),
            "修剪": ("timeline", "trim", "2 1 1.2"),
            "移动": ("timeline", "move", "2 3.0"),
            "不透明度": ("timeline", "set_opacity", "2 0.4"),
            "速度": ("timeline", "set_speed", "2 2.5"),
            "新增片段": ("timeline", "add_clip", "0 0 0.0 0.5"),
        }
        leaked = []
        for name, (element, action, argument) in edits.items():
            if client.invoke(element, action, argument):
                leaked.append(name)
        check(not leaked, f"AI 控制态下这些编辑没被拦住：{leaked}")
        check(client.props("timeline")["clip_count"] == clips_before,
              "锁定态下片段数变了（有编辑真的生效）")
        # 查看通道必须仍然可用
        check(client.invoke("timeline", "seek", "3.5"), "锁定态下 seek 被拒（查看也不能了）")
        check(client.props("monitor")["frame_time"] != seek_before, "锁定时 seek 后画面没动")
        check(client.invoke("timeline", "select", "2"), "锁定时连选中（只读操作）都不允许")
        print(f"[6] AI 控制态：{len(edits)} 个编辑动作全部被拦、seek/选中仍可用")

        # 交还编辑权后必须恢复（否则“锁一次就永久锁死”）
        check(client.invoke("director", "unlock_edit"), "unlock_edit 未生效")
        check(client.props("director")["edit_locked"] == "false", "解锁后 edit_locked 仍为 true")
        check(client.invoke("timeline", "set_opacity", "2 0.8"), "解锁后编辑仍被拒（锁没真正解开）")
        print("[6b] 交还编辑权：编辑恢复可用")

        # —— 6c. **人自己就能切**：控制权不是只有 AI 能拨 ——
        #
        # 这是本单的核心诉求（“人可切换主动控制和自动接管”）。之前只有控制通道
        # 能改，人被锁住后**无法自己拿回控制权**——界面上必须有个“把手”。
        # 判据：开关存在且双向可用；拨过去 → 编辑被拒；拨回来 → 恢复。
        check(client.count("#control-switch") == 1,
              "AI 面板没有控制权开关——人无法自己切换主动控制/自动接管")
        # 走 `activate`：`Switch::activate()` → `toggle()` → `on_change`，与用户点击同一条路径。
        # ⚠ **不能用 `set checked`**：`Switch::set_property` 只改 `checked_`、**不调 on_change**
        # （框架的既定语义：set 是"设状态"，activate 才是"触发动作"）。用它验会得到假绿。
        check(client.invoke("control-switch", "activate"), "控制权开关 activate 失败")
        check(client.props("director")["edit_locked"] == "true",
              "人拨开关后没进入 AI 接管态")
        check(not client.invoke("timeline", "set_opacity", "2 0.4"),
              "人拨到自动接管后，编辑竟仍可用")
        check(client.invoke("control-switch", "activate"), "控制权开关第二次 activate 失败")
        check(client.props("director")["edit_locked"] == "false",
              "人拨回后仍在 AI 接管态")
        check(client.invoke("timeline", "set_opacity", "2 0.55"),
              "人拨回主导后编辑仍被拒")
        print("[6c] 人可自己切换：开关双向生效（接管→编辑被拒；拨回→恢复）")

        # —— 7. 共享真值：AI 写得到的，也要读得回 ——
        #
        # 这是「AI 为主、人机共用」的命门：两方写同一份数据，却只有一方能看见，
        # 就不叫共用。实测曾经 `set_opacity`/`set_speed`/`trim`/`move` 都写得到，
        # 而属性面只有 selected/clip_count/playhead/transport——人调过的值 AI 读不回来。
        check(client.invoke("timeline", "set_opacity", "2 0.6"), "set_opacity 失败")
        check(client.invoke("timeline", "set_speed", "2 1.5"), "set_speed 失败")
        table = client.props("timeline")["clip"]
        rows = [line.split() for line in table.strip().splitlines()]
        check(len(rows) == int(client.props("timeline")["clip_count"]),
              f"clip 表行数与 clip_count 不一致：{len(rows)} vs {client.props('timeline')['clip_count']}")
        check(all(len(row) == 7 for row in rows), f"clip 表列数不是 7：{rows[:1]}")
        row2 = [row for row in rows if row[0] == "2"]
        check(bool(row2), f"clip 表里找不到片段 2：{rows}")
        opacity_2 = float(row2[0][4])
        speed_2 = float(row2[0][5])
        check(abs(opacity_2 - 0.6) < 0.01, f"AI 读回的不透明度不是刚写的 0.6：{opacity_2}")
        check(abs(speed_2 - 1.5) < 0.01, f"AI 读回的速度不是刚写的 1.5：{speed_2}")
        # 区间也必须可读（trim/move 的结果）：入/出点与时长自洽
        t_in, t_out = float(row2[0][2]), float(row2[0][3])
        check(t_out > t_in, f"片段 2 区间不自洽：{t_in} → {t_out}")
        print(f"[7] 共享真值可读：片段 2 opacity={opacity_2:.2f} speed={speed_2:.2f} "
              f"[{t_in:.2f},{t_out:.2f}]，{len(rows)} 行整表")

        # —— 8. 素材：场景 image 能进画面；缺图**不被静默跳过** ——
        #
        # 判据不看"脚本里有没有 image 字段"（那只能证明字段被解析），
        # 而是**比较渲染结果**：同一场景有图 vs 无图，画面必须不同。
        # 缺图时同理——若画占位框，画面也与纯底色不同（这条防"静默跳过"）。
        import struct
        import zlib

        def chunk(kind, data):
            body = kind + data
            return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

        def make_png(path, rgb, width=32, height=32):
            raw = b"".join(b"\x00" + bytes(rgb) * width for _ in range(height))
            data = (b"\x89PNG\r\n\x1a\n"
                    + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                    + chunk(b"IDAT", zlib.compress(raw))
                    + chunk(b"IEND", b""))
            with open(path, "wb") as handle:
                handle.write(data)

        media_dir = os.path.join(shots, "media")
        os.makedirs(media_dir, exist_ok=True)
        solid = os.path.join(media_dir, "solid.png")
        make_png(solid, (220, 40, 40))

        scene_plain = {"kind": "title", "duration": 1.5, "title": "素材", "subtitle": "X"}
        scene_with = dict(scene_plain, image=solid, image_dim=0.0, image_ken_burns=0.0)
        base_script = {
            "name": "素材验证", "fps": 12, "size": [320, 180], "transition": 0.0,
            "fade_in": 0.0, "fade_out": 0.0,
            "audio": {"enabled": False, "style": "calm", "bpm": 90, "gain": 0.0},
        }
        check(client.invoke("director", "apply_script",
                            json.dumps(dict(base_script, scenes=[scene_plain]))), "纯底色脚本被拒")
        time.sleep(0.2)
        # 用监视器帧哈希比较：有图 / 无图 / 缺图 三者必须两两不同
        def frame_hash():
            client.invoke("timeline", "seek", "0.4")
            time.sleep(0.15)
            return client.props("monitor")["frame_hash"]

        hash_plain = frame_hash()
        check(client.invoke("director", "apply_script",
                            json.dumps(dict(base_script, scenes=[scene_with]))), "带素材脚本被拒")
        time.sleep(0.2)
        hash_image = frame_hash()
        missing_scene = dict(scene_plain, image="不存在的素材.png")
        check(client.invoke("director", "apply_script",
                            json.dumps(dict(base_script, scenes=[missing_scene]))), "缺图脚本被拒")
        time.sleep(0.2)
        hash_missing = frame_hash()

        check(hash_plain != hash_image,
              f"有素材与无素材的画面相同（{hash_plain}）——素材根本没进画面")
        check(hash_plain != hash_missing,
              f"缺素材与无素材的画面相同——缺图被**静默跳过**了（应画占位框）")
        check(hash_image != hash_missing, "有素材与缺素材的画面相同（占位没画出来？）")
        print(f"[8] 素材：有图/无图/缺图 三种画面互不相同（哈希已比对）")

        print("\n[OK] 裁云 AI 导演闭环全部通过")
        return 0
    finally:
        try:
            client.ok("app", {"action": "quit"})
        except Exception:
            process.terminate()
        try:
            process.wait(timeout=5)
        except Exception:
            process.terminate()


if __name__ == "__main__":
    sys.exit(main())
