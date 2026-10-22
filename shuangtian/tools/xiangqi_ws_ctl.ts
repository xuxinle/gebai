/**
 * 相期的端到端验收：起一个最小 WebSocket 控制器 → 投卡片 → 截图 → 收答复。
 *
 * 为什么用真 WebSocket 服务端而不是 mock：相期的 WS 客户端是自研协议实现
 * （握手校验 Sec-WebSocket-Accept、客户端帧必须掩码），mock 掉就测不到这些。
 * Bun 自带 WebSocket 服务端，正好当这个"控制器"。
 *
 * 验收点（对应四条真实故障形态）：
 * 1. 握手通过（Accept 算错 → 客户端直接判"握手被拒"）
 * 2. 客户端帧带掩码（服务端按 RFC 拒未掩码帧）
 * 3. 三张卡都渲染出来（提示/选择/待办）
 * 4. 点选项后答复能回到服务端（含卡片被移除）
 */
const port = 7717
const received: unknown[] = []
const shots: string[] = []
let clientRef: { send(data: string): void } | null = null

const server = Bun.serve({
  port,
  fetch(req, srv) {
    if (srv.upgrade(req)) return undefined
    return new Response("相期 test controller", { status: 200 })
  },
  websocket: {
    open(ws) {
      clientRef = ws as unknown as { send(data: string): void }
      console.log("[ctl] 相期已连接")
      // 投三张卡：三种类型各一
      ws.send(
        JSON.stringify({
          type: "cards",
          cards: [
            {
              id: "n1",
              kind: "notice",
              title: "转码队列已清空",
              body: "夜间批处理跑完了，共 128 个文件，失败 0。",
              from: "小霜",
            },
            {
              id: "c1",
              kind: "choice",
              title: "这份报告发到哪个群？",
              body: "内容已生成，等你选一个去处。",
              from: "小霜",
              options: [
                { label: "支持群", value: "oc_support", hint: "外部可见" },
                { label: "内部群", value: "oc_internal", hint: "仅组内" },
                { label: "先存草稿", value: "draft" },
              ],
            },
            {
              id: "t1",
              kind: "todo",
              title: "把 TSDB 那条慢查询加到监控",
              body: "阈值建议 3s，超过就告警。",
              from: "小霜",
              due: "今天",
            },
          ],
        }),
      )
    },
    message(_ws, data) {
      const text = typeof data === "string" ? data : new TextDecoder().decode(data)
      try {
        const msg = JSON.parse(text)
        received.push(msg)
        console.log("[ctl] 收到答复:", JSON.stringify(msg))
      } catch {
        console.log("[ctl] 非 JSON:", text.slice(0, 120))
      }
    },
    close() {
      console.log("[ctl] 相期断开")
      clientRef = null
    },
  },
})

console.log(`[ctl] 控制器监听 ws://127.0.0.1:${port}/xiangqi`)

// 给外部调用方留一个"收尾报告"的出口：写到文件，由编排脚本读
const reportPath = process.argv[2] ?? "xiangqi-e2e-report.json"
const deadline = Date.now() + 60_000
while (Date.now() < deadline) {
  await Bun.sleep(500)
  if (received.length >= 1) break
}
await Bun.write(
  reportPath,
  JSON.stringify({ connected: clientRef !== null, answers: received, shots }, null, 2),
)
console.log(`[ctl] 报告写入 ${reportPath}：收到 ${received.length} 条答复`)
server.stop(true)
process.exit(0)
