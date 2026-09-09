/** 文本文件编解码与行尾工具（read/edit 共用）：编码探测（BOM / UTF-8 / UTF-16 / GBK）、按原编码回写、
 *  LF 归一位置映射——文件工具在不同平台与编码/行尾环境下行为一致（非 UTF-8 文件不再被按 UTF-8 误读误写）。 */

export type TextEncoding = "utf-8" | "utf-8-bom" | "utf-16le" | "utf-16be" | "gbk"

/** 编码显示名（工具输出注记用）。 */
export const ENCODING_LABEL: Record<TextEncoding, string> = {
  "utf-8": "UTF-8",
  "utf-8-bom": "UTF-8 BOM",
  "utf-16le": "UTF-16 LE",
  "utf-16be": "UTF-16 BE",
  gbk: "GBK",
}

export interface DecodedText {
  /** 解码文本：不含 BOM，行尾保持原样（CRLF/LF/CR 混合不归一） */
  text: string
  encoding: TextEncoding
  /** 文件主导行尾（无换行时 "\n"） */
  eol: "\n" | "\r\n"
  /** 原始字节 */
  bytes: Uint8Array
}

/** 按编码标签严格解码（非法序列抛错 → null）；gbk 等标签不在 @types/node 的编码枚举内，断言绕过类型限制。 */
function decodeStrict(label: string, bytes: Uint8Array): string | null {
  try {
    return new TextDecoder(label as never, { fatal: true }).decode(bytes)
  } catch {
    return null
  }
}

/** 无 BOM UTF-16 启发式：NUL 字节占比 ≥30% 且集中在同一字节位（LE 的高字节在奇数位，BE 在偶数位），
 *  且按该字节序解码后无控制字符（	

 除外）——纯 ASCII 内容的 UTF-16 才能与二进制/其他编码区分。 */
function sniffUtf16NoBom(bytes: Uint8Array): TextEncoding | null {
  if (bytes.length < 4 || bytes.length % 2 !== 0) return null
  let nul = 0
  let evenNul = 0
  let oddNul = 0
  for (let i = 0; i < bytes.length; i++) {
    if (bytes[i] !== 0) continue
    nul++
    if (i % 2 === 0) evenNul++
    else oddNul++
  }
  if (nul * 10 < bytes.length * 3) return null
  const guess: TextEncoding | null = oddNul >= evenNul * 3 ? "utf-16le" : evenNul >= oddNul * 3 ? "utf-16be" : null
  if (!guess) return null
  const text = decodeStrict(guess, bytes)
  if (text === null || /[\u0000-\u0008\u000b\u000c\u000e-\u001f\ufffd]/.test(text)) return null
  return guess
}

/** GBK 文本判定：无 NUL、控制字符极少、按 GBK 严格解码成功且含 CJK/全角字符（纯 ASCII 归 UTF-8）。 */
function looksGbk(bytes: Uint8Array): boolean {
  if (bytes.includes(0)) return false
  let ctrl = 0
  for (const b of bytes) if (b < 0x20 && b !== 9 && b !== 10 && b !== 13) ctrl++
  if (ctrl / Math.max(1, bytes.length) > 0.05) return false
  const s = decodeStrict("gbk", bytes)
  return s !== null && /[\u4e00-\u9fff\u3000-\u303f\uff01-\uff60]/.test(s)
}

/** 编码探测（字节级）：BOM 优先，其次无 BOM UTF-16 启发式，再 UTF-8 严格解码，最后 GBK；
 *  均不命中返回 null（二进制或未知编码）。 */
export function detectEncoding(bytes: Uint8Array): { encoding: TextEncoding; bomLength: number } | null {
  if (bytes.length >= 3 && bytes[0] === 0xef && bytes[1] === 0xbb && bytes[2] === 0xbf) return { encoding: "utf-8-bom", bomLength: 3 }
  if (bytes.length >= 2 && bytes[0] === 0xff && bytes[1] === 0xfe) {
    // UTF-32 LE 的 BOM 前缀同为 FF FE，其后两字节为 0——按二进制处理而非 UTF-16
    if (!(bytes.length >= 4 && bytes[2] === 0 && bytes[3] === 0)) return { encoding: "utf-16le", bomLength: 2 }
  }
  if (bytes.length >= 2 && bytes[0] === 0xfe && bytes[1] === 0xff) return { encoding: "utf-16be", bomLength: 2 }
  const u16 = sniffUtf16NoBom(bytes)
  if (u16) return { encoding: u16, bomLength: 0 }
  const utf8 = decodeStrict("utf-8", bytes)
  if (utf8 !== null && !looksBinaryText(utf8)) return { encoding: "utf-8", bomLength: 0 }
  if (looksGbk(bytes)) return { encoding: "gbk", bomLength: 0 }
  return null
}

