"""把导出的资产 JSON 内联进组装脚本，生成可直接运行的场景。

## 为什么需要这一步

本框架的 three 脚本**没有文件读取能力**（QuickJS 沙箱里没有 fs / fetch）。
所以资产不能运行时加载，必须在**生成期**内联进脚本。

这是"示例级"实现的代价：资产变了要重新跑一次这个工具。真正的运行时
资产加载需要框架支持（`scene.assets` 段或跨解释器传几何），见
`docs/render_alignment.md` 的待办。

## 用法

    py tools/inline_assets.py <组装脚本.js> <资产.json> <输出.js>

## 做了什么

把 `__ASSETS__` 占位符替换成资产 JSON 的 `assets` 对象字面量。
"""

import json
import sys


def main(argv):
    if len(argv) < 4:
        print('用法：py tools/inline_assets.py <组装脚本.js> <资产.json> <输出.js>')
        return 2
    script_path, asset_path, out_path = argv[1], argv[2], argv[3]

    with open(script_path, 'r', encoding='utf-8') as handle:
        script = handle.read()
    with open(asset_path, 'r', encoding='utf-8') as handle:
        payload = json.load(handle)

    assets = payload.get('assets') or {}
    if not assets:
        print('资产文件里没有 assets：%s' % asset_path)
        return 1

    # 紧凑但有换行（便于 diff 与排查；纯一行会让文件变成几 MB 的单行）
    parts = []
    for name in sorted(assets):
        parts.append('  %s: %s' % (json.dumps(name), json.dumps(assets[name],
                                                              separators=(',', ':'))))
    literal = '{\n' + ',\n'.join(parts) + '\n}'

    if '__ASSETS__' not in script:
        print('组装脚本里没有 __ASSETS__ 占位符')
        return 1
    output = script.replace('__ASSETS__', literal)

    with open(out_path, 'w', encoding='utf-8', newline='\n') as handle:
        handle.write(output)

    total = sum(len(a.get('positions', [])) // 3 for a in assets.values())
    print('已生成 %s' % out_path)
    print('  资产 %d 个，顶点 %d，%d 字节' % (len(assets), total, len(output)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
