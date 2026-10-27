"""把七份多镜承接示例与配套片段登记进资产库。

## 为什么用脚本登记而不是手改 JSON

`library.json` 已有 16 条、结构固定（id/kind/path/summary/use_for/tags/verified/
measured/highlights/caveats）。手改容易漏字段或写坏缩进，而**资产库写坏了是静默的**
（`assets` 工具解析失败会退回"库尚未建立"，模型以为没资产）。
脚本登记保证结构一致，且可重复执行（按 id 去重）。

## 数据来源

`measured` 不是编的：七份的 carry score 与在动帧比均出自
`tools/caiyun_audit_multishot.py` 的同一次实测（2026-10-27）。

用法：`python tools/caiyun_register_multishot.py`
"""

import io
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIB = os.path.join(ROOT, "examples", "caiyun", "assets", "library.json")

# 实测结果（tools/caiyun_audit_multishot.py 一次跑出，2026-10-27）
# 注意：`moving` 是**像素口径**（64 宽面积加权，小元素会被稀释），只作粗筛；
# 真正的「有没有在动」看 `traj_still`（元素轨迹真值）。两者差距最大的 07：
# 像素报 93.2% 静止，轨迹口径只有 66.5%——差异全部来自「40px 的点在动」。
MEASURED = {
    "01-capability-list": {"carry": 1.00, "moving": 6.8, "traj_still": 32.5, "carrier": "光标·本体"},
    "02-single-point": {"carry": 1.00, "moving": 12.5, "traj_still": 36.6, "carrier": "单点·本体"},
    "03-causal-chain": {"carry": 1.00, "moving": 9.3, "traj_still": 22.8, "carrier": "火花·动量"},
    "04-scale-dive": {"carry": 1.00, "moving": 35.2, "traj_still": 38.8, "carrier": "锚点·本体"},
    "05-contrast-split": {"carry": 1.00, "moving": 16.4, "traj_still": 16.6, "carrier": "对照·中线"},
    "06-spine": {"carry": 1.00, "moving": 18.2, "traj_still": 21.4, "carrier": "脊椎·主线"},
    "07-pointer": {"carry": 1.00, "moving": 7.1, "traj_still": 66.5, "carrier": "角色·本体"},
}

# 每份的承接手法（取自该模板的 method 文档，不是编的）
CARRIER_NOTE = {
    "01-capability-list": "因果：手是开关。光标点下去，界面才响应（响应晚 0.05~0.12s）",
    "02-single-point": "自身 morph：点 → 线 → 面，全程只有一个元素在变形",
    "03-causal-chain": "动量：被撞飞的方块跨幕砸在下一环上，大位移而非原地变形",
    "04-scale-dive": "锚点：同一块从 40px 长到满屏成背景版式，再缩回胶囊（跨尺度）",
    "05-contrast-split": "并置：竖中线加宽成带、再转成横线，两态始终围着它",
    "06-spine": "主线：一条线拉长、串起三幕，每次偏离都回到它",
    "07-pointer": "角色即指针：同一个点跨三幕，每个界面反应都能追溯到它的动作",
}

