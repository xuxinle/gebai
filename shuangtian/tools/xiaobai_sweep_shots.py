"""逐屏截取小白消息区，供逐条审视渲染质量。

用法：py -3 tools/xiaobai_sweep_shots.py [--out <目录>] [--step 900]
用官方客户端库驱动控制通道（不手搓协议）。
"""
import argparse
import os
import subprocess
import sys
import time

sys.path.insert(0, r'D:\code\gebai-pro\shuangtian\tools')
import st_client_lib as lib

CF = r'D:\code\gebai-pro\users\admin\sessions\e3\d2\e3d29f78f5a146ac93bd9839e7da3609\tmp\.shuangtian\xiaobai-control.json'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=r'D:\code\gebai-pro\users\admin\sessions\e3\d2\e3d29f78f5a146ac93bd9839e7da3609\tmp\qa')
    ap.add_argument('--step', type=float, default=950.0)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    ctl = lib.load_control(CF)
    port, token = ctl['port'], ctl.get('token', '')

    def call(method, params=None):
        return lib.call_with_token(port, method, params or {}, token=token)

    # 先读内容总高与视口
    r = call('get', {'id': 'msg-scroll'})
    props = (r.get('result') or {}).get('props') or {}
    content_h = float(props.get('content_height', 0))
    view_h = float(props.get('viewport_height', 0))
    print(f'content={content_h:.0f} viewport={view_h:.0f}')

    # 滚到顶
    call('invoke', {'id': 'msg-scroll', 'action': 'scroll_to_top'})
    time.sleep(0.4)

    idx = 0
    pos = 0.0
    while pos <= content_h:
        # 用动作滚到指定位置（比键盘可靠：不依赖焦点）
        call('invoke', {'id': 'msg-scroll', 'action': 'scroll_to',
                        'argument': str(int(pos))})
        time.sleep(0.35)
        path = os.path.join(args.out, f'sweep-{idx:02d}.png')
        call('capture', {'path': path})
        print(f'  [{idx}] scroll={pos:.0f} -> {path}')
        idx += 1
        pos += args.step
        if idx > 40:
            break
    print(f'共 {idx} 屏')


if __name__ == '__main__':
    main()
