/**
 * 深层链接解析测试：`/files?root=&project=&path=&line=&session=` → 「哪个根 + 根内路径 + 行号」。
 *
 * 这是消息流产物链接（「在工作台打开」）能否落到正确文件的关键——主界面只透传原始路径，
 * 由本模块定位所属根。覆盖：显式根 / 绝对路径最长前缀匹配（含嵌套根、Windows 大小写）/
 * 会话 tmp 命中会话根 / 相对路径 / 无参数默认 / 无匹配回退 / 行号与 tmp 前缀。
 */
import { describe, expect, test } from "bun:test"
import { isAbsPath, resolveDeepLink, type DeepLinkRoot } from "./deeplink"

const SESS = "6e39342200d246c2ab3ca14802964c13"
const roots: DeepLinkRoot[] = [
  { id: `sess:${SESS}`, kind: "sess", path: `/gebai/users/admin/sessions/6e/39/${SESS}/tmp` },
  { id: "proj:gebai", kind: "proj", path: "/workspaces/gebai" },
  { id: "proj:gebai-web", kind: "proj", path: "/workspaces/gebai/packages/web" },
  { id: "user:", kind: "user", path: "/gebai/users/admin" },
  { id: "abs:/data", kind: "abs", path: "/data" },
]

const resolve = (search: string, isWin = false) => resolveDeepLink(roots, search, { isWin })

describe("deeplink 绝对路径判定", () => {
  test("POSIX / Windows 盘符 / UNC / 相对路径", () => {
    expect(isAbsPath("/a/b")).toBe(true)
    expect(isAbsPath("C:\\a\\b")).toBe(true)
    expect(isAbsPath("c:/a/b")).toBe(true)
    expect(isAbsPath("\\\\host\\share")).toBe(true)
    expect(isAbsPath("tmp/a.txt")).toBe(false)
    expect(isAbsPath("./a.txt")).toBe(false)
  })
})

describe("deeplink 显式根优先", () => {
  test("?root= 指定根：路径按该根内相对解释", () => {
    expect(resolve("?root=proj:gebai&path=src/main.ts")).toEqual({ rootId: "proj:gebai", dir: "src", file: "src/main.ts", line: undefined })
  })

  test("?project= 等价于 root=proj:<name>", () => {
    expect(resolve("?project=gebai&path=packages/web/src/main.ts")).toEqual({
      rootId: "proj:gebai",
      dir: "packages/web/src",
      file: "packages/web/src/main.ts",
      line: undefined,
    })
  })

  test("显式根不存在时：abs: 绝对路径直接接受（清单外目录可链接直达）", () => {
    expect(resolve("?root=abs:/tmp/mrg&path=f.txt")).toEqual({ rootId: "abs:/tmp/mrg", dir: "", file: "f.txt", line: undefined })
    // 非 abs: 的未知根仍忽略（继续按后续档定位）
    expect(resolve("?root=proj:nope&path=/workspaces/gebai/src/app.ts")?.rootId).toBe("proj:gebai")
  })

  test("显式根不存在时忽略它（不报错），继续按绝对路径/会话根定位", () => {
    expect(resolve("?root=proj:nope&path=/workspaces/gebai/src/app.ts")?.rootId).toBe("proj:gebai")
  })

  test("显式根 + 绝对路径：按根内相对解释（不再做前缀匹配）", () => {
    // 显式指定根时路径是「根内相对」语义，绝对路径按原样交给该根（服务端再判越界）
    expect(resolve("?root=proj:gebai&path=/workspaces/gebai/a.ts")).toEqual({
      rootId: "proj:gebai",
      dir: "/workspaces/gebai",
      file: "/workspaces/gebai/a.ts",
      line: undefined,
    })
  })
})

