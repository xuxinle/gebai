"""JUnit 分片报告比对（口径：用例名集合 + 通过状态）。

用途：证明"多进程分片 == 单进程"不是想当然。只比计数会漏掉
"某用例被换成了另一条"这种错，所以按**名字集合**与**状态**双向比。

用法：
    python tools/compare_junit_shards.py 串行.xml 并行.xml [更多.xml...]
退出码：0 = 一致；1 = 有差异（并打印差异明细）。
"""
import re
import sys
import xml.etree.ElementTree as ET


def load(path):
    tree = ET.parse(path)
    out = {}
    for case in tree.iter("testcase"):
        name = case.get("name")
        failed = case.find("failure") is not None
        out[name] = failed
    return out


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    base = load(argv[1])
    ok = True
    for path in argv[2:]:
        other = load(path)
        missing = sorted(set(base) - set(other))
        extra = sorted(set(other) - set(base))
        diff = sorted(k for k in set(base) & set(other) if base[k] != other[k])
        line = ("%-32s 用例 %4d / %4d  缺失 %d  多余 %d  状态不一致 %d"
                % (path, len(other), len(base), len(missing), len(extra), len(diff)))
        if missing or extra or diff:
            ok = False
            line += "   <== 不一致"
        print(line)
        for name in missing[:5]:
            print("    缺失: %s" % name)
        for name in extra[:5]:
            print("    多余: %s" % name)
        for name in diff[:5]:
            print("    状态不一致: %s（串行 failed=%s, 本次 failed=%s）" % (name, base[name], other[name]))
    print("一致" if ok else "有差异")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
