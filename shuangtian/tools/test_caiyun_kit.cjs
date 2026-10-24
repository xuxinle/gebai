// ============================================================================
// caiyun-kit 的自测：每个断言都对应一条**实测教训**，不是形式覆盖。
// ----------------------------------------------------------------------------
// 为什么 kit 需要自己的测试：它是 13 部片子共用的底座。它错了不是"一部片子
// 不好看"，而是**所有片子同时错**——而 layout 抛错会静默降级，很可能表现为
// "画面变空了但没有报错"。
//
// 跑法（在 shuangtian 根目录）：
//   node tools/test_caiyun_kit.cjs
//
// 每条断言后面标注它对应的 case 与实测数字。
// ============================================================================

const fs = require("fs");
const path = require("path");

const KIT = path.join(__dirname, "..", "examples", "caiyun", "kit", "caiyun-kit.js");
const src = fs.readFileSync(KIT, "utf8");
// 用 new Function 取 KIT：仓库根的 package.json 是 type:module，
// 而 kit 是给脚本引擎用的 plain script，require 会报 ERR_REQUIRE_ESM。
const K = new Function(src + "\n; return KIT;")();

let pass = 0;
let fail = 0;
function check(ok, what) {
  if (ok) { pass++; console.log("  [OK] " + what); }
  else { fail++; console.log("  [X ] " + what); }
}
function near(a, b, eps) {
  return Math.abs(a - b) <= (eps === undefined ? 1e-6 : eps);
}

console.log("caiyun-kit v" + K.version + "\n");

// ── 1. keys() 的边界与缓动 ──────────────────────────────────────────────
// 教训：c06-pith 因 keys() 里调用了不存在的符号而每帧抛错，
// 整份 layout 静默降级 —— 而 keys 是所有脚本的基石。
check(near(K.keys(0, [[0, 5], [10, 15]]), 5), "keys: 首端点取左值");
check(near(K.keys(10, [[0, 5], [10, 15]]), 15), "keys: 末端点取右值");
check(near(K.keys(20, [[0, 5], [10, 15]]), 15), "keys: 超出末端钳住右值");
check(near(K.keys(-3, [[0, 5], [10, 15]]), 5), "keys: 低于首端钳住左值");
check(near(K.keys(5, [[0, 0], [10, 10]]), 5), "keys: 线性中点为 5");
check(near(K.keys(5, [[0, 0], [10, 10], [10, 10]]), 10) ||
      near(K.keys(5, [[0, 0], [10, 10]]), 5), "keys: 重复时刻不崩（除零保护）");
check(near(K.keys(5, [[3, 7]]), 7), "keys: 单点时取该值");

// ── 2. 缓动曲线必须真的 S 形 ────────────────────────────────────────────
// 教训：c05-clearing 改 dayH 26→58 却几乎没效果，因为要经过 zoomIn 归一化。
// 这里确认曲线本身是对的，避免"改的是错的层"。
check(near(K.kEase.linear(0.5), 0.5, 1e-9), "ease.linear(0.5)=0.5");
check(K.kEase.expoOut(0.5) > 0.9, "ease.expoOut(0.5) 已走完大半（>0.9）");
check(K.kEase.expoIn(0.5) < 0.1, "ease.expoIn(0.5) 才刚起步（<0.1）");
check(near(K.kEase.expoInOut(0.5), 0.5, 1e-6), "ease.expoInOut 在 0.5 处对称");
check(near(K.kEase.expoOut(1), 1) && near(K.kEase.expoOut(0), 0), "ease.expoOut 两端精确");

// ── 3. hash01：确定性 + 有散布 ──────────────────────────────────────────
// 教训：裁云要求逐帧可复现（同脚本两次出片同像素），所以禁用 Math.random；
// 但"疏密不均/错峰"仍需要随机感。
const h1 = K.hash01(7);
const h2 = K.hash01(7);
check(h1 === h2, "hash01 同输入同输出（可复现）");
const spread = [1, 2, 3, 4, 5, 6, 7, 8].map(i => K.hash01(i));
const allSame = spread.every(v => v === spread[0]);
check(!allSame, "hash01 不同输入有散布（不是常数）");
check(spread.every(v => v >= 0 && v < 1), "hash01 落在 [0,1)");

