import { describe, expect, test } from "bun:test"

describe("desktop host", () => {
  test("module loads", async () => {
    const mod = await import("./index")
    expect(typeof mod.runDesktop).toBe("function")
  })
})
