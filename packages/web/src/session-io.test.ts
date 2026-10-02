/**
 * 会话导入/导出（session-io）：格式「批量包」的打包与解析、文件名与载荷剥离。
 *
 * 覆盖四件事：
 * ① detailToImportData 剥离 id/userId/ctx 统计（跨实例导入不冲突的根基）、可选字段只在非空时携带；
 * ② buildExportFile/parseImportText 单/批量同构往返（一个会话的包能原样解析回一条载荷）；
 * ③ 解析对坏文件给可读错误（非 JSON / 格式不符 / 版本不符 / 空会话数组 / 条目缺名）；
 * ④ 文件名消毒与单/批量命名规则（Windows 非法字符、首尾点空格、超长截断、空名回退）。
 */
import { describe, expect, test } from "bun:test"
import {
  SESSION_EXPORT_FORMAT,
  SESSION_EXPORT_MAX_SESSIONS,
  batchExportFileName,
  buildExportFile,
  detailToImportData,
  parseImportText,
  safeFileStem,
  singleExportFileName,
} from "./session-io"
import type { SessionDetail } from "@gebai/sdk"

function detail(name: string, extra?: Partial<SessionDetail>): SessionDetail {
  return {
    id: "cafe1234cafe1234cafe1234cafe1234",
    userId: "admin",
    name,
    createdAt: 1000,
    updatedAt: 2000,
    messages: [{ id: "m1", role: "user", content: "hi", createdAt: 1 }],
    ...extra,
  }
}

describe("会话导出载荷", () => {
  test("剥离 id/userId/ctx 统计，保留名称/时间/置顶/消息", () => {
    const d = detail("测试会话", { pinned: true, ctxTokens: 12345 })
    const p = detailToImportData(d)
    expect(p).not.toHaveProperty("id")
    expect(p).not.toHaveProperty("userId")
    expect(p).not.toHaveProperty("ctxTokens")
    expect(p.name).toBe("测试会话")
    expect(p.pinned).toBe(true)
    expect(p.createdAt).toBe(1000)
    expect(p.updatedAt).toBe(2000)
    expect(p.messages).toHaveLength(1)
  })

  test("todos/loadedSubAgents 仅非空时携带（默认导出不带空数组）", () => {
    expect(detailToImportData(detail("a"))).not.toHaveProperty("todos")
    expect(detailToImportData(detail("a"))).not.toHaveProperty("loadedSubAgents")
    const p = detailToImportData(detail("a", { todos: [{ id: "t1", title: "x", status: "pending" }], loadedSubAgents: ["code"] }))
    expect(p.todos).toHaveLength(1)
    expect(p.loadedSubAgents).toEqual(["code"])
  })

  test("无名会话回退「未命名会话」", () => {
    expect(detailToImportData(detail("")).name).toBe("未命名会话")
  })
})

describe("批量包打包/解析", () => {
  test("单会话与批量同构：打包后可原样解析回等量载荷", () => {
    const one = buildExportFile([detailToImportData(detail("单"))])
    expect(one.format).toBe(SESSION_EXPORT_FORMAT)
    expect(one.version).toBe(1)
    expect(parseImportText(JSON.stringify(one))).toHaveLength(1)

    const many = buildExportFile([detailToImportData(detail("a")), detailToImportData(detail("b")), detailToImportData(detail("c"))])
    expect(parseImportText(JSON.stringify(many)).map((s) => s.name)).toEqual(["a", "b", "c"])
  })

  test("空会话数组拒绝打包（导出文件不会是空的）", () => {
    expect(() => buildExportFile([])).toThrow("没有可导出的会话")
  })

  test("超过单包上限拒绝（防御护栏）", () => {
    const items = Array.from({ length: SESSION_EXPORT_MAX_SESSIONS + 1 }, (_, i) => detailToImportData(detail(`s${i}`)))
    expect(() => buildExportFile(items)).toThrow("最多")
  })

  test("非 JSON 文本报可读错误", () => {
    expect(() => parseImportText("not-json{")).toThrow("JSON")
  })

  test("格式标识不符拒绝（不是歌白会话导出文件）", () => {
    expect(() => parseImportText("{}")).toThrow("格式不符")
    expect(() => parseImportText(JSON.stringify({ format: "other", version: 1, sessions: [{ name: "x" }] }))).toThrow("格式不符")
  })

  test("版本不符给出当前支持的版本号", () => {
    const bad = JSON.stringify({ format: SESSION_EXPORT_FORMAT, version: 99, sessions: [{ name: "x" }] })
    expect(() => parseImportText(bad)).toThrow("v1")
  })

  test("空会话数组/缺名条目拒绝", () => {
    expect(() => parseImportText(JSON.stringify({ format: SESSION_EXPORT_FORMAT, version: 1, sessions: [] }))).toThrow("没有会话")
    expect(() => parseImportText(JSON.stringify({ format: SESSION_EXPORT_FORMAT, version: 1, sessions: [{ name: "  " }] }))).toThrow("缺少名称")
  })
})

describe("导出文件名", () => {
  test("单会话命名：gebai-session-{名称}.json", () => {
    expect(singleExportFileName("我的会话")).toBe("gebai-session-我的会话.json")
  })

  test("批量命名含条数与日期", () => {
    const name = batchExportFileName(7)
    expect(name).toMatch(/^gebai-sessions-7会话-\d{8}\.json$/)
  })

  test("消毒：Windows 非法字符替换、首尾点空格去除、超长截断、空名回退", () => {
    expect(safeFileStem('a<b>:c/"d|e?f*g', "fb")).toBe("a_b__c__d_e_f_g")
    expect(safeFileStem("  .名字. ", "fb")).toBe("名字")
    expect(safeFileStem("x".repeat(300), "fb")).toHaveLength(80)
    // 全非法字符替换后非空（___），不算空名；仅空串才回退——与 exportSession 既有消毒同规则
    expect(safeFileStem("///", "fb")).toBe("___")
    expect(safeFileStem("", "fb")).toBe("fb")
  })
})
