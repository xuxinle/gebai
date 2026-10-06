// 浏览器侧参照截图（**真窗口 headed**，不是 headless）+ 逐行实际字体归属导出。
//
// 为什么必须 headed：headless Chromium 不走桌面 GPU 合成与字体渲染链路，
// 字比用户实际看到的更浅——拿它当基准会把方向判反（见 DESIGN §4.3.7.18 / SKIA 调研 §一）。
//
// 为什么必须导出**实际字体**：对照页写的是族名，浏览器按 fontconfig 回退、霜天按自己的候选链
// 回退——两侧最终用的可能**不是同一套字体**（实测：'Segoe UI','Microsoft YaHei' 在 Linux 上
// 两个名字都不存在，浏览器落到 Liberation Serif + Noto Sans CJK **JP**）。那时量到的是
// 字体差异而不是渲染差异。导出的归属由 `text_ab_diff.py` 与霜天侧字体链比对。
//
// 用法：node tools/text_ab_shot.mjs <html> <out.png> <name> [scale] [rows]
//   <name> 用于给 boxes/fonts 侧车文件命名（<name>.boxes.json / <name>.fonts.json）
import { chromium } from "playwright";
import fs from "node:fs";
import path from "node:path";

const [html, out, name, scaleArg, rowsArg] = process.argv.slice(2);
if (!html || !out || !name) {
  console.error("用法: node text_ab_shot.mjs <html> <out.png> <name> [scale=1.5] [rows=64]");
  process.exit(2);
}
const scale = Number(scaleArg ?? 1.5);
const rows = Number(rowsArg ?? 64);

// 浏览器可执行文件：优先 ST_BROWSER，其次系统 Chrome，最后 playwright 缓存。
const candidates = [
  process.env.ST_BROWSER,
  "/usr/bin/google-chrome",
  "/usr/bin/chromium",
  ...fs.existsSync("/root/.cache/ms-playwright")
    ? fs.readdirSync("/root/.cache/ms-playwright")
        .filter((d) => d.startsWith("chromium-"))
        .map((d) => `/root/.cache/ms-playwright/${d}/chrome-linux64/chrome`)
    : [],
].filter(Boolean);
const exe = candidates.find((p) => fs.existsSync(p));
if (!exe) throw new Error("找不到浏览器可执行文件（设 ST_BROWSER 指定）");

const browser = await chromium.launch({
  executablePath: exe,
  headless: false,                       // 真窗口：走桌面字体渲染链路
  args: [
    "--force-color-profile=srgb",
    "--font-render-hinting=medium",
    "--hide-scrollbars",
    "--no-sandbox",
    "--disable-dev-shm-usage",
  ],
});
// 视口按页面实际盒子给，避免出现滚动条或外边距影响坐标。
const box = await (async () => {
  const raw = fs.readFileSync(html, "utf8");
  const w = /width:\s*([\d.]+)px/.exec(raw)?.[1] ?? "900";
  const h = /height:\s*([\d.]+)px/.exec(raw)?.[1] ?? "625";
  return { w: Number(w), h: Number(h) };
})();
const ctx = await browser.newContext({
  deviceScaleFactor: scale,
  viewport: { width: Math.ceil(box.w), height: Math.ceil(box.h) },
});
const page = await ctx.newPage();
await page.goto("file://" + path.resolve(html));
await page.waitForTimeout(500);

const boxes = await page.evaluate((n) => {
  const outRows = [];
  for (let i = 0; i < n; i += 1) {
    const el = document.getElementById("r" + String(i).padStart(2, "0"));
    if (!el) continue;
    const r = el.getBoundingClientRect();
    outRows.push([r.left, r.top, r.bottom, r.right].map((v) => Math.round(v * devicePixelRatio)));
  }
  return outRows;
}, rows);

// 逐行实际字体（CDP）：两侧字体链必须同一套，否则量尺无意义。
const cdp = await ctx.newCDPSession(page);
await cdp.send("DOM.enable");
await cdp.send("CSS.enable");
const { root } = await cdp.send("DOM.getDocument", { depth: -1 });
const fonts = [];
for (let i = 0; i < rows; i += 1) {
  const selector = "#r" + String(i).padStart(2, "0");
  const found = await cdp.send("DOM.querySelector", { nodeId: root.nodeId, selector });
  if (!found.nodeId) continue;
  const res = await cdp.send("CSS.getPlatformFontsForNode", { nodeId: found.nodeId });
  fonts.push({
    row: i,
    faces: res.fonts.map((f) => ({ family: f.familyName, glyphs: f.glyphCount })),
  });
}

await page.screenshot({ path: out, clip: { x: 0, y: 0, width: box.w, height: box.h } });
const dir = path.dirname(out);
fs.writeFileSync(path.join(dir, `${name}.boxes.json`), JSON.stringify(boxes));
fs.writeFileSync(path.join(dir, `${name}.fonts.json`), JSON.stringify(fonts, null, 1));
console.log(JSON.stringify({
  size: [Math.round(box.w * scale), Math.round(box.h * scale)],
  rows: boxes.length,
  dpr: scale,
  faces: [...new Set(fonts.flatMap((f) => f.faces.map((x) => x.family)))],
}));
await browser.close();
