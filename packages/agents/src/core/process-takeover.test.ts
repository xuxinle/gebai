/**
 * 进程接管用例：disable 开关、正常路径（注入 spawn 替身避免真杀进程）。
 */
import { describe, expect, test } from "bun:test"
import { takeoverProcess } from "./process-takeover"

describe("takeoverProcess", () => {
  test("GEBAI_TAKEOVER_DISABLE=1 时整体跳过（CI/无权限环境）", async () => {
    const r = await takeoverProcess("C:/x/xiangqi.exe", { env: { GEBAI_TAKEOVER_DISABLE: "1" } })
    expect(r.killed).toEqual([])
    expect(r.warnings).toEqual([])
    expect(r.survivors).toEqual([])
  })

  test("没有旧进程时返回空（枚举替身返回空表）", async () => {
    // 借助 disable 之外的注入没法直接 mock 内部枚举——但空结果路径不 spawn 额外进程。
    // 这里真实跑一次查找一个必定不存在的 exe 路径（fast-path：枚举返回值空，不 kill）。
    const r = await takeoverProcess("C:/path/definitely-not-existing-xyz/xiangqi.exe", {
      env: { PATH: "" },
      waitExitMs: 100,
    })
    expect(r.killed).toEqual([])
    expect(r.survivors).toEqual([])
  })
})