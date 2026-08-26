/**
 * 飞书机器人桥接的引擎接口层（BotPromptAdapter）：
 * - bot 不直接接触 AgentEngine / EventBus（不侵入引擎层），只依赖本接口
 * - 固定以「多轮交互 + 仅最终回复」运行：关键操作（审批/选择）经回调询问用户，
 *   文本仅最终回复（无流式增量预览），任务内其他过程事件（工具调用/推理）不暴露
 */
import type { AgentEvent, AttachmentInput } from "@gebai/sdk"
import type { AgentEngine } from "../core/engine"

/** 任务运行期语义回调（引擎事件流映射；bot 只处理回调，不解析事件）。 */
export interface BotRunHandlers {
  /** 关键操作审批请求（requiresApproval 工具，多轮交互询问用户；arguments 为本次调用参数——
   *  飞书通道无消息流工具卡片，审批卡片需自行展示参数供用户判断）。 */
  onApproval?(toolCallId: string, tool: string, args: Record<string, unknown>, retries: number): void
  /** 用户选择请求（ask 选项询问分支）。 */
  onChoice?(choiceId: string, prompt: string, options: unknown[], multi: boolean): void
  /** 图表渲染请求（draw，飞书通道后端渲染成图片；format 图表语言，后端仅支持 plantuml）。 */
  onDraw?(renderId: string, code: string, name?: string, format?: string): void
  /** 最终回复文本（仅最终回复模式下唯一的文本信号，非子Agent 过程文本）。 */
  onDone?(text: string): void
  /** 任务出错（LLM 失败等）。 */
  onError?(error: string): void
  /** 任务结束（完成/出错，收尾清理用）。 */
  onEnd?(): void
}

/** 飞书桥接依赖的最小引擎接口。 */
export interface BotPromptAdapter {
  isRunning(sessionId: string): boolean
  cancel(sessionId: string): void
  run(sessionId: string, user: string, prompt: string, opts: { attachments?: AttachmentInput[]; messageId?: string }, handlers: BotRunHandlers): Promise<void>
  decideApproval(sessionId: string, toolCallId: string, approve: boolean): Promise<void>
  decideChoice(sessionId: string, choiceId: string, selection: string | string[] | null): Promise<void>
  decideDrawResult(sessionId: string, renderId: string, result: { ok: boolean; error?: string }): Promise<void>
}

/** 飞书通道环境注记（注入系统提示词，模型据此感知对话宿主与能力边界）：
 *  渲染形态（Markdown 卡片，2.0 组件支持标题/表格/代码块）、交互形态（审批/选择经卡片按钮、命令兜底）、
 *  通道能力边界（依赖前端页面的工具不可用）、富媒体边界（图片可、其余不支持）。 */
export const FEISHU_CHANNEL_NOTE =
  "当前对话经飞书机器人通道进行：用户在飞书聊天中与你对话。" +
  "回复以 Markdown 卡片渲染（支持标题/表格/代码块/列表/链接），超长内容会被截断——大段产物建议落盘文件并提示用户到 Web UI 对应会话查看；" +
  "审批与选择询问经飞书卡片按钮作答，用户也可能回复 /approve、/reject 等文本命令；" +
  "依赖前端页面的工具（page_capture、widgets 四工具、show 的 html 分支）在本通道不可用，调用会被拦截，请改用其他方案；" +
  "图片消息会自动转为附件供你分析，其余富媒体消息类型暂不支持。"

/** 引擎适配器：包 AgentEngine，按「多轮交互 + 仅最终回复」运行，事件流映射为语义回调。 */
export class EngineBotAdapter implements BotPromptAdapter {
  constructor(
    private engine: AgentEngine,
    private events: { subscribe(fn: (e: AgentEvent) => void): () => void },
  ) {}

  isRunning(sessionId: string): boolean {
    return this.engine.isRunning(sessionId)
  }

  cancel(sessionId: string): void {
    this.engine.cancel(sessionId)
  }

  async run(sessionId: string, user: string, prompt: string, opts: { attachments?: AttachmentInput[]; messageId?: string }, handlers: BotRunHandlers): Promise<void> {
    // 先订阅再运行：避免错过先于注册到达的事件（审批/选择/最终回复等）
    const unsub = this.events.subscribe((ev) => {
      if (ev.sessionId !== sessionId) return
      const p = ev.payload as Record<string, unknown>
      switch (ev.type) {
        case "event.approval.request":
          handlers.onApproval?.(String(p.toolCallId ?? ""), String(p.tool ?? ""), (p.arguments ?? {}) as Record<string, unknown>, Number(p.retries ?? 0))
          break
        case "event.choice.request":
          handlers.onChoice?.(String(p.choiceId ?? ""), String(p.prompt ?? ""), Array.isArray(p.options) ? p.options : [], p.multi === true)
          break
        case "event.draw.render":
          handlers.onDraw?.(String(p.renderId ?? ""), String(p.code ?? ""), p.name != null ? String(p.name) : undefined, p.format != null ? String(p.format) : undefined)
          break
        case "event.message.done":
          // 仅最终回复：非子Agent 的 done 即最终文本（final_only 无 delta，done 是唯一文本信号）
          if (p.session !== true) handlers.onDone?.(String(p.text ?? ""))
          break
        case "event.task.done":
          handlers.onEnd?.()
          break
        case "event.task.error":
          handlers.onError?.(String(p.error ?? "unknown error"))
          handlers.onEnd?.()
          break
      }
    })
    try {
      await this.engine.run(sessionId, user, prompt, {
        attachments: opts.attachments,
        messageId: opts.messageId,
        // 通道环境注记：模型感知飞书宿主（渲染/交互/能力边界），注入系统提示词
        channelNote: FEISHU_CHANNEL_NOTE,
        // 多轮交互：关键操作（requiresApproval）询问用户，实时前端工具自动禁用
        interactionMode: "multi_turn",
        // 仅最终回复：不推送文本增量/推理流（对接接口层，非流式桥接）
        outputMode: "final_only",
      })
    } finally {
      unsub()
    }
  }

  decideApproval(sessionId: string, toolCallId: string, approve: boolean): Promise<void> {
    return this.engine.decideApproval(sessionId, toolCallId, approve)
  }

  decideChoice(sessionId: string, choiceId: string, selection: string | string[] | null): Promise<void> {
    return this.engine.decideChoice(sessionId, choiceId, selection)
  }

  decideDrawResult(sessionId: string, renderId: string, result: { ok: boolean; error?: string }): Promise<void> {
    return this.engine.decideDrawResult(sessionId, renderId, result)
  }
}
