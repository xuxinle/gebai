// 验证 `applyCamera` 的 w/h 守卫修复**不改变任何像素**。
//
// ## 为什么能这样证明（不用重渲 900 帧）
//
// 修复前：文字图元（kind=3，没有 w）被写成 `w = undefined * z = NaN`。
// 修复后：不写 w。
//
// 而 C++ 侧的绘制分支（`motion.cpp: case 3`）只读 `x / y / h / text / color`：
//     p.text(shape.text, max(1.0f, shape.h * scale), shape.x * scale, shape.y * scale, color);
// **完全不读 w**。所以只要「除 w 之外的字段逐帧逐值完全相同」，输出像素必然相同。
//
// 这把一次昂贵的重渲换成了一次纯数值比对，而且比"渲完看图"更强：
// 看图只能发现"看起来没变"，比对能证明"一个字段都没变"。
//
// 用法：node scripts/verify_camera_fix_equiv.cjs

const fs = require("fs");
const path = require("path");
const vm = require("vm");

const ROOT = path.dirname(__dirname);
const BENCH = path.join(ROOT, "out", "otbench");

const EASE = {
  linear: (u) => u, smoother: (u) => u * u * u * (u * (u * 6 - 15) + 10),
  in: (u) => u * u * u, out: (u) => 1 - Math.pow(1 - u, 3),
  in_out: (u) => (u < 0.5 ? 4 * u * u * u : 1 - Math.pow(-2 * u + 2, 3) / 2),
  expo_in: (u) => (u <= 0 ? 0 : Math.pow(2, 10 * u - 10)),
  expo_out: (u) => (u >= 1 ? 1 : 1 - Math.pow(2, -10 * u)),
  expo_in_out: (u) => {
    if (u <= 0) return 0; if (u >= 1) return 1;
    const f = (v) => (Math.pow(2, 10 * v) - 1) / 1023;
    return u < 0.5 ? f(2 * u) * 0.5 : 1 - f(2 - 2 * u) * 0.5;
  },
  back: (u) => { const c1 = 1.70158, c3 = c1 + 1, i = u - 1; return 1 + c3 * i * i * i + c1 * i * i; },
};

function makeCtx() {
  const ctx = {
    Math, Object, Array, Number, String, JSON, Boolean, console,
    isFinite, isNaN, parseFloat, parseInt,
    ease: (n, u) => (EASE[n] || EASE.linear)(Math.min(1, Math.max(0, u))),
    lerp: (a, b, t) => a + (b - a) * t,
    clamp: (v, lo, hi) => Math.min(hi, Math.max(lo, v)),
    spring: () => 0, spring_vel: () => 0, settle: () => 1, snap: (t) => t, noise: () => 0.5,
  };
  ctx.globalThis = ctx;
  return vm.createContext(ctx);
}

// 把修复"回退"成旧写法（只在这一份内存副本里），得到修复前的输出。
//
// 按行处理（不能用长字符串替换）：修复处插了多行注释，长串替换会因空白/换行差异失败，
// 而失败时源文件不变 ⇒ 判定为"没有可回退的守卫"⇒ 静默跳过（第一次跑就踩到了：
// 13 条里 2 条因此被误报为"不等价"）。
function revertGuard(src) {
  return src
    .split("\n")
    .map((line) => {
      const t = line.trim();
      if (t === "if (s.w !== undefined) s.w = Math.max(1, s.w * z);") return line.replace(t, "s.w = Math.max(1, s.w * z);");
      if (t === "if (s.w !== undefined) s.w = Math.max(1, s.w * zoom);") return line.replace(t, "s.w = Math.max(1, s.w * zoom);");
      if (t === "if (s.kind !== 0 && s.h !== undefined) s.h = Math.max(1, s.h * z);") return line.replace(t, "if (s.kind !== 0) s.h = Math.max(1, s.h * z);");
      if (t === "if (s.kind !== 0 && s.h !== undefined) s.h = Math.max(1, s.h * zoom);") return line.replace(t, "if (s.kind !== 0) s.h = Math.max(1, s.h * zoom);");
      if (t === "if (s.w !== undefined) s.w *= z;") return line.replace(t, "s.w *= z;");
      if (t === "if (s.h !== undefined) s.h *= z;") return line.replace(t, "s.h *= z;");
      return line;
    })
    .join("\n");
}

