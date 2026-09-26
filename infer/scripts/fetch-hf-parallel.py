#!/usr/bin/env python
# fetch-hf-parallel.py -- 大模型仓库并发分片下载器（ModelScope 兼容，断点续传 + 预算分批）
#
# 为什么不用后台任务：本机环境下 sh 的 async 子进程会被系统降级/挂起
# （实测同一 URL 前台 ~68MB/s、后台 0），所以大文件改由**前台并发拉取**，
# 用 -BudgetSec 控制单次时长以适配工具超时（<=540s），重复调用直至打印 ALL-DONE。
#
# 用法:
#   python scripts/fetch-hf-parallel.py --repo Qwen/Qwen3.6-35B-A3B-FP8 \
#          --out-dir <dir> --workers 24 --budget-sec 450
#
# 特性:
#   * 分片落盘为 <file>.part<idx>，只有尺寸正确的分片才计为已完成 -> 天然断点续传
#   * 全部文件下完后校验总大小；已完整的目标文件直接跳过（幂等）
#   * 退出码: 0=全部完成 2=本次预算用尽（可再调用） 1=错误

import argparse
import concurrent.futures as cf
import json
import os
import sys
import time
import urllib.error
import urllib.request

UA = {"User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) gebai-fetch"}
API = "https://www.modelscope.cn/api/v1/models/{repo}/repo/files?Revision={rev}&Root="
RESOLVE = "https://www.modelscope.cn/models/{repo}/resolve/{rev}/{path}"

CHUNK = 32 * 1024 * 1024  # 32MB/分片


def list_files(repo, rev):
    url = API.format(repo=repo, rev=rev)
    with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=60) as r:
        d = json.load(r)
    files = d.get("Data", {}).get("Files", [])
    out = []
    for f in files:
        if f.get("Type") == "tree":
            continue
        out.append((f["Path"], int(f.get("Size") or 0)))
    return out


def fetch_chunk(url, start, end, part, retries=5):
    want = end - start + 1
    if os.path.exists(part) and os.path.getsize(part) == want:
        return 0  # 已完成
    last = None
    for attempt in range(retries):
        try:
            req = urllib.request.Request(url, headers={**UA, "Range": f"bytes={start}-{end}"})
            with urllib.request.urlopen(req, timeout=300) as r:
                data = r.read()
            if len(data) != want:
                raise IOError(f"size mismatch {len(data)} != {want}")
            tmp = part + ".tmp"
            with open(tmp, "wb") as fh:
                fh.write(data)
            os.replace(tmp, part)
            return len(data)
        except Exception as e:  # noqa: BLE001
            last = e
            time.sleep(1.5 * (attempt + 1))
    raise RuntimeError(f"chunk {start}-{end} failed: {last!r}")


