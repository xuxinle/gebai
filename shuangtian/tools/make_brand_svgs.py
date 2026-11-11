#!/usr/bin/env python3
"""生成各应用的**标题栏品牌标记**（SVG，矢量）。

## 为什么要单独一份 SVG，而不是复用 .ico / PNG

两处图标走的是**不同链路**，用途也不同：

| | 来源 | 给谁看 | 关键要求 |
|---|---|---|---|
| 任务栏/窗口图标 | `window_icon_png` → 后端 `set_window_icon` | **窗口系统** | 只吃位图；尺寸由系统给（16/32/48） |
| 标题栏品牌标记 | `svg_registry()` → `TitleBar::set_icon` | **自绘画面** | 任意 DPI 重新光栅化，必须矢量 |

用位图当标题栏图标，在高 DPI（200%）下会被放大糊掉；用矢量当窗口图标，
系统 API 又只接受位图。所以两者并存，但**同一记号**（同配色、同构形）——
否则用户看到的是"两个不同的东西"（xiangqi 的注释里记过这个教训）。

## 家族语言（与 gbcode 的 `< >` 一致）

- 24×24 视框；蓝/红/青等**应用主色**的圆角方底（21×21，圆角 5.25）
- 前景单色（白），**纯填充、无描边**——16px 下描边会与抗锯齿糊成灰边
- 每个标记只用 1~3 个大特征：小尺寸是"特征逐个塌陷"，不是等比缩小

用法：
    py -3 tools/make_brand_svgs.py          # 全部重写
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# 家族底：圆角方（21×21 居 24 视框，圆角 5.25 ≈ 25%）
BASE = ('  <path fill="{color}" d="M6.75 0.5 L17.25 0.5 C20.15 0.5 22.5 2.85 22.5 5.75 '
        'L22.5 16.25\n    C22.5 19.15 20.15 21.5 17.25 21.5 L6.75 21.5 C3.85 21.5 1.5 19.15 '
        '1.5 16.25\n    L1.5 5.75 C1.5 2.85 3.85 0.5 6.75 0.5 Z"/>')


def svg(color: str, body: str, comment: str) -> str:
    return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24">
  <!-- {comment}

       家族语言（与 gbcode 的 `< >` 同源）：圆角方底 + 白色单色前景、纯填充无描边。
       几何约定见 tools/make_brand_svgs.py 的文件头。 -->
{BASE.format(color=color)}
  <g fill="#FFFFFF">
{body}
  </g>
</svg>
'''


# —— 各应用标记 ——
# 配色与 make_icons.py 里的 PNG 图标主色一致（同一记号，两个链路）

GALLERY = svg("#5C6BDB", '''    <rect x="4.6" y="4.6" width="5.4" height="5.4" rx="1.4"/>
    <rect x="11.3" y="4.6" width="5.4" height="5.4" rx="1.4" opacity="0.5"/>
    <rect x="4.6" y="11.3" width="5.4" height="5.4" rx="1.4" opacity="0.5"/>
    <rect x="11.3" y="11.3" width="5.4" height="5.4" rx="1.4"/>
    <rect x="17.9" y="4.6" width="1.6" height="12.1" rx="0.8" opacity="0.32"/>''',
    "gallery —— 组件画廊。九宫格的四个角块（对角线同亮、另两个半透明），\n"
    "       加上右侧一道窄条暗示\"还有更多\"。不做 3×3 全格：9 个块在 16px 下会糊成一片灰。")

LOUYUE = svg("#D96B5C", '''    <rect x="5.2" y="4.4" width="13.6" height="15.2" rx="1.6" opacity="0.30"/>
    <rect x="7.2" y="6.6" width="9.6" height="2.4" rx="0.9"/>
    <rect x="7.2" y="11.2" width="4.6" height="1.4" rx="0.7" opacity="0.85"/>
    <rect x="7.2" y="13.8" width="5.8" height="1.4" rx="0.7" opacity="0.85"/>
    <rect x="13.6" y="11.2" width="3.2" height="4.6" rx="0.9" opacity="0.6"/>''',
    "louyue —— 镂月（海报/排版）。一张版式：标题横条 + 两行正文 + 右侧图像块。\n"
    "       16px 下正文行会塌陷，标题条与图像块仍在——宁可少特征，不要糊成一团。")

CAIYUN = svg("#339E99", '''    <rect x="4.2" y="6.4" width="15.6" height="11.2" rx="1.5" opacity="0.30"/>
    <rect x="5.6" y="4.6" width="2.0" height="2.0" rx="0.6" opacity="0.85"/>
    <rect x="9.4" y="4.6" width="2.0" height="2.0" rx="0.6" opacity="0.85"/>
    <rect x="13.2" y="4.6" width="2.0" height="2.0" rx="0.6" opacity="0.85"/>
    <rect x="5.6" y="17.4" width="2.0" height="2.0" rx="0.6" opacity="0.85"/>
    <rect x="9.4" y="17.4" width="2.0" height="2.0" rx="0.6" opacity="0.85"/>
    <rect x="13.2" y="17.4" width="2.0" height="2.0" rx="0.6" opacity="0.85"/>
    <path d="M10.4 9.2 L10.4 14.8 L15.2 12.0 Z"/>''',
    "caiyun —— 裁云（AI 视频）。胶片框 + 上下齿孔 + 播放三角。\n"
    "       三角是主特征（最先被识别），齿孔在 16px 下退化为上下两条点线——可接受。")

GEDOU = svg("#C74252", '''    <!-- 拳套：拳身（圆角方）+ 拇指包边 + 护腕 -->
    <rect x="8.4" y="5.6" width="8.4" height="9.6" rx="3.2"/>
    <rect x="6.2" y="8.6" width="3.4" height="5.0" rx="1.6" opacity="0.9"/>
    <rect x="8.8" y="14.0" width="7.6" height="3.0" rx="1.2" opacity="0.75"/>
    <!-- 指节凹槽：两道短横（16px 下会塌陷，拳形仍在） -->
    <path d="M10.6 8.2 L14.6 8.2 M10.6 10.6 L14.6 10.6"
          stroke="#C74252" stroke-width="1.1" stroke-linecap="round" opacity="0.85"/>
    <!-- 冲击线：右侧两道 -->
    <path d="M18.4 8.0 L20.4 8.0 M18.4 14.4 L20.2 14.4"
          stroke="#FFFFFF" stroke-width="1.4" stroke-linecap="round" opacity="0.7"/>''',
    "gedou —— 格斗。一只拳套（圆角拳形 + 护腕）+ 两侧冲击线。\n"
    "       ⚠ 冲击线用了 stroke：它们**不是**图标主体，16px 下消失也不影响识别\n"
    "       （主体拳形是纯填充）。这比\"两圆夹一球\"（像哑铃）明确得多。")

CEFU = svg("#6B5CAD", '''    <rect x="5.0" y="6.2" width="10.2" height="3.0" rx="1.2"/>
    <rect x="5.7" y="10.4" width="10.2" height="3.0" rx="1.2" opacity="0.72"/>
    <rect x="6.4" y="14.6" width="10.2" height="3.0" rx="1.2" opacity="0.48"/>
    <circle cx="16.6" cy="9.4" r="3.4" fill="none" stroke="#FFFFFF" stroke-width="1.6"/>
    <path d="M19.0 11.8 L21.2 14.0" stroke="#FFFFFF" stroke-width="1.8" 
          stroke-linecap="round"/>''',
    "cefu —— 册府（结构化知识库）。三本叠放的书（透明度递减表达\"摞\"）+ 检索环。\n"
    "       环是空心圆（stroke）——放大镜的\"孔\"必须是空的，否则与书本糊成一坨。")

XIANGQI = svg("#3D4249", '''    <rect x="4.6" y="5.0" width="14.8" height="14.0" rx="2.6" opacity="0.32"/>
    <circle cx="12.0" cy="10.2" r="2.6"/>
    <path d="M7.9 16.6 C7.9 14.2 9.8 12.9 12.0 12.9 C14.2 12.9 16.1 14.2 16.1 16.6 Z"/>''',
    "xiangqi —— 相期（桌面常驻面板）。卡片底 + 人像（头 + 肩）。\n"
    "       \"有个人在等你\"是它的语义，比原来的 bell（铃铛=通知）更准：\n"
    "       它不只是提醒，是**待办/等待用户介入**。")

JIANMU = svg("#2F6E52", '''    <path d="M12 10.359 L16.547 12.984 L12 15.609 L7.454 12.984 Z" opacity="0.66"/>
    <path d="M12 15.609 L12 20.859 L7.454 18.234 L7.454 12.984 Z" opacity="0.40"/>
    <path d="M12 15.609 L12 20.859 L16.547 18.234 L16.547 12.984 Z" opacity="0.24"/>
    <g stroke="#FFFFFF" stroke-linecap="round">
      <path d="M12 13.805 L12 5.438" stroke-width="1.477"/>
      <path d="M12 9.047 L9.703 6.586 M12 9.047 L14.297 6.586" stroke-width="1.066"/>
    </g>''',
    "jianmu —— 建木（三维建模与演示）。等轴测立方体 + 顶上长出的树。\n"
    "       立方体说\"三维\"，树说\"建木\"（纯几何体太像任意 3D 软件）。\n"
    "       树干从顶面中心起笔、往下压进顶面——不压进去会读成\"浮在方块上方\"；\n"
    "       顶面明度必须与树干拉开（0.66 / 左0.40 / 右0.24）：初版顶面 0.88 时\n"
    "       纯白树干与纯白顶面连成一片，树干几乎看不见。\n"
    "       坐标与 tools/make_icons.py 的 icon_jianmu 同源：256 画布按 21/255 映射进\n"
    "       1.5..22.5 的家族底，两边改一边必改另一边。")

APPS = {
    "gallery": ("apps/gallery/assets/gallery-logo.svg", GALLERY),
    "louyue":  ("apps/louyue/assets/louyue-logo.svg",   LOUYUE),
    "caiyun":  ("apps/caiyun/assets/caiyun-logo.svg",   CAIYUN),
    "gedou":   ("apps/gedou/assets/gedou-logo.svg",     GEDOU),
    "cefu":    ("apps/cefu/assets/cefu-logo.svg",       CEFU),
    "xiangqi": ("apps/xiangqi/assets/xiangqi-logo.svg", XIANGQI),
    "jianmu":  ("apps/jianmu/assets/jianmu-logo.svg",   JIANMU),
}


def main():
    only = sys.argv[1:] or list(APPS)
    for name in only:
        if name not in APPS:
            print(f"未知应用：{name}（可选：{', '.join(APPS)}）")
            return 2
        rel, content = APPS[name]
        path = ROOT / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        print(f"{name:9s} -> {rel}  ({len(content)} 字节)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
