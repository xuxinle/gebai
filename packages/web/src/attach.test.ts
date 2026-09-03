import { describe, expect, test } from "bun:test"
import { readFileSync } from "node:fs"
import { join } from "node:path"

// attach.ts 是纯副作用模块（向 sessions.ts 注册 setRunningAttach 钩子，无具名导出）：
// 入口 main.ts 丢失 `import "./attach"` 不会触发 typecheck/lint 报错、运行期钩子恒为 null 静默跳过
// ——刷新后运行中会话不恢复（在途流不续接/待决卡片不重建、任务干等到超时）。本测试以源码扫描守住该导入。
describe("attach 副作用导入", () => {
  test("入口 main.ts 保留 import \"./attach\"（丢失即运行中会话恢复链路断裂）", () => {
    const src = readFileSync(join(import.meta.dirname, "main.ts"), "utf8")
    expect(/import\s+["']\.\/attach["']/.test(src)).toBe(true)
  })
})
