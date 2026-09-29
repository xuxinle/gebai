/**
 * 工作台相对 URL 的产物形态与回落口径。
 *
 * 相对写法要在两种部署位置下都成立——根部署 `/files` 与反向代理子路径 `/gebai/files`
 * （以及多级前缀），这是「同一份产物挂到任意前缀下都不用改代码」的依据；
 * 页面位置非标准时回落前端基准前缀，避免请求落到错误层级。
 */
import { describe, expect, test } from "bun:test"
import { wbUrl, wbAbsUrl } from "./url-base"

/** 以指定文档位置运行（`document.baseURI` 即浏览器的相对解析基准）。 */
function at(href: string, fn: () => void): void {
  const g = globalThis as Record<string, unknown>
  const had = "document" in g
  const prev = g.document
  Object.defineProperty(g, "document", { value: { baseURI: href }, configurable: true, writable: true })
  try {
    fn()
  } finally {
    if (had) Object.defineProperty(g, "document", { value: prev, configurable: true, writable: true })
    else delete g.document
  }
}

describe("工作台相对 URL", () => {
  test("反向代理子路径：相对写法由页面位置解析，请求自带前缀", () => {
    at("http://host/gebai/files", () => {
      expect(wbUrl("/api/v1/fs/list")).toBe("./api/v1/fs/list")
      expect(wbAbsUrl("/vendor/monaco/vs")).toBe("http://host/gebai/vendor/monaco/vs")
    })
  })

  test("根部署：无前缀", () => {
    at("http://host/files", () => {
      expect(wbUrl("/api/v1/fs/list")).toBe("./api/v1/fs/list")
      expect(wbAbsUrl("/api/v1/fs/list")).toBe("http://host/api/v1/fs/list")
    })
  })

  test("多级前缀：前缀层数不影响写法", () => {
    at("http://host/a/b/files", () => {
      expect(wbAbsUrl("/vendor/xterm/xterm.css")).toBe("http://host/a/b/vendor/xterm/xterm.css")
    })
  })

  test("端点写法统一：带不带前导斜杠等价", () => {
    at("http://host/gebai/files", () => {
      expect(wbUrl("api/v1/fs/list")).toBe(wbUrl("/api/v1/fs/list"))
      expect(wbUrl("//api/v1/fs/list")).toBe(wbUrl("/api/v1/fs/list"))
    })
  })

  test("页面位置非标准时回落基准前缀（相对解析会落到别处）", () => {
    at("http://host/gebai/nested/page", () => {
      expect(wbUrl("/api/v1/fs/list")).toBe("/gebai/nested/page/api/v1/fs/list")
    })
  })
})
