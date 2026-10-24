// 13 条 caiyun layout 脚本的**逐帧校验**（不是数图元，是验“能不能无错跑通 + 输出合法”）。
//
// ## 为什么必须有它（这是 G8 的唯一拦网）
//
// 渲染器对 `layout` 抛错的处理是：**静默降级为纯 JSON**（只画图层）——
// 画面只剩背景，而渲染日志一个字都不打。overlap / clawd 两个子会话各自撞到过一次
// （overlap 是删了一行 `const push`），两次都靠"抽帧发现画面空了"才定位。
//
// layout 脚本是一段被渲染器 eval 的代码，没有单测入口，所以能做的验证就是：
//   ① 逐帧调用 layout()，任何抛错立刻暴露（不用渲 450 帧再看画面）；
//   ② 结构合法：states 是数组且长度 == layers 数；shapes 是数组；
//   ③ 数值有限：NaN / Infinity 会被渲染器当"没写这个字段"，同样是静默的。
//
// ## 桩必须复现真实宿主环境
//
// `ease()` 的行为会直接影响 t 时刻的数值。桩若和渲染器不一致，验的就是一个不存在的
// 环境（桩把差异抹平 = 假绿）。这里按 `docs/caiyun_layout_script.md` 列出的宿主函数实现。
//
// 用法：node scripts/verify_layouts.cjs [glob 子串过滤]

const fs = require("fs");
const path = require("path");
const vm = require("vm");

const ROOT = path.dirname(__dirname);
const BENCH = path.join(ROOT, "out", "otbench");

const EASE = {
  linear: (u) => u,
  smoother: (u) => u * u * u * (u * (u * 6 - 15) + 10),
  in: (u) => u * u * u,
  out: (u) => 1 - Math.pow(1 - u, 3),
  in_out: (u) => (u < 0.5 ? 4 * u * u * u : 1 - Math.pow(-2 * u + 2, 3) / 2),
  expo_in: (u) => (u <= 0 ? 0 : Math.pow(2, 10 * u - 10)),
  expo_out: (u) => (u >= 1 ? 1 : 1 - Math.pow(2, -10 * u)),
  expo_in_out: (u) => {
    if (u <= 0) return 0;
    if (u >= 1) return 1;
    const f = (v) => (Math.pow(2, 10 * v) - 1) / 1023;
    return u < 0.5 ? f(2 * u) * 0.5 : 1 - f(2 - 2 * u) * 0.5;
  },
  back: (u) => {
    const c1 = 1.70158, c3 = c1 + 1, i = u - 1;
    return 1 + c3 * i * i * i + c1 * i * i;
  },
};

function makeSandbox() {
  const ctx = {
    Math, Object, Array, Number, String, JSON, Boolean, console,
    isFinite, isNaN, parseFloat, parseInt,
    ease: (name, u) => (EASE[name] || EASE.linear)(Math.min(1, Math.max(0, u))),
    lerp: (a, b, t) => a + (b - a) * t,
    clamp: (v, lo, hi) => Math.min(hi, Math.max(lo, v)),
    spring: () => 0,
    spring_vel: () => 0,
    settle: () => 1,
    snap: (t) => t,
    noise: () => 0.5,
  };
  ctx.globalThis = ctx;
  return vm.createContext(ctx);
}

