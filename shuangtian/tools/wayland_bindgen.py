#!/usr/bin/env python3
"""Wayland 协议绑定生成器（**自研，不依赖 `wayland-scanner` 与任何头文件**）。

## 为什么需要它

Wayland 的客户端代码本应由官方 `wayland-scanner` 从协议 XML 生成。而本机（以及任何
没装 `libwayland-dev` + `wayland-protocols` 的机器）**两者都没有**：没有 `wayland-scanner`、
没有 `<wayland-client.h>`、没有 `/usr/share/wayland-protocols`。

而霜天的硬约束是「系统能力一律运行时探测，缺失即回退或明确报错」——所以这里把
scanner 的**核心产物**自己生成出来：

  1. **消息签名串**：签名按官方规则逐字符编码
     （`i` int / `u` uint / `f` fixed / `s` string / `o` object / `n` new_id /
      `a` array / `h` fd；可空参数前缀 `?`；消息 `since > 1` 时**首字符是版本号**）。
  2. **`wl_interface` 描述**（名称 + 版本 + 请求/事件消息表 + 各参数的类型接口指针）。
  3. **消息 opcode**（请求按声明顺序 0,1,2…，事件同理——`wl_proxy_marshal_flags` 用它）。

生成物是**自包含的 C++**（不 include 任何 wayland 头），配合 `wl_proxy_marshal_flags`
这套**泛型调用**接口即可完成协议交互：客户端因此**不需要链接 libwayland-client**
（仍然 `dlopen`，与 X11 后端同一姿态），也不需要它的头文件。

## 与官方 scanner 的一致性（依据：`src/scanner.c` 的 `emit_messages`/`emit_types`）

- 签名串：`since > 1` 时前置版本号；`new_id` 无 `interface` 属性时补 `"su"`（接口名 + 版本）
  两个**隐式参数**（官方 `if (a->interface_name == NULL) printf("su")`）。
- `types[]` 数组：**所有接口共用一张扁平表**，每条消息记自己的起始下标。
  表前置 `null_run_length` 个 `NULL` —— 它等于"参数最长的**全 NULL 消息**"的参数个数
  （官方用它让 `type_index = 0` 的消息不必额外占槽位）。
- 每条消息在表里占的槽位数 = 参数个数（`new_id` 无 interface 时**多占两个**）。

## 用法

    python3 tools/wayland_bindgen.py --out src/shell/wayland_protocol.hpp

输入：`third_party/wayland/{wayland,xdg-shell,xdg-decoration-unstable-v1}.xml`
（官方协议 XML 的原样副本；来源与 SHA-256 见同目录 `SOURCES.md`）。
生成物**入库**（与 `third_party/` 的其它内联依赖同一口径：代码进仓库，构建不联网）。
"""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import xml.etree.ElementTree as ET

ROOT = pathlib.Path(__file__).resolve().parent.parent
XML_DIR = ROOT / "third_party/wayland"

# 官方 scanner 的类型码（`scanner.c` 的 `emit_messages`）
TYPE_CODES = {
    "int": "i",
    "uint": "u",
    "fixed": "f",
    "string": "s",
    "object": "o",
    "new_id": "n",
    "array": "a",
    "fd": "h",
}

# 允许 nullable 的类型（官方 `is_nullable_type`）
NULLABLE_TYPES = {"string", "object", "new_id", "array", "fd"}


class Arg:
    def __init__(self, node: ET.Element) -> None:
        self.type = node.get("type", "int")
        self.interface = node.get("interface")
        self.allow_null = node.get("allow-null") == "true"

    def code(self) -> str:
        text = TYPE_CODES.get(self.type, "i")
        if self.allow_null and self.type in NULLABLE_TYPES:
            text = "?" + text
        return text

    def slots(self) -> list[str]:
        """该参数在 types[] 里占用的槽位（空串 = NULL）。"""
        if self.type == "new_id" and not self.interface:
            # 隐式的 `interface`（string）与 `version`（uint）参数各占一个 NULL 槽
            return ["", "", ""]
        if self.type in ("object", "new_id"):
            return [self.interface or ""]
        return [""]