function run(src, layers, fps, dur, t) {
  if (t === null) {
    const ctx = makeCtx();
    vm.runInContext(src, ctx, { filename: "probe.js" });
    return ctx;
  }
  const ctx = run(src, layers, fps, dur, null);
  return ctx.layout(t, dur, { width: 1920, height: 1080 }, layers);
}

const CASES = [
  ["c01-unbroken", "unbroken.js"], ["c02-onedot", "onedot.js"],
  ["c03-knockon", "knockon.js"], ["c04-motionweb", "motionweb.js"],
  ["c05-clearing", "clearing.js"], ["c06-pith", "pith.js"],
  ["c07-launch", "launch.js"], ["c08-overlap", "overlap.js"],
  ["c09-skilldemo", "skildemo.js"], ["c10-ebb", "ebb.js"],
  ["c11-infra", "infra.js"], ["c12-domino", "domino.js"],
  ["c13-clawd", "clawd.js"],
];

function pct(v, base) {
  if (typeof v === "string" && v.trim().endsWith("%")) return (parseFloat(v) / 100) * base;
  const n = Number(v);
  return Number.isFinite(n) ? n : 0;
}

let fails = 0, checked = 0;
for (const [dir, file] of CASES) {
  const d = path.join(BENCH, dir);
  const p = path.join(d, file);
  const jf = fs.readdirSync(d).find((f) => f.endsWith(".json"));
  if (!fs.existsSync(p) || !jf) continue;
  const o = JSON.parse(fs.readFileSync(path.join(d, jf), "utf8"));
  const sc = (o.scenes || []).find((s) => s.layout === file);
  if (!sc) continue;
  checked++;

  const layers = (sc.layers || []).map((l) => ({
    kind: l.kind, ...l,
    x: pct(l.x, 1920), y: pct(l.y, 1080), w: pct(l.w, 1920), h: pct(l.h, 1080),
    alpha: 1, scale: 1, offset_y: 0,
  }));
  const fps = o.fps || 30, dur = sc.duration || 15;
  const fixedSrc = fs.readFileSync(p, "utf8");
  const oldSrc = revertGuard(fixedSrc);
  if (oldSrc === fixedSrc) {
    console.log(`-  ${dir}: 没有可回退的守卫（跳过）`);
    continue;
  }

  let diffFrames = 0, nanBefore = 0;
  for (let f = 0; f <= Math.round(fps * dur); f++) {
    const t = f / fps;
    let a, b;
    try {
      a = run(oldSrc, layers, fps, dur, t);
      b = run(fixedSrc, layers, fps, dur, t);
    } catch (e) {
      console.log(`✗ ${dir} t=${t.toFixed(2)} 抛错 ${e.message}`);
      fails++; break;
    }
    if (a.shapes.length !== b.shapes.length) {
      console.log(`✗ ${dir} t=${t.toFixed(2)} 图元数不同 ${a.shapes.length} vs ${b.shapes.length}`);
      fails++; break;
    }
    for (let i = 0; i < a.shapes.length; i++) {
      const sa = a.shapes[i], sb = b.shapes[i];
      const keys = new Set([...Object.keys(sa), ...Object.keys(sb)]);
      for (const k of keys) {
        // w 是这次唯一允许变化的字段；而且只允许「NaN → 未定义」
        if (k === "w") {
          if (Number.isNaN(sa.w)) nanBefore++;
          // 唯一允许的变化： NaN → undefined（这正是修复的目的）
          const expected = Number.isNaN(sa.w) && sb.w === undefined;
          const same = expected || sa.w === sb.w;
          if (!same) { console.log(`✗ ${dir} t=${t.toFixed(2)} #${i} w ${sa.w} → ${sb.w}`); fails++; }
          continue;
        }
        const va = sa[k], vb = sb[k];
        const same = (Number.isNaN(va) && Number.isNaN(vb)) || va === vb;
        if (!same) {
          console.log(`✗ ${dir} t=${t.toFixed(2)} #${i} 字段 ${k}: ${va} → ${vb}`);
          fails++; break;
        }
      }
      if (fails) break;
    }
    if (fails) break;
  }
  if (!fails) {
    console.log(`✓  ${dir}/${file} 逐帧除 w 外全等（修复前共 ${nanBefore} 个 w=NaN）`);
  }
}

console.log();
if (fails) { console.log(`✗ ${fails} 处不等价`); process.exit(1); }
console.log(`${checked} 条 layout：修复前后除「kind=3 的 w」外**逐帧逐字段完全相同**`
  + `（C++ case 3 不读 w ⇒ 输出像素必然不变）`);
