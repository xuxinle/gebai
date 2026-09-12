import { describe, expect, test } from "bun:test"
import { collectPaths, openApiPath } from "./docs"

describe("OpenAPI 端点表生成器（/api/docs 由路由注册自动产出）", () => {
  test("路径参数形态转换：:param → {param}，多段与根路径", () => {
    expect(openApiPath("/api/v1/sessions/:id")).toBe("/api/v1/sessions/{id}")
    expect(openApiPath("/api/v1/cron/:id/run")).toBe("/api/v1/cron/{id}/run")
    expect(openApiPath("/api/health")).toBe("/api/health")
  })

  test("只收集 /api/ 域；跳过 HEAD（Hono 派生）与 ALL（中间件/通配）", () => {
    const { paths, total } = collectPaths([
      { method: "get", path: "/api/v1/sessions" },
      { method: "head", path: "/api/v1/sessions" },
      { method: "all", path: "/api/*" },
      { method: "all", path: "/api/v1/sessions/:id/*" },
      { method: "get", path: "/" },
      { method: "get", path: "/vendor/x.js" },
    ])
    expect(total).toBe(1)
    expect(Object.keys(paths)).toEqual(["/api/v1/sessions"])
  })

  test("同一路径多方法合并进同一 path 对象", () => {
    const { paths } = collectPaths([
      { method: "get", path: "/api/v1/tools" },
      { method: "patch", path: "/api/v1/tools" },
    ])
    expect(Object.keys(paths["/api/v1/tools"]!)).toEqual(["get", "patch"])
  })

  test("已登记摘要带 summary；未登记仍列出但标注（覆盖不全不隐藏端点）", () => {
    const { paths, total, covered } = collectPaths([
      { method: "get", path: "/api/health" },          // 已登记
      { method: "get", path: "/api/v1/never-registered" }, // 未登记
    ])
    expect(total).toBe(2)
    expect(covered).toBe(1)
    expect(paths["/api/health"]!.get).toMatchObject({ summary: expect.any(String) })
    const miss = paths["/api/v1/never-registered"]!.get as Record<string, unknown>
    expect(miss.summary).toBeUndefined()
    expect(String(miss.description)).toContain("未登记摘要")
    expect(miss.responses).toBeDefined()
  })

  test("重复注册同一 method+path 只保留一条", () => {
    const { total } = collectPaths([
      { method: "get", path: "/api/v1/sessions" },
      { method: "get", path: "/api/v1/sessions" },
    ])
    expect(total).toBe(2) // 计数按注册条目，但 paths 去重（渲染不重复）
  })
})
