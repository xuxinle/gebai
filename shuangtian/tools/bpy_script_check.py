"""用真 CPython 跑 bpy_watch.py，把 bpy/bmesh/mathutils 打桩。

## 为什么这样做

MicroPython 在 MINIMUM 档下**异常信息不完整**：

    NameError: name not defined        ← 不说是哪个名字
    TypeError: unsupported type        ← 不说是什么类型

对 400 行脚本盲猜是没戏的。而真 CPython 会把**完整**信息打出来
（`NameError: name 'foo' is not defined` + 行号 + 调用栈）。

所以先用 CPython 把**语法与名字解析**问题挑干净（那是脚本本身的错），
剩下的才可能是 MicroPython 的能力限制（比如不支持 `@`）。

## 用法

    py tools/bpy_script_check.py examples/jianmu/scenes/bpy_watch.py

退出码 0 = 脚本在 CPython 下能跑通（bpy 打桩）。
"""

import sys
import types


class _Stub:
    """能接受任意调用/属性/下标/算术的万能桩。"""

    def __init__(self, name='stub'):
        self._name = name

    def __call__(self, *args, **kwargs):
        return _Stub(self._name + '()')

    def __getattr__(self, item):
        if item.startswith('__') and item.endswith('__'):
            raise AttributeError(item)
        return _Stub(self._name + '.' + item)

    def __setattr__(self, key, value):
        if key == '_name':
            object.__setattr__(self, key, value)
            return
        # 允许脚本给桩对象设属性（bpy 脚本里到处是 obj.location = ...）

    def __getitem__(self, item):
        return _Stub(self._name + '[]')

    def __setitem__(self, key, value):
        pass

    def __len__(self):
        return 8

    def __iter__(self):
        return iter([])

    def __add__(self, other):
        return _Stub('+')

    __radd__ = __add__

    def __sub__(self, other):
        return _Stub('-')

    def __mul__(self, other):
        return _Stub('*')

    __rmul__ = __mul__

    def __truediv__(self, other):
        return _Stub('/')

    def __neg__(self):
        return _Stub('-')

    def __float__(self):
        return 0.0

    def __int__(self):
        return 0

    def __repr__(self):
        return '<%s>' % self._name


def _module(name, **attrs):
    module = types.ModuleType(name)
    for key, value in attrs.items():
        setattr(module, key, value)
    return module


def install_stubs():
    """把 bpy / bmesh / mathutils 装成桩模块。"""
    bpy = _module('bpy')
    bpy.data = _Stub('bpy.data')
    bpy.context = _Stub('bpy.context')
    bpy.ops = _Stub('bpy.ops')
    bpy.app = _Stub('bpy.app')

    bmesh = _module('bmesh')
    bmesh.new = lambda: _Stub('bm')

    mathutils = _module('mathutils')
    for name in ('Vector', 'Matrix', 'Euler', 'Quaternion'):
        setattr(mathutils, name, _Stub(name))

    sys.modules['bpy'] = bpy
    sys.modules['bmesh'] = bmesh
    sys.modules['mathutils'] = mathutils


def main():
    if len(sys.argv) < 2:
        print('用法：py tools/bpy_script_check.py <脚本.py>')
        return 2
    path = sys.argv[1]
    with open(path, encoding='utf-8') as handle:
        source = handle.read()

    install_stubs()
    namespace = {'__name__': '__main__', '__file__': path}
    try:
        compiled = compile(source, path, 'exec')
    except SyntaxError as exc:
        print('语法错：第 %s 行：%s' % (exc.lineno, exc.msg))
        print('  源码：%s' % (exc.text or '').rstrip())
        return 1

    try:
        exec(compiled, namespace)
    except NameError as exc:
        # 这是最想抓的一类：MINIMUM 档下 MicroPython 会吞掉名字
        print('NameError：%s' % exc)
        import traceback
        traceback.print_exc(limit=6)
        return 1
    except Exception as exc:  # noqa: BLE001 - 诊断工具故意兜住全部
        print('%s：%s' % (type(exc).__name__, exc))
        import traceback
        traceback.print_exc(limit=6)
        return 1

    print('CPython 下执行通过（bpy 打桩）')
    print('顶层定义：%s' % ', '.join(sorted(
        k for k in namespace if not k.startswith('__'))))
    return 0


if __name__ == '__main__':
    sys.exit(main())
