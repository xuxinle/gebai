// 像素级对标分析器：把「霜天 vs 浏览器」的差拆成可归因的几块。
//
// 只给均值/直方图不够——它们会把"整个画面系统性偏移"和"只有高光处差"混成一团。
// 这里按**同一几何的同一像素**对齐（两图同尺寸、同相机、同无抗锯齿），算：
//
//   ① 主体/背景分离：以浏览器图与霜天图都"非背景"的像素为交集
//   ② 逐像素通道差：均值 / p50 / p90 / p99 / 最大 / 超阈值占比
//   ③ 分位对比：双方 luma 的 p1/p10/p50/p90/p99 —— 看层次是否被压平
//   ④ 差异的空间分布：按 8×8 网格给热点，指出"差在哪里"
//   ⑤ 亮度偏移分解：霜天 - 浏览器 的符号分布（全局偏亮？只在亮部？）
//
// 用法: analyze <fw.png> <browser.png> [--json]
import { readFileSync } from 'node:fs';
import { inflateSync } from 'node:zlib';

function decodePng(buf) {
  let pos = 8, width = 0, height = 0, bitDepth = 0, colorType = 0;
  const idat = [];
  while (pos < buf.length) {
    const len = buf.readUInt32BE(pos);
    const type = buf.toString('ascii', pos + 4, pos + 8);
    const data = buf.subarray(pos + 8, pos + 8 + len);
    if (type === 'IHDR') {
      width = data.readUInt32BE(0); height = data.readUInt32BE(4);
      bitDepth = data[8]; colorType = data[9];
    } else if (type === 'IDAT') idat.push(data);
    else if (type === 'IEND') break;
    pos += 12 + len;
  }
  const channels = colorType === 6 ? 4 : colorType === 2 ? 3 : 1;
  const raw = inflateSync(Buffer.concat(idat));
  const stride = width * channels;
  const out = Buffer.alloc(width * height * channels);
  for (let y = 0; y < height; y++) {
    const filter = raw[y * (stride + 1)];
    const line = raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1));
    for (let x = 0; x < stride; x++) {
      const a = x >= channels ? out[y * stride + x - channels] : 0;
      const b = y > 0 ? out[(y - 1) * stride + x] : 0;
      const c = x >= channels && y > 0 ? out[(y - 1) * stride + x - channels] : 0;
      let v = line[x];
      if (filter === 1) v += a;
      else if (filter === 2) v += b;
      else if (filter === 3) v += (a + b) >> 1;
      else if (filter === 4) {
        const p = a + b - c;
        const pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
        v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
      }
      out[y * stride + x] = v & 0xff;
    }
  }
  return { width, height, channels, data: out };
}

const px = (img, x, y) => {
  const i = (y * img.width + x) * img.channels;
  return [img.data[i], img.data[i + 1], img.data[i + 2]];
};
const luma = ([r, g, b]) => 0.2126 * r + 0.7152 * g + 0.0722 * b;
const pct = (sorted, p) => sorted[Math.min(sorted.length - 1, Math.max(0, Math.floor(sorted.length * p)))];

const aPath = process.argv[2];
const bPath = process.argv[3];
const wantJson = process.argv.includes('--json');
const A = decodePng(readFileSync(aPath));   // 霜天
const B = decodePng(readFileSync(bPath));   // 浏览器

if (A.width !== B.width || A.height !== B.height) {
  console.error(`尺寸不同：${A.width}x${A.height} vs ${B.width}x${B.height}`);
  process.exit(1);
}

const W = A.width, H = A.height;
const bgA = px(A, 0, 0), bgB = px(B, 0, 0);
const near = (c1, c2, t) => Math.abs(c1[0] - c2[0]) <= t && Math.abs(c1[1] - c2[1]) <= t && Math.abs(c1[2] - c2[2]) <= t;

// 主体 = 任一图里不是背景的像素（用两图各自角落色当背景色）
const diffAll = [], diffSubj = [];
const lumaA = [], lumaB = [];
let subjCount = 0, bgBgCount = 0;
const buckets = Array.from({ length: 9 }, () => ({ n: 0, sumAbs: 0, sumSigned: 0, sumDark: 0, sumBright: 0 }));
const grid = new Map();

for (let y = 0; y < H; y++) {
  for (let x = 0; x < W; x++) {
    const ca = px(A, x, y), cb = px(B, x, y);
    const isBgA = near(ca, bgA, 3), isBgB = near(cb, bgB, 3);
    const la = luma(ca), lb = luma(cb);
    // 差异里跳过抗锯齿软边（两边都非背景但一边明显是边缘过渡）。
    // 用通道差本身不入统计来避免：这里只记录，分离由「按浏览器 luma 分桶」完成。
    const d = [Math.abs(ca[0] - cb[0]), Math.abs(ca[1] - cb[1]), Math.abs(ca[2] - cb[2])];
    const maxd = Math.max(...d);
    const mean = (d[0] + d[1] + d[2]) / 3;
    if (isBgA && isBgB) { bgBgCount++; continue; }
    if (!isBgA || !isBgB) {
      subjCount++;
      diffSubj.push(mean);
      lumaA.push(la); lumaB.push(lb);
      const signed = (la - lb);
      const bucket = Math.min(8, Math.floor(lb / 28.34));
      const b = buckets[bucket];
      b.n++; b.sumAbs += mean; b.sumSigned += signed;
      const key = `${Math.floor(x / (W / 8))},${Math.floor(y / (H / 8))}`;
      const g = grid.get(key) || { n: 0, sum: 0, max: 0 };
      g.n++; g.sum += mean; g.max = Math.max(g.max, maxd);
      grid.set(key, g);
    }
    diffAll.push(mean);
  }
}

