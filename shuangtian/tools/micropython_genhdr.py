#!/usr/bin/env python3
"""生成 MicroPython 嵌入所需的 `genhdr/` 头文件。

## 为什么需要这一步

MicroPython 不是「把 .c 丢给编译器」就能编的：它有三个**构建期生成**的头文件，
全部由源码扫描得出：

| 文件 | 内容 | 谁需要 |
|---|---|---|
| `qstrdefs.generated.h` | 全部 qstr（短字符串）的枚举与查表 | 每个 .c（`qstr.h` 直接 include） |
| `moduledefs.h` | 所有 `MP_REGISTER_MODULE` 的模块表 | `py/builtinimport.c` |
| `root_pointers.h` | 根指针（GC 扫描入口） | `py/gc.c` |

这三个**不能手写**（qstr 编号必须与运行时一致），只能跑上游脚本生成。
上游用 Makefile 做，我们这里用 Python 复刻同样的三步——不引 Make。

## 用法

    py tools/micropython_genhdr.py <micropython 根> <输出目录>

输出目录里会有 `genhdr/` 三个头文件。
"""
import os
import subprocess
import sys

# 需要扫描 qstr 的源文件目录（相对 micropython 根）。
#
# `py/` 是上游。而**我们自己的 C 模块**（`src/python/`）同样会用到
# `MP_QSTR_xxx`（方法表/类型名），它们也必须进 qstr 表——否则编到
# `MP_QSTR_dot` 会报 "undeclared"（看起来像“漏了 include”，实际是“qstr 没生成”）。
#
# 通过环境变量 `ST_EXTRA_QSTR_DIRS` 传入额外目录（分号分隔，相对 micropython 根）。
import os as _os
QSTR_SCAN_DIRS = ["py"] + [d for d in _os.environ.get("ST_EXTRA_QSTR_DIRS", "").split(";") if d]

# 扫描时要排除的文件（上游 Makefile 里同样排除）。
# `qstrdefs.h` 本身是**定义清单**不是扫描对象——它会被单独作为输入喂给
# makeqstrdata.py。把它当源码扫会得到重复定义。
QSTR_EXCLUDE = {"qstrdefs.h"}

# 预处理器的 include 路径与宏开关。
#
# ## 为什么必须先预处理
#
# `makeqstrdefs.py split` 期待的是**预处理后的输出**（带 `# n "file"` 行标记），
# 上游 Makefile 就是先跑 `gcc -E` 再 split。直接喂源码会漏掉所有
# `#if MICROPY_...` 里的 qstr，于是编到 `compile.c` / `modbuiltins.c`
# 时报 `MP_QSTR___build_class__ undeclared`——而那看起来像“配置缺开关”，
# 实际是“扫描阶段漏了”。实测就是这样卡了一轮。
PP_INCLUDES = []
PP_DEFINES = []

# ## 扫描 qstr / module / root_pointer 时必须定义 `NO_QSTR`
#
# `py/obj.h` 里这些宏的定义是：
#
#     #ifndef NO_QSTR
#     #define MP_REGISTER_MODULE(module_name, obj_module)      // 展开成**空**
#     #endif
#
# 也就是说“正常编译时它们什么都不产生”（模块表靠构建期生成的头）。
# 若不定义 `NO_QSTR` 就去预处理，这些宏在输出里**已经消失**，扫描自然是 0 个
# ——而那看起来像“上游没注册任何模块”，实际是“宏被展开掉了”。
#
# 这也是上游 `makeqstrdefs.py pp` 子命令存在的理由。
SCAN_DEFINES = ["NO_QSTR"]


def run(cmd, cwd=None):
    """跑一个子进程；失败就带着输出抛（不要静默返回空文件）。"""
    # ## 强制 UTF-8
    #
    # 上游的 makeqstrdefs.py / makeqstrdata.py 用 io.open(...) 读文件时不指定
    # 编码，于是走**系统默认**。Windows 中文区域下那是 GBK，而我们的源文件含
    # 中文注释 → UnicodeDecodeError: 'gbk' codec can't decode。
    #
    # PYTHONUTF8=1 让 Python 3.7+ 全程用 UTF-8，不依赖区域设置。
    env = dict(os.environ)
    env["PYTHONUTF8"] = "1"
    env["PYTHONIOENCODING"] = "utf-8"
    result = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True,
                            encoding="utf-8", errors="replace", env=env)
    if result.returncode != 0:
        raise RuntimeError("命令失败：%s\n%s\n%s"
                           % (" ".join(cmd), result.stdout[-2000:], result.stderr[-2000:]))
    return result.stdout


