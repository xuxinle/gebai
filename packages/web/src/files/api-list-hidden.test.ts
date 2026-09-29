/**
 * `FsApi.list` 的 `showHidden` 序列化：**false 必须显式发 `0`**。
 *
 * 为什么值得测：`req()` 会把 `false` 参数整条丢掉（只把 `true` 发成 `1`），于是「不显示隐藏文件」
 * 在请求里退化成「没给」、被服务端配置的默认值接管——菜单里点了关，树里照旧列着 `.env`/`.git`
 * （实机复现过，且不报错）。三态口径：`true` → `1`、`false` → `0`、不给 → 不带参数。
 */
import { describe, expect, test } from "bun:test"
import { FsApi } from "./api"

/** 拦下 fetch，返回合法 JSON；用完还原（不整体替换全局，只换 fetch 一个）。 */
function captureFetch(): { urls: string[]; restore: () => void } {
  const urls: string[] = []
  const prev = globalThis.fetch
  globalThis.fetch = (async (url: string) => {
    urls.push(String(url))
    const body = { root: "r1", path: "", entries: [], truncated: false, total: 0, showHidden: false }
    return new Response(JSON.stringify(body), { status: 200, headers: { "content-type": "application/json" } })
  }) as typeof fetch
  return {
    urls,
    restore: () => {
      globalThis.fetch = prev
    },
  }
}

/** 本文件只关心查询串（请求地址由 `withCtx` 拼成相对路径，见 url-base）。 */
if (!(location as { origin?: string }).origin) {
  Object.defineProperty(location, "origin", { value: "http://localhost", configurable: true })
}

describe("FsApi.list 的 showHidden 序列化", () => {
  test("false 显式发 showHidden=0（与服务端默认值区分开）", async () => {
    const cap = captureFetch()
    try {
      await new FsApi(() => ({}), () => undefined).list("r1", "", { showHidden: false, sort: "name" })
      expect(cap.urls.at(-1)).toContain("showHidden=0")
    } finally {
      cap.restore()
    }
  })

  test("true 发 showHidden=1", async () => {
    const cap = captureFetch()
    try {
      await new FsApi(() => ({}), () => undefined).list("r1", "", { showHidden: true })
      expect(cap.urls.at(-1)).toContain("showHidden=1")
    } finally {
      cap.restore()
    }
  })

  test("不给时不带该参数（由服务端配置的默认值接管）", async () => {
    const cap = captureFetch()
    try {
      await new FsApi(() => ({}), () => undefined).list("r1", "")
      expect(cap.urls.at(-1)).not.toContain("showHidden")
    } finally {
      cap.restore()
    }
  })
})
