/**
 * 图标生成：canonical 源 `packages/web/public/favicon.svg`（大脑）→
 * ① 多尺寸 PNG 打包 Windows ICO（16-128 无压缩 BMP + 256 PNG）→ `packages/desktop/icons/icon.ico`
 *    （供 bun build --compile --icon 与 launcher winresource 嵌入 exe）
 * ② 回写 `packages/web/index.html` 的 favicon data URI（与 canonical 源单一来源，basePath 免疫）。
 * 依赖 @resvg/resvg-js（desktop devDependency）。幂等，构建链 server:build 前置调用。
 */
import { readFileSync, writeFileSync, mkdirSync } from "node:fs"
import { join } from "node:path"
import { Resvg } from "@resvg/resvg-js"

const root = join(import.meta.dirname, "..")
const svgPath = join(root, "..", "web", "public", "favicon.svg")
const icoPath = join(root, "icons", "icon.ico")
const htmlPath = join(root, "..", "web", "index.html")

const svg = readFileSync(svgPath, "utf8")

/** BMP（无压缩 32bpp，底行序 + 全零 AND 掩码）与 PNG（256）混合的 ICO 条目。rgba 为原始 RGBA 像素。 */
function rgbaToBmpEntry(rgba: Uint8Array, size: number): Buffer {
  const row = size * 4
  const maskRow = Math.ceil(size / 8 / 4) * 4
  // AND 掩码区必须全零（不透明）：alloc 清零分配，防随机透明像素
  const img = Buffer.alloc(40 + row * size + maskRow * size)
  img.writeUInt32LE(40, 0)
  img.writeInt32LE(size, 4)
  img.writeInt32LE(size * 2, 8)
  img.writeUInt16LE(1, 12)
  img.writeUInt16LE(32, 14)
  img.writeUInt32LE(row * size + maskRow * size, 20)
  for (let y = 0; y < size; y++) {
    const src = Buffer.from(rgba).subarray((size - 1 - y) * row, (size - y) * row)
    src.copy(img, 40 + y * row)
    for (let x = 0; x < row; x += 4) {
      const r = img[40 + y * row + x]
      img[40 + y * row + x] = img[40 + y * row + x + 2]
      img[40 + y * row + x + 2] = r
    }
  }
  return img
}

function buildIco(entries: Map<number, { rgba: Uint8Array; png: Uint8Array }>): Buffer {
  const sizes = [...entries.keys()].sort((a, b) => a - b)
  const header = Buffer.alloc(6 + sizes.length * 16)
  header.writeUInt16LE(1, 2)
  header.writeUInt16LE(sizes.length, 4)
  let offset = header.length
  const datas: Buffer[] = []
  sizes.forEach((size, i) => {
    const entry = 6 + i * 16
    const { rgba, png } = entries.get(size)!
    const data = size === 256 ? Buffer.from(png) : rgbaToBmpEntry(rgba, size)
    header[entry] = size === 256 ? 0 : size
    header[entry + 1] = size === 256 ? 0 : size
    header.writeUInt16LE(1, entry + 4)
    header.writeUInt16LE(32, entry + 6)
    header.writeUInt32LE(data.length, entry + 8)
    header.writeUInt32LE(offset, entry + 12)
    offset += data.length
    datas.push(data)
  })
  return Buffer.concat([header, ...datas])
}

function renderPng(size: number): Uint8Array {
  const resvg = new Resvg(svg, { fitTo: { mode: "width", value: size } })
  return resvg.render().asPng()
}

function renderRgba(size: number): Uint8Array {
  const resvg = new Resvg(svg, { fitTo: { mode: "width", value: size } })
  return resvg.render().pixels
}

mkdirSync(join(root, "icons"), { recursive: true })
const entries = new Map<number, { rgba: Uint8Array; png: Uint8Array }>()
for (const size of [16, 24, 32, 48, 64, 128, 256]) entries.set(size, { rgba: renderRgba(size), png: renderPng(size) })
writeFileSync(icoPath, buildIco(entries))
// 32px 原始 RGBA：tao 窗口图标（任务栏/标题栏）用，launcher include_bytes 内嵌
writeFileSync(join(root, "icons", "icon32.rgba"), Buffer.from(entries.get(32)!.rgba))
console.log(`[gen-icon] icon.ico (${entries.size} sizes) -> ${icoPath}; icon32.rgba 已生成`)

// 回写 index.html favicon data URI（保持内联：basePath/反代场景零依赖）
const dataUri = `data:image/svg+xml,${encodeURIComponent(svg.trim()).replace(/'/g, "%27")}`
const html = readFileSync(htmlPath, "utf8")
const faviconRe = /<link rel="icon" href="data:image\/svg\+xml,[^"]*"\s*\/?>/
if (!faviconRe.test(html)) {
  console.warn("[gen-icon] index.html 未找到 favicon data URI（可能已被替换为文件引用），跳过回写")
} else {
  writeFileSync(htmlPath, html.replace(faviconRe, `<link rel="icon" href="${dataUri}"/>`))
  console.log("[gen-icon] index.html favicon data URI 已更新")
}