// ── 4. camera：默认无平台（最重要的一条）──────────────────────────────
// 教训：5 个 case 里 4 个的"静止占比过高"主因是 keys() 两端同值 → 画面站定。
// 所以 camera 默认必须叠 creep，让恒定输入也产出每帧不同的输出。
const flat = { keys: [[0, 1], [10, 1]], creep: 1 };
const c = K.camera(flat);
check(c.zoom(3) !== c.zoom(3.5), "camera: 恒定关键帧下 zoom 仍在变（creep 生效）");
check(c.zoom(3) !== c.zoom(3 + 1 / 30), "camera: 相邻两帧（1/30s）zoom 就不同");

const ax1 = c.anchor(2);
const ax2 = c.anchor(2 + 1 / 30);
check(Math.abs(ax1[0] - ax2[0]) > 0.01, "camera: 锚点相邻帧也在动");

// creep=0 时必须真的恒定（品牌定格是有意的静止）
const still = K.camera({ keys: [[0, 1], [10, 1]], creep: 0 });
check(still.zoom(3) === still.zoom(3.5), "camera: creep=0 时恒定（定格可用）");

// at() 必须与 zoom/anchor 一致（同一帧用两条路径算同一点）
const tProbe = 4.2;
const [cax, cay] = c.anchor(tProbe);
const zz = c.zoom(tProbe);
const pt = c.at(tProbe, 1234, 567);
check(near(pt[0], (1234 - cax) * zz + cax, 1e-6) &&
      near(pt[1], (567 - cay) * zz + cay, 1e-6),
      "camera.at() 与 zoom/anchor 口径一致");

// creep 幅度必须"跨得过 60px 的格"——实测 δ≈2·d·s/3600，位移太小测不到
let maxShift = 0;
for (let t = 0; t < 30; t += 0.1) {
  const a = c.anchor(t);
  const b = c.anchor(t + 0.1);
  maxShift = Math.max(maxShift, Math.hypot(a[0] - b[0], a[1] - b[1]));
}
check(maxShift > 0.5, "camera: creep 的逐帧位移可测（>0.5px/0.1s）");

// ── 5. lattice：尺寸必须够大、必须能覆盖画面 ──────────────────────────
// 教训：c02-onedot 的 d=2.6 @48px 版本 p95 只有 1.63（静止 90.9%），
// 换成 d=14 @60px 后 p95 3.22（静止 54.6%）。Δ ≈ 2·d·s/3600。
const lat = K.lattice({ step: 60, size: 14, drop: 0, width: 1920, height: 1080 });
check(lat.length > 200, "lattice: 覆盖整屏（" + lat.length + " 个点）");
check(lat.every(s => s.kind === 0), "lattice: 全部是圆形图元（kind 0）");
// 注意：这里查 `color` 而不是 `fill` —— 渲染器只认 `color`（见下方字段名断言）
check(lat.every(s => s.color && s.alpha !== undefined), "lattice: 每个点都有 color 与 alpha");

// ## 尺寸必须"大到能被测到"——这条是逆向验证补上的
//
// 第一版只断言了"点数 > 200"，于是把默认 size 从 14 改回 2.6（原片尺寸）
// 测试**依然全绿**。而 d=2.6 正是实测 p95 只有 1.63、静止 90.9% 的那一版。
//
// 判据用公式：Δ ≈ 2·d·s/3600，要过阈值 1.2 需 d·s ≥ 2200。
// 取 s 的保守估计（creep 的逐帧位移 ~0.5px 累计到 0.1s 约 1px；
// 实际推镜位移几十像素），这里只要求 d ≥ 10 ——低于它必然测不到。
check(lat.every(s => s.w >= 10 && s.h >= 10),
      "lattice: 点直径 ≥10px（d<10 时 60px 格里测不到，实测 p95 掉到 1.6）");
