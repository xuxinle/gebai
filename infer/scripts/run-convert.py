#!/usr/bin/env python
# run-convert.py -- 用引擎自带转换器把 HF 权重转成 GGUF（自动挂 gguf-py 到 PYTHONPATH）
#
# 为什么需要它：引擎源码树里的 convert_hf_to_gguf.py 依赖同树的 gguf-py 包，
# 但本机并未 `pip install -e gguf-py`（保持引擎目录零污染），故用一个入口脚本注入路径。
#
# 用法:
#   python scripts/run-convert.py --hf <hf_dir> --out <out.gguf> --outtype bf16 --extra --fp8-as-q8
import argparse
import os
import runpy
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)                                   # infer/
SRC = os.path.join(ROOT, "engine", "llama.cpp-b11100")         # 引擎源码树


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--hf", required=True, help="HF 权重目录")
    ap.add_argument("--out", required=True, help="输出 GGUF 路径")
    ap.add_argument("--outtype", default="bf16", help="bf16/f16/q8_0/...")
    ap.add_argument("--extra", nargs=argparse.REMAINDER, default=[],
                    help="透传给 convert_hf_to_gguf.py 的额外参数（须置于最后）")
    a = ap.parse_args()

    script = os.path.join(SRC, "convert_hf_to_gguf.py")
    if not os.path.isfile(script):
        print(f"[err] 找不到 {script}", file=sys.stderr)
        return 1
    if not os.path.isdir(a.hf):
        print(f"[err] 找不到 HF 目录 {a.hf}", file=sys.stderr)
        return 1

    sys.path.insert(0, os.path.join(SRC, "gguf-py"))
    sys.path.insert(0, SRC)          # conversion/ 包在源码树根下
    sys.argv = [script, a.hf, "--outfile", a.out, "--outtype", a.outtype, *a.extra]
    os.chdir(SRC)
    print(f"[run] cwd={SRC}\n[run] argv={' '.join(sys.argv[1:])}", flush=True)
    runpy.run_path(script, run_name="__main__")
    return 0


if __name__ == "__main__":
    sys.exit(main())