class Message:
    def __init__(self, node: ET.Element) -> None:
        self.name = node.get("name", "")
        self.since = int(node.get("since", "1"))
        self.args = [Arg(child) for child in node.findall("arg")]

    def signature(self) -> str:
        text = str(self.since) if self.since > 1 else ""
        parts: list[str] = []
        for arg in self.args:
            if arg.type == "new_id" and not arg.interface:
                parts.append("su")
            parts.append(arg.code())
        return text + "".join(parts)

    def is_all_null(self) -> bool:
        return all(not arg.interface for arg in self.args)

    def slots(self) -> list[str]:
        out: list[str] = []
        for arg in self.args:
            out += arg.slots()
        return out


class Interface:
    def __init__(self, node: ET.Element) -> None:
        self.name = node.get("name", "")
        self.version = int(node.get("version", "1"))
        self.requests = [Message(child) for child in node.findall("request")]
        self.events = [Message(child) for child in node.findall("event")]


def parse(path: pathlib.Path) -> list[Interface]:
    root = ET.parse(path).getroot()
    return [Interface(node) for node in root.findall("interface")]


def generate(interfaces: list[Interface]) -> str:
    # —— 第一遍：算全局 types 表的布局 ——
    #
    # 表结构（与官方 scanner 的 `emit_types` 等价）：
    #   [ null_run 个 NULL ] [ 接口1 请求消息逐个的槽位 … 接口1 事件消息逐个的槽位 ] [ 接口2 … ]
    # 每条消息记住自己的**起始下标**；全 NULL 消息官方给 0（指向 null_run 起点）。
    all_messages = [m for i in interfaces for m in i.requests + i.events]
    null_lengths = [len(m.args) for m in all_messages if m.is_all_null()]
    null_run = max(null_lengths) if null_lengths else 0

    flat: list[str] = [""] * null_run
    offsets: dict[tuple[str, str, str], int] = {}  # (接口, 方向, 消息名) → 起始下标
    for interface in interfaces:
        for direction, messages in (("request", interface.requests), ("event", interface.events)):
            for message in messages:
                if message.is_all_null():
                    offsets[(interface.name, direction, message.name)] = 0
                    continue
                offsets[(interface.name, direction, message.name)] = len(flat)
                flat += message.slots()

    # —— 第二遍：出代码 ——
    lines: list[str] = []
    lines.append("// ⚠ 本文件由 tools/wayland_bindgen.py 生成——**请勿手改**。")
    lines.append("//")
    lines.append("// 输入：third_party/wayland/{wayland,xdg-shell,xdg-decoration-unstable-v1}.xml")
    lines.append("//（官方协议 XML 的原样副本；来源与 SHA-256 见 third_party/wayland/SOURCES.md）")
    lines.append("// 重新生成：python3 tools/wayland_bindgen.py --out src/shell/wayland_protocol.hpp")
    lines.append("//")
    lines.append("// 生成物**自包含**：不 include 任何 wayland 头，也不链接 libwayland-client")
    lines.append("// （后端运行时 dlopen 它 —— 与 X11 后端同一姿态，见 CONVENTIONS.md §10 第 1 条）。")
    lines.append("#pragma once")
    lines.append("")
    lines.append("#include <cstdint>")
    lines.append("")
    lines.append("namespace st::shell::wayland::proto {")
    lines.append("")
    lines.append("// ————————————————————————————————————————————————————————————————————————————")
    lines.append("// libwayland 的 ABI 结构")
    lines.append("//")
    lines.append("// 按 `wayland-util.h` 逐字段抄写。这些对象由**我们**构造（不是从库里取），")
    lines.append("// 再传给 `wl_proxy_marshal_flags`——它用签名与 types 表做参数自证。")
    lines.append("// ⚠ 字段顺序即 ABI：改了顺序，libwayland 会以错误的方式解释这些表。")
    lines.append("// ————————————————————————————————————————————————————————————————————————————")
    lines.append("")
    lines.append("struct Interface;")
    lines.append("")
    lines.append("struct Message {")
    lines.append("  const char* name{nullptr};")
    lines.append("  /// 签名串：类型码序列（可空参数前缀 `?`；消息 since>1 时**首字符是版本号**）。")
    lines.append("  const char* signature{nullptr};")
    lines.append("  /// `Interface*` 表中的起始位置（只有 object/new_id 参数占非空槽位）。")
    lines.append("  const Interface* const* types{nullptr};")
    lines.append("};")
    lines.append("")
    lines.append("struct Interface {")
    lines.append("  const char* name{nullptr};")
    lines.append("  int version{0};")
    lines.append("  int request_count{0};")
    lines.append("  const Message* requests{nullptr};")
    lines.append("  int event_count{0};")
    lines.append("  const Message* events{nullptr};")
    lines.append("};")
    lines.append("")
    lines.append("// ————————————————————————————————————————————————————————————————————————————")
    lines.append("// 接口声明（互相引用，故先全部前置声明）")
    lines.append("// ————————————————————————————————————————————————————————————————————————————")
    lines.append("")
    for interface in interfaces:
        lines.append(f"extern const Interface k{interface.name};")
    lines.append("")
    lines.append("// ————————————————————————————————————————————————————————————————————————————")
    lines.append(f"// 类型表（所有消息共用一张扁平表；表首是 {null_run} 个空槽 = 官方 `null_run_length`）")
    lines.append("// ————————————————————————————————————————————————————————————————————————————")
    lines.append("")
    lines.append("namespace detail {")
    lines.append("")
    lines.append(f"inline const Interface* const kTypes[] = {{")
    for slot in flat if flat else [""]:
        lines.append(f"    {'&k' + slot if slot else 'nullptr'},")
    lines.append("};")
    lines.append("")
    lines.append(f"inline constexpr int kNullRun = {null_run};")
    lines.append("")
    lines.append("}  // namespace detail")
    lines.append("")
    lines.append("// ————————————————————————————————————————————————————————————————————————————")
    lines.append("// 消息表与接口定义（消息顺序 = 协议 opcode 顺序，不可重排）")
    lines.append("// ————————————————————————————————————————————————————————————————————————————")
    lines.append("")

    for interface in interfaces:
        for direction, messages, symbol in (
            ("request", interface.requests, "Requests"),
            ("event", interface.events, "Events"),
        ):
            if not messages:
                continue
            lines.append(f"inline const Message k{interface.name}{symbol}[] = {{")
            for message in messages:
                offset = offsets[(interface.name, direction, message.name)]
                lines.append(
                    f'    {{"{message.name}", "{message.signature()}", '
                    f"detail::kTypes + {offset}}},"
                )
            lines.append("};")
            lines.append("")

        request_ref = f"k{interface.name}Requests" if interface.requests else "nullptr"
        event_ref = f"k{interface.name}Events" if interface.events else "nullptr"
        lines.append(f"inline const Interface k{interface.name} = {{")
        lines.append(f'    "{interface.name}", {interface.version},')
        lines.append(f"    {len(interface.requests)}, {request_ref},")
        lines.append(f"    {len(interface.events)}, {event_ref},")
        lines.append("};")
        lines.append("")

    lines.append("}  // namespace st::shell::wayland::proto")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description="Wayland 协议绑定生成器")
    parser.add_argument("--out", default=str(ROOT / "src/shell/wayland_protocol.hpp"))
    args = parser.parse_args()

    interfaces: list[Interface] = []
    for name in ("wayland.xml", "xdg-shell.xml", "xdg-decoration-unstable-v1.xml"):
        path = XML_DIR / name
        if not path.exists():
            print(f"缺少 {path}（见 third_party/wayland/SOURCES.md）")
            return 1
        found = parse(path)
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        print(f"{name}: {len(found)} 个接口 · sha256={digest}")
        interfaces += found

    seen: set[str] = set()
    unique: list[Interface] = []
    for interface in interfaces:
        if interface.name in seen:  # 两个 XML 无重叠；这里只是防御
            continue
        seen.add(interface.name)
        unique.append(interface)

    text = generate(unique)
    pathlib.Path(args.out).write_text(text, encoding="utf-8")
    requests = sum(len(i.requests) for i in unique)
    events = sum(len(i.events) for i in unique)
    print(f"已写入 {args.out}：{len(unique)} 接口 · {requests} 请求 · {events} 事件 · "
          f"{len(text.splitlines())} 行")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
