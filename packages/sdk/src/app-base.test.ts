/**
 * 应用基准路径（app-base）用例：由页面 URL 推出 REST/WS/资源前缀——反向代理子路径部署下，
 * 前端不需要任何配置即自带前缀（代理剥离前缀即可）。
 */
import { afterAll, describe, expect, test } from "bun:test"
import { appBase, appPath, appWsUrl, resolveAppBase } from "./app-base"

describe("resolveAppBase：由页面 pathname 推出挂载基准", () => {
  test("根部署：主界面 / 与工作台 /files 均为空基准", () => {
    for (const p of ["/", "/files", "/index.html"]) expect(resolveAppBase(p)).toBe("")
  })

  test("子路径部署：尾斜杠可有可无（/gebai、/gebai/、/gebai/files 同基准）", () => {
    for (const p of ["/gebai", "/gebai/", "/gebai/files", "/gebai/index.html"]) expect(resolveAppBase(p)).toBe("/gebai")
  })

  test("多层子路径", () => {
    expect(resolveAppBase("/a/b/")).toBe("/a/b")
    expect(resolveAppBase("/a/b/files")).toBe("/a/b")
    expect(resolveAppBase("/a/b")).toBe("/a/b")
  })

  test("带 .html 扩展名的页面访问形式同样视为页面，不把文件名当挂载根", () => {
    // 服务端实际会以 /files.html 形式提供工作台（静态兜底直接把构建产物 files.html 作为页面返回），
    // 此时若把末段 /files.html 当成挂载根，全页请求会变成 /files.html/api/v1/… 而全崩
    expect(resolveAppBase("/files.html")).toBe("")
    expect(resolveAppBase("/gebai/files.html")).toBe("/gebai")
    expect(resolveAppBase("/a/b/files.html")).toBe("/a/b")
    // 其它入口页（多入口构建产物）同样按页面处理
    expect(resolveAppBase("/preview.html")).toBe("")
    expect(resolveAppBase("/gebai/preview.html")).toBe("/gebai")
  })

  test("非层级路径名（about:blank 等无站点基准）→ 根部署", () => {
    expect(resolveAppBase("")).toBe("")
    expect(resolveAppBase("blank")).toBe("")
  })

  test(".html 识别不误伤挂载目录（只有**末段**是页面文件时才去页名）", () => {
    // 挂载根目录名里带 html 字样（如 /htmlapps）不能被当成页面文件
    expect(resolveAppBase("/htmlapps")).toBe("/htmlapps")
    expect(resolveAppBase("/htmlapps/files")).toBe("/htmlapps")
    // 页面文件名出现在中间段不影响基准推导（只有末段参与判断）
    expect(resolveAppBase("/gebai/preview.html")).toBe("/gebai")
    // 非 .html 扩展名不当页面文件（避免把静态资源路径当页面）
    expect(resolveAppBase("/gebai/assets/app.js")).toBe("/gebai/assets/app.js")
  })
})

describe("appBase / appPath / appWsUrl：按文档 URL 解析", () => {
  const g = globalThis as unknown as Record<string, unknown>
  const prevDoc = g.document
  const prevLoc = g.location
  afterAll(() => {
    g.document = prevDoc
    g.location = prevLoc
  })

  /** 模拟文档（浏览器环境：baseURI 与 location.href 同源同路径）。 */
  const setDoc = (url: string): void => {
    g.document = { baseURI: url }
    g.location = { href: url, protocol: url.slice(0, url.indexOf(":")) + ":", host: new URL(url).host, pathname: new URL(url).pathname }
  }

  test("根部署：REST 路径原样，WS 同源根路径", () => {
    setDoc("http://localhost/")
    expect(appBase()).toBe("")
    expect(appPath("/api/v1/health")).toBe("/api/v1/health")
    expect(appWsUrl("/ws")).toBe("ws://localhost/ws")
  })

  test("子路径部署：REST 与 WS 均带页面基准", () => {
    setDoc("http://h/gebai/")
    expect(appBase()).toBe("/gebai")
    expect(appPath("/api/v1/health")).toBe("/gebai/api/v1/health")
    expect(appWsUrl("/ws")).toBe("ws://h/gebai/ws")
  })

  test("子路径 + 工作台页（/files 只改 query 不改 pathname）", () => {
    setDoc("http://h/gebai/files?root=proj&path=a.ts")
    expect(appBase()).toBe("/gebai")
    expect(appPath("/api/v1/fs/list")).toBe("/gebai/api/v1/fs/list")
    expect(appWsUrl("/ws")).toBe("ws://h/gebai/ws")
  })

  test("以 /files.html 打开工作台：API 与 WS 不得带页面文件名", () => {
    // 回归：此前 /files.html 会被当成挂载根 → /files.html/api/v1/… 全部 404
    setDoc("http://h/files.html")
    expect(appBase()).toBe("")
    expect(appPath("/api/v1/roots")).toBe("/api/v1/roots")
    setDoc("http://h/gebai/files.html")
    expect(appBase()).toBe("/gebai")
    expect(appPath("/api/v1/roots")).toBe("/gebai/api/v1/roots")
    expect(appWsUrl("/ws")).toBe("ws://h/gebai/ws")
  })

  test("https 下 WS 用 wss", () => {
    setDoc("https://h/gebai/")
    expect(appWsUrl("/ws")).toBe("wss://h/gebai/ws")
  })

  test("非层级文档（srcdoc/about:blank）：回落根部署与相对 WS", () => {
    setDoc("about:blank")
    expect(appBase()).toBe("")
    expect(appPath("/api/v1/health")).toBe("/api/v1/health")
    expect(appWsUrl("/ws")).toBe("/ws")
  })

  test("非 DOM 环境（无 document/location）：路径原样透传", () => {
    g.document = undefined
    g.location = undefined
    expect(appBase()).toBe("")
    expect(appPath("/api/v1/health")).toBe("/api/v1/health")
    expect(appWsUrl("/ws")).toBe("/ws")
  })
})
