/**
 * 仓库 BOM 自检判定核的单测。
 *
 * 闸门通过时是绿、失败时是红——只有单测能在它「悄悄放过该报的」时立刻发现（与 check-repo-core
 * 同一理由）。这里钉三件事：① 只检文本类扩展名（二进制里出现同样三字节是数据，不是 BOM）；
 * ② 恰好 `EF BB BF` 才算（`EF BB` 两字节或 `EF BB BF` 出现在文件中部都不算）；
 * ③ 判定核的读数只取前 3 字节（大文件不该被读进来）。
 */
import { describe, expect, test } from "bun:test"
import { findBomOffenders, hasUtf8Bom, needsBomCheck } from "./check-bom-core"

describe("仓库 BOM 自检判定核", () => {
  test("只检文本类扩展名；二进制/图片/字体不检（避免误报）", () => {
    for (const f of ["a.ts", "b.json", "c.md", "d.css", "e.py", "g.yml", "h.txt", "i.cpp", "j.hpp"]) expect(`${f}:${needsBomCheck(f)}`).toBe(`${f}:true`)
    for (const f of ["a.png", "b.woff2", "c.wasm", "d.zip", "e.pdf", "noext"]) expect(`${f}:${needsBomCheck(f)}`).toBe(`${f}:false`)
    // 大小写不敏感
    expect(needsBomCheck("README.MD")).toBe(true)
    expect(needsBomCheck("A.Ts")).toBe(true)
  })

  test("PowerShell/批处理脚本**不进检出**（它们必须带 BOM，见 BOM_REQUIRED_EXTENSIONS）", () => {
    /* 不是“忘了检”，而是实测的反向要求：PowerShell 5.1 读无 BOM 的 .ps1 会按 ANSI（简中 GBK）解，
     * 带中文的脚本去 BOM 后**整个脚本静默无输出**。故它们不进检出集合，
     * 即使文件头真是 BOM，也不该被报成违规。 */
    for (const f of ["a.ps1", "b.bat", "c.cmd", "D.PS1"]) expect(`${f}:${needsBomCheck(f)}`).toBe(`${f}:false`)
    const read = () => new Uint8Array([0xef, 0xbb, 0xbf])
    expect(findBomOffenders(["x.ps1", "y.bat"], read)).toEqual([])
  })

  test("恰好 EF BB BF 才算 BOM（长度不足或前缀不同都不算）", () => {
    expect(hasUtf8Bom(new Uint8Array([0xef, 0xbb, 0xbf]))).toBe(true)
    expect(hasUtf8Bom(new Uint8Array([0xef, 0xbb, 0xbf, 0x61]))).toBe(true)
    expect(hasUtf8Bom(new Uint8Array([0xef, 0xbb]))).toBe(false)
    expect(hasUtf8Bom(new Uint8Array([0xef, 0xbb, 0x40]))).toBe(false)
    expect(hasUtf8Bom(new Uint8Array([0x61, 0xef, 0xbb, 0xbf]))).toBe(false) // BOM 必须在文件头
    expect(hasUtf8Bom(new Uint8Array([]))).toBe(false)
  })

  test("挑出带 BOM 的文件、跳过非文本类与被读失败的文件", () => {
    const table: Record<string, number[]> = {
      "ok.ts": [0x63, 0x6f, 0x6e],
      "bom.ts": [0xef, 0xbb, 0xbf],
      "bom.png": [0xef, 0xbb, 0xbf], // 非文本扩展名：不检
      "missing.md": [],
    }
    const read = (rel: string) => (table[rel] && table[rel]!.length >= 3 ? new Uint8Array(table[rel]!) : null)
    expect(findBomOffenders(Object.keys(table), read)).toEqual(["bom.ts"])
  })

  test("读数只问前 3 字节（大仓库不为查文件头把文件读进来）", () => {
    const asked: string[] = []
    const read = (rel: string) => {
      asked.push(rel)
      return new Uint8Array([0x61, 0x62, 0x63])
    }
    findBomOffenders(["a.ts", "b.png"], read)
    expect(asked).toEqual(["a.ts"]) // 非文本类根本不去读
  })
})