const latSizes = new Set(lat.map(s => s.w));
check(latSizes.size === 1, "lattice: 所有点同尺寸（不是随机大小）");

// ## 默认值也要查——这是第二次逆向验证才补上的
//
// 上一版只查了显式传 `size: 14` 的路径，于是把**默认值**从 14 改成 2.6 时
// 测试仍然全绿（正是那条实测 p95 1.63 的坏配置）。
// 教训与 CONVENTIONS §7.1 的"默认值是产品决策，实现与测试必须走同一入口"同源：
// 测试必须覆盖**调用方不传参**的那条路径。
const latDefault = K.lattice({ drop: 0 });   // 不传 size/step/width
check(latDefault.length > 0, "lattice: 无参数调用有产出");
check(latDefault.every(s => s.w >= 10),
      "lattice: **默认** size ≥10px（d=2.6 是实测 p95 1.63 的坏值）");

// drop 必须真的丢点（疏密不均），但不能全丢
const latDrop = K.lattice({ step: 60, size: 14, drop: 0.5, width: 1920, height: 1080 });
check(latDrop.length < lat.length && latDrop.length > 0, "lattice: drop 生效且有剩余");

// skipBands 必须让开文字带（否则点阵盖住图层文字，实测抽帧可见）
const latSkip = K.lattice({ step: 60, size: 14, drop: 0, width: 1920, height: 1080,
                            skipBands: [[500, 600]] });
const inBand = latSkip.filter(s => s.y > 500 && s.y < 600);
check(inBand.length === 0, "lattice: skipBands 内没有点（让开文字带）");

// ── 6. 图元工厂：字段名必须与渲染器契约一致 ───────────────────────────
// 教训：裁云的 shape 字段是 kind/x/y/w/h/radius/fill/color/stroke/strokeWidth/text，
// 写错名字不会报错、只是不生效——这类静默失败最难查。
const circ = K.shapes.circle(100, 200, 30, "#F00", 0.8);
check(circ.kind === 0 && circ.radius === 30 && circ.w === 60 && circ.h === 60,
      "shapes.circle: kind/radius/w/h 正确");
const rect = K.shapes.rect(10, 20, 300, 100, "#0F0", 0.5);
check(rect.kind === 1 && rect.w === 300 && rect.h === 100, "shapes.rect: w/h 正确");
const line = K.shapes.line(0, 0, 100, 100, "#00F", 2, 1);
check(line.kind === 2 && line.x2 === 100 && line.strokeWidth === 2,
      "shapes.line: 用 x2/y2 表示终点（不是 w/h）");
const txt = K.shapes.text(50, 60, 24, "hi", "#EEE", 0.9);
check(txt.kind === 3 && txt.text === "hi" && txt.h === 24,
      "shapes.text: kind 3 用 h 表示字号");

// ## 字段名必须与渲染器契约一致（逆向验证补上的关键一条）
//
// 教训：kit 第一版把颜色写成 `fill`（SVG 的习惯），而裁云的渲染器
// （layout.cpp）只读 `color` / `stroke` —— **没有 `fill` 字段**。
// 后果：写 `fill` 不报错，只是用默认色；模板渲出来背景点阵完全不见，
// 静止占比 94%，我一度以为是相机没生效。
//
// 这条断言把"渲染器认识的字段"作为白名单硬编码，写错即红。
const RENDERER_FIELDS = new Set([
  "kind", "x", "y", "w", "h", "radius", "color", "stroke", "strokeWidth",
  "text", "alpha", "x2", "y2",
]);
function unknownFields(obj) {
  return Object.keys(obj).filter(k => !RENDERER_FIELDS.has(k));
}
check(unknownFields(circ).length === 0, "shapes.circle: 无渲染器不认识的字段");
check(unknownFields(rect).length === 0, "shapes.rect: 无渲染器不认识的字段");
check(unknownFields(line).length === 0, "shapes.line: 无渲染器不认识的字段");
check(unknownFields(txt).length === 0, "shapes.text: 无渲染器不认识的字段");
check(lat.every(s => unknownFields(s).length === 0),
      "lattice: 无渲染器不认识的字段（`fill` 是错的，应为 `color`）");

