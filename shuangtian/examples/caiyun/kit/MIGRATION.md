# 把已有片子迁移到 kit（渐进式，不要求重写）

> 现有 13 个 case 的 layout 各自带一份 `keys()` 与相机代码。**不要一次性重写**——
> 那会把已经调好的时间曲线打乱，而那是几轮实测才调对的。
>
> 正确做法：**按需替换**。哪个 case 下次要改，就顺手把那部分换成 kit。

## 迁移顺序（从风险最低到最高）

### 第 1 步：只加 `check_layout.py` 门禁（零风险，立刻有收益）

```bash
py scripts/check_layout.py out/otbench/<case>/<layout>.js --duration <dur>
```

`ot_bench.py` 已经接好这道门禁（渲染前自动跑）。**这一步不需要改任何脚本**，
但它能立刻抓到"layout 抛错 → 静默降级 → 画面只剩背景"这类问题。

> 本轮它抓到过：`pith.js` 里 `keys()` 调了一个与宿主冲突的符号，
> 每帧抛错，而我为此白改了 3 轮相机关键帧。

### 第 2 步：把 `keys()` 换成 kit 的（低风险）

各 case 的 `keys()` 实现基本一致（都是关键帧线性/缓动插值）。换成 `K.keys` 后：

- 少一份重复实现（现在有 13 份，改一处忘别处是必然）
- 缓动名统一（各脚本的 `ease()` 名字不完全一致，如 `"in_out"` vs `"inOut"`）

**迁移前先确认缓动名映射**：kit 用的是 `linear/in/out/in_out/expo_in/expo_out/expo_in_out/back`。
若原脚本用了别的名字，要么改名，要么在 kit 的 `kEase` 表里补一个（后者更好，集中一处）。

```js
// 原：脚本内自带
function keys(t, list) { ... }
function ease(name, u) { ... }

// 后：删掉上面两个，改用
const px = keys(t, [[0, CX], [2, 1.2, "expo_out"], ...]);   // 签名不变
```

### 第 3 步：加 `lattice`（中风险，改观感）

这是**收益最大也最容易改坏美术**的一步。

**只在片子确实"静止占比过高"时才做。** 判据：

```bash
# 看逐秒帧差：某几秒 mean 精确为 0 或 < 1.2，才是这个问题
py scripts/check_layout.py ...           # 先排除"降级"
# 再用 ot_bench 的 report.json 看 still_ratio 与参考片对比
```

做的时候按 `RECIPES.md` R6 的三个数：

| 参数 | 值 | 说明 |
|---|---|---|
| `step` | **76** | 别用 60 的整数倍（与评测格拍频），84 实测 94.5% |
| `size` | 18 | 大到此程度才够，但别到 26（像波点壁纸） |
| `alpha` | 0.5 | 退到背景，不抢主体 |
| `flow` | 780 | 主体占屏多时加到 1500 |

**并且必须**：
- 传 `t`（`lattice({...}, t)`）
- 用 `skipBands` 让开文字横带（文字是图层画的，在 shapes 之下）
- 点阵放在**相机变换之前** push（屏幕空间固定的纹理在推镜时纹丝不动）

改完**必须抽帧看**：指标过而画面坏（像波点壁纸）是真实存在的失败模式。

### 第 4 步：相机换 `camera()`（高风险，别急着做）

只在"确实要找平台期"时才做。各 case 的相机结构差异大
（`c02` 是 zoom+锚点、`c12` 是 3D yaw/pitch、`c13` 是透视投影），
统一成 kit 的 `camera()` 反而可能丢掉它们各自调好的东西。

**替代做法**：不改结构，只按 kit 的规则**检查平台**：

```js
// 自检：把关键帧之间的每一段都算一遍，两端同值就是平台
let flat = 0;
for (let i = 0; i + 1 < keysList.length; i++) {
  if (keysList[i][1] === keysList[i + 1][1]) flat += keysList[i + 1][0] - keysList[i][0];
}
```

## 不该迁移的部分

| 部分 | 为什么不迁 |
|---|---|
| **时间曲线本身**（`STORY` / 各 `keys` 的关键帧值） | 那是几轮实测调出来的，kit 给不了 |
| **3D 投影类相机**（`c12` 的 `makeProjector`） | kit 的 `camera()` 是 2D zoom+锚点，语义不同 |
| **case 特有的图元组合** | kit 只提供工厂，组合是内容 |
| **配色** | 每片子的调性是设计决策，kit 不带皮肤 |

## 迁移后的验收

```bash
py scripts/check_layout.py --scan out/otbench      # 全部 case 无运行期错误
node tools/test_caiyun_kit.cjs                     # kit 自身 60 条断言
py scripts/ot_bench.py --script <case>.json --out out/otbench/<case> --ref-baseline <ref>
```

三条都过才算迁移完成。**只过指标不算**——还要抽帧看画面。
