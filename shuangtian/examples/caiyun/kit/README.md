# caiyun-kit —— 裁云的复用层

> 目标：**写一部新片子，只写它独有的东西**。通用能力（缓动、相机、背景、
> 图元工厂、常见动效）都在这里，且带着前 13 部片子的实测教训。

## 三件套

| 目录/文件 | 是什么 | 什么时候用 |
|---|---|---|
| `caiyun-kit.js` | 能力层（纯函数，无宿主依赖） | 永远用 |
| `templates/tpl-*.js` | 起点模板（两份：单体特写 / 多元素场景） | 开新片子 |
| `RECIPES.md` | 场景配方（产品界面 / 数据 / 图标阵 / 收尾 / 转场） | 写具体段落 |

配套工具（在 `shuangtian/` 下）：

| 工具 | 作用 |
|---|---|
| `kit/build.py` | 把 kit 与你的脚本拼成单文件（裁云无模块系统） |
| `scripts/check_layout.py` | **渲染前**逐帧跑 layout，抓运行期错误 |
| `tools/test_caiyun_kit.cjs` | kit 自己的回归（46 条断言，每条对应一条教训） |

## 五分钟上手

```bash
# ① 从模板起一份
cp examples/caiyun/kit/templates/tpl-single-subject.js my-film.js
# 改 PARAMS / STORY / ③ 那一段

# ② 拼成裁云能吃的单文件（把 kit 拼在前面）
py examples/caiyun/kit/build.py --script my-film.js --out _build/my-film.bundle.js

# ③ 先检查，再渲染（10 秒，省几十分钟）
py scripts/check_layout.py _build/my-film.bundle.js --duration 15

# ④ json 的 layout 指向 bundle，然后照常出片
```

## 为什么是"拼接"而不是 import

裁云的 `layout` 是**单文件整体 eval**（`LayoutEngine::load` → `engine.eval(source)`），
脚本引擎没有模块系统。`import` / `require` 都会失败。

所以 `build.py` 做的是最朴素的事：把 kit 正文拼在你的脚本前面。你的脚本仍是
普通单文件、仍能独立读懂。产物顶部会写清"报错时行号减多少"，方便对账。

## 能力清单

### 缓动与关键帧
`clamp` `lerp` `seg` `sstep` `ssstep` `ease{...}` `keys(t, list)`

`keys` 与各 case 脚本里手写的那份**同语义**（第三项是缓动名，缺省 linear）。
现在统一走 kit 这份，避免"每个脚本一份、改了一处忘了别的"。

### 相机 `camera({keys, ax, ay, creep})`

**默认叠 creep（持续微动）**。这是最重要的一条：

> `keys()` 两端值相同时输出恒定，而**相机站定就是画面站定**。
> 实测 5 个 case 里 4 个的"静止占比过高"主因是这个。

要真正定格（品牌定格）时显式传 `creep: 0` —— 那时静止是**设计意图**。

返回 `{zoom(t), anchor(t), at(t,x,y)}`，三者口径一致（自测里有一条断言专门查这个）。

### 背景纹理 `lattice({step, size, alpha, fill, skipBands, drop})`

给相机运动提供**可测参照**。尺寸不是按好看定的：

```
评测网格 32×18（一格 60×60px），静止阈值 1.2（0~255）
Δ ≈ 2·d·s / 3600        d = 点直径，s = 相机平移像素
```

实测（c02-onedot，只改这一点）：

| d | 间距 | p95 | 静止占比 |
|---|---|---|---|
| 2.6 | 48 | 1.63 | 90.9%（不合格） |
| **14** | 60 | **3.22** | **54.6%（通过）** |

**放法也决定成败**：必须放在相机变换**之前**的 shapes 里。
屏幕空间固定的纹理在推镜时纹丝不动，实测 p95 掉回 1.61。

`skipBands` 让开文字横带——文字由**图层**画，而图层在 shapes 之下，
不让开就会被点阵盖住（抽帧可见）。

### 图元工厂 `shapes.{circle, rect, line, text}`

包一层是为了**字段名不出错**。裁云的 shape 字段是
`kind/x/y/w/h/radius/fill/color/stroke/strokeWidth/text`，
写错名字不会报错、只是不生效 —— 这类静默失败最难查。

注意 `line` 用 `x2/y2` 表示终点（**不是** w/h），`text` 的 `h` 是字号。

### 常见动效
`typeOn(str,t,start,cps)` · `wordRise(t,start,dur,dist)` · `stagger(i,gap,jitter)` · `breathe(t,amp,hz)`

`breathe` 的幅度默认 20px —— **1~2px 的微动在 60px 的格里测不到**，
这是"加了个呼吸但是指标没变"的常见原因。

### 确定性 `hash01(n)`

同输入同输出，替代 `Math.random`（裁云要求逐帧可复现）。

## 硬约束（违反任何一条，kit 的自测会红）

1. 不用 `Math.random` / `Date.now` / `performance.now`
2. 不用 `import` / `require`
3. 不引用宿主函数（`ease` 等由裁云注入，kit 里不能再声明同名）
4. 所有函数纯函数：同参数同结果

## 自测

```bash
node tools/test_caiyun_kit.cjs      # 46 条断言
```

每条断言都标注了它对应的实测教训。**并且做过逆向验证**：
故意回退某个修复（如把 lattice 默认尺寸改回 2.6），测试必须变红——
第一版就没抓住（只查了点数量没查尺寸），补上之后才真正有护栏作用。

## 已知局限（不粉饰）

- **kit 不提供转场**。裁云的转场是场景级的（`transition` 字段），脚本层做不了。
  `RECIPES.md` 的 R5 只讲硬切怎么卡拍。
- **文字排版仍靠图层**。shapes 的 `kind=3` 只给一个高度，长文本要自己断行。
  所以模板里文字一律用图层 + `skipBands` 让开。
- **`lattice` 在推镜时会被一起放大**（因为它必须随相机走），观感可能偏"坐标纸"。
  缓解：低不透明度 + 让开文字带 + 稀疏化（`drop`）。这是**为可测性付的美术代价**，
  不接受就改用"元素本身持续位移"（见 RECIPES R3）。