def _generate_version_header(mp_root, py_dir, genhdr):
    """生成 `genhdr/mpversion.h`。

    签名：`makeversionhdr.py [-r REPO] <dest>`；它把结果**直接写入 dest**，
    同时 print 到 stdout。
    """
    target = os.path.join(genhdr, "mpversion.h")
    run([sys.executable, os.path.join(py_dir, "makeversionhdr.py"),
         "--repo-path", mp_root, target], cwd=mp_root)



def _first_register_module(frag):
    """从一段 .module 片段里取出**第一个** MP_REGISTER_MODULE(...) 的 (name, obj)。

    ## 为什么要自己配平括号

    `makeqstrdefs.py split module` 的切分是按**行**归并的，相邻两条宏
    （比如 `modsys.c` 里的 `MP_REGISTER_MODULE(...)` 紧跟
    `MP_REGISTER_MODULE_DELEGATION(...)`）会被合并成一段。直接取
    `text[:-1]` 会把两条宏都当成参数，生成出
    `MP_ROM_PTR(&mp_module_sys);\nMP_REGISTER_MODULE_DELEGATION(...)`
    这种坏代码——而错误位置在 genhdr 里，看不出根因。

    所以这里扫描出第一个括号配平的 `MP_REGISTER_MODULE(...)`，忽略后面
    的内容（DELEGATION 那条本框架用不上：它只是给模块加属性委托）。
    """
    marker = "MP_REGISTER_MODULE("
    start = frag.find(marker)
    if start < 0:
        return None
    i = start + len(marker)
    depth = 1
    while i < len(frag) and depth > 0:
        ch = frag[i]
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        i += 1
    if depth != 0:
        return None
    inner = frag[start + len(marker):i - 1]
    parts = [piece.strip() for piece in inner.split(",", 1)]
    if len(parts) != 2:
        return None
    return parts[0], parts[1]

