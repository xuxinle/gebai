import type { SubAgentDef } from "../../core/base/types"
import {
  screenshotTool,
  windowListTool,
  windowFocusTool,
  windowMoveTool,
  typeTextTool,
  keyPressTool,
  mouseMoveTool,
  mouseClickTool,
  clipboardReadTool,
  screenInfoTool,
} from "./desktop_tools"

export const name = "desktop"
export const description =
  "涉及宿主机桌面操作时装载本子Agent（仅本地模式，服务端部署不可用）：截图、窗口控制、键盘鼠标输入与剪贴板；窗口优先 PID 定位，输入/点击类操作需审批。输入：操作目标；输出：操作结果与屏幕图片。"
export const systemPrompt =
  "你是桌面控制助手，直接操作宿主机桌面（仅本地模式；服务端部署下所有工具拒绝执行）。工作流程：\n" +
  "1) 明确目标：先 window_list 确认目标窗口（PID/进程名/标题；窗口优先用 PID 定位——标题可能重复），截图前先说明截图用途；\n" +
  "2) 准备：截图/坐标操作前先 screen_info 确认显示器分辨率与主屏原点（主屏左上角为坐标原点），避免 region/坐标错位；\n" +
  "3) 执行：输入/点击/窗口控制类操作（window_focus/window_move/type_text/key_press/mouse_move/mouse_click）需审批，操作前先说明操作意图与目标窗口；执行前必须先用 window_focus 确认目标窗口已激活，避免输入到错误窗口；只读类（screenshot/window_list/screen_info/clipboard_read）免审批可直接执行；\n" +
  "4) 反馈：执行后反馈结果——截图返回图片，mouse_click 坐标基于截图实际尺寸判断，说明截图位置与关键结论；\n" +
  "5) 约束：只执行用户明确要求的操作，不做额外破坏性动作（不改系统设置、不删除文件、不触发危险快捷键）。\n" +
  "验证多通道：不依赖单一验证通道——截图黑屏/纯色（工具会主动提示）时立即切换通道，不要反复重试截图：用 window_list 确认窗口是否在前台、用 clipboard_read 验证剪贴板状态，或经 agent_run 委托 code 子Agent 读取应用数据文件断言结果；任何通道失效即降级并明确告知用户当前采用的验证方式。"
export const tools = {
  screenshot: screenshotTool,
  window_list: windowListTool,
  window_focus: windowFocusTool,
  window_move: windowMoveTool,
  type_text: typeTextTool,
  key_press: keyPressTool,
  mouse_move: mouseMoveTool,
  mouse_click: mouseClickTool,
  clipboard_read: clipboardReadTool,
  screen_info: screenInfoTool,
  // 编排（agent_run 委托 code 验证等）用全局工具 agent_run（主会话恒有，新会话默认继承）——
  // 子Agent 只声明独有工具，不复刻全局编排工具
}
export const requiresApproval = {
  window_focus: true,
  window_move: true,
  type_text: true,
  key_press: true,
  mouse_move: true,
  mouse_click: true,
}
export const preload = false

export const def: SubAgentDef = { name, description, systemPrompt, tools, requiresApproval, preload }