function validate(file, layers, fps, duration) {
  const src = fs.readFileSync(file, "utf8");
  const ctx = makeSandbox();
  try {
    vm.runInContext(src, ctx, { filename: path.basename(file) });
  } catch (e) {
    return { ok: false, reason: `加载即报错: ${e.message}` };
  }
  if (typeof ctx.layout !== "function") {
    return { ok: false, reason: "没有定义 layout(t, duration, canvas, layers)" };
  }

  const canvas = { width: 1920, height: 1080 };
  const frames = Math.round(fps * duration) + 1;

  let maxShapes = 0, nanCount = 0;
  const problems = [];
  for (let f = 0; f < frames; f++) {
    const t = f / fps;
    let r;
    try {
      r = ctx.layout(t, duration, canvas, layers);
    } catch (e) {
      problems.push(`t=${t.toFixed(2)} 抛错: ${e.message}`);
      if (problems.length >= 4) break;
      continue;
    }
    if (!r || typeof r !== "object") {
      problems.push(`t=${t.toFixed(2)} 返回 ${typeof r}`);
      if (problems.length >= 4) break;
      continue;
    }
    if (!Array.isArray(r.shapes)) {
      problems.push(`t=${t.toFixed(2)} shapes 不是数组（${typeof r.shapes}）`);
      if (problems.length >= 4) break;
      continue;
    }
    if (!r.states || typeof r.states !== "object") {
      problems.push(`t=${t.toFixed(2)} 缺 states`);
      if (problems.length >= 4) break;
      continue;
    }
    // states 可以是数组（按图层下标）或对象（按图层名）——两种都合法，
    // 只有“是数组且长度不对”才是真问题。
    if (Array.isArray(r.states) && r.states.length !== layers.length) {
      problems.push(`t=${t.toFixed(2)} states 长度 ${r.states.length} ≠ layers ${layers.length}`);
      if (problems.length >= 4) break;
    }
    maxShapes = Math.max(maxShapes, r.shapes.length);
    for (const s of r.shapes) {
      if (!s || typeof s !== "object") { nanCount++; continue; }
      let bad = false;
      for (const k of ["x", "y", "w", "h", "alpha", "stroke", "radius"]) {
        // ## null ≠ NaN（这里踩过一次假红）
        //
        // 脚本不写某个字段时，layout 运行时会把缺失的数值字段补成 `null`
        // （实测：`{kind:3,text,h,x,y,color,alpha}` 返回时多了 `w:null, radius:0`）。
        // 而 C++ 侧读法器是 `number(key, fallback)`：
        //   `if (field==nullptr || !field->is_number()) return fallback;`
        // 即 `null` 是**合法且常见**的“未指定”，会静默回落默认值。
        //
        // 第一版探针用 `"k" in s && !Number.isFinite(s[k])` ⇒ 把 `null` 判成 NaN，
        // 6 条片子全是假红（它们早就渲染成功过）。桩不能比真实契约更严。
        if (!(k in s)) continue;
        const v = s[k];
        if (v === null || v === undefined) continue;   // 未指定：渲染器会回落
        if (typeof v !== "number" || !Number.isFinite(v)) { bad = true; break; }
      }
      if (bad) {
        nanCount++;
        if (problems.length < 4) problems.push(`t=${t.toFixed(2)} 图元字段非数值（NaN/Infinity/字符串）`);
      }
    }
  }
  if (problems.length) return { ok: false, reason: problems.join(" | "), maxShapes };
  if (nanCount) return { ok: false, reason: `${nanCount} 个图元字段非有限数`, maxShapes };
  return { ok: true, maxShapes, frames };
}

// 每片的 fps / duration / 图层数都**从真实导演脚本读**，不写死。
// 实测代价：第一版探针固定给 5 个图层，而各片实际是 2~8 个——
// 于是 unbroken 的 `states[4]`/`states[5]` 越界、motionweb/launch/overlap/ebb/infra
// 的 `layers[i].w` 取到 undefined ⇒ NaN。**全部是探针的错，不是片子的错。**
// （这正是「桩把差异抹平」的反面：桩给了不存在的环境，于是造出假红。）
function loadCases() {
  const out = [];
  for (const dir of fs.readdirSync(BENCH)) {
    if (!dir.startsWith("c") || dir.startsWith("contact")) continue;
    const d = path.join(BENCH, dir);
    if (!fs.statSync(d).isDirectory()) continue;
    for (const jf of fs.readdirSync(d).filter((f) => f.endsWith(".json"))) {
      let o;
      try { o = JSON.parse(fs.readFileSync(path.join(d, jf), "utf8")); } catch { continue; }
      const fps = o.fps || 30;
      for (const sc of o.scenes || []) {
        if (!sc.layout) continue;
        const layers = (sc.layers || []).map((l) => ({
          kind: l.kind, x: 0, y: 0, w: 0, h: 0,
          ...l,
          // 尺寸字段可能是 "50%" 这类比例字符串；layout 里做算术会得 NaN。
          // 渲染器会先把它解成像素——所以探针也必须解，否则又是假红。
          x: pct(l.x, 1920), y: pct(l.y, 1080), w: pct(l.w, 1920), h: pct(l.h, 1080),
          alpha: 1, scale: 1, offset_y: 0,
        }));
        out.push([dir, sc.layout, layers, fps, sc.duration || 15]);
      }
    }
  }
  return out;
}

function pct(v, base) {
  if (typeof v === "string" && v.trim().endsWith("%")) {
    return (parseFloat(v) / 100) * base;
  }
  const n = Number(v);
  return Number.isFinite(n) ? n : 0;
}

const filter = process.argv[2] || "";
let fails = 0, checked = 0;
for (const [dir, file, layers, fps, dur] of loadCases()) {
  if (filter && !dir.includes(filter) && !file.includes(filter)) continue;
  const p = path.join(BENCH, dir, file);
  if (!fs.existsSync(p)) {
    console.log(`-  ${dir}/${file} 不存在，跳过`);
    continue;
  }
  checked++;
  const r = validate(p, layers, fps, dur);
  if (r.ok) {
    console.log(`✓  ${dir}/${file.padEnd(14)} ${layers.length} 层  ${r.frames} 帧跑通，图元峰值 ${r.maxShapes}`);
  } else {
    fails++;
    console.log(`✗  ${dir}/${file}  ${r.reason}`);
  }
}
console.log();
if (!checked) { console.log("没有可校验的文件"); process.exit(2); }
if (fails) { console.log(`${fails}/${checked} 个 layout 脚本有问题`); process.exit(1); }
console.log(`${checked} 个 layout 脚本全部跑通（无抛错、无 NaN、states 长度匹配）`);