def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    mp_root = os.path.abspath(sys.argv[1])
    out_root = os.path.abspath(sys.argv[2])
    genhdr = os.path.join(out_root, "genhdr")
    os.makedirs(genhdr, exist_ok=True)
    # `py/qstr.h` 里是 `#include "genhdr/qstrdefs.generated.h"`，
    # 所以 include 路径要给 genhdr 的**父目录**（那才是 `genhdr/` 可见的位置）。
    gen_parent = out_root

    # ## 先放空占位再做预处理（鸡与蛋）
    #
    # 预处理 `py/*.c` 会 include `py/qstr.h`，而后者 include
    # `genhdr/qstrdefs.generated.h`——那个文件正是我们要生成的。
    # 所以先写空文件，让预处理能跑通；它只需要 `MP_QSTR_xxx` 这些名字
    # 在宏展开后仍然**字面出现**在结果里（空头文件不影响）。
    for placeholder in ("qstrdefs.generated.h", "moduledefs.h", "root_pointers.h"):
        path = os.path.join(genhdr, placeholder)
        if not os.path.exists(path):
            with open(path, "w", encoding="utf-8") as handle:
                handle.write("")

    # `mpversion.h` 必须在**预处理之前**就存在（`py/modsys.c` 无条件 include 它），
    # 而它不依赖 qstr，所以可以提前生成。
    py_dir = os.path.join(mp_root, "py")
    _generate_version_header(mp_root, py_dir, genhdr)
    # 空占位里没有 MP_QSTR_ 定义 → 预处理时会报未定义标识符。
    # 用一个“把所有 MP_QSTR_x 都当标识符”的空定义清单避开：
    # 真正需要的是它们**以文本形式出现在预处理输出里**，而 `MP_QSTR_xxx`
    # 本身就是普通 C 标识符，未定义不影响 `-E`（只警告，不报错）。

    py_dir = os.path.join(mp_root, "py")
    if not os.path.isdir(py_dir):
        print("找不到 %s：第二个参数应是 MicroPython 源码根" % py_dir)
        return 2

    # ── ① 收集 qstr ──────────────────────────────────────────────────────
    #
    # `makeqstrdefs.py split` 从每个 .c 里抽出 `MP_QSTR_xxx` 与
    # `MP_ROM_QSTR(MP_QSTR_xxx)` 用到的字符串。上游是"每文件一个 .qstr 中间文件"，
    # 这里同样做，因为下一步 `cat` 阶段要按目录归并。
    tmp_qstr = os.path.join(out_root, "qstr-tmp")
    os.makedirs(tmp_qstr, exist_ok=True)

    sources = []
    for rel_dir in QSTR_SCAN_DIRS:
        abs_dir = os.path.join(mp_root, rel_dir)
        for name in sorted(os.listdir(abs_dir)):
            if not name.endswith(".c") or name in QSTR_EXCLUDE:
                continue
            sources.append(os.path.join(abs_dir, name))
    if not sources:
        print("没找到任何 .c 源文件")
        return 2

    # split 子命令签名（上游 v1.24，实测）：
    #   makeqstrdefs.py split <mode> <input_file> <output_dir> <output_file>
    #   —— 必须 6 个 argv，否则直接打 usage 退出。
    #
    # 输入必须是**预处理器输出**（见 PP_INCLUDES 那段说明）。
    for index, source in enumerate(sources):
        per_file_dir = os.path.join(tmp_qstr, "src%03d" % index)
        os.makedirs(per_file_dir, exist_ok=True)
        placeholder = os.path.join(per_file_dir, "_placeholder")
        # gcc -E：只做预处理，不编。
        # -P 去掉行标记？不行——process_file 靠行标记定位来源文件，
        # 所以我们保留标记（不加 -P）。
        pp_cmd = ["gcc", "-E", "-I", mp_root, "-I", py_dir, "-I", gen_parent,
                  "-I", _os.path.join(mp_root, "..", "..", "include")]
        for inc in PP_INCLUDES:
            pp_cmd += ["-I", inc]
        for define in PP_DEFINES + SCAN_DEFINES:
            pp_cmd.append("-D" + define)
        pp_cmd.append(source)
        pp_text = run(pp_cmd)
        pp_file = os.path.join(per_file_dir, "preprocessed.i")
        with open(pp_file, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(pp_text)
        run([sys.executable, os.path.join(py_dir, "makeqstrdefs.py"), "split", "qstr",
             pp_file, per_file_dir, placeholder])

    # qstrdefs.h 本身（`MP_QSTR_` 的基础清单）也要参与归并
    base_qstr = os.path.join(py_dir, "qstrdefs.h")
    merged = os.path.join(out_root, "qstrdefs.collected.h")
    with open(merged, "w", encoding="utf-8", newline="\n") as out:
        # cat 阶段：把所有中间文件按路径排序归并
        for root, _dirs, files in os.walk(tmp_qstr):
            for name in sorted(files):
                path = os.path.join(root, name)
                with open(path, encoding="utf-8", errors="replace") as handle:
                    out.write(handle.read())
        if os.path.exists(base_qstr):
            with open(base_qstr, encoding="utf-8", errors="replace") as handle:
                out.write(handle.read())

    # `makeqstrdata.py` 需要三个配置值（BYTES_IN_LEN / BYTES_IN_HASH /
    # MAX_CRC_LEN），它们从输入文件的 `#define` 行里读。
    #
    # 上游 Makefile 会把 mpconfig.h（预处理后的）拼在清单前面，这里直接写两行
    # —— 我们用的是默认值（最小档下 MicroPython 自己就取这三个）。
    #
    # 值来源：py/qstr.h 里 `MICROPY_QSTR_BYTES_IN_LEN` 的缺省（1），
    # `MICROPY_QSTR_BYTES_IN_HASH` 缺省（1）。写错会让 qstr 查表**静默错位**
    # （编译通过、运行时报奇怪的 KeyError），所以这里必须是数值不是宏名。
    merged_cfg = os.path.join(out_root, "qstrdefs.configured.h")
    with open(merged_cfg, "w", encoding="utf-8", newline="\n") as out:
        # 格式是 `QCFG(<名>, <值>)`（`parse_input_headers` 用 `^QCFG\((.+), (.+)\)` 匹配）。
        #
        # ⚠ 必须**写在前面**且**把清单里的同名行滤掉**：
        # `py/qstrdefs.h` 自带两行 `QCFG(BYTES_IN_LEN, MICROPY_QSTR_BYTES_IN_LEN)`
        # ——值是**宏名**，上游靠 C 预处理器展开成数字。我们这里不跑预处理器，
        # 所以那两行会让 `int()` 拿到宏名字符串直接掍。
        # 做法：只保留清单里非 QCFG 的行，自己写正确的数值版。
        out.write("QCFG(BYTES_IN_LEN, 1)\n")
        out.write("QCFG(BYTES_IN_HASH, 1)\n")
        with open(merged, encoding="utf-8", errors="replace") as handle:
            for line in handle:
                if line.lstrip().startswith("QCFG("):
                    continue
                out.write(line)

    # ── ② 生成 qstrdefs.generated.h ─────────────────────────────────────
    #
    # 输入是**已拼好配置的**合并清单，输出是枚举 + 查表。
    with open(os.path.join(genhdr, "qstrdefs.generated.h"), "w",
              encoding="utf-8", newline="\n") as out:
        text = run([sys.executable, os.path.join(py_dir, "makeqstrdata.py"), merged_cfg])
        out.write(text)

    # ── ③ moduledefs.h ───────────────────────────────────────────────────
    #
    # ## 为什么必须真扫，不能给空表
    #
    # `MP_REGISTER_MODULE(MP_QSTR_math, mp_module_math)` 写在 `py/modmath.c` 里。
    # 若 moduledefs.h 是空的，模块表就没有 `math`，于是脚本里 `import math`
    # 报 `ImportError: module not found`——看起来像“没编 math”，实际是
    # **注册表漏了它**。
    #
    # 做法：用 `makeqstrdefs.py split module` 从预处理输出里抽
    # `MP_REGISTER_MODULE(...)`，再把结果拼成模块表宏。
    module_dir = os.path.join(out_root, "module-tmp")
    os.makedirs(module_dir, exist_ok=True)
    module_fragments = []
    for index, source in enumerate(sources):
        per_file_dir = os.path.join(module_dir, "src%03d" % index)
        os.makedirs(per_file_dir, exist_ok=True)
        pp_cmd = ["gcc", "-E", "-I", mp_root, "-I", py_dir, "-I", gen_parent,
                  "-I", _os.path.join(mp_root, "..", "..", "include")]
        for inc in PP_INCLUDES:
            pp_cmd += ["-I", inc]
        for define in PP_DEFINES + SCAN_DEFINES:
            pp_cmd.append("-D" + define)
        pp_cmd.append(source)
        pp_text = run(pp_cmd)
        pp_file = os.path.join(per_file_dir, "preprocessed.i")
        with open(pp_file, "w", encoding="utf-8", newline="\n") as handle:
            handle.write(pp_text)
        placeholder = os.path.join(per_file_dir, "_placeholder")
        run([sys.executable, os.path.join(py_dir, "makeqstrdefs.py"), "split", "module",
             pp_file, per_file_dir, placeholder])
        for name in sorted(os.listdir(per_file_dir)):
            # 输出文件名形如 `<path with __>.c.module`：**后缀**是 .module，不是前缀。
            # 按 `startswith("module")` 过滤会全部落空，模块表恒为空——而那是静默的
            # （不报错，只是 `import math` 报 ModuleNotFound）。
            if name.endswith(".module"):
                with open(os.path.join(per_file_dir, name), encoding="utf-8",
                          errors="replace") as handle:
                    module_fragments.append(handle.read().strip())

    with open(os.path.join(genhdr, "moduledefs.h"), "w", encoding="utf-8",
              newline="\n") as out:
        out.write("// 由 tools/micropython_genhdr.py 生成：从 MP_REGISTER_MODULE 抽取\n")
        # ## 片段是**原始宏文本**，要自己转换成表项
        #
        # `makeqstrdefs.py split module` 抽出来的形如：
        #     MP_REGISTER_MODULE(MP_QSTR_math, mp_module_math);
        # 而 `MICROPY_REGISTERED_MODULES` 要的是：
        #     { MP_ROM_QSTR(MP_QSTR_math), MP_ROM_PTR(&mp_module_math) },
        #
        # 两个形态不一样（宏 vs 结构体初始化），必须转换。
        # 直接拼原始宏会让 `objmodule.c` 报语法错——而那个错的位置在
        # genhdr 里，第一眼很难看出根本原因。
        entries = []
        for frag in module_fragments:
            parsed = _first_register_module(frag)
            if parsed is None:
                continue
            name, obj = parsed
            entries.append("    { MP_ROM_QSTR(%s), MP_ROM_PTR(&%s) }" % (name, obj))

        joined = ", \\\n".join(entries)
        # ## 每个模块对象必须先声明
        #
        # `moduledefs.h` 被 `objmodule.c` 引用，而那里没有（也不该有）
        # `#include "modmath.h"` 之类的模块头。所以这里补 `extern` 声明。
        #
        # 不补会报 `'mp_module_math' undeclared`——而错误位置在 genhdr 里，
        # 第一眼看上去像“生成器写错了”，实际是“缺声明”。
        for frag in module_fragments:
            parsed = _first_register_module(frag)
            if parsed is not None:
                out.write("extern const mp_obj_module_t %s;\n" % parsed[1])

        out.write("#define MICROPY_REGISTERED_MODULES \\\n")
        if entries:
            out.write(joined + ", \\\n")
        out.write("    { MP_ROM_QSTR(MP_QSTRnull), MP_ROM_PTR(NULL) }\n")
        out.write("#define MICROPY_REGISTERED_EXTENSIBLE_MODULES \\\n")
        out.write("    { MP_ROM_QSTR(MP_QSTRnull), MP_ROM_PTR(NULL) }\n")
        out.write("// 共 %d 个模块\n" % len(entries))

    # ── ⑤ root_pointers.h ────────────────────────────────────────────────
    #
    # 根指针清单：GC 从这里出发扫描。上游由 `make_root_pointers.py` 从
    # 指定文件里抽 `MP_REGISTER_ROOT_POINTER(...)`。
    # ## root pointer 必须扫**所有** py/*.c，不只是 mpstate.h
    #
    # `MP_REGISTER_ROOT_POINTER(...)` 声明"这个全局变量是 GC 根（别回收它）"。
    # 它们散落在各个 .c 里（`modsys.c` 有 `sys_mutable` / `mp_sys_argv_obj` /
    # `cur_exception` 等）。
    #
    # 只扫 mpstate.h 会漏掉它们，症状是编译报
    # `'mp_state_vm_t' has no member named 'sys_mutable'` ——
    # 因为 `MP_STATE_VM(x)` 展开成 `mp_state_ctx.vm.x`，而那个成员正是
    # 由 root_pointers.h 生成的。报错看起来像“mpstate.h 定义不全”，
    # 实际是“生成器扫漏了”。
    root_sources = [os.path.join(py_dir, name)
                    for name in sorted(os.listdir(py_dir))
                    if name.endswith(".c")]
    # `make_root_pointers.py` 只接**一个**输入文件，所以先把散落的
    # `MP_REGISTER_ROOT_POINTER(...)` 行抽出来汇到一份临时文件里。
    collected = os.path.join(out_root, "root_pointers.collected.h")
    with open(collected, "w", encoding="utf-8", newline="\n") as out:
        for source in root_sources:
            with open(source, encoding="utf-8", errors="replace") as handle:
                for line in handle:
                    stripped = line.strip()
                    if stripped.startswith("MP_REGISTER_ROOT_POINTER("):
                        out.write(stripped + "\n")
    with open(os.path.join(genhdr, "root_pointers.h"), "w", encoding="utf-8",
              newline="\n") as out:
        text = run([sys.executable, os.path.join(py_dir, "make_root_pointers.py"),
                    collected])
        out.write(text)

    print("已生成 genhdr：%s" % genhdr)
    for name in ("qstrdefs.generated.h", "moduledefs.h", "root_pointers.h"):
        path = os.path.join(genhdr, name)
        print("  %-28s %d 字节" % (name, os.path.getsize(path)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