diffSubj.sort((x, y) => x - y);
lumaA.sort((x, y) => x - y);
lumaB.sort((x, y) => x - y);

const s = (arr, p) => pct(arr, p);
const total = W * H;
const report = {
  size: [W, H],
  bgA, bgB,
  subjectPixels: subjCount,
  subjectRatio: subjCount / total,
  bgMismatch: bgBgCount / total,
  diff: {
    mean: diffSubj.reduce((a, b) => a + b, 0) / diffSubj.length,
    p50: s(diffSubj, 0.5), p90: s(diffSubj, 0.9), p99: s(diffSubj, 0.99),
    max: diffSubj[diffSubj.length - 1],
    over4: diffSubj.filter((v) => v > 4).length / diffSubj.length,
    over8: diffSubj.filter((v) => v > 8).length / diffSubj.length,
    over16: diffSubj.filter((v) => v > 16).length / diffSubj.length,
  },
  lumaFw: { p1: s(lumaA, 0.01), p10: s(lumaA, 0.1), p50: s(lumaA, 0.5), p90: s(lumaA, 0.9), p99: s(lumaA, 0.99), max: lumaA[lumaA.length - 1] },
  lumaBrowser: { p1: s(lumaB, 0.01), p10: s(lumaB, 0.1), p50: s(lumaB, 0.5), p90: s(lumaB, 0.9), p99: s(lumaB, 0.99), max: lumaB[lumaB.length - 1] },
  byBrowserLuma: buckets.map((b, i) => b.n ? ({
    range: `${Math.round(i * 28.34)}-${Math.round((i + 1) * 28.34)}`,
    n: b.n, meanAbsDiff: +(b.sumAbs / b.n).toFixed(2), meanSignedLumaDiff: +(b.sumSigned / b.n).toFixed(2),
  }) : null).filter(Boolean),
  grid: [...grid.entries()].map(([k, g]) => ({ cell: k, n: g.n, mean: +(g.sum / g.n).toFixed(1), max: g.max }))
    .sort((a, b) => b.mean - a.mean).slice(0, 8),
};

if (wantJson) { console.log(JSON.stringify(report, null, 2)); process.exit(0); }

const f = (v, d = 1) => Number(v).toFixed(d);
console.log(`尺寸 ${W}x${H}  主体 ${subjCount} px (${f(report.subjectRatio * 100)}%)`);
console.log(`背景  霜天 rgb(${bgA})  浏览器 rgb(${bgB})   背景不一致占比 ${f(report.bgMismatch * 100, 2)}%`);
console.log('');
console.log('── 逐像素通道差（主体区，单通道平均）──');
console.log(`  均值 ${f(report.diff.mean, 2)}   p50 ${f(report.diff.p50)}   p90 ${f(report.diff.p90)}   p99 ${f(report.diff.p99)}   最大 ${f(report.diff.max)}`);
console.log(`  差>4 的像素 ${f(report.diff.over4 * 100)}%   差>8 ${f(report.diff.over8 * 100)}%   差>16 ${f(report.diff.over16 * 100)}%`);
console.log('');
console.log('── 亮度层次（主体区 luma）──');
console.log(`        p1    p10   p50   p90   p99   max`);
console.log(`  霜天  ${[report.lumaFw.p1, report.lumaFw.p10, report.lumaFw.p50, report.lumaFw.p90, report.lumaFw.p99, report.lumaFw.max].map((v) => f(v).padStart(5)).join(' ')}`);
console.log(`  浏览器${[report.lumaBrowser.p1, report.lumaBrowser.p10, report.lumaBrowser.p50, report.lumaBrowser.p90, report.lumaBrowser.p99, report.lumaBrowser.max].map((v) => f(v).padStart(6)).join(' ')}`);
console.log('');
console.log('── 按浏览器亮度分桶（归因：全局偏移 or 只差亮部）──');
console.log('  浏览器luma区间     像素数    平均绝对差   平均亮度偏移(霜天-浏览器)');
for (const b of report.byBrowserLuma) {
  console.log(`  ${b.range.padEnd(10)} ${String(b.n).padStart(9)} ${String(b.meanAbsDiff).padStart(11)} ${String(b.meanSignedLumaDiff).padStart(14)}`);
}
console.log('');
console.log('── 差异最集中的 8 个网格（8×8 分格）──');
for (const g of report.grid) console.log(`  格 ${g.cell.padEnd(5)} n=${String(g.n).padStart(6)} 平均差 ${g.mean}  最大 ${g.max}`);
