import { describe, expect, test } from "bun:test"
import { def } from "./wps"

describe("wps 子Agent（Office 文档处理）", () => {
  test("八工具齐备：word/excel/ppt 读写全覆盖，均经 projectAware 包装（project 参数）", () => {
    const names = Object.keys(def.tools!)
    expect(names.sort()).toEqual(["excel_edit", "excel_read", "excel_write", "ppt_create", "ppt_read", "word_append", "word_create", "word_read"])
    for (const n of names) {
      expect(def.tools![n].parameters.properties).toHaveProperty("project")
      // 卡片头声明 path 参数（project 由 projectAware 自动前插）
      expect(def.tools![n].card?.titleParams).toContain("path")
    }
  })

  test("安全模式姿态：写工具显式不提供（safeMode:false），读工具免声明（默认注册，实现只读）", () => {
    for (const n of ["word_create", "word_append", "excel_write", "excel_edit", "ppt_create"]) {
      expect(def.tools![n].safeMode).toBe(false)
    }
    for (const n of ["word_read", "excel_read", "ppt_read"]) {
      expect(def.tools![n].safeMode).toBeUndefined()
    }
  })

  test("全部免审批（与全局文件工具姿态一致，防盲覆盖守卫在工具体内）、按需装载", () => {
    expect(def.requiresApproval).toBeUndefined()
    for (const n of Object.keys(def.tools!)) expect(def.tools![n].requiresApproval).toBeUndefined()
    expect(def.preload).toBe(false)
    expect(def.systemPrompt).toContain("word_create")
    expect(def.systemPrompt).toContain(".docx")
  })
})
