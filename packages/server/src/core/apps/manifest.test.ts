import { describe, expect, test } from "bun:test"
import { APP_DEFAULT_ENTRY, APP_ID_RE, appPathFromUrl, looksLikePagePath, normalizeAppRelPath, parseAppManifest } from "./manifest"

describe("托管应用：清单解析与路径规则", () => {
  test("默认清单：只有目录名也是可用的应用（丢一个 index.html 就成型）", () => {
    const { manifest, warnings } = parseAppManifest(undefined, "hello")
    expect(manifest).toEqual({
      id: "hello",
      title: "hello",
      description: "",
      version: "",
      entry: APP_DEFAULT_ENTRY,
      spa: true,
      backend: "",
      listed: true,
      env: {},
    })
    expect(warnings).toEqual([])
  })

  test("id 白名单挡住路径穿越与大小写歧义形态", () => {
    // 合法：小写字母/数字开头，其后可含 - 与 _
    for (const ok of ["a", "hello", "hello-world", "app_1", "x".repeat(64), "9lives"]) expect(APP_ID_RE.test(ok)).toBe(true)
    // 非法：空、大写、下划线开头、点、斜杠、反斜杠、盘符、超长、空格
    for (const bad of ["", "Hello", "_x", "-x", "a.b", "a/b", "a\\b", "C:", "x".repeat(65), "a b", "..", "."]) {
      expect(APP_ID_RE.test(bad)).toBe(false)
    }
  })

  test("相对路径归一：折叠空段与 .，拒绝绝对/穿越/盘符/NUL", () => {
    expect(normalizeAppRelPath("")).toBeNull()
    expect(normalizeAppRelPath("index.html")).toBe("index.html")
    expect(normalizeAppRelPath("/index.html")).toBeNull()
    expect(normalizeAppRelPath("./js/app.js")).toBe("js/app.js")
    expect(normalizeAppRelPath("js//app.js")).toBe("js/app.js")
    expect(normalizeAppRelPath("js\\app.js")).toBe("js/app.js")
    expect(normalizeAppRelPath("../secret")).toBeNull()
    expect(normalizeAppRelPath("js/../../secret")).toBeNull()
    expect(normalizeAppRelPath("C:/windows")).toBeNull()
    expect(normalizeAppRelPath("a\0b")).toBeNull()
  })

  test("URL 段解码：坏编码与穿越形态一律 null（不把异常抛到路由层）", () => {
    expect(appPathFromUrl("/js/app.js")).toBe("js/app.js")
    expect(appPathFromUrl("/%E4%B8%AD%E6%96%87.html")).toBe("中文.html")
    expect(appPathFromUrl("/%E4%B8%AD")).toBe("中")
    expect(appPathFromUrl("/%E4%B8")).toBeNull() // 坏编码
    expect(appPathFromUrl("/../etc/passwd")).toBeNull()
    expect(appPathFromUrl("/%2e%2e/etc")).toBeNull() // 编码后的穿越
  })

  test("清单字段：非法 entry/backend 回落并记警告，env 逐键校验", () => {
    const { manifest, warnings } = parseAppManifest(
      {
        title: "示例",
        description: "说明",
        version: "1.2.3",
        entry: "../escape.html",
        backend: "server.exe",
        spa: false,
        listed: false,
        env: { OK: "v", NUM: 3, "BAD-KEY": "x", OBJ: { a: 1 } },
      },
      "demo",
    )
    expect(manifest.title).toBe("示例")
    expect(manifest.entry).toBe(APP_DEFAULT_ENTRY) // 落回默认
    expect(manifest.backend).toBe("") // 扩展名白名单外 → 无后端
    expect(manifest.spa).toBe(false)
    expect(manifest.listed).toBe(false)
    expect(manifest.env).toEqual({ OK: "v", NUM: "3" })
    expect(warnings.join("\n")).toContain("entry")
    expect(warnings.join("\n")).toContain("backend")
    expect(warnings.join("\n")).toContain("BAD-KEY")
  })

  test("清单下发的 id 与目录名不一致时以目录名为准，并记警告", () => {
    const { manifest, warnings } = parseAppManifest({ id: "other-name", title: "x" }, "real-dir")
    expect(manifest.id).toBe("real-dir")
    expect(warnings.join()).toContain("不一致")
  })

  test("app.json 写成数组/字符串时不抛错：按默认清单 + 警告", () => {
    for (const bad of [[], "字符串", 42]) {
      const { manifest, warnings } = parseAppManifest(bad, "x")
      expect(manifest.id).toBe("x")
      expect(warnings.length).toBe(1)
    }
  })

  test("SPA 回退判据：只有无扩展名的路径算页面", () => {
    expect(looksLikePagePath("")).toBe(true)
    expect(looksLikePagePath("settings/profile")).toBe(true)
    expect(looksLikePagePath("app.js")).toBe(false)
    expect(looksLikePagePath("css/main.css")).toBe(false)
    expect(looksLikePagePath("data.json")).toBe(false)
  })
})
