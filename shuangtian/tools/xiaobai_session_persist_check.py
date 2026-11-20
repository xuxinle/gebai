"""验证小白会话持久化：发消息 → 检查存档 → 重启 → 检查恢复。"""
import json
import os
import subprocess
import sys
import time

sys.path.insert(0, r'D:\code\gebai-pro\shuangtian\tools')
import st_client_lib as lib

TMP = r'D:\code\gebai-pro\users\admin\sessions\e3\d2\e3d29f78f5a146ac93bd9839e7da3609\tmp'
CF = os.path.join(TMP, '.shuangtian', 'xiaobai-control.json')
SF = os.path.join(TMP, 'test-session.json')
EXE = r'D:\code\gebai-pro\shuangtian\build\dev\bin\xiaobai.exe'


def read_session():
    if not os.path.exists(SF):
        return None
    return json.load(open(SF, encoding='utf-8'))


def start_app():
    env = dict(os.environ)
    env['XIAOBAI_SESSION'] = SF
    env['XIAOBAI_BASE_URL'] = 'http://127.0.0.1:8770/v1'
    env['XIAOBAI_API_KEY'] = 'mock-key'
    env['XIAOBAI_MODEL'] = 'replay-mock'
    if os.path.exists(CF):
        os.remove(CF)
    p = subprocess.Popen([EXE, '--headless', '--control-port', '0'], env=env,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(60):
        time.sleep(0.5)
        if os.path.exists(CF):
            try:
                ctl = json.load(open(CF, encoding='utf-8'))
                if ctl.get('port'):
                    return p, ctl
            except Exception:
                pass
    return p, None


def main():
    # ① 清掉旧存档，冷启动
    if os.path.exists(SF):
        os.remove(SF)
    proc, ctl = start_app()
    if not ctl:
        print('冷启动失败'); return 1
    print(f'[冷启动] port={ctl["port"]}')
    s = read_session()
    print(f'[冷启动后] 存档存在={s is not None} 消息数={len(s["messages"]) if s else 0}')
    print(f'          config={s["config"] if s else None}')

    # ② 发一条消息，等流式结束
    port, token = ctl['port'], ctl.get('token', '')
    lib.call_with_token(port, 'set', {'id': 'msg-input', 'props': {'value': '测试持久化'}}, token=token)
    lib.call_with_token(port, 'invoke', {'id': 'btn-send', 'action': 'click'}, token=token)
    print('[发送] 已发出，等待流式…')
    for i in range(40):
        time.sleep(1)
        s = read_session()
        n = len(s['messages']) if s else 0
        if i % 5 == 0:
            print(f'   t={i}s 存档消息数={n}')
        # 稳态判定：连续 2 秒消息数不变且 > 2
        if n >= 3:
            time.sleep(2)
            s2 = read_session()
            if s2 and len(s2['messages']) == n:
                break

    s = read_session()
    print(f'\n[存档] 消息数={len(s["messages"])}')
    for m in s['messages'][:6]:
        role = m.get('role')
        content = (m.get('content') or '')[:50].replace('\n', ' ')
        reason = len(m.get('reasoning') or '')
        calls = len(m.get('tool_calls') or [])
        print(f'   {role:9} 正文{len(m.get("content") or ""):5} 思考{reason:5} 工具{calls}  {content}')

    # ③ 关掉，重启，验证恢复
    lib.call_with_token(port, 'shutdown', {}, token=token)
    time.sleep(1.5)
    try:
        proc.terminate()
    except Exception:
        pass
    time.sleep(1)

    before = len(read_session()['messages'])
    proc2, ctl2 = start_app()
    if not ctl2:
        print('重启失败'); return 1
    time.sleep(2)
    port2, token2 = ctl2['port'], ctl2.get('token', '')
    r = lib.call_with_token(port2, 'get', {'id': 'msg-scroll'}, token=token2)
    props = (r.get('result') or {}).get('props') or {}
    print(f'\n[重启后] 消息区 content_height={props.get("content_height")}')
    # 数一下界面上有多少气泡
    tr = lib.call_with_token(port2, 'find', {'selector': 'Card', 'limit': 100}, token=token2)
    cards = ((tr.get('result') or {}).get('matches') or [])
    print(f'[重启后] 界面 Card 数={len(cards)}  存档消息数={before}')
    lib.call_with_token(port2, 'shutdown', {}, token=token2)
    return 0


if __name__ == '__main__':
    sys.exit(main())
