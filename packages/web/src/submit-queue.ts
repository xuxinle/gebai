/**
 * 会话提交串行化（单飞 + 尾随合并）。
 *
 * 为什么需要：草稿页的首条消息要**先 await 建会话**，而「是否草稿页」的判定读的是当前会话状态——
 * 建会话结果回来之前当前会话仍是空。这段时间里到达的第二次提交会重新走同一条建会话分支，
 * 于是同一条提示词落进两个新会话（用户看到的是「只发了一次，却多出一个会话」）。
 * 重复来源都是正常输入形态，不必是误操作：按住回车的键盘自动重复、触屏软键盘发送键双发、
 * 连点发送按钮。
 *
 * 语义：
 * - **单飞**：进行中的那一轮独占，其余提交不并发进入；
 * - **尾随合并**：期间到达的提交只记一个标记，当前轮结束后再跑一轮——不丢输入（用户在这段
 *   时间里接着敲的第二段话照常发出）；若内容已被上一轮消费，调用方自身的空内容判定会让这一轮
 *   立即返回，代价至多是一次空跑，而不是一条消息迟到或丢失；
 * - **异常不粘住**：`run` 抛错时先把状态复位，再决定是否继续尾随轮，最后把错误抛给调用方。
 */
export function createSubmitQueue(run: () => Promise<void>): () => Promise<void> {
  let running = false
  let trailing = false
  let failure: unknown = null
  return async (): Promise<void> => {
    if (running) {
      trailing = true
      return
    }
    running = true
    try {
      do {
        trailing = false
        try {
          await run()
        } catch (err) {
          // 单轮的异常不终止尾随轮（用户的第二次提交仍要发出去），但要在全部轮次结束后如实抛给调用方
          failure ??= err
        }
      } while (trailing)
    } finally {
      running = false
      trailing = false
    }
    if (failure !== null) {
      const err = failure
      failure = null
      throw err
    }
  }
}
