import { describe, expect, test } from "bun:test"
import { currentToolFetchSession, runInToolFetchScope, runWithoutFetchProxy } from "./fetch-scope"

describe("fetch-scope", () => {
  test("outside scope: no session", () => {
    expect(currentToolFetchSession()).toBeUndefined()
  })

  test("runInToolFetchScope carries sessionId through async chain", async () => {
    const inner = await runInToolFetchScope("s1", async () => {
      await Bun.sleep(1)
      return currentToolFetchSession()
    })
    expect(inner).toBe("s1")
    expect(currentToolFetchSession()).toBeUndefined()
  })

  test("nested runWithoutFetchProxy clears scope (LLM 豁免)", async () => {
    const inner = await runInToolFetchScope("s2", async () => {
      expect(currentToolFetchSession()).toBe("s2")
      return runWithoutFetchProxy(async () => currentToolFetchSession())
    })
    expect(inner).toBeUndefined()
  })

  test("runWithoutFetchProxy restores outer scope after completion", async () => {
    await runInToolFetchScope("s3", async () => {
      await runWithoutFetchProxy(async () => Bun.sleep(1))
      expect(currentToolFetchSession()).toBe("s3")
    })
  })
})
