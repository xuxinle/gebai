// 浏览器侧参照截图（Windows 版）：**真窗口 headed** + CDP 截图（不走屏幕复制）。
//
// 为什么这么取（承接 tools/text_ab_shot.mjs 的教训）：
//  1. headless 的字体渲染链路与真窗口不同（无 ClearType 调校），拿它当基准会把方向判反；
//  2. 屏幕 CopyFromScreen 会被前台全屏窗口遮挡（本机实测：全屏 IDE 盖住浏览器，
//     截到的是 IDE）。CDP `page.screenshot` 由浏览器自身合成输出，不受遮挡影响。
//  3. 必须导出**实际字体归属**：页面写的是族名，浏览器可能回退到别的字族，
//     那时量到的是字体差而不是渲染差。
//
// 用法：node tools/dw_ab_shot.mjs <html> <out.png> <name> [scale=1.5]
import { chromium } from "playwright";
import fs from "node:fs";
import path from "node:path";

const [html, out, name, scaleArg] = process.argv.slice(2);
if (!html || !out || !name) {
  console.error("用法: node dw_ab_shot.mjs <html> <out.png> <name> [scale=1.5]");
  process.exit(2);
}
const scale = Number(scaleArg ?? 1.5);

const candidates = [
  process.env.ST_BROWSER,
  "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
  "C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
].filter(Boolean);
const exe = candidates.find((p) => fs.existsSync(p));
if (!exe) throw new Error("找不到浏览器（设 ST_BROWSER 指定）");

const browser = await chromium.launch({
  executablePath: exe,
  headless: false,                    // 真窗口：走桌面 ClearType 链路
  args: ["--force-color-profile=srgb", "--hide-scrollbars", "--no-sandbox",
         "--disable-features=Translate", "--force-device-scale-factor=" + scale],
});

// 页面尺寸从 HTML 的 #page 元素直接量（不靠正则猜）
const probe = await browser.newPage();
await probe.goto("file:///" + path.resolve(html).replace(/\\/g, "/"));
await probe.waitForTimeout(300);
const box = await probe.evaluate(() => {
  const p = document.getElementById("page") || document.body;
  const r = p.getBoundingClientRect();
  return { w: Math.ceil(r.width), h: Math.ceil(r.height) };
});
await probe.close();

const ctx = await browser.newContext({
  deviceScaleFactor: scale,
  viewport: { width: box.w, height: box.h },
});
const page = await ctx.newPage();
await page.goto("file:///" + path.resolve(html).replace(/\\/g, "/"));
await page.waitForTimeout(600);

// 逐行实际字体归属（CDP）：与霜天字体链比对，不一致就报错
const cdp = await ctx.newCDPSession(page);
await cdp.send("DOM.enable");
await cdp.send("CSS.enable");
const { root } = await cdp.send("DOM.getDocument", { depth: -1 });
const { nodeIds } = await cdp.send("DOM.querySelectorAll", {
  nodeId: root.nodeId, selector: ".t",
});
const fonts = [];
for (const nodeId of nodeIds) {
  const res = await cdp.send("CSS.getPlatformFontsForNode", { nodeId });
  fonts.push(res.fonts.map((f) => ({ family: f.familyName, glyphs: f.glyphCount })));
}

await page.screenshot({ path: out, clip: { x: 0, y: 0, width: box.w, height: box.h } });
const dir = path.dirname(out);
fs.writeFileSync(path.join(dir, `${name}.fonts.json`), JSON.stringify(fonts, null, 1));
console.log(JSON.stringify({
  size: [Math.round(box.w * scale), Math.round(box.h * scale)],
  dpr: scale,
  rows: fonts.length,
  faces: [...new Set(fonts.flatMap((f) => f.map((x) => x.family)))],
}));
await browser.close();