describe("deeplink 绝对路径 → 最长前缀匹配", () => {
  test("项目内文件命中项目根", () => {
    expect(resolve("?path=/workspaces/gebai/packages/server/src/app.ts")).toEqual({
      rootId: "proj:gebai",
      dir: "packages/server/src",
      file: "packages/server/src/app.ts",
      line: undefined,
    })
  })

  test("嵌套根覆盖外层根（命中更精确的那一个）", () => {
    // /workspaces/gebai/packages/web 同时是 proj:gebai 与 proj:gebai-web 的前缀 → 取更长的
    expect(resolve("?path=/workspaces/gebai/packages/web/src/main.ts")?.rootId).toBe("proj:gebai-web")
    expect(resolve("?path=/workspaces/gebai/packages/web/src/main.ts")?.file).toBe("src/main.ts")
  })

  test("会话 tmp 内文件命中会话根（相对路径不含 tmp/ 前缀）", () => {
    const r = resolve(`?path=/gebai/users/admin/sessions/6e/39/${SESS}/tmp/out/report.md`)
    expect(r).toEqual({ rootId: `sess:${SESS}`, dir: "out", file: "out/report.md", line: undefined })
  })

  test("根自身（等于根路径）也能命中", () => {
    expect(resolve("?path=/data")?.rootId).toBe("abs:/data")
  })

  test("会话根只接相对路径：绝对路径不落会话根，而以所在目录为 abs 根直达", () => {
    // 绝对路径交给会话根必然越界（无意义），改用其所在目录作根——项目外产物也能开
    expect(resolve(`?session=${SESS}&path=/etc/hosts`)).toEqual({ rootId: "abs:/etc", dir: "", file: "hosts", line: undefined })
  })

  test("无匹配且无会话时：以所在目录为 abs 根直接定位（项目外产物/临时目录也能开）", () => {
    expect(resolve("?path=/tmp/mrg/f.txt")).toEqual({ rootId: "abs:/tmp/mrg", dir: "", file: "f.txt", line: undefined })
    // 根目录下的文件：无目录可作根 → 交给默认根（不自造 `abs:` 空路径）
    expect(resolve("?path=/f.txt")?.rootId).not.toBe("abs:")
  })

  test("Windows 客户端：盘符大小写不敏感匹配", () => {
    const win: DeepLinkRoot[] = [{ id: "abs:C:/Work", kind: "abs", path: "C:\\Work" }]
    const r = resolveDeepLink(win, "?path=c:\\work\\src\\a.ts", { isWin: true })
    expect(r?.rootId).toBe("abs:C:/Work")
    expect(r?.file).toBe("src/a.ts")
    // POSIX 语义下不该命中（大小写敏感）
    expect(resolveDeepLink(win, "?path=c:\\work\\src\\a.ts", { isWin: false })?.file).toBe("c:\\work\\src\\a.ts")
  })

  test("前缀相似但非子路径不算命中（/data2 不属于 /data）", () => {
    expect(resolve("?session=" + SESS + "&path=/data2/x.txt")?.rootId).toBe("abs:/data2")
  })

  test("同长前缀时项目类根优先（本地模式的 abs: 白名单根可指向同一目录）", () => {
    // 本地模式下 abs:<cwd> 与 proj:gebai 常指向同一目录；前缀长度相同，应按类型让项目根胜出
    const dup: DeepLinkRoot[] = [
      { id: "abs:/workspaces/gebai", kind: "abs", path: "/workspaces/gebai" },
      { id: "proj:gebai", kind: "proj", path: "/workspaces/gebai" },
    ]
    expect(resolveDeepLink(dup, "?path=/workspaces/gebai/src/app.ts")?.rootId).toBe("proj:gebai")
    // 反过来排也一样（与清单顺序无关）
    expect(resolveDeepLink([...dup].reverse(), "?path=/workspaces/gebai/src/app.ts")?.rootId).toBe("proj:gebai")
  })
})

describe("deeplink 相对路径与会话根", () => {
  test("?session= + 相对路径 → 会话根", () => {
    expect(resolve(`?session=${SESS}&path=out/a.txt`)).toEqual({ rootId: `sess:${SESS}`, dir: "out", file: "out/a.txt", line: undefined })
  })

  test("tmp/ 前缀被剥离（产物逻辑路径带 tmp/）", () => {
    expect(resolve(`?session=${SESS}&path=tmp/out/a.txt`)?.file).toBe("out/a.txt")
    expect(resolve(`?session=${SESS}&path=./a.txt`)?.file).toBe("a.txt")
  })

  test("仅 ?session= → 会话根、无文件（打开该工作区）", () => {
    expect(resolve(`?session=${SESS}`)).toEqual({ rootId: `sess:${SESS}`, dir: "", file: "", line: undefined })
  })

  test("?session= 但该会话根不在清单 → 回退项目根", () => {
    expect(resolve("?session=ffffffffffffffffffffffffffffffff&path=a.txt")?.rootId).toBe("proj:gebai")
  })
})

describe("deeplink 默认与边界", () => {
  test("无参数 → 项目根优先（手工打开 /files 看代码）", () => {
    expect(resolve("")).toEqual({ rootId: "proj:gebai", dir: "", file: "", line: undefined })
  })

  test("无项目根时回退绑定根，再回退第一个根", () => {
    const bindOnly: DeepLinkRoot[] = [
      { id: "sess:x", kind: "sess", path: "/s/tmp" },
      { id: "bind:code", kind: "bind", path: "/proj" },
    ]
    expect(resolveDeepLink(bindOnly, "")?.rootId).toBe("bind:code")
    const only: DeepLinkRoot[] = [{ id: "user:", kind: "user", path: "/gebai/users/admin" }]
    expect(resolveDeepLink(only, "")).toEqual({ rootId: "user:", dir: "", file: "", line: undefined })
  })

  test("根清单为空：绝对路径仍可自造 abs 根（无根可依时相对路径返回 null）", () => {
    expect(resolveDeepLink([], "?path=/a/b")).toEqual({ rootId: "abs:/a", dir: "", file: "b", line: undefined })
    expect(resolveDeepLink([], "?path=a/b")).toBeNull()
    expect(resolveDeepLink([], "")).toBeNull()
  })

  test("line 参数解析（1 起始；非法/0/负数忽略）", () => {
    expect(resolve("?root=proj:gebai&path=a.ts&line=42")?.line).toBe(42)
    expect(resolve("?root=proj:gebai&path=a.ts&line=0")?.line).toBeUndefined()
    expect(resolve("?root=proj:gebai&path=a.ts&line=abc")?.line).toBeUndefined()
  })

  test("search 带不带 ? 均可（服务端拼接/直接传串）", () => {
    expect(resolve("root=proj:gebai&path=a.ts")?.rootId).toBe("proj:gebai")
  })

  test("路径含中文与空格（URL 编码）不丢字符", () => {
    const r = resolve("?root=proj:gebai&path=" + encodeURIComponent("文档/设计 说明.md"))
    expect(r?.file).toBe("文档/设计 说明.md")
    expect(r?.dir).toBe("文档")
  })
})
