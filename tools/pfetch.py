#!/usr/bin/env python3
"""并行分段下载器：突破单连接限速。

为什么需要：PyTorch 官方 wheel 源单连接只有 ~0.2 MB/s（2.4GB 要 3.4 小时），
而国内镜像**都没有** pytorch CUDA wheel（已逐个实测：清华/阿里/北外/中科大全 404）。
但官方源支持 Range 请求，所以用 N 条连接各下一段再拼接——这是唯一能在合理时间内
拿到文件的路径。

用法：
    python tools/pfetch.py <url> <输出路径> [并发数=8]

特性：
  - 先 HEAD 拿 Content-Length，再按 range 切段并行；
  - **断点续传**：每段写 <输出>.part<i>，重跑会跳过已完成的段；
  - 进度按已落盘字节汇总（不依赖各连接的内部计数）；
  - 任一段失败即报错并保留已完成段（可重试）。
"""
import os
import sys
import threading
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed

UA = {"User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64)"}


def head(url):
    req = urllib.request.Request(url, method="HEAD", headers=UA)
    with urllib.request.urlopen(req, timeout=30) as resp:
        length = resp.headers.get("Content-Length")
        if length is None:
            raise RuntimeError("服务器未返回 Content-Length，无法分段")
        return int(length)


def fetch_range(url, start, end, path, progress, lock):
    """下载 [start, end] 闭区间到 path（已存在则跳过——断点续传）。"""
    want = end - start + 1
    if os.path.exists(path) and os.path.getsize(path) == want:
        with lock:
            progress["done"] += want
        return
    req = urllib.request.Request(url, headers={**UA, "Range": f"bytes={start}-{end}"})
    written = 0
    with urllib.request.urlopen(req, timeout=60) as resp, open(path, "wb") as out:
        while True:
            chunk = resp.read(262144)
            if not chunk:
                break
            out.write(chunk)
            written += len(chunk)
            if written > want:  # 服务器忽略 Range，整份返回
                raise RuntimeError("服务器未按 Range 分片返回")
            with lock:
                progress["done"] += len(chunk)
    if written != want:
        raise RuntimeError(f"分段长度不足：{written} != {want}（{path}）")


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    url, out_path = sys.argv[1], sys.argv[2]
    workers = int(sys.argv[3]) if len(sys.argv) > 3 else 8

    total = head(url)
    print(f"总大小 {total / 1024 / 1024:.1f} MB，{workers} 路并行", flush=True)
    # 每段至少 8MB，避免段数过多
    seg = max(8 * 1024 * 1024, total // workers)
    ranges = []
    start = 0
    while start < total:
        end = min(start + seg - 1, total - 1)
        ranges.append((start, end))
        start = end + 1
    print(f"切成 {len(ranges)} 段", flush=True)

    progress = {"done": 0}
    lock = threading.Lock()
    stop = threading.Event()

    def report():
        began = time.time()
        last = 0
        while not stop.wait(2.0):
            with lock:
                done = progress["done"]
            rate = (done - last) / 2.0 / 1024 / 1024
            last = done
            pct = done * 100.0 / total
            print(f"  {pct:5.1f}%  {done/1024/1024:7.1f}/{total/1024/1024:.0f} MB  "
                  f"{rate:5.1f} MB/s  {time.time()-began:.0f}s", flush=True)

    reporter = threading.Thread(target=report, daemon=True)
    reporter.start()

    try:
        with ThreadPoolExecutor(max_workers=workers) as pool:
            futures = [
                pool.submit(fetch_range, url, s, e,
                            f"{out_path}.part{i}", progress, lock)
                for i, (s, e) in enumerate(ranges)
            ]
            for future in as_completed(futures):
                future.result()
    finally:
        stop.set()
        reporter.join(timeout=1.0)

    # 拼接
    print("拼接…", flush=True)
    with open(out_path, "wb") as out:
        for i in range(len(ranges)):
            part = f"{out_path}.part{i}"
            with open(part, "rb") as src:
                while chunk := src.read(8 * 1024 * 1024):
                    out.write(chunk)
            os.remove(part)
    got = os.path.getsize(out_path)
    if got != total:
        raise SystemExit(f"拼接后大小不符：{got} != {total}")
    print(f"完成：{out_path}（{got/1024/1024:.1f} MB）", flush=True)


if __name__ == "__main__":
    main()