// ## flow：点阵必须**自己**每帧流动（本轮最关键的一次机制）
//
// 教训链：
//   ① 只靠相机 creep → 逐帧位移 3px → Δ≈0.02 → 静止 94%（测不到）
//   ② 把点做大/做亮 → 画面像波点壁纸，静止仍是 94%
//   ③ 让点阵自己按 flow px/s 流动 → 逐帧位移 26px → 静止 **2.2%**
//
// 所以 flow 不是装饰参数，是"整屏可测"的机制。传 t 才生效。
const latT0 = K.lattice({ step: 76, size: 18, drop: 0, flow: 780 }, 0);
const latT1 = K.lattice({ step: 76, size: 18, drop: 0, flow: 780 }, 1 / 30);
check(latT0.length === latT1.length, "lattice: 流动不改变点数");
const moved = latT0.length > 0 &&
  (latT0[0].x !== latT1[0].x || latT0[0].y !== latT1[0].y);
check(moved, "lattice: 传 t 时点阵逐帧流动（flow 生效）");

// ## 水平与垂直**都要**流
//
// 第一版只查"x 或 y 有一个变了"，于是把 fx 置 0（只剩垂直流）时**测试仍绿**。
// 而实测里横向流动是主要贡献（点阵是横排密、横移扫过的格子最多）。
// 所以分开断言，两个方向都必须变。
check(latT0[0].x !== latT1[0].x, "lattice: 水平方向逐帧流动（fx）");
check(latT0[0].y !== latT1[0].y, "lattice: 垂直方向逐帧流动（fy）");

// 不传 t 时必须与传 t=0 等价（向后兼容：老调用不传 t 时不该崩）
const latNoT = K.lattice({ step: 76, size: 18, drop: 0, flow: 780 });
check(latNoT.length > 0 && latNoT[0].x === latT0[0].x,
      "lattice: 不传 t 等价于 t=0（向后兼容）");

// flow=0 时必须静止（有些场景要一张固定底纹）
const latStill = K.lattice({ step: 76, size: 18, drop: 0, flow: 0 }, 0);
const latStill1 = K.lattice({ step: 76, size: 18, drop: 0, flow: 0 }, 1);
check(latStill[0].x === latStill1[0].x && latStill[0].y === latStill1[0].y,
      "lattice: flow=0 时静止（固定底纹可用）");
check(unknownFields(circ).indexOf("fill") < 0 || circ.fill === undefined,
      "shapes.circle: 不用 `fill`（渲染器不读它）");

// ── 7. typeOn：打字进度 ────────────────────────────────────────────────
check(K.typeOn("abcdef", 0.5, 1.0, 28) === "", "typeOn: 起始前为空");
check(K.typeOn("abcdef", 2.0, 1.0, 28) === "abcdef", "typeOn: 足够时间后完整");
check(K.typeOn("abcdef", 1.1, 1.0, 28).length === 2, "typeOn: 0.1s @28cps 打出 2 字");
check(K.typeOn("abc", 5.0, 1.0, 28) === "abc", "typeOn: 不超出原文长度");

// ── 8. wordRise / stagger / breathe ───────────────────────────────────
const rise0 = K.wordRise(0, 1.0, 0.6, 40);
check(rise0.alpha === 0 && rise0.dy === 40, "wordRise: 起点全透明且偏移满额");
const rise1 = K.wordRise(2.0, 1.0, 0.6, 40);
check(near(rise1.alpha, 1) && near(rise1.dy, 0), "wordRise: 结束后完全落定");

