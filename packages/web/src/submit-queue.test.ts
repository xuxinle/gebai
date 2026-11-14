/**
 * 提交串行化（`createSubmitQueue`）与「草稿页只发一次只建一个会话」的接线守卫。
 *
 * 现场：草稿页发送一次提示词，偶发多出一个会话（同一句提示词落进两个新会话）。根因是首条消息的
 * 建会话分支要 await 网络往返，而「是否草稿页」读的是当前会话状态——空窗里到达的第二次提交会再走
 * 一遍建会话。重复来源都是正常输入形态（按住回车的键盘自动重复、软键盘发送键、连点发送按钮）。
 *
 * 本文件锁四件事：
 * 1. 单飞：并发提交不并发进入 run；
 * 2. 尾随合并：期间到达的提交在当轮结束后补跑一轮（不丢用户在那段时间里接着敲的话）、多次合并仍只补跑一次；
 * 3. 异常不粘住：run 抛错后队列仍可用，且错误如实抛给调用方；
 * 4. 接线：提交处理器经该队列串行化，输入框的提交键不吃键盘自动重复。
 */
import { describe, expect, test } from "bun:test"
import { readFileSync } from "node:fs"
import { join } from "node:path"
import { createSubmitQueue } from "./submit-queue"

const src = (file: string) => readFileSync(join(import.meta.dirname, file), "utf8")

/** 手动控制的挂起闸门：让一轮 run 停在 await 上，模拟「建会话/上传附件的网络往返」。 */
function gate() {
  let open: () => void = () => {}
  const wait = new Promise<void>((resolve) => {
    open = resolve
  })
  return { wait, open: () => open() }
}

describe("提交串行化", () => {
  test("单飞：进行中的一轮独占，并发提交不会并发进入", async () => {
    let inside = 0
    let maxInside = 0
    const g = gate()
    const submit = createSubmitQueue(async () => {
      inside++
      maxInside = Math.max(maxInside, inside)
      await g.wait
      inside--
    })
    const first = submit()
    const second = submit()
    const third = submit()
    expect(maxInside).toBe(1)
    g.open()
    await Promise.all([first, second, third])
    expect(maxInside).toBe(1)
  })

  test("尾随合并：空窗里的提交在当轮结束后补跑一轮，且只补一次", async () => {
    const seen: string[] = []
    const gates = [gate(), gate()]
    let round = 0
    const submit = createSubmitQueue(async () => {
      const idx = round++
      seen.push(`start${idx}`)
      await gates[Math.min(idx, gates.length - 1)]!.wait
      seen.push(`end${idx}`)
    })
    const first = submit()
    // 同一轮里连来三次（键盘重复/连点）：合并成一个尾随轮
    const rest = Promise.all([submit(), submit(), submit()])
    expect(seen).toEqual(["start0"])
    gates[0]!.open()
    await new Promise((r) => setTimeout(r, 0)) // 让当轮收尾、尾随轮起步
    // 尾随轮开始，且只开一轮（三次提交合并成一次）
    expect(seen).toEqual(["start0", "end0", "start1"])
    gates[1]!.open()
    await Promise.all([first, rest])
    expect(seen).toEqual(["start0", "end0", "start1", "end1"])
  })

  test("尾随轮若内容已被上一轮消费则立即返回（空跑不是丢消息）", async () => {
    let consumed = ""
    let calls = 0
    const g = gate()
    const submit = createSubmitQueue(async () => {
      calls++
      const text = consumed || "第一条"
      if (!text) return
      consumed = ""
      if (calls === 1) await g.wait
      consumed = text // 模拟：本轮取走了输入内容
    })
    const first = submit()
    const second = submit()
    g.open()
    await Promise.all([first, second])
    expect(calls).toBe(2) // 尾随轮跑了，但内容已空
  })

  test("异常不粘住：抛错后队列仍可用，错误如实抛给调用方", async () => {
    let calls = 0
    const submit = createSubmitQueue(async () => {
      calls++
      if (calls === 1) throw new Error("创建会话失败")
    })
    await expect(submit()).rejects.toThrow("创建会话失败")
    await submit() // 未粘住
    expect(calls).toBe(2)
  })
})

describe("接线：草稿页提交必须串行化", () => {
  test("提交处理器经 createSubmitQueue 串行化，而非直接并发执行建会话分支", () => {
    const s = src("main.ts")
    const at = s.indexOf('composer.addEventListener("submit"')
    expect(at).toBeGreaterThan(-1)
    const handler = s.slice(at, at + 400)
    expect(handler).toContain("void submitOnce()")
    // 建会话分支必须在被串行化的闭包里（否则空窗里的第二次提交仍会再建一个会话）
    const queueAt = s.indexOf("const submitOnce = createSubmitQueue(")
    expect(queueAt).toBeGreaterThan(-1)
    const closure = s.slice(queueAt, s.indexOf('composer.addEventListener("submit"'))
    expect(closure).toContain("await createSessionWithDraftModel()")
  })

  test("输入框的提交键不吃键盘自动重复（按住回车不该连发）", () => {
    const c = src("composer.ts")
    const at = c.indexOf('input.addEventListener("keydown"')
    expect(at).toBeGreaterThan(-1)
    const handler = c.slice(at, at + 600)
    expect(handler).toContain("e.repeat")
    expect(handler.indexOf("e.repeat")).toBeLessThan(handler.indexOf("composer.requestSubmit()"))
  })
})
