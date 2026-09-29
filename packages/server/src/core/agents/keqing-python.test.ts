/**
 * 客卿 Python 侧测试的 bun 包装：把 `keqing/python/tests/run_tests.py` 纳入 `bun test` 视野。
 *
 * 为什么需要：`keqing/python/**` 的工具（vision / docqa / imgproc）此前没有可重复的测试，
 * 改动只能靠临时脚本验证，回归无处沉淀；而它们的失败方式往往很隐晦（协议握手、
 * 资源路径回退、分块 OCR 去重），正是最该有回归的地方。
 *
 * 约定：
 * - 具体用例在 `keqing/python/tests/*_test.py`（纯标准库运行器，pytest 风格命名但不依赖 pytest）
 * - **缺解释器即跳过**（不是失败）：不是所有部署形态都装 Python，缺它不该让 CI 变红
 * - 用 `GEBAI_TEST_PYTHON` 可指定解释器（默认 `python3`）
 */
import { describe, expect, test } from "bun:test"
import { existsSync } from "node:fs"
import { join } from "node:path"

const repoRoot = join(import.meta.dirname, "../../../../..")
const runner = join(repoRoot, "keqing/python/tests/run_tests.py")

function pythonBin(): string {
  return process.env.GEBAI_TEST_PYTHON || "python3"
}

function available(bin: string): boolean {
  const probe = Bun.spawnSync({ cmd: [bin, "-c", "import sys; print(sys.version_info[0])"], stdout: "pipe", stderr: "pipe" })
  return probe.exitCode === 0
}

describe("客卿 Python 侧测试（keqing/python/tests）", () => {
  test("纯函数与协议契约用例全部通过", async () => {
    if (!existsSync(runner)) {
      console.warn(`[keqing-python] 跳过：未找到运行器 ${runner}`)
      return
    }
    const bin = pythonBin()
    if (!available(bin)) {
      console.warn(`[keqing-python] 跳过：未找到 Python 解释器（${bin}；可用 GEBAI_TEST_PYTHON 指定）`)
      return
    }
    const proc = Bun.spawn({ cmd: [bin, runner], cwd: repoRoot, stdout: "pipe", stderr: "pipe" })
    const [stdout, stderr] = await Promise.all([new Response(proc.stdout).text(), new Response(proc.stderr).text()])
    const code = await proc.exited
    // 失败时把运行器输出原样带出（否则只看到"退出码非 0"，定位还得再跑一次）
    expect(code, `${stdout}\n${stderr}`).toBe(0)
    expect(stdout).toContain("0 failed")
  }, 120_000)
})