check(K.stagger(0, 0.08, 0) === 0, "stagger: 第 0 个无延迟");
check(K.stagger(3, 0.1, 0) > K.stagger(2, 0.1, 0), "stagger: 索引越大延迟越大");

// breathe 的幅度必须够大（60px 格里 1~2px 测不到）
let bmax = 0;
for (let t = 0; t < 5; t += 0.01) bmax = Math.max(bmax, Math.abs(K.breathe(t, 22, 0.42)));
check(bmax > 15, "breathe: 幅度可测（>15px，不是 1~2px 的无效微动）");

// ── 9. 禁止项自检（kit 自己不能引入不确定源）──────────────────────────
// ## 只在**代码**里查，不在注释里查
//
// 第一版直接搜整份源码，被自己的注释绊倒（注释里写了"禁止 Math.random"）。
// 这不是"测试太严"，是**检查对象错了**——要查的是会影响执行的部分。
function stripCommentsAndStrings(code) {
  let out = "";
  let i = 0;
  let inS = null;
  while (i < code.length) {
    const c = code[i];
    if (inS) {
      if (c === "\\") { i += 2; continue; }
      if (c === inS) inS = null;
      i++; continue;
    }
    if (c === '"' || c === "'" || c === "`") { inS = c; i++; continue; }
    if (c === "/" && code[i + 1] === "/") { while (i < code.length && code[i] !== "\n") i++; continue; }
    if (c === "/" && code[i + 1] === "*") {
      i += 2; while (i + 1 < code.length && !(code[i] === "*" && code[i + 1] === "/")) i++;
      i += 2; continue;
    }
    out += c; i++;
  }
  return out;
}
const codeOnly = stripCommentsAndStrings(src);
check(codeOnly.indexOf("Math.random") < 0, "kit 代码不含 Math.random（可复现）");
check(codeOnly.indexOf("Date.now") < 0, "kit 代码不含 Date.now（可复现）");
check(codeOnly.indexOf("require(") < 0 && !/^\s*import\s/m.test(codeOnly),
      "kit 代码不含 require/import（脚本引擎无模块系统）");

// ── 10. 绝不能与宿主函数重名（本仓库最贵的坑之一）─────────────────────
//
// 宿主注入的名字（layout.cpp 的 register_function 白名单）：
//   ease / spring / spring_vel / settle / lerp / clamp / snap
//
// kit 被**拼在脚本前面**，所以这里若声明同名 const，整份 layout 会以
// `Identifier 'X' has already been declared` **加载失败** —— 而渲染器对此
// 是**静默降级**：不报错、照常出片、画面只剩背景。
//
// 实测代价：kit 第一版声明了 `clamp` 与 `ease`，模板因此渲出 94% 静止的空画面，
// 我一度以为是相机/背景没生效，白查一轮。这条断言就是为它加的护栏。
const HOST_NAMES = ["ease", "spring", "spring_vel", "settle", "lerp", "clamp", "snap"];
const collisions = HOST_NAMES.filter(n =>
  new RegExp("^(const|let|var|function)\\s+" + n + "\\b", "m").test(codeOnly));
check(collisions.length === 0,
      "kit 不与宿主函数重名" + (collisions.length ? "（冲突：" + collisions.join(",") + "）" : ""));

// 导出的对象里也不能出现宿主名（脚本里 K.clamp 会遮蔽宿主，行为难以预测）
const exported = Object.keys(K);
const exportedCollisions = HOST_NAMES.filter(n => exported.indexOf(n) >= 0);
check(exportedCollisions.length === 0,
      "kit 的导出名不遮蔽宿主" +
      (exportedCollisions.length ? "（冲突：" + exportedCollisions.join(",") + "）" : ""));

console.log("\n通过 " + pass + " · 失败 " + fail);
process.exit(fail === 0 ? 0 : 1);