/** 文本内容判定：含 NUL 或控制字符（\t\n\r 除外）占比 >30% 视为二进制（合法 UTF-8 的控制字节流不算文本）。 */
function looksBinaryText(text: string): boolean {
  let ctrl = 0
  for (let i = 0; i < text.length; i++) {
    const c = text.charCodeAt(i)
    if (c === 0) return true
    if (c < 0x20 && c !== 9 && c !== 10 && c !== 13) ctrl++
  }
  return ctrl / Math.max(1, text.length) > 0.3
}

/** 主导行尾判定：CRLF 数 ≥ 孤立 LF/CR 数且非零时按 CRLF（与行尾还原口径一致）。 */
export function dominantEol(text: string): "\n" | "\r\n" {
  const crlf = (text.match(/\r\n/g) || []).length
  const lone = (text.match(/\r(?!\n)/g) || []).length + (text.match(/(?<!\r)\n/g) || []).length
  return crlf > 0 && crlf >= lone ? "\r\n" : "\n"
}

/** 解码为文本：探测编码 → 去 BOM 解码 → 附原始字节与主导行尾；不可识别（二进制/未知编码）返回 null。 */
export function decodeTextFile(bytes: Uint8Array): DecodedText | null {
  const det = detectEncoding(bytes)
  if (!det) return null
  const label = det.encoding === "utf-8-bom" ? "utf-8" : det.encoding
  let text = decodeStrict(label, bytes.subarray(det.bomLength))
  if (text === null) return null
  // 无 BOM 标签下解码保留的前导 U+FEFF 兜底剥离（与 BOM 检测口径一致）
  if (text.charCodeAt(0) === 0xfeff) text = text.slice(1)
  return { text, encoding: det.encoding, eol: dominantEol(text), bytes }
}

/** 按编码编码文本（含 BOM）；GBK 无编码表，需字节级替换（见 edit），此处不支持。 */
export function encodeText(text: string, encoding: TextEncoding): Uint8Array {
  switch (encoding) {
    case "utf-8":
      return new TextEncoder().encode(text)
    case "utf-8-bom":
      return concatBytes([new Uint8Array([0xef, 0xbb, 0xbf]), new TextEncoder().encode(text)])
    case "utf-16le":
      return concatBytes([new Uint8Array([0xff, 0xfe]), utf16Bytes(text, true)])
    case "utf-16be":
      return concatBytes([new Uint8Array([0xfe, 0xff]), utf16Bytes(text, false)])
    default:
      throw new Error("GBK 编码需字节级替换")
  }
}

function utf16Bytes(text: string, le: boolean): Uint8Array {
  const out = new Uint8Array(text.length * 2)
  for (let i = 0; i < text.length; i++) {
    const c = text.charCodeAt(i)
    out[i * 2] = le ? c & 0xff : c >> 8
    out[i * 2 + 1] = le ? c >> 8 : c & 0xff
  }
  return out
}

/** 字节拼接。 */
export function concatBytes(parts: Uint8Array[]): Uint8Array {
  const total = parts.reduce((n, p) => n + p.length, 0)
  const out = new Uint8Array(total)
  let off = 0
  for (const p of parts) {
    out.set(p, off)
    off += p.length
  }
  return out
}

/** LF 归一（CRLF 与裸 CR → LF）并返回映射：归一文本第 i 个字符对应源文本的索引（末位哨兵 = 源文本长度）。 */
export function normalizeEol(text: string): { text: string; toSource: number[] } {
  const chars: string[] = []
  const toSource: number[] = []
  for (let i = 0; i < text.length; i++) {
    const ch = text[i]
    if (ch === "\r") {
      chars.push("\n")
      toSource.push(i)
      if (text[i + 1] === "\n") i++
    } else {
      chars.push(ch)
      toSource.push(i)
    }
  }
  toSource.push(text.length)
  return { text: chars.join(""), toSource }
}

/** GBK 文本的字符 → 字节偏移映射（ASCII 单字节、其余双字节；末位哨兵 = 字节长度）。
 *  GB18030 四字节序列（BMP 外码位）在 GBK 解码器下产出代理对，字符与字节不再一一对应 → 返回 null 表示不可映射。 */
export function gbkCharOffsets(text: string): number[] | null {
  for (let i = 0; i < text.length; i++) {
    const c = text.charCodeAt(i)
    if (c >= 0xd800 && c <= 0xdfff) return null
  }
  const offs = new Array<number>(text.length + 1)
  let off = 0
  for (let i = 0; i < text.length; i++) {
    offs[i] = off
    off += text.charCodeAt(i) < 0x80 ? 1 : 2
  }
  offs[text.length] = off
  return offs
}
