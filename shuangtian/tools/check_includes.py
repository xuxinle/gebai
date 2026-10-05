#!/usr/bin/env python3
"""IWYU 体检：列出每个源文件「直接用到的 std 实体」与「显式 include 的标准库头」的差集。

判定原则（CONVENTIONS §6.6）：翻译单元自包含——直接使用的标准库实体，
必须有对应的显式 #include，不得依赖 st/pch.hpp 或其他头的传递引入。
"""
import re
import sys
from pathlib import Path

STD_TO_HEADER = {
    'std::size_t': '<cstddef>', 'std::ptrdiff_t': '<cstddef>', 'std::nullptr_t': '<cstddef>',
    'std::byte': '<cstddef>',
    'std::string_view': '<string_view>',
    'std::string': '<string>', 'std::to_string': '<string>', 'std::stoi': '<string>',
    'std::vector': '<vector>', 'std::array': '<array>', 'std::span': '<span>',
    'std::map': '<map>', 'std::multimap': '<map>',
    'std::set': '<set>', 'std::multiset': '<set>',
    'std::unordered_map': '<unordered_map>', 'std::unordered_set': '<unordered_set>',
    'std::optional': '<optional>', 'std::nullopt': '<optional>',
    'std::variant': '<variant>', 'std::visit': '<variant>',
    'std::unique_ptr': '<memory>', 'std::shared_ptr': '<memory>',
    'std::make_unique': '<memory>', 'std::make_shared': '<memory>',
    'std::move': '<utility>', 'std::forward': '<utility>', 'std::pair': '<utility>',
    'std::swap': '<utility>', 'std::exchange': '<utility>',
    'std::sort': '<algorithm>', 'std::find': '<algorithm>', 'std::min': '<algorithm>',
    'std::max': '<algorithm>', 'std::clamp': '<algorithm>', 'std::ranges': '<algorithm>',
    'std::format': '<format>', 'std::vformat': '<format>', 'std::formatter': '<format>',
    'std::isalnum': '<cctype>', 'std::isspace': '<cctype>', 'std::isdigit': '<cctype>',
    'std::tolower': '<cctype>', 'std::toupper': '<cctype>',
    'std::function': '<functional>',
    'std::chrono': '<chrono>',
    'std::atomic': '<atomic>',
    'std::mutex': '<mutex>', 'std::lock_guard': '<mutex>', 'std::scoped_lock': '<mutex>',
    'std::jthread': '<thread>', 'std::thread': '<thread>',
    'std::filesystem': '<filesystem>',
    'std::expected': '<expected>', 'std::unexpected': '<expected>',
    'std::bit_cast': '<bit>',
    'std::tuple': '<tuple>', 'std::tie': '<tuple>',
    'std::source_location': '<source_location>',
    'std::uint8_t': '<cstdint>', 'std::uint16_t': '<cstdint>', 'std::uint32_t': '<cstdint>',
    'std::uint64_t': '<cstdint>', 'std::int8_t': '<cstdint>', 'std::int16_t': '<cstdint>',
    'std::int32_t': '<cstdint>', 'std::int64_t': '<cstdint>',
    'std::snprintf': '<cstdio>', 'std::fprintf': '<cstdio>', 'std::printf': '<cstdio>',
    'std::sqrt': '<cmath>', 'std::abs': '<cmath>', 'std::floor': '<cmath>',
    'std::ceil': '<cmath>', 'std::round': '<cmath>', 'std::pow': '<cmath>',
    'std::numeric_limits': '<limits>',
    'std::initializer_list': '<initializer_list>',
    'std::runtime_error': '<stdexcept>', 'std::exception': '<stdexcept>',
    'std::ostringstream': '<sstream>', 'std::istringstream': '<sstream>',
    'std::stringstream': '<sstream>',
    'std::regex': '<regex>',
    'std::priority_queue': '<queue>', 'std::queue': '<queue>', 'std::deque': '<deque>',
    'std::cout': '<iostream>', 'std::cerr': '<iostream>',
    'std::invoke': '<functional>', 'std::apply': '<functional>',
}

# 这些实体在多个头里都有定义，任一头显式包含即可
ALIASES = {
    '<cstddef>': {'<cstddef>', '<cstdint>', '<cstdio>', '<cstdlib>'},
    '<string>': {'<string>', '<string_view>', '<sstream>', '<filesystem>'},
    '<string_view>': {'<string_view>', '<string>'},
    '<utility>': {'<utility>', '<tuple>', '<memory>', '<functional>'},
    '<algorithm>': {'<algorithm>', '<ranges>'},
    '<vector>': {'<vector>'},
    '<cstdint>': {'<cstdint>', '<cstddef>'},
}

COMMENT_RE = re.compile(r'//[^\n]*|/\*.*?\*/', re.S)


def strip_comments(text):
    return COMMENT_RE.sub(lambda m: '\n' * m.group(0).count('\n'), text)


def analyze(path):
    text = Path(path).read_text(encoding='utf-8', errors='replace')
    body = strip_comments(re.sub(r'^#\s*include.*$', '', text, flags=re.M))
    explicit = {'<' + h + '>' for h in re.findall(r'^#\s*include\s*<([^>]+)>', text, re.M)}
    needed = {}
    for ent, hdr in STD_TO_HEADER.items():
        if re.search(r'\b' + re.escape(ent) + r'\b', body):
            needed.setdefault(hdr, set()).add(ent)
    missing = {}
    for hdr, ents in needed.items():
        allowed = ALIASES.get(hdr, {hdr})
        if not (allowed & explicit):
            missing[hdr] = ents
    return explicit, needed, missing


def main():
    root = Path(sys.argv[1] if len(sys.argv) > 1 else '.')
    total_files = 0
    offenders = []
    for pattern in ('src/**/*.cpp', 'tests/*.cpp', 'examples/**/*.cpp', 'tools/*.cpp'):
        for f in sorted(root.glob(pattern)):
            if 'third_party' in str(f):
                continue
            total_files += 1
            explicit, needed, missing = analyze(f)
            if missing:
                offenders.append((str(f.relative_to(root)), missing))
    print(f"扫描 {total_files} 个 .cpp，其中 {len(offenders)} 个存在「依赖传递包含」\n")
    offenders.sort(key=lambda x: -len(x[1]))
    for name, missing in offenders:
        detail = ' '.join(f"{h}({','.join(sorted(e))})" for h, e in sorted(missing.items()))
        print(f"{name}\n    {detail}")
    print(f"\n合计缺失显式包含 {sum(len(m) for _, m in offenders)} 处")


if __name__ == '__main__':
    main()