ENTRIES = []
for name, m in MEASURED.items():
    ENTRIES.append({
        "id": f"multishot-{name[:2]}-{name[3:]}",
        "kind": "script",
        "path": f"examples/caiyun/styles/{name}/film-multishot.json",
        "summary": (
            f"{name[3:]} 模板的三镜承接示例（承接分 {m['carry']:.2f}，"
            f"承接物：{m['carrier']}）"
        ),
        "use_for": (
            "要学「分镜之间怎么接」时读它——一镜版 film.json 演示不了承接，"
            "而承接是唯一能判定像不像 PPT 的指标"
        ),
        "tags": ["script", "multishot", "carry", "transition", name[3:],
                 name[:2], "three-shot"],
        "verified": "verified",
        "measured": {
            "carry_score": m["carry"],
            "moving_pair_ratio": m["moving"],
            "still_ratio_trajectory": m["traj_still"],
            "scenes": 3,
        },
        "highlights": [
            f"承接手法：{CARRIER_NOTE[name]}",
            "carry_out 的 from 必须与该幕末帧实景吻合、to 必须与下幕 from 完全一致",
            "布局里不要再画承接物（由 carry 绘制，两边都画会双影）",
            "每幕文字层必须逐幕不同名（title0/title1/title2）——"
            "重名会让 carry_qa 把两个不同文案误判成「同一元素存活」",
        ],
        "caveats": [
            f"像素口径静止占比会报 85~95%，而元素轨迹口径只有 {m['traj_still']}%——"
            "两者都是真的，差在量法：64 宽面积加权会把小元素运动稀释掉。判“是否卡住”看轨迹",
            "bodies/ 改完必须重跑 tools/caiyun_gen_layouts.py 生成 layout-ms*.js，"
            "否则改动静默不生效",
        ],
    })

SNIPPET = {
    "id": "multishot-carry-recipe",
    "kind": "snippet",
    "inline": True,
    "summary": "多镜承接的通用配方：三幕骨架 + carry_out 链 + 两个必查的断点",
    "use_for": "任何需要多镜片的场景；照它搭骨架，再按模板换承接物与隐喻",
    "tags": ["carry", "multishot", "layout", "transition"],
    "verified": "verified",
    "highlights": [
        "三幕骨架：幕 0 立主体（3.0s）→ 幕 1 展开（3.4s）→ 幕 2 收束（3.0s）。时长不能等分（CV<0.25 判 PPT）",
        "carry_out 写法：at 在幕内、at+dur 跨过边界（dur 3.0 的幕用 at=2.7 dur=0.7）",
        "链的闭合：幕 N 的 to 必须逐字等于幕 N+1 的 from——这是唯一定义『同一个东西』的地方",
        "layout 分文件写（每幕一份），不要靠 duration 判断当前是哪一幕"
        "（改名/改时长就会串场，且静默降级）",
        "每幕末帧必须与 carry_out.from 吻合：若幕内让元素长大/鼓起，必须收回再进边界",
        "**carry_out 只在 at~at+dur 那 0.7s 绘制，之后什么都不画**——"
        "目标场景必须自己再画一份终态（值与 carry_out.to 完全一致），"
        "否则“它一直在”会变成“它消失了”",
    ],
    "caveats": [
        "carry_out 不许整屏不透明盖上（会盖掉下一幕的文字）；铺满阶段用 fade_out 让位",
        "**双影是固有陷阱**：carry 与目标场景自绘框会同时可见（实测错开 3px，帧上看是两个套着的框）。"
        "解法：carry_out.fade_out 0.35 + 自绘层在**同一个窗口** alpha 0→1 淡入",
        "淡入/滑入不算承接（那是换掉的礼貌说法）——承接分 0.00 会被判不合格",
        "没有 carry_out 也能承接（靠坐标对齐），但拿不到承接分；只在边界本就该安静时用",
    ],
}


def main() -> int:
    if not os.path.exists(LIB):
        print(f"资产库不存在：{LIB}")
        return 2
    lib = json.load(io.open(LIB, encoding="utf-8"))
    before = len(lib["assets"])

    # 按 id 去重（可重复执行）
    lib["assets"] = [a for a in lib["assets"] if not a["id"].startswith("multishot-")]
    lib["assets"].extend(ENTRIES)
    lib["assets"].append(SNIPPET)

    with io.open(LIB, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(lib, fh, ensure_ascii=False, indent=2)
        fh.write("\n")

    print(f"登记 {len(ENTRIES)} 条多镜示例 + 1 条配方片段（{before} → {len(lib['assets'])} 条）")
    for e in ENTRIES:
        print(f"  {e['id']:32s} carry={e['measured']['carry_score']:.2f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
