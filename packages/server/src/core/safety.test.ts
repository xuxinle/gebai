import { describe, expect, test } from "bun:test"
import { isRiskyToolName } from "./safety"

describe("safe mode risky tool names", () => {
  test("命令执行/文件修改/删除/定时任务 命中（全局名与子Agent 短名）", () => {
    for (const n of ["sh", "py", "write", "edit", "patch", "file", "delete", "cron_add", "cron_update", "cron_remove"]) {
      expect(isRiskyToolName(n)).toBe(true)
    }
    // 子Agent 命名空间剥离 {agent}_ 前缀后按短名命中
    for (const n of ["code_sh", "code_write", "code_edit", "code_patch", "code_file", "widgets_delete"]) {
      expect(isRiskyToolName(n)).toBe(true)
    }
  })

  test("只读/展示/交互类不命中", () => {
    for (const n of [
      "read", "ls", "grep", "glob", "draw", "render_html", "fetch_url", "todo", "current_time",
      "code_read", "code_glob", "code_git", "widgets_save", "widgets_get", "widgets_list",
    ]) {
      expect(isRiskyToolName(n)).toBe(false)
    }
  })
})