def assemble(dst, parts, expect):
    """按序拼接分片 -> 目标文件；成功返回 True"""
    tmp = dst + ".asm"
    with open(tmp, "wb") as out:
        for p in parts:
            with open(p, "rb") as fh:
                while True:
                    b = fh.read(4 * 1024 * 1024)
                    if not b:
                        break
                    out.write(b)
    if os.path.getsize(tmp) != expect:
        os.remove(tmp)
        return False
    os.replace(tmp, dst)
    for p in parts:
        try:
            os.remove(p)
        except OSError:
            pass
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--rev", default="master")
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--workers", type=int, default=24)
    ap.add_argument("--budget-sec", type=int, default=450, help="本次前台执行的时间预算（秒）")
    ap.add_argument("--only", default=None, help="只处理匹配该子串的文件（调试用）")
    args = ap.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)
    files = list_files(args.repo, args.rev)
    if args.only:
        files = [f for f in files if args.only in f[0]]
    grand_total = sum(s for _, s in files)
    print(f"[清单] {len(files)} 个文件，合计 {grand_total/2**30:.2f} GB -> {args.out_dir}", flush=True)

    # 待办：为每个未完成文件切分片
    tasks = []       # (url, start, end, part)
    pending = {}     # dst -> (parts, expect_bytes)
    already = 0
    for path, size in files:
        dst = os.path.join(args.out_dir, path.replace("/", os.sep))
        os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
        if os.path.exists(dst) and os.path.getsize(dst) == size:
            already += size
            continue
        parts = []
        pos = 0
        idx = 0
        url = RESOLVE.format(repo=args.repo, rev=args.rev, path=path)
        while pos < size:
            end = min(pos + CHUNK, size) - 1
            part = f"{dst}.part{idx}"
            if not (os.path.exists(part) and os.path.getsize(part) == end - pos + 1):
                tasks.append((url, pos, end, part))
            parts.append(part)
            pos = end + 1
            idx += 1
        if parts:
            pending[dst] = (parts, size)

    print(f"[状态] 已完整 {already/2**30:.2f} GB；待下载分片 {len(tasks)} 个（{sum(e-s+1 for _, s, e, _ in tasks)/2**30:.2f} GB）", flush=True)
    if not pending:
        print("ALL-DONE 全部文件已就绪")
        return 0
    if not tasks:
        print("[合并] 分片齐全，开始拼接…", flush=True)
        for dst, (parts, size) in pending.items():
            ok = assemble(dst, parts, size)
            print(f"   {'OK  ' if ok else 'FAIL'} {dst}", flush=True)
        _verify(files, args.out_dir)
        print("ALL-DONE 全部文件已就绪")
        return 0

    t_start = time.time()
    deadline = t_start + args.budget_sec
    got = 0
    got_lock = __import__("threading").Lock()
    stop = False

    def worker(task):
        nonlocal got
        url, s, e, part = task
        n = fetch_chunk(url, s, e, part)
        if n:
            with got_lock:
                got += n
        return n

    print(f"[开始] 并发 {args.workers}，本次预算 {args.budget_sec}s", flush=True)
    last_report = time.time()
    inflight = set()
    ti = 0
    with cf.ThreadPoolExecutor(args.workers) as ex:
        while (ti < len(tasks) or inflight) and not stop:
            while ti < len(tasks) and len(inflight) < args.workers:
                if time.time() > deadline - 5:
                    stop = True
                    break
                inflight.add(ex.submit(worker, tasks[ti]))
                ti += 1
            if not inflight:
                break
            done, inflight = cf.wait(inflight, timeout=3, return_when=cf.FIRST_COMPLETED)
            for f in done:
                exc = f.exception()
                if exc is not None:
                    print(f"  !! 分片失败: {exc}", flush=True)
            if time.time() - last_report >= 10:
                el = time.time() - t_start
                sp = got / 2**20 / max(el, 1e-6)
                left = sum(e - s + 1 for _, s, e, _ in tasks[ti:])
                remain_txt = f"，队列剩余 {left/2**30:.2f} GB ≈ {left/2**20/max(sp,1e-6)/60:.0f} min" if sp > 1 else ""
                print(f"[进度] {got/2**30:.2f} GB @ {sp:.1f} MB/s（已用时 {el:.0f}s{remain_txt}）", flush=True)
                last_report = time.time()

    # 拼装
    assembled = 0
    for dst, (parts, size) in pending.items():
        if all(os.path.exists(p) for p in parts):
            if os.path.exists(dst) and os.path.getsize(dst) == size:
                continue
            if assemble(dst, parts, size):
                assembled += 1
                print(f"[合并] OK {os.path.basename(dst)} ({size/2**30:.2f} GB)", flush=True)
    rc = _verify(files, args.out_dir)
    el = time.time() - t_start
    print(f"[本次] 下载 {got/2**30:.2f} GB，用时 {el:.0f}s（{got/2**20/max(el,1e-6):.1f} MB/s），拼接 {assembled} 个文件", flush=True)
    if rc:
        print("ALL-DONE 全部文件已就绪")
        return 0
    print("未完成——可再次调用本脚本续传（已完成分片/文件自动跳过）")
    return 2


def _verify(files, out_dir):
    ok = True
    total = 0
    for path, size in files:
        dst = os.path.join(out_dir, path.replace("/", os.sep))
        if os.path.exists(dst) and os.path.getsize(dst) == size:
            total += size
        else:
            ok = False
    print(f"[校验] 已就绪 {total/2**30:.2f} GB / {sum(s for _, s in files)/2**30:.2f} GB", flush=True)
    return ok


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(1)
