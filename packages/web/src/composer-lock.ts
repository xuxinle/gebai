/**
 * 输入锁判据（会话有待处理审批时禁输入/发送/附件）。
 *
 * 判据是**布尔**而非计数：卡片集合里只要还有属于当前会话的审批卡就锁，没有就解锁。
 * 为什么不用计数：服务端会把同一条审批请求重复推送（断线重连的事件重放、attach 快照与实时事件
 * 各来一次），而重复推送的卡片会被替换掉、只留一张——计数在这种替换里必然虚高（加一减零），
 * 用户处理掉最后那张卡之后计数仍大于零，输入框就一直 `disabled`（回车与 Ctrl+Enter 都毫无反应，
 * 刷新页面才恢复）。以「现在还有没有这样一张卡」为准就没有可失配的中间量。
 *
 * 传入卡片集合而不是 DOM 节点，便于把判据单独固定下来（调用方用 DOM 一次性算出会话 id 列表）。
 */
export function composerLocked(cardSessions: Iterable<string | undefined>, sessionId: string | undefined): boolean {
  if (!sessionId) return false
  for (const sid of cardSessions) if (sid === sessionId) return true
  return false
}
