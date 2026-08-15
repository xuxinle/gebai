/**
 * Windows exe 图标嵌入：解析 icons/icon.ico（scripts/gen-icon.ts 生成），经
 * kernel32 BeginUpdateResource/UpdateResource/EndUpdateResource 写入
 * RT_GROUP_ICON(14)+RT_ICON(3) 资源（条目图像数据原样保留，无重编码/重采样）。
 *
 * 背景：bun build --windows-icon 与 rcedit 均会改动像素数据（alpha 异常），故自实现。
 * 用法：bun run scripts/embed-icon.ts [exePath]（缺省 dist/gebai.exe）。
 */
import { dlopen, ptr } from "bun:ffi"
import { readFileSync } from "node:fs"
import { join } from "node:path"

const root = join(import.meta.dirname, "..")
const exePath = process.argv[2] ? join(process.cwd(), process.argv[2]) : join(root, "dist", "gebai.exe")
const icoPath = join(root, "icons", "icon.ico")

interface IcoEntry {
  width: number
  height: number
  planes: number
  bitCount: number
  data: Buffer
}

function parseIco(buf: Buffer): IcoEntry[] {
  const count = buf.readUInt16LE(4)
  const entries: IcoEntry[] = []
  for (let i = 0; i < count; i++) {
    const e = 6 + i * 16
    const size = buf.readUInt32LE(e + 8)
    const offset = buf.readUInt32LE(e + 12)
    entries.push({
      width: buf[e] || 256,
      height: buf[e + 1] || 256,
      planes: buf.readUInt16LE(e + 4),
      bitCount: buf.readUInt16LE(e + 6),
      data: Buffer.from(buf.subarray(offset, offset + size)),
    })
  }
  return entries
}

const kernel32 = dlopen("kernel32", {
  BeginUpdateResourceW: { args: ["pointer", "bool"], returns: "pointer" },
  UpdateResourceW: { args: ["pointer", "pointer", "pointer", "u16", "pointer", "u32"], returns: "bool" },
  EndUpdateResourceW: { args: ["pointer", "bool"], returns: "bool" },
})

const RT_ICON = 3
const RT_GROUP_ICON = 14
const LANG = 0x0409

const entries = parseIco(readFileSync(icoPath))
// 资源条目顺序：32px 主尺寸在首（ExtractAssociatedIcon 等简化提取器取首条目），
// 其余按尺寸降序，16/24 垫底
entries.sort((a, b) => (b.width === 32 ? 1 : 0) - (a.width === 32 ? 1 : 0) || b.width - a.width)

// RT_GROUP_ICON：ICONDIR 头 + GRPICONDIRENTRY（文件内 dwImageOffset 换成资源 ID）
const group = Buffer.alloc(6 + entries.length * 14)
group.writeUInt16LE(1, 2)
group.writeUInt16LE(entries.length, 4)
entries.forEach((entry, i) => {
  const e = 6 + i * 14
  group[e] = entry.width === 256 ? 0 : entry.width
  group[e + 1] = entry.height === 256 ? 0 : entry.height
  group.writeUInt16LE(entry.planes, e + 4)
  group.writeUInt16LE(entry.bitCount, e + 6)
  group.writeUInt32LE(entry.data.length, e + 8)
  group.writeUInt16LE(i + 1, e + 12)
})

const pathW = Buffer.from(exePath + "\0", "utf16le")
const handle = kernel32.symbols.BeginUpdateResourceW(ptr(pathW), false)
if (!handle) throw new Error(`BeginUpdateResource 失败：${exePath}`)
try {
  for (let i = 0; i < entries.length; i++) {
    const d = entries[i].data
    if (!kernel32.symbols.UpdateResourceW(handle, RT_ICON, i + 1, LANG, ptr(d), d.length)) {
      throw new Error(`UpdateResource(RT_ICON #${i + 1}) 失败`)
    }
  }
  const g = group
  if (!kernel32.symbols.UpdateResourceW(handle, RT_GROUP_ICON, 1, LANG, ptr(g), g.length)) {
    throw new Error("UpdateResource(RT_GROUP_ICON) 失败")
  }
} finally {
  if (!kernel32.symbols.EndUpdateResourceW(handle, false)) throw new Error("EndUpdateResource（写入提交）失败")
}
console.log(`[embed-icon] ${entries.length} 个图标条目 -> ${exePath}`)
