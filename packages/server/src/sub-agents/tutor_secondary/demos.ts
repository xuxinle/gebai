/* tutor_secondary / tutor_primary 共享教学演示模板库——服务端纯函数渲染 HTML：
 * 模型只传 template + params（几十 token），不再现场手写整页 HTML（数千 token、流式慢），
 * 模板代码内置进二进制（版本化、可测试、零 LLM 开销）；params 参数化 = 动态调整与个性化
 * （难度/题量/数值/学生名/标题）。产物经全局 show 的 html 分支展示（实时通道门控/落盘/内容块复用）。
 * 新增模板：在 DEMO_TEMPLATES 加实现 + 工具描述参数表同步（描述是模型选用的第一信息来源）。 */

import { REF_ENTRIES, refIndexBySubject } from "./reference-data"

function esc(s: string): string {
  return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;")
}

/** JSON 内嵌 <script> 安全转义（防 </script> 截断）。 */
function jsonForScript(v: unknown): string {
  return JSON.stringify(v).replace(/</g, "\\u003c")
}

function demoPage(title: string, body: string, script = ""): string {
  return (
    '<!DOCTYPE html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">' +
    `<title>${esc(title)}</title><style>` +
    ":root{--accent:#4f7cff;--ok:#2ea44f;--bad:#e5534b;--ink:#1f2328;--muted:#6b7280;--bg:#f6f8fa;--card:#fff}" +
    "*{box-sizing:border-box}body{margin:0;font-family:system-ui,-apple-system,'Segoe UI','Microsoft YaHei',sans-serif;color:var(--ink);background:var(--bg);padding:16px}" +
    ".wrap{max-width:720px;margin:0 auto}h1{font-size:20px;margin:0 0 4px}.sub{color:var(--muted);font-size:13px;margin-bottom:14px}" +
    ".card{background:var(--card);border-radius:14px;box-shadow:0 1px 4px rgba(0,0,0,.08);padding:16px;margin-bottom:12px}" +
    "button{background:var(--accent);color:#fff;border:none;border-radius:10px;padding:10px 22px;font-size:15px;cursor:pointer}" +
    "button.ghost{background:#eef1f6;color:var(--ink)}.ok{color:var(--ok);font-weight:600}.bad{color:var(--bad);font-weight:600}" +
    "input[type=text]{border:2px solid #d5dbe3;border-radius:8px;padding:8px 10px;font-size:16px;width:130px}" +
    "input[type=text]:focus{outline:none;border-color:var(--accent)}label.opt{display:block;padding:6px 10px;border-radius:8px;cursor:pointer}" +
    "label.opt:hover{background:#f0f4ff}label.opt input{margin-right:8px}.explain{background:#f0f7ff;border-radius:8px;padding:8px 12px;margin-top:6px;font-size:14px}" +
    "input[type=range]{width:190px;accent-color:var(--accent);vertical-align:middle}" +
    ".crow{display:flex;justify-content:center}.cell{display:inline-block;width:36px;text-align:center;font-family:Consolas,'Cascadia Mono',monospace;font-size:26px;line-height:1.5}" +
    ".cmark .cell{font-size:13px;color:var(--bad);height:20px;line-height:20px}" +
    ".hline{border-top:3px solid var(--ink);width:100%;margin:4px 0}.hline.thin{border-top-width:2px;border-color:#999}" +
    ".cell.on{background:#fff3d6;border-radius:6px;animation:pulse 1.1s infinite}@keyframes pulse{50%{background:#ffe0a8}}" +
    ".rd{color:#c8ccd4}.stepbox{background:#f0f7ff;border-radius:10px;padding:10px 14px;min-height:46px;font-size:15px;line-height:1.6}" +
    ".tri{transition:transform 1.2s cubic-bezier(.4,0,.2,1)}.fadeA,.fadeB{transition:opacity 1.2s}" +
    ".dim{opacity:.12;transition:opacity .4s}svg .el{transition:opacity .4s}" +
    ".flip{perspective:700px;cursor:pointer}.flip-in{position:relative;min-height:110px;transition:transform .5s;transform-style:preserve-3d}" +
    ".flip.flipped .flip-in{transform:rotateY(180deg)}" +
    ".face{position:absolute;inset:0;backface-visibility:hidden;-webkit-backface-visibility:hidden;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:8px;background:#fff;border-radius:12px;box-shadow:0 1px 4px rgba(0,0,0,.08);padding:12px;font-size:17px;text-align:center}" +
    ".face.back{transform:rotateY(180deg)}" +
    "</style></head><body><div class=\"wrap\">" +
    body +
    "</div><script>" +
    script +
    "</script></body></html>"
  )
}

/* ---------- 参数读取与校验 ---------- */

function reqStr(p: Record<string, unknown>, k: string, label: string, max = 2000): string {
  const v = p[k]
  const s = String(v ?? "").trim()
  if (!s) throw new Error(`${label}不能为空（参数 ${k}）`)
  if (s.length > max) throw new Error(`${label}超长（${s.length} 字符，上限 ${max}）`)
  return s
}

function optStr(p: Record<string, unknown>, k: string, max = 200): string {
  const v = p[k]
  if (v == null) return ""
  const s = String(v)
  if (s.length > max) throw new Error(`参数 ${k} 超长（${s.length} 字符，上限 ${max}）`)
  return s
}

function optNum(p: Record<string, unknown>, k: string, def: number, min: number, max: number): number {
  const v = p[k]
  if (v == null || v === "") return def
  const n = Number(v)
  if (!Number.isFinite(n)) throw new Error(`参数 ${k} 须为数字`)
  if (n < min || n > max) throw new Error(`参数 ${k} 超出范围（${min}~${max}）`)
  return n
}

function optInt(p: Record<string, unknown>, k: string, def: number, min: number, max: number): number {
  return Math.round(optNum(p, k, def, min, max))
}

/* ---------- 模板实现 ---------- */

type DemoRenderer = (params: Record<string, unknown>) => string

export interface DemoTemplate {
  id: string
  /** 工具描述中的参数说明（单源：模板参数表只写在这里与各 render 校验）。 */
  usage: string
  render: DemoRenderer
}

/** ① quiz：通用可作答练习卷（单选/填空 + 自动批改 + 解析 + 得分）——出题练习/错题重做首选。 */
const quiz: DemoRenderer = (p) => {
  const raw = p.questions
  if (!Array.isArray(raw) || !raw.length) throw new Error("questions 不能为空（题目数组 [{q, options?, answer, explain?}]）")
  if (raw.length > 50) throw new Error(`题目过多（${raw.length}，上限 50）`)
  const questions = raw.map((item, i) => {
    const q = item as Record<string, unknown>
    const stem = reqStr(q, "q", `第 ${i + 1} 题题干`, 1000)
    const answer = reqStr(q, "answer", `第 ${i + 1} 题答案`, 500)
    let options: string[] | undefined
    if (q.options != null) {
      if (!Array.isArray(q.options) || q.options.length < 2 || q.options.length > 6) {
        throw new Error(`第 ${i + 1} 题 options 须为 2~6 个选项字符串`)
      }
      options = q.options.map((o) => String(o))
      if (!options.includes(answer)) throw new Error(`第 ${i + 1} 题 answer（${answer}）须为 options 之一`)
    }
    const explain = optStr(q, "explain", 1000)
    return { q: stem, options, answer, explain }
  })
  const title = optStr(p, "title", 100) || "练习卷"
  const studentName = optStr(p, "studentName", 40)
  const sub = studentName ? `${esc(studentName)} · 共 ${questions.length} 题` : `共 ${questions.length} 题`
  const body =
    `<h1>${esc(title)}</h1><div class="sub">${sub}（作答后点击批改）</div>` +
    `<div id="qs"></div>` +
    `<div class="card" style="text-align:center"><button id="grade">提交批改</button> <span id="score" style="margin-left:12px"></span></div>`
  const script =
    `var DATA=${jsonForScript(questions)};` +
    "var box=document.getElementById('qs');" +
    "DATA.forEach(function(item,i){" +
    "var d=document.createElement('div');d.className='card';var h='<div style=\"font-weight:600;margin-bottom:8px\">'+(i+1)+'. '+item.q+'</div>';" +
    "if(item.options){item.options.forEach(function(o,j){h+='<label class=\"opt\"><input type=\"radio\" name=\"q'+i+'\" value=\"'+j+'\">'+o+'</label>'})}" +
    "else{h+='<input type=\"text\" name=\"q'+i+'\" style=\"width:100%\" placeholder=\"填写答案\">'}" +
    "h+='<div class=\"explain\" id=\"ex'+i+'\" style=\"display:none\"></div>';d.innerHTML=h;box.appendChild(d)});" +
    "document.getElementById('grade').onclick=function(){" +
    "var right=0;" +
    "DATA.forEach(function(item,i){" +
    "var el=document.querySelector('input[name=q'+i+']:checked');" +
    "var val=el?item.options[el.value]:(document.querySelector('input[name=q'+i+']')||{}).value||'';" +
    "var ok=val.trim()===String(item.answer).trim();" +
    "if(ok)right++;" +
    "var ex=document.getElementById('ex'+i);ex.style.display='block';" +
    "ex.innerHTML='<span class=\"'+(ok?'ok':'bad')+'\">'+(ok?'✓ 正确':'✗ 错误，正确答案：'+item.answer)+'</span>'+(item.explain?'<br>'+item.explain:'')});" +
    "document.getElementById('score').innerHTML='得分：<b>'+right+' / '+DATA.length+'</b>'};"
  return demoPage(title, body, script)
}

/** ② mental_math：口算闯关（页面内随机出题、计时、评分、错题回顾）。 */
const mentalMath: DemoRenderer = (p) => {
  const opsRaw = p.ops == null ? ["add", "sub"] : Array.isArray(p.ops) ? p.ops.map(String) : String(p.ops).split(",")
  const ops = opsRaw.map((s) => s.trim()).filter(Boolean)
  for (const op of ops) {
    if (!["add", "sub", "mul", "div"].includes(op)) throw new Error(`ops 含非法运算 "${op}"（可用 add/sub/mul/div）`)
  }
  if (!ops.length) throw new Error("ops 不能为空（add/sub/mul/div 至少一项）")
  const digits = optInt(p, "digits", 2, 1, 3)
  const count = optInt(p, "count", 20, 5, 100)
  const seconds = optInt(p, "seconds", 60, 10, 600)
  const studentName = optStr(p, "studentName", 40)
  const title = optStr(p, "title", 100) || `${studentName ? `${studentName}的` : ""}口算闯关`
  const opLabel: Record<string, string> = { add: "加法", sub: "减法", mul: "乘法", div: "除法" }
  const body =
    `<h1>${esc(title)}</h1><div class="sub">${ops.map((o) => opLabel[o]).join(" + ")} · ${digits} 位数 · ${count} 题 · ${seconds} 秒</div>` +
    `<div class="card" style="display:flex;align-items:center;gap:16px;flex-wrap:wrap">` +
    `<button id="start">开始</button><div>剩余时间 <b id="time" style="font-size:20px">${seconds}</b> 秒</div>` +
    `<div>进度 <b id="prog">0/${count}</b></div><div>答对 <b id="okc" class="ok">0</b></div></div>` +
    `<div class="card" id="qa" style="display:none;font-size:28px;text-align:center"></div>` +
    `<div class="card" id="done" style="display:none"></div>`
  const script =
    `var OPS=${jsonForScript(ops)},DIGITS=${digits},COUNT=${count},SECS=${seconds};` +
    "var max=function(){return Math.pow(10,DIGITS)-1},min=function(){return DIGITS===1?0:Math.pow(10,DIGITS-1)}," +
    "rnd=function(a,b){return a+Math.floor(Math.random()*(b-a+1))},cur=null,ok=0,done=0,left=SECS,timer=null,wrong=[];" +
    "function gen(){var op=OPS[rnd(0,OPS.length-1)],a,b;" +
    "if(op==='add'){a=rnd(min(),max());b=rnd(min(),max());return{q:a+' + '+b,r:a+b}}" +
    "if(op==='sub'){a=rnd(min(),max());b=rnd(min(),a);return{q:a+' − '+b,r:a-b}}" +
    "if(op==='mul'){var m=DIGITS===1?9:Math.pow(10,DIGITS-1)-1;a=rnd(2,m);b=rnd(2,max());return{q:a+' × '+b,r:a*b}}" +
    "b=rnd(2,DIGITS===1?9:12);var t=rnd(2,DIGITS===1?9:30);return{q:(b*t)+' ÷ '+b,r:t}}" +
    "function next(){cur=gen();document.getElementById('qa').innerHTML=cur.q+' = <input type=\"text\" id=\"ans\" autofocus>';" +
    "var el=document.getElementById('ans');el.onkeydown=function(e){if(e.key==='Enter'){judge()}};el.focus()}" +
    "function judge(){var v=parseInt(document.getElementById('ans').value,10);" +
    "if(!isNaN(v)){done++;if(v===cur.r){ok++;document.getElementById('okc').textContent=ok}else{wrong.push({q:cur.q,r:cur.r,a:v})}" +
    "document.getElementById('prog').textContent=done+'/'+COUNT;if(done>=COUNT)return finish();next()}}" +
    "function finish(){clearInterval(timer);document.getElementById('qa').style.display='none';" +
    "var d=document.getElementById('done');d.style.display='block';" +
    "d.innerHTML='<h1 style=\"margin-bottom:6px\">完成！</h1><div>答对 <b class=\"ok\">'+ok+'</b> / '+done+'，用时 '+(SECS-left)+' 秒</div>'+" +
    "(wrong.length?'<h1 style=\"margin-top:14px\">错题回顾</h1>'+wrong.map(function(w){return'<div class=\"card\">'+w.q+' = <b class=\"ok\">'+w.r+'</b>（你答 '+w.a+'）</div>'}).join(''):'<div class=\"ok\" style=\"margin-top:10px\">全部正确，太棒了！</div>')}" +
    "document.getElementById('start').onclick=function(){this.disabled=true;this.textContent='挑战中…';" +
    "document.getElementById('qa').style.display='block';next();" +
    "timer=setInterval(function(){left--;document.getElementById('time').textContent=left;if(left<=0)finish()},1000)}"
  return demoPage(title, body, script)
}

/** ③ column：竖式计算分步演示（下一步/上一步/自动播放；数位高亮、进位借位浮现、结果逐位揭示）。 */

/** 竖式分步数据（col 从右往左 0 起：0=个位；导出供测试）。 */
export interface ColumnStep {
  col: number
  text: string
  /** 本步写出的结果位数字（加减法）。 */
  resultDigit?: string
  /** 进位/借位标记（标注在左一列上方）。 */
  carry?: number
  borrow?: number
  /** 乘法：本步部分积（含末尾补零）。 */
  partial?: string
  /** 乘法最后一步：揭示全部结果位。 */
  revealAll?: boolean
}

export function columnSteps(a: number, b: number, op: "add" | "sub" | "mul"): ColumnStep[] {
  const posName = (i: number): string => ["个位", "十位", "百位", "千位", "万位"][i] ?? `第 ${i + 1} 位`
  const digitsA = String(a).split("").map(Number)
  const digitsB = String(b).split("").map(Number)
  const steps: ColumnStep[] = []
  if (op === "add") {
    const len = Math.max(digitsA.length, digitsB.length)
    const da = [...Array(len - digitsA.length).fill(0), ...digitsA]
    const db = [...Array(len - digitsB.length).fill(0), ...digitsB]
    let carry = 0
    for (let i = len - 1; i >= 0; i--) {
      const col = len - 1 - i
      const s = da[i] + db[i] + carry
      const c = s >= 10 ? 1 : 0
      steps.push({
        col,
        text: `${posName(col)}：${da[i]} + ${db[i]}${carry ? " + 进 1" : ""} = ${s}${c ? `，满十向${col + 1 < len ? posName(col + 1) : "更高位"}进 1` : ""}，写 ${s % 10}`,
        resultDigit: String(s % 10),
        carry: c,
      })
      carry = c
    }
    if (carry) steps.push({ col: digitsA.length > digitsB.length ? digitsA.length : digitsB.length, text: "最高位相加仍进 1，向左多写出一位 1", resultDigit: "1" })
    return steps
  }
  if (op === "sub") {
    const len = digitsA.length
    const db = [...Array(len - digitsB.length).fill(0), ...digitsB]
    let borrow = 0
    for (let i = len - 1; i >= 0; i--) {
      const col = len - 1 - i
      const top = digitsA[i] - borrow
      const need = top < db[i]
      const v = need ? top + 10 : top
      const intro = borrow ? `${digitsA[i]}（被借走 1，剩 ${top}）` : `${digitsA[i]}`
      steps.push({
        col,
        text: need
          ? `${posName(col)}：${intro} 不够减 ${db[i]}，向前一位借 1 当 10：${v} − ${db[i]} = ${v - db[i]}，写 ${v - db[i]}`
          : `${posName(col)}：${intro} − ${db[i]} = ${top - db[i]}，写 ${top - db[i]}`,
        resultDigit: String(v - db[i]),
        borrow: need ? 1 : 0,
      })
      borrow = need ? 1 : 0
    }
    return steps
  }
  for (let j = digitsB.length - 1; j >= 0; j--) {
    const zeros = digitsB.length - 1 - j
    if (digitsB[j] === 0) {
      steps.push({ col: j, text: `用 b 的${posName(j)} 0 去乘 a：任何数乘 0 得 0，此行省略不写` })
      continue
    }
    const part = a * digitsB[j]
    steps.push({
      col: j,
      partial: `${part}${"0".repeat(zeros)}`,
      text: `用 b 的${posName(j)} ${digitsB[j]} 去乘 a：${a} × ${digitsB[j]} = ${part}${zeros ? `，从${posName(j)}写起，末尾补 ${zeros} 个 0 → ${part}${"0".repeat(zeros)}` : ""}`,
    })
  }
  steps.push({ col: 0, text: `把各部分积按位相加：${a} × ${b} = ${a * b}`, revealAll: true })
  return steps
}

const column: DemoRenderer = (p) => {
  const op = String(p.op ?? "add")
  if (!["add", "sub", "mul"].includes(op)) throw new Error(`op 须为 add/sub/mul（收到 "${op}"）`)
  const a = optInt(p, "a", NaN, 0, 99999)
  const b = optInt(p, "b", NaN, 0, 9999)
  if (!Number.isFinite(a) || !Number.isFinite(b)) throw new Error("a 与 b 必填（非负整数）")
  if (op === "sub" && b > a) throw new Error("减法竖式要求 a ≥ b（小学不演示负数结果）")
  const sym = op === "add" ? "+" : op === "sub" ? "−" : "×"
  const result = op === "add" ? a + b : op === "sub" ? a - b : a * b
  const steps = columnSteps(a, b, op as "add" | "sub" | "mul")
  const width = Math.max(
    String(a).length,
    String(b).length + 1,
    String(result).length,
    op === "mul" ? String(a).length + String(b).length : 0,
  )
  const placeRight = (s: string): string[] => {
    const arr: string[] = Array(width).fill("")
    for (let i = 0; i < s.length && i < width; i++) arr[width - s.length + i] = s[i]
    return arr
  }
  const opRow = placeRight(String(b))
  if (width - String(b).length - 1 >= 0) opRow[width - String(b).length - 1] = sym
  const rowHtml = (cells: string[], cls = ""): string =>
    `<div class="crow ${cls}">${cells.map((c, i) => `<span class="cell" data-col="${i}">${esc(c)}</span>`).join("")}</div>`
  const partialRows = steps
    .filter((s) => s.partial != null)
    .map((s) => `<div class="crow prow" style="display:none">${placeRight(s.partial!).map((c, i) => `<span class="cell" data-col="${i}">${esc(c)}</span>`).join("")}</div>`)
    .join("")
  const resultCells = placeRight(String(result))
    .map((c, i) => `<span class="cell rd" data-col="${i}" data-d="${esc(c)}">${c === "" ? "" : "·"}</span>`)
    .join("")
  const caption =
    op === "add"
      ? "红色小字是进位：哪一位相加满十，就向前一位进 1。"
      : op === "sub"
        ? "红色圆点是借位标记：哪一位不够减，就向前一位借 1 当 10。"
        : "用 b 的每一位分别乘 a（部分积），第二位起末尾补 0，再把部分积相加。"
  const body =
    `<h1>竖式演示</h1><div class="sub">${a} ${sym} ${b} = ${result}（点「下一步」逐步看过程）</div>` +
    `<div class="card"><div class="colwrap" id="cw">` +
    `<div class="crow cmark">${Array.from({ length: width }, (_, i) => `<span class="cell" data-col="${i}"></span>`).join("")}</div>` +
    rowHtml(placeRight(String(a))) +
    rowHtml(opRow) +
    `<div class="hline"></div>` +
    partialRows +
    (steps.filter((s) => s.partial != null).length > 1 ? `<div class="hline thin"></div>` : "") +
    `<div class="crow">${resultCells}</div>` +
    `</div></div>` +
    `<div class="card"><div style="text-align:center;margin-bottom:10px">` +
    `<button class="ghost" id="prev">上一步</button> <button id="next">下一步 ▶</button> <button class="ghost" id="auto">自动播放</button></div>` +
    `<div class="stepbox" id="steptext">点击「下一步」开始逐步演示竖式计算</div></div>` +
    `<div class="card" style="font-size:14px;color:var(--muted)">${caption}</div>`
  const script =
    `var STEPS=${jsonForScript(steps)},W=${width},OP=${JSON.stringify(op)},idx=-1,timer=null;` +
    "function cells(sel){return document.querySelectorAll(sel)}" +
    "function left(col){return W-1-col}" +
    "function apply(){" +
    // 全量重算（幂等）：清空动态位 → 重放到 idx
    "cells('.cmark .cell').forEach(function(c){c.textContent=''});" +
    "cells('.rd').forEach(function(c){c.textContent=c.getAttribute('data-d')===''?'':'·'});" +
    "cells('.prow').forEach(function(r){r.style.display='none'});" +
    "cells('.cell.on').forEach(function(c){c.classList.remove('on')});" +
    "for(var k=0;k<=idx;k++){var s=STEPS[k];" +
    "if(s.resultDigit!=null){var cs=document.querySelectorAll('.rd');cs[left(s.col)]&&(cs[left(s.col)].textContent=s.resultDigit)}" +
    "if(s.revealAll){cells('.rd').forEach(function(c){c.textContent=c.getAttribute('data-d')===''?'':c.getAttribute('data-d')})}" +
    "if((s.carry||s.borrow)&&left(s.col)-1>=0){var m=cells('.cmark .cell')[left(s.col)-1];if(m)m.textContent=s.carry?'1':'•'}" +
    "if(s.partial!=null){var pr=cells('.prow');var vis=0;for(var w=0;w<=idx;w++){if(STEPS[w].partial!=null)vis++}for(var v=0;v<vis&&v<pr.length;v++){pr[v].style.display='flex'}}}" +
    "if(idx>=0){var cur=STEPS[idx];var cl=left(cur.col);if(cl>=0&&cl<W){cells('.crow .cell[data-col=\"'+cl+'\"]').forEach(function(c){c.classList.add('on')})}}" +
    "document.getElementById('steptext').innerHTML=idx<0?'点击「下一步」开始逐步演示竖式计算':('<b>第 '+(idx+1)+' / '+STEPS.length+' 步</b>：'+STEPS[idx].text)}" +
    "document.getElementById('next').onclick=function(){if(idx<STEPS.length-1){idx++;apply()}};" +
    "document.getElementById('prev').onclick=function(){if(idx>=0){idx--;apply()}};" +
    "document.getElementById('auto').onclick=function(){if(timer){clearInterval(timer);timer=null;this.textContent='自动播放';return}" +
    "this.textContent='停止';var self=this;timer=setInterval(function(){if(idx>=STEPS.length-1){clearInterval(timer);timer=null;self.textContent='自动播放';return}idx++;apply()},1600)};" +
    "apply()"
  return demoPage("竖式演示", body, script)
}

/** ④ function_graph：函数图像（SVG 静态绘制 + 关键点标注）。 */
const functionGraph: DemoRenderer = (p) => {
  const fn = String(p.fn ?? "linear")
  const defs: Record<string, { desc: (a: number, b: number, c: number) => string; f: (a: number, b: number, c: number, x: number) => number }> = {
    linear: { desc: (a, b) => `y = ${fmtCoef(a)}x${b ? fmtConst(b) : ""}`, f: (a, b, _c, x) => a * x + b },
    quadratic: { desc: (a, b, c) => `y = ${fmtCoef(a)}x²${b ? fmtConst(b) + "x" : ""}${c ? fmtConst(c) : ""}`, f: (a, b, c, x) => a * x * x + b * x + c },
    inverse: { desc: (a) => `y = ${a}/x${a < 0 ? "（k<0，双曲线在二、四象限）" : "（k>0，双曲线在一、三象限）"}`, f: (a, _b, _c, x) => (x === 0 ? NaN : a / x) },
    absolute: { desc: (a, b, c) => `y = ${fmtCoef(a)}|x${b ? fmtConst(b) : ""}|${c ? fmtConst(c) : ""}`, f: (a, b, c, x) => a * Math.abs(x + b) + c },
  }
  const def = defs[fn]
  if (!def) throw new Error(`fn 须为 linear/quadratic/inverse/absolute（收到 "${fn}"）`)
  const a = optNum(p, "a", 1, -50, 50)
  const b = optNum(p, "b", 0, -50, 50)
  const c = optNum(p, "c", 0, -50, 50)
  if (fn === "quadratic" && a === 0) throw new Error("quadratic 的 a 不能为 0（否则不是二次函数）")
  if (fn === "inverse" && a === 0) throw new Error("inverse 的 a 不能为 0（否则不是反比例函数）")
  if (fn === "absolute" && a === 0) throw new Error("absolute 的 a 不能为 0")
  const xMin = optNum(p, "xMin", -5, -100, 100)
  const xMax = optNum(p, "xMax", 5, -100, 100)
  if (xMax - xMin < 1) throw new Error("xMax 与 xMin 至少相差 1")
  // 采样并计算 y 范围（截断奇点附近极端值）
  const pts: Array<[number, number]> = []
  let yMin = Infinity
  let yMax = -Infinity
  const N = 400
  for (let i = 0; i <= N; i++) {
    const x = xMin + ((xMax - xMin) * i) / N
    const y = def.f(a, b, c, x)
    if (!Number.isFinite(y)) continue
    pts.push([x, y])
    if (y < yMin) yMin = y
    if (y > yMax) yMax = y
  }
  yMin = Math.max(yMin, -50)
  yMax = Math.min(yMax, 50)
  if (yMax - yMin < 1e-9) {
    yMax += 1
    yMin -= 1
  }
  const W = 640
  const H = 440
  const px = (x: number) => ((x - xMin) / (xMax - xMin)) * (W - 60) + 40
  const py = (y: number) => H - 30 - ((y - yMin) / (yMax - yMin)) * (H - 60)
  const sx = xScaleStep(xMin, xMax)
  const sy = yScaleStep(yMin, yMax)
  let svg = ""
  for (let gx = Math.ceil(xMin / sx) * sx; gx <= xMax; gx += sx) {
    svg += `<line x1="${px(gx)}" y1="10" x2="${px(gx)}" y2="${H - 30}" stroke="#e5e9f0"/>`
  }
  for (let gy = Math.ceil(yMin / sy) * sy; gy <= yMax; gy += sy) {
    svg += `<line x1="40" y1="${py(gy)}" x2="${W - 20}" y2="${py(gy)}" stroke="#e5e9f0"/>`
  }
  if (yMin <= 0 && yMax >= 0) svg += `<line x1="40" y1="${py(0)}" x2="${W - 20}" y2="${py(0)}" stroke="#444"/><text x="${W - 16}" y="${py(0) + 14}" font-size="12">x</text>`
  if (xMin <= 0 && xMax >= 0) svg += `<line x1="${px(0)}" y1="10" x2="${px(0)}" y2="${H - 30}" stroke="#444"/><text x="${px(0) + 5}" y="20" font-size="12">y</text>`
  let path = ""
  let pen = false
  for (const [x, y] of pts) {
    if (y < yMin - 1 || y > yMax + 1) {
      pen = false
      continue
    }
    path += `${pen ? "L" : "M"}${px(x).toFixed(1)},${py(y).toFixed(1)}`
    pen = true
  }
  svg += `<path d="${path}" fill="none" stroke="var(--accent,#4f7cff)" stroke-width="2.5"/>`
  const mark = (x: number, y: number, label: string) =>
    `<circle cx="${px(x)}" cy="${py(y)}" r="4" fill="var(--bad,#e5534b)"/><text x="${px(x) + 7}" y="${py(y) - 7}" font-size="13" fill="#e5534b">${esc(label)}</text>`
  if (fn === "linear" && a !== 0 && -b / a >= xMin && -b / a <= xMax) svg += mark(-b / a, 0, `x轴交点(${fmtNum(-b / a)}, 0)`)
  if (fn === "linear" && b >= yMin && b <= yMax) svg += mark(0, b, `y轴交点(0, ${fmtNum(b)})`)
  if (fn === "quadratic") {
    const vx = -b / (2 * a)
    const vy = def.f(a, b, c, vx)
    if (vx >= xMin && vx <= xMax && vy >= yMin && vy <= yMax) svg += mark(vx, vy, `顶点(${fmtNum(vx)}, ${fmtNum(vy)})`)
  }
  if (fn === "absolute") {
    const vy = c
    if (-b >= xMin && -b <= xMax && vy >= yMin && vy <= yMax) svg += mark(-b, vy, `顶点(${fmtNum(-b)}, ${fmtNum(vy)})`)
  }
  if (fn === "inverse") svg += `<text x="${W / 2 - 60}" y="26" font-size="14" fill="#4f7cff">k = ${fmtNum(a)}</text>`
  // 交互滑块：各函数可调参数（linear: a,b / quadratic·absolute: a,b,c / inverse: a）
  const sliders: Array<{ key: string; label: string; val: number }> =
    fn === "linear"
      ? [
          { key: "A", label: "a（斜率）", val: a },
          { key: "B", label: "b（截距）", val: b },
        ]
      : fn === "inverse"
        ? [{ key: "A", label: "k（比例系数）", val: a }]
        : [
            { key: "A", label: "a（开口/系数）", val: a },
            { key: "B", label: "b", val: b },
            { key: "C", label: "c", val: c },
          ]
  const sliderHtml = sliders
    .map(
      (s) =>
        `<div style="display:flex;align-items:center;gap:10px;justify-content:center;margin:6px 0">` +
        `<span style="width:110px;text-align:right">${s.label} = <b id="v${s.key}">${fmtNum(s.val)}</b></span>` +
        `<input type="range" id="s${s.key}" min="-5" max="5" step="0.1" value="${s.val}"></div>`,
    )
    .join("")
  const body =
    `<h1>函数图像（可交互）</h1><div class="sub" id="ftext">${esc(def.desc(a, b, c))}，x ∈ [${fmtNum(xMin)}, ${fmtNum(xMax)}]</div>` +
    `<div class="card" style="text-align:center"><svg viewBox="0 0 ${W} ${H}" style="max-width:100%;height:auto"><g id="gs">${svg}</g></svg></div>` +
    `<div class="card"><div style="text-align:center;font-size:13px;color:var(--muted);margin-bottom:8px">拖动滑块实时观察图像变化${fn !== "linear" ? "（a 不能为 0，会自动跳过）" : ""}</div>${sliderHtml}</div>`
  const script =
    `var FN=${JSON.stringify(fn)},A=${a},B=${b},C=${c},XMIN=${xMin},XMAX=${xMax},W=${W},H=${H};` +
    "function f(x){if(FN==='linear')return A*x+B;if(FN==='quadratic')return A*x*x+B*x+C;if(FN==='inverse')return x===0?NaN:A/x;return A*Math.abs(x+B)+C}" +
    "function fmt(v){return Number.isInteger(v)?String(v):String(Math.round(v*100)/100)}" +
    "function coef(v){return v===1?'':v===-1?'−':fmt(v)}function cst(v){return v>0?' + '+fmt(v):' − '+fmt(-v)}" +
    "function expr(){if(FN==='linear')return 'y = '+coef(A)+'x'+(B?cst(B):'');if(FN==='quadratic')return 'y = '+coef(A)+'x²'+(B?cst(B)+'x':'')+(C?cst(C):'');if(FN==='inverse')return 'y = '+fmt(A)+'/x'+(A<0?'（k<0，二、四象限）':'（k>0，一、三象限）');return 'y = '+coef(A)+'|x'+(B?cst(B):'')+'|'+(C?cst(C):'')}" +
    "function step(mn,mx){var span=mx-mn,ss=[1,2,5,10,20,50,100];for(var i=0;i<ss.length;i++){if(span/ss[i]<=12)return ss[i]}return 200}" +
    "function draw(){" +
    "var pts=[],ymin=Infinity,ymax=-Infinity;" +
    "for(var i=0;i<=400;i++){var x=XMIN+(XMAX-XMIN)*i/400,y=f(x);if(!isFinite(y))continue;pts.push([x,y]);if(y<ymin)ymin=y;if(y>ymax)ymax=y}" +
    "ymin=Math.max(ymin,-50);ymax=Math.min(ymax,50);if(ymax-ymin<1e-9){ymax+=1;ymin-=1}" +
    "function px(x){return (x-XMIN)/(XMAX-XMIN)*(W-60)+40}function py(y){return H-30-(y-ymin)/(ymax-ymin)*(H-60)}" +
    "var sx=step(XMIN,XMAX),sy=step(ymin,ymax),s='';" +
    "for(var gx=Math.ceil(XMIN/sx)*sx;gx<=XMAX;gx+=sx){s+='<line x1=\"'+px(gx)+'\" y1=\"10\" x2=\"'+px(gx)+'\" y2=\"'+(H-30)+'\" stroke=\"#e5e9f0\"/>'}" +
    "for(var gy=Math.ceil(ymin/sy)*sy;gy<=ymax;gy+=sy){s+='<line x1=\"40\" y1=\"'+py(gy)+'\" x2=\"'+(W-20)+'\" y2=\"'+py(gy)+'\" stroke=\"#e5e9f0\"/>'}" +
    "if(ymin<=0&&ymax>=0){s+='<line x1=\"40\" y1=\"'+py(0)+'\" x2=\"'+(W-20)+'\" y2=\"'+py(0)+'\" stroke=\"#444\"/><text x=\"'+(W-16)+'\" y=\"'+(py(0)+14)+'\" font-size=\"12\">x</text>'}" +
    "if(XMIN<=0&&XMAX>=0){s+='<line x1=\"'+px(0)+'\" y1=\"10\" x2=\"'+px(0)+'\" y2=\"'+(H-30)+'\" stroke=\"#444\"/><text x=\"'+(px(0)+5)+'\" y=\"20\" font-size=\"12\">y</text>'}" +
    "var path='',pen=false;" +
    "for(var j=0;j<pts.length;j++){var q=pts[j];if(q[1]<ymin-1||q[1]>ymax+1){pen=false;continue}path+=(pen?'L':'M')+px(q[0]).toFixed(1)+','+py(q[1]).toFixed(1);pen=true}" +
    "s+='<path d=\"'+path+'\" fill=\"none\" stroke=\"#4f7cff\" stroke-width=\"2.5\"/>';" +
    "function mark(x,y,l){s+='<circle cx=\"'+px(x)+'\" cy=\"'+py(y)+'\" r=\"4\" fill=\"#e5534b\"/><text x=\"'+(px(x)+7)+'\" y=\"'+(py(y)-7)+'\" font-size=\"13\" fill=\"#e5534b\">'+l+'</text>'}" +
    "if(FN==='linear'&&A!==0&&-B/A>=XMIN&&-B/A<=XMAX)mark(-B/A,0,'x轴交点('+fmt(-B/A)+', 0)');" +
    "if(FN==='linear'&&B>=ymin&&B<=ymax)mark(0,B,'y轴交点(0, '+fmt(B)+')');" +
    "if(FN==='quadratic'){var vx=-B/(2*A),vy=f(vx);if(vx>=XMIN&&vx<=XMAX&&vy>=ymin&&vy<=ymax)mark(vx,vy,'顶点('+fmt(vx)+', '+fmt(vy)+')')}" +
    "if(FN==='absolute'&&-B>=XMIN&&-B<=XMAX&&C>=ymin&&C<=ymax)mark(-B,C,'顶点('+fmt(-B)+', '+fmt(C)+')');" +
    "if(FN==='inverse'){s+='<text x=\"'+(W/2-60)+'\" y=\"26\" font-size=\"14\" fill=\"#4f7cff\">k = '+fmt(A)+'</text>'}" +
    "document.getElementById('gs').innerHTML=s;" +
    "document.getElementById('ftext').textContent=expr()+'，x ∈ ['+fmt(XMIN)+', '+fmt(XMAX)+']';" +
    "['A','B','C'].forEach(function(k){var el=document.getElementById('v'+k);if(el)el.textContent=fmt(k==='A'?A:k==='B'?B:C)})}" +
    "document.querySelectorAll('input[type=range]').forEach(function(el){el.oninput=function(){var v=parseFloat(el.value);" +
    "if(el.id==='sA'){A=v;if(FN!=='linear'&&A===0){A=0.1}}if(el.id==='sB')B=v;if(el.id==='sC')C=v;draw()}});" +
    "draw()"
  return demoPage("函数图像", body, script)
}

function fmtCoef(a: number): string {
  return a === 1 ? "" : a === -1 ? "−" : fmtNum(a)
}
function fmtConst(v: number): string {
  return v > 0 ? ` + ${fmtNum(v)}` : ` − ${fmtNum(-v)}`
}
function fmtNum(v: number): string {
  return Number.isInteger(v) ? String(v) : String(Math.round(v * 100) / 100)
}
function xScaleStep(min: number, max: number): number {
  const span = max - min
  for (const s of [1, 2, 5, 10, 20, 50, 100]) if (span / s <= 12) return s
  return 200
}
function yScaleStep(min: number, max: number): number {
  return xScaleStep(min, max)
}

/** ⑤ geometry：几何交互演示（预设图形 + 动画/拖动/滑块/高亮）。 */
const geometry: DemoRenderer = (p) => {
  const preset = String(p.preset ?? "")
  const presets: Record<string, { title: string; body: string; script: string; note: string }> = {
    pythagorean: {
      title: "勾股定理（拼图动画证明）",
      body: geomPythagoreanBody(),
      script: geomPythagoreanScript(),
      note: "外正方形边长 a+b=7：左图 4 个全等直角三角形围出中间的斜正方形 c²（=(a+b)² − 4×ab/2）；点「重排拼图」后同样 4 个三角形移位，中间变成两个正方形 a² 与 b²（同样 =(a+b)² − 4×ab/2）。两块中间区域面积相等，即 a² + b² = c²（这里 9 + 16 = 25）。配静态图可用 demo 的 geometry 与本页互补。",
    },
    triangle_height: {
      title: "三角形的高（可拖动）",
      body: geomTriangleHeightBody(),
      script: geomTriangleHeightScript(),
      note: "从顶点 A 向对边 BC（或其延长线）作垂线，垂线段就是 BC 边上的高。拖动顶点 A 观察：高始终与底边垂直（垂足处红色直角标记）；当垂足落到 BC 延长线上时，高在三角形外部——钝角三角形的高就是这样。",
    },
    circle: {
      title: "圆的要素（点击高亮）",
      body: geomCircleBody(),
      script: geomCircleScript(),
      note: "圆心 O 决定位置，半径 r 决定大小，直径 d=2r；弦是连接圆上两点的线段（直径是特殊的弦）；两条半径与所夹弧围成扇形。点击按钮分别高亮观察各要素。",
    },
    parallel: {
      title: "平行线与截线的角（拖滑块）",
      body: geomParallelBody(),
      script: geomParallelScript(),
      note: "拖动滑块改变截线的倾斜角：同色角是同位角——始终相等（∠1=∠5、∠2=∠6、∠3=∠7、∠4=∠8）；夹在两平行线之间、截线两侧的内错角相等；截线同侧的同旁内角互补（和为 180°）。",
    },
  }
  const def = presets[preset]
  if (!def) throw new Error(`preset 须为 pythagorean/triangle_height/circle/parallel（收到 "${preset}"）`)
  return demoPage(
    def.title,
    `<h1>${esc(def.title)}</h1>${def.body}<div class="card" style="font-size:14px;color:var(--muted)">${esc(def.note)}</div>`,
    def.script,
  )
}

/** 勾股拼图：4 个全等直角三角形两种布局切换（CSS transform 过渡动画）。 */
function geomPythagoreanBody(): string {
  const u = 26
  const s = 7 * u
  const tri = (id: string, a: string, b: string): string =>
    `<g id="${id}" class="tri" style="transform:${a}" data-a="${a}" data-b="${b}"><polygon points="0,0 ${4 * u},0 0,${3 * u}" fill="#c9dbff" stroke="#345" stroke-width="2"/></g>`
  return (
    `<div class="card" style="text-align:center">` +
    `<svg viewBox="-14 -14 ${s + 42} ${s + 42}" style="max-width:420px;height:auto">` +
    `<rect x="0" y="0" width="${s}" height="${s}" fill="none" stroke="#345" stroke-width="2"/>` +
    // 状态 A：中间斜正方形 c²（淡入淡出于两状态）
    `<polygon id="c2" class="fadeA" points="${4 * u},0 ${s},${3 * u} ${3 * u},${s} 0,${4 * u}" fill="#ffe9b8" stroke="#c90" stroke-width="2"/>` +
    `<text id="c2t" class="fadeA" x="${s / 2 + 10}" y="${s / 2 + 6}" font-size="15" fill="#a80">c²=25</text>` +
    // 状态 B：两个正方形 a² 与 b²
    `<rect id="a2" class="fadeB" x="0" y="0" width="${3 * u}" height="${3 * u}" fill="#e3f4e3" stroke="#4a4" stroke-width="2" style="opacity:0"/>` +
    `<text id="a2t" class="fadeB" x="${1.5 * u - 18}" y="${1.5 * u}" font-size="14" fill="#4a4" style="opacity:0">a²=9</text>` +
    `<rect id="b2" class="fadeB" x="${3 * u}" y="${3 * u}" width="${4 * u}" height="${4 * u}" fill="#ffe3e3" stroke="#b46" stroke-width="2" style="opacity:0"/>` +
    `<text id="b2t" class="fadeB" x="${5 * u - 16}" y="${5 * u}" font-size="14" fill="#b46" style="opacity:0">b²=16</text>` +
    // 4 个全等三角形（两态 transform 由 data-a/data-b 提供，CSS transition 动画）
    tri("t0", `translate(0px,0px) rotate(0deg)`, `translate(${3 * u}px,0px) rotate(0deg)`) +
    tri("t1", `translate(${s}px,0px) rotate(90deg)`, `translate(${s}px,${3 * u}px) rotate(180deg)`) +
    tri("t2", `translate(${s}px,${s}px) rotate(180deg)`, `translate(${3 * u}px,${3 * u}px) rotate(90deg)`) +
    tri("t3", `translate(0px,${s}px) rotate(-90deg)`, `translate(0px,${s}px) rotate(-90deg)`) +
    `<text x="${s - 30}" y="${s + 26}" font-size="13" fill="#888">a+b = 7</text>` +
    `</svg>` +
    `<div style="margin-top:10px"><button id="rearr">🔀 重排拼图证明</button></div>` +
    `<div id="pythcap" style="margin-top:10px;font-size:15px">大正方形 = 4 个三角形 + <b>斜正方形 c²</b>　即　(a+b)² = 2ab + c²</div>` +
    `</div>`
  )
}
function geomPythagoreanScript(): string {
  return (
    "var st=0;" +
    "document.getElementById('rearr').onclick=function(){" +
    "st^=1;" +
    "document.querySelectorAll('.tri').forEach(function(t){t.style.transform=t.getAttribute(st?'data-b':'data-a')});" +
    "document.querySelectorAll('.fadeA').forEach(function(e){e.style.opacity=st?0:1});" +
    "document.querySelectorAll('.fadeB').forEach(function(e){e.style.opacity=st?1:0});" +
    "document.getElementById('pythcap').innerHTML=st?'大正方形 = 4 个三角形 + <b>两个正方形 a² 与 b²</b>　即　(a+b)² = 2ab + a² + b²':'大正方形 = 4 个三角形 + <b>斜正方形 c²</b>　即　(a+b)² = 2ab + c²';" +
    "document.getElementById('rearr').textContent=st?'↩ 恢复弦图':'🔀 重排拼图证明'}"
  )
}

/** 三角形的高：拖动顶点 A，高线/垂足/直角标记实时跟随。 */
function geomTriangleHeightBody(): string {
  return (
    `<div class="card" style="text-align:center">` +
    `<svg id="thsvg" viewBox="20 10 360 240" style="max-width:100%;height:auto;touch-action:none">` +
    `<polygon id="thtri" points="200,50 50,220 330,220" fill="#dbe7ff" stroke="#345" stroke-width="2"/>` +
    `<line id="thalt" x1="200" y1="50" x2="200" y2="220" stroke="#e5534b" stroke-width="2" stroke-dasharray="6 4"/>` +
    `<rect id="thraq" x="200" y="208" width="12" height="12" fill="none" stroke="#e5534b" stroke-width="1.5"/>` +
    `<text id="thA" x="206" y="44" font-size="15">A（拖我）</text>` +
    `<text x="34" y="236" font-size="15">B</text><text x="334" y="236" font-size="15">C</text>` +
    `<text id="thH" x="210" y="140" fill="#e5534b" font-size="14">高 h</text>` +
    `<text x="160" y="236" font-size="14">底边 BC</text>` +
    `<circle id="thdot" cx="200" cy="50" r="11" fill="#e5534b" opacity="0.35" style="cursor:grab"/>` +
    `</svg>` +
    `<div style="font-size:14px;color:var(--muted);margin-top:8px">按住红圈拖动顶点 A，观察高的变化</div>` +
    `</div>`
  )
}
function geomTriangleHeightScript(): string {
  return (
    "var svg=document.getElementById('thsvg'),dot=document.getElementById('thdot');" +
    "function pt(ev){var r=svg.getBoundingClientRect();var vx=20+360*(ev.clientX-r.left)/r.width,vy=10+240*(ev.clientY-r.top)/r.height;" +
    "return [Math.max(30,Math.min(350,vx)),Math.max(30,Math.min(200,vy))]}" +
    "function upd(x,y){" +
    "document.getElementById('thtri').setAttribute('points',x+','+y+' 50,220 330,220');" +
    "document.getElementById('thalt').setAttribute('x1',x);document.getElementById('thalt').setAttribute('y1',y);" +
    "document.getElementById('thalt').setAttribute('x2',x);" +
    "var ra=document.getElementById('thraq');ra.setAttribute('x',x);ra.setAttribute('y',208);" +
    "document.getElementById('thdot').setAttribute('cx',x);document.getElementById('thdot').setAttribute('cy',y);" +
    "var t=document.getElementById('thA');t.setAttribute('x',x+8);t.setAttribute('y',y-8);" +
    "var h=document.getElementById('thH');h.setAttribute('x',x+10);h.setAttribute('y',(y+220)/2)}" +
    "var drag=false;" +
    "dot.addEventListener('pointerdown',function(e){drag=true;dot.setPointerCapture(e.pointerId)});" +
    "dot.addEventListener('pointermove',function(e){if(!drag)return;var q=pt(e);upd(q[0],q[1])});" +
    "dot.addEventListener('pointerup',function(){drag=false});dot.addEventListener('pointercancel',function(){drag=false});"
  )
}

/** 圆的要素：按钮切换高亮（其余淡出）。 */
function geomCircleBody(): string {
  return (
    `<div class="card" style="text-align:center">` +
    `<svg viewBox="0 0 380 280" style="max-width:100%;height:auto">` +
    `<circle cx="190" cy="140" r="100" fill="#f4f8ff" stroke="#345" stroke-width="2"/>` +
    `<g class="el" id="g-arc"><path d="M190,140 L261,90 A100,100 0 0 1 261,190 Z" fill="#ffe9b8"/><path d="M261,90 A100,100 0 0 1 261,190" fill="none" stroke="#e5534b" stroke-width="3"/></g>` +
    `<g class="el" id="g-r"><line x1="190" y1="140" x2="261" y2="90" stroke="#4f7cff" stroke-width="3"/><text x="205" y="103" font-size="14" fill="#4f7cff">半径 r</text></g>` +
    `<g class="el" id="g-d"><line x1="90" y1="140" x2="290" y2="140" stroke="#2ea44f" stroke-width="3"/><text x="168" y="132" font-size="14" fill="#2ea44f">直径 d=2r</text></g>` +
    `<g class="el" id="g-chord"><line x1="130" y1="210" x2="261" y2="190" stroke="#a05a2c" stroke-width="3"/><text x="150" y="224" font-size="14" fill="#a05a2c">弦</text></g>` +
    `<g class="el" id="g-o"><circle cx="190" cy="140" r="4" fill="#345"/><text x="196" y="134" font-size="15">O（圆心）</text></g>` +
    `<text x="268" y="145" font-size="14" fill="#e5534b">弧与扇形</text>` +
    `</svg>` +
    `<div style="margin-top:10px;display:flex;gap:8px;justify-content:center;flex-wrap:wrap">` +
    `<button class="ghost pick" data-g="">全部</button>` +
    `<button class="ghost pick" data-g="g-o">圆心</button>` +
    `<button class="ghost pick" data-g="g-r">半径 r</button>` +
    `<button class="ghost pick" data-g="g-d">直径 d</button>` +
    `<button class="ghost pick" data-g="g-chord">弦</button>` +
    `<button class="ghost pick" data-g="g-arc">弧与扇形</button>` +
    `</div></div>`
  )
}
function geomCircleScript(): string {
  return (
    "document.querySelectorAll('.pick').forEach(function(btn){btn.onclick=function(){" +
    "var g=btn.getAttribute('data-g');" +
    "document.querySelectorAll('.el').forEach(function(e){e.classList.toggle('dim',!!g&&e.id!==g)})}})"
  )
}

/** 平行线截角：滑块改变截线倾角，八个角实时标值（同位角同色）。 */
function geomParallelBody(): string {
  return (
    `<div class="card" style="text-align:center">` +
    `<svg id="ppsvg" viewBox="20 30 420 250" style="max-width:100%;height:auto"></svg>` +
    `<div style="margin-top:10px">截线倾角 θ = <b id="ppdeg">45</b>° <input type="range" id="ppslider" min="15" max="80" step="1" value="45"></div>` +
    `<div style="margin-top:8px;font-size:14px;color:var(--muted)">同色 = 同位角（相等）；红＝θ，蓝＝180−θ</div>` +
    `</div>`
  )
}
function geomParallelScript(): string {
  return (
    "var svg=document.getElementById('ppsvg'),slider=document.getElementById('ppslider');" +
    "function draw(){" +
    "var th=parseFloat(slider.value)*Math.PI/180;" +
    "document.getElementById('ppdeg').textContent=slider.value;" +
    "var P2x=150,P2y=230,P1x=150+150*(Math.cos(th)/Math.sin(th)),P1y=80;" +
    "var dx=Math.cos(th),dy=-Math.sin(th);" +
    "var ax=P2x-260*dx,ay=P2y-260*dy,bx=P1x+260*dx,by=P1y+260*dy;" +
    "var s='';" +
    "s+='<line x1=\"30\" y1=\"80\" x2=\"440\" y2=\"80\" stroke=\"#345\" stroke-width=\"2\"/><text x=\"446\" y=\"84\" font-size=\"15\">a</text>';" +
    "s+='<line x1=\"30\" y1=\"230\" x2=\"440\" y2=\"230\" stroke=\"#345\" stroke-width=\"2\"/><text x=\"446\" y=\"234\" font-size=\"15\">b</text>';" +
    "s+='<line x1=\"'+ax.toFixed(1)+'\" y1=\"'+ay.toFixed(1)+'\" x2=\"'+bx.toFixed(1)+'\" y2=\"'+by.toFixed(1)+'\" stroke=\"#888\" stroke-width=\"2\"/>';" +
    "function ang(x,y,a0,a1,label,color){" +
    "var r=24,rad=function(d){return d*Math.PI/180};" +
    "var x0=x+r*Math.cos(rad(a0)),y0=y-r*Math.sin(rad(a0)),x1=x+r*Math.cos(rad(a1)),y1=y-r*Math.sin(rad(a1));" +
    "var deg=slider.value;" +
    "s+='<path d=\"M'+x0.toFixed(1)+','+y0.toFixed(1)+' A'+r+','+r+' 0 0 0 '+x1.toFixed(1)+','+y1.toFixed(1)+'\" fill=\"none\" stroke=\"'+color+'\" stroke-width=\"2.5\"/>';" +
    "var mid=(a0+a1)/2;" +
    "s+='<text x=\"'+(x+36*Math.cos(rad(mid))-8).toFixed(1)+'\" y=\"'+(y-36*Math.sin(rad(mid))+4).toFixed(1)+'\" font-size=\"14\" fill=\"'+color+'\" font-weight=\"600\">'+label+'='+deg+'°</text>'}" +
    "function angC(x,y,a0,a1,label,color){" +
    "var r=24,rad=function(d){return d*Math.PI/180},deg=180-parseFloat(slider.value);" +
    "var x0=x+r*Math.cos(rad(a0)),y0=y-r*Math.sin(rad(a0)),x1=x+r*Math.cos(rad(a1)),y1=y-r*Math.sin(rad(a1));" +
    "s+='<path d=\"M'+x0.toFixed(1)+','+y0.toFixed(1)+' A'+r+','+r+' 0 0 0 '+x1.toFixed(1)+','+y1.toFixed(1)+'\" fill=\"none\" stroke=\"'+color+'\" stroke-width=\"2.5\"/>';" +
    "var mid=(a0+a1)/2;" +
    "s+='<text x=\"'+(x+36*Math.cos(rad(mid))-8).toFixed(1)+'\" y=\"'+(y-36*Math.sin(rad(mid))+4).toFixed(1)+'\" font-size=\"14\" fill=\"'+color+'\" font-weight=\"600\">'+label+'='+deg+'°</text>'}" +
    "var t=parseFloat(slider.value),c1='#e5534b',c2='#4f7cff',c3='#2ea44f',c4='#a05a2c';" +
    "var sec=[[2,t],[t,180],[180,180+t],[180+t,358]];" +
    "var colors=[c1,c2,c3,c4];" +
    "function marks(x,y,base){" +
    "ang(x,y,sec[0][0],sec[0][1],'∠'+(base+1),colors[0]);" +
    "angC(x,y,sec[1][0],sec[1][1],'∠'+(base+2),colors[1]);" +
    "ang(x,y,sec[2][0],sec[2][1],'∠'+(base+3),colors[2]);" +
    "angC(x,y,sec[3][0],sec[3][1],'∠'+(base+4),colors[3])}" +
    "marks(P1x,P1y,0);marks(P2x,P2y,4);" +
    "svg.innerHTML=s}" +
    "slider.oninput=draw;draw()"
  )
}

/** ⑥ fraction：分数可视化（饼图/条形，可双分数对比，附约分与百分数）。 */
const fraction: DemoRenderer = (p) => {
  const den = optInt(p, "denominator", NaN, 1, 100)
  const num = optInt(p, "numerator", NaN, 0, 100)
  if (!Number.isFinite(den) || !Number.isFinite(num)) throw new Error("numerator 与 denominator 必填")
  if (num > den) throw new Error(`numerator（${num}）不能大于 denominator（${den}）——演示真分数/假分数请分别展示或说明`)
  const shape = p.shape === "bar" ? "bar" : "pie"
  const num2Raw = p.numerator2
  const has2 = num2Raw != null && num2Raw !== ""
  let num2 = 0
  let den2 = 0
  if (has2) {
    den2 = optInt(p, "denominator2", NaN, 1, 100)
    num2 = optInt(p, "numerator2", NaN, 0, 100)
    if (!Number.isFinite(den2) || !Number.isFinite(num2)) throw new Error("对比模式下 numerator2 与 denominator2 必填")
    if (num2 > den2) throw new Error("numerator2 不能大于 denominator2")
  }
  const gcd = (x: number, y: number): number => (y ? gcd(y, x % y) : x)
  const fracText = (n: number, d: number) => {
    const g = gcd(n, d) || 1
    const sn = n / g
    const sd = d / g
    const pct = ((n / d) * 100).toFixed(d <= 10 ? 0 : 1)
    return `${n}/${d}${g > 1 ? `＝${sn}/${sd}` : ""}（${pct}%）`
  }
  const oneFrac = (n: number, d: number, label: string): string => {
    if (shape === "pie") {
      const R = 90
      const cx = 110
      const cy = 110
      let paths = ""
      for (let i = 0; i < d; i++) {
        const a0 = (i / d) * 2 * Math.PI - Math.PI / 2
        const a1 = ((i + 1) / d) * 2 * Math.PI - Math.PI / 2
        const x0 = cx + R * Math.cos(a0)
        const y0 = cy + R * Math.sin(a0)
        const x1 = cx + R * Math.cos(a1)
        const y1 = cy + R * Math.sin(a1)
        // d=1 整圆：单段弧首尾重合会退化，直接用 circle
        paths +=
          d === 1
            ? `<circle cx="${cx}" cy="${cy}" r="${R}" fill="${i < n ? "#4f7cff" : "#f0f2f6"}" stroke="#889"/>`
            : `<path d="M${cx},${cy} L${x0.toFixed(1)},${y0.toFixed(1)} A${R},${R} 0 ${a1 - a0 > Math.PI ? 1 : 0},1 ${x1.toFixed(1)},${y1.toFixed(1)} Z" fill="${i < n ? "#4f7cff" : "#f0f2f6"}" stroke="#889" stroke-width="1"/>`
      }
      return `<div style="text-align:center"><svg viewBox="0 0 220 220" style="max-width:200px">${paths}</svg><div style="font-size:20px;margin-top:4px"><b>${esc(label)}</b><br>${fracText(n, d)}</div></div>`
    }
    let cells = ""
    const w = 300 / d
    for (let i = 0; i < d; i++) {
      cells += `<rect x="${(i * w).toFixed(1)}" y="70" width="${w.toFixed(1)}" height="56" fill="${i < n ? "#4f7cff" : "#f0f2f6"}" stroke="#889"/>`
    }
    return `<div style="text-align:center"><svg viewBox="0 0 300 140" style="max-width:280px">${cells}</svg><div style="font-size:20px;margin-top:4px"><b>${esc(label)}</b><br>${fracText(n, d)}</div></div>`
  }
  let cmp = ""
  if (has2) {
    const l = num * den2
    const r = num2 * den
    const sign = l > r ? "＞" : l < r ? "＜" : "＝"
    cmp = `<div style="font-size:26px;text-align:center;font-weight:600" id="cmp">${num}/${den} ${sign} ${num2}/${den2}</div>`
  }
  const ctrl = (i: number, n: number, d: number): string =>
    `<div style="display:flex;gap:10px;align-items:center;justify-content:center;flex-wrap:wrap;margin-top:8px">` +
    `分子 <button class="ghost fnb" data-t="n${i}" data-d="-1" style="padding:4px 12px">−</button><b id="vn${i}">${n}</b>/<b id="vd${i}">${d}</b><button class="ghost fnb" data-t="n${i}" data-d="1" style="padding:4px 12px">＋</button>` +
    `　分母 <button class="ghost fdb" data-t="d${i}" data-d="-1" style="padding:4px 12px">−</button><button class="ghost fdb" data-t="d${i}" data-d="1" style="padding:4px 12px">＋</button></div>`
  const body =
    `<h1>分数可视化（可交互）</h1><div class="sub">${shape === "pie" ? "饼图分格" : "条形分格"}：涂色部分表示分数，点按钮改变分数</div>` +
    `<div class="card" style="display:flex;justify-content:space-around;align-items:center;flex-wrap:wrap;gap:12px">` +
    `<div id="f1">${oneFrac(num, den, has2 ? "第一个分数" : `${num}/${den}`)}${ctrl(1, num, den)}</div>` +
    (has2 ? `<div style="font-size:34px">vs</div><div id="f2">${oneFrac(num2, den2, "第二个分数")}${ctrl(2, num2, den2)}</div>` : "") +
    `</div>` +
    (has2 ? `<div class="card" id="cmpcard">${cmp}<div style="text-align:center;color:var(--muted);font-size:13px" id="cmptext">通分比较：${num}×${den2} 与 ${num2}×${den}（分母同为 ${den}×${den2}=${den * den2}）</div></div>` : "")
  const script =
    `var SHAPE=${JSON.stringify(shape)},N1=${num},D1=${den},N2=${num2},D2=${den2},HAS2=${has2};` +
    "function gcd(x,y){return y?gcd(y,x%y):x}" +
    "function ftext(n,d){var g=gcd(n,d)||1,pct=(n/d*100).toFixed(d<=10?0:1);return n+'/'+d+(g>1?'＝'+(n/g)+'/'+(d/g):'')+'（'+pct+'%）'}" +
    "function draw(n,d,label){" +
    "var inner='';" +
    "if(SHAPE==='pie'){var R=90,cx=110,cy=110;" +
    "for(var i=0;i<d;i++){var a0=i/d*2*Math.PI-Math.PI/2,a1=(i+1)/d*2*Math.PI-Math.PI/2;" +
    "if(d===1){inner='<circle cx=\"110\" cy=\"110\" r=\"90\" fill=\"'+(n>0?'#4f7cff':'#f0f2f6')+'\" stroke=\"#889\"/>';break}" +
    "inner+='<path d=\"M110,110 L'+(cx+R*Math.cos(a0)).toFixed(1)+','+(cy+R*Math.sin(a0)).toFixed(1)+' A90,90 0 '+((a1-a0)>Math.PI?1:0)+',1 '+(cx+R*Math.cos(a1)).toFixed(1)+','+(cy+R*Math.sin(a1)).toFixed(1)+' Z\" fill=\"'+(i<n?'#4f7cff':'#f0f2f6')+'\" stroke=\"#889\" stroke-width=\"1\"/>'}" +
    "return '<div style=\"text-align:center\"><svg viewBox=\"0 0 220 220\" style=\"max-width:200px\">'+inner+'</svg><div style=\"font-size:20px;margin-top:4px\"><b>'+label+'</b><br>'+ftext(n,d)+'</div></div>'}" +
    "var cells='';var w=300/d;" +
    "for(var i=0;i<d;i++){cells+='<rect x=\"'+(i*w).toFixed(1)+'\" y=\"70\" width=\"'+w.toFixed(1)+'\" height=\"56\" fill=\"'+(i<n?'#4f7cff':'#f0f2f6')+'\" stroke=\"#889\"/>'}" +
    "return '<div style=\"text-align:center\"><svg viewBox=\"0 0 300 140\" style=\"max-width:280px\">'+cells+'</svg><div style=\"font-size:20px;margin-top:4px\"><b>'+label+'</b><br>'+ftext(n,d)+'</div></div>'}" +
    "function renderAll(){" +
    "var l1=HAS2?'第一个分数':N1+'/'+D1;" +
    "document.getElementById('f1').innerHTML=draw(N1,D1,l1)+ctrl(1);" +
    "document.getElementById('vn1').textContent=N1;document.getElementById('vd1').textContent=D1;" +
    "if(HAS2){document.getElementById('f2').innerHTML=draw(N2,D2,'第二个分数')+ctrl(2);document.getElementById('vn2').textContent=N2;document.getElementById('vd2').textContent=D2;" +
    "var s=N1*D2>N2*D1?'＞':(N1*D2<N2*D1?'＜':'＝');" +
    "document.getElementById('cmp').textContent=N1+'/'+D1+' '+s+' '+N2+'/'+D2;" +
    "document.getElementById('cmptext').textContent='通分比较：'+N1+'×'+D2+' 与 '+N2+'×'+D1+'（分母同为 '+D1+'×'+D2+'＝'+(D1*D2)+'）'}" +
    "function ctrl(i){return '<div style=\"display:flex;gap:10px;align-items:center;justify-content:center;flex-wrap:wrap;margin-top:8px\">分子 <button class=\"ghost fnb\" data-t=\"n'+i+'\" data-d=\"-1\" style=\"padding:4px 12px\">−</button><b id=\"vn'+i+'\">'+(i===1?N1:N2)+'</b>/<b id=\"vd'+i+'\">'+(i===1?D1:D2)+'</b><button class=\"ghost fnb\" data-t=\"n'+i+'\" data-d=\"1\" style=\"padding:4px 12px\">＋</button>　分母 <button class=\"ghost fdb\" data-t=\"d'+i+'\" data-d=\"-1\" style=\"padding:4px 12px\">−</button><button class=\"ghost fdb\" data-t=\"d'+i+'\" data-d=\"1\" style=\"padding:4px 12px\">＋</button></div>'}" +
    "bind()}" +
    "function bind(){document.querySelectorAll('.fnb,.fdb').forEach(function(btn){btn.onclick=function(){var t=btn.getAttribute('data-t'),dv=parseInt(btn.getAttribute('data-d'),10);" +
    "if(t==='n1'){N1=Math.max(0,Math.min(D1,N1+dv))}if(t==='d1'){D1=Math.max(1,Math.min(24,D1+dv));N1=Math.min(N1,D1)}" +
    "if(t==='n2'){N2=Math.max(0,Math.min(D2,N2+dv))}if(t==='d2'){D2=Math.max(1,Math.min(24,D2+dv));N2=Math.min(N2,D2)}" +
    "renderAll()}})}" +
    "bind()"
  return demoPage("分数可视化", body, script)
}

/** ⑦ flashcards：记忆翻卡（3D 翻转动画 + 认识/需复习标记统计）。 */
const flashcards: DemoRenderer = (p) => {
  const raw = p.items
  if (!Array.isArray(raw) || !raw.length) throw new Error("items 不能为空（[{front, back}]）")
  if (raw.length > 60) throw new Error(`卡片过多（${raw.length}，上限 60）`)
  const items = raw.map((item, i) => {
    const it = item as Record<string, unknown>
    return { front: reqStr(it, "front", `第 ${i + 1} 张正面`), back: reqStr(it, "back", `第 ${i + 1} 张背面`, 1000) }
  })
  const title = optStr(p, "title", 100) || "记忆卡片"
  const cardHtml = items
    .map(
      (it, i) =>
        `<div class="flip" data-i="${i}"><div class="flip-in">` +
        `<div class="face"><span>${esc(it.front)}</span><span style="font-size:11px;color:var(--muted)">点击翻面</span></div>` +
        `<div class="face back"><span>${esc(it.back)}</span><span style="display:flex;gap:8px">` +
        `<button class="ghost mark" data-i="${i}" data-s="1" style="padding:5px 12px;font-size:13px">认识 ✓</button>` +
        `<button class="ghost mark" data-i="${i}" data-s="2" style="padding:5px 12px;font-size:13px;background:#fdecea">需复习</button>` +
        `</span></div></div></div>`,
    )
    .join("")
  const body =
    `<h1>${esc(title)}</h1><div class="sub">点击卡片 3D 翻面；翻面后标记「认识 / 需复习」</div>` +
    `<div class="card" style="display:flex;gap:16px;align-items:center;flex-wrap:wrap">已标记：<b class="ok" id="okc">0</b> 认识 · <b class="bad" id="badc">0</b> 需复习 · <span id="leftc" style="color:var(--muted)"></span>` +
    `<button class="ghost" id="reset" style="margin-left:auto;padding:6px 14px">全部重置</button></div>` +
    `<div id="grid" style="display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));gap:10px">${cardHtml}</div>`
  const script =
    `var N=${items.length};` +
    "var st=Array.apply(null,Array(N)).map(function(){return 0});" +
    "document.querySelectorAll('.flip').forEach(function(card){card.onclick=function(ev){" +
    "if(ev.target.closest('.mark'))return;card.classList.toggle('flipped')}});" +
    "document.querySelectorAll('.mark').forEach(function(btn){btn.onclick=function(ev){ev.stopPropagation();st[parseInt(btn.getAttribute('data-i'),10)]=parseInt(btn.getAttribute('data-s'),10);stat()}});" +
    "function stat(){var o=0,x=0;st.forEach(function(s){if(s===1)o++;if(s===2)x++});" +
    "document.getElementById('okc').textContent=o;document.getElementById('badc').textContent=x;" +
    "document.getElementById('leftc').textContent=(N-o-x)+' 张未标记'}" +
    "document.getElementById('reset').onclick=function(){st=st.map(function(){return 0});stat()};" +
    "stat()"
  return demoPage(title, body, script)
}

/** ⑧ reference：内置公式与定理速查库（索引 / 按学科主题过滤 / 单条大卡）。 */
const reference: DemoRenderer = (p) => {
  const id = optStr(p, "id", 80)
  const subject = optStr(p, "subject", 40)
  const topic = optStr(p, "topic", 60)
  const entryCard = (e: (typeof REF_ENTRIES)[number], big: boolean): string => {
    const formula = e.formula
      ? `<div style="font-family:'Cascadia Mono',Consolas,monospace;font-size:${big ? 19 : 15}px;line-height:1.7;background:#f0f4ff;border-radius:10px;padding:12px 14px;margin:10px 0">${e.formula.replace(/\n/g, "<br>")}</div>`
      : ""
    const line = (label: string, v?: string) => (v ? `<div style="margin-top:8px"><b>${label}</b>：${esc(v)}</div>` : "")
    return (
      `<div class="card"><div style="display:flex;align-items:baseline;gap:8px;flex-wrap:wrap">` +
      `<span style="font-size:${big ? 20 : 17}px;font-weight:700">${esc(e.name)}</span>` +
      `<span style="color:var(--muted);font-size:12px">${esc(e.subject)} · ${esc(e.topic)}${e.stage === "primary" ? " · 小学" : e.stage === "secondary" ? " · 中学" : ""}</span></div>` +
      formula +
      line("表述", e.statement) +
      line("证明思路", e.proof) +
      line("要点与易错", e.notes) +
      (e.svg ? `<div style="text-align:center;margin-top:10px">${e.svg}</div>` : "") +
      `<div style="color:var(--muted);font-size:11px;margin-top:8px">id: ${e.id}</div></div>`
    )
  }
  if (id) {
    const e = REF_ENTRIES.find((x) => x.id === id)
    if (!e) throw new Error(`未找到 id 为 "${id}" 的条目（可用清单：不传参数渲染索引页查看）`)
    return demoPage(e.name, `<h1>${esc(e.name)}</h1><div class="sub">内置知识速查</div>${entryCard(e, true)}`)
  }
  const matched = REF_ENTRIES.filter(
    (e) => (!subject || e.subject === subject) && (!topic || e.topic.includes(topic) || e.name.includes(topic)),
  )
  if (!matched.length) {
    const subjects = [...new Set(REF_ENTRIES.map((e) => e.subject))].join("、")
    throw new Error(`没有匹配的条目（subject=${subject || "未传"}, topic=${topic || "未传"}）。可用学科：${subjects}；不传参数渲染索引页查看全部条目`)
  }
  if (!subject && !topic) {
    // 索引页：按学科分组的条目清单（不展开正文，作目录/总览用）
    const groups = refIndexBySubject(matched)
      .map(
        ([subj, list]) =>
          `<div class="card"><h1 style="font-size:16px">${esc(subj)}</h1>` +
          list
            .map((e) => `<div style="padding:5px 0;border-bottom:1px dashed #e5e9f0;font-size:14px"><b>${esc(e.name)}</b>（${esc(e.topic)}${e.stage === "primary" ? "·小学" : e.stage === "secondary" ? "·中学" : ""}）<span style="color:var(--muted)">id: ${e.id}</span></div>`)
            .join("") +
          `</div>`,
      )
      .join("")
    return demoPage(
      "公式与定理速查 · 目录",
      `<h1>公式与定理速查</h1><div class="sub">共 ${matched.length} 条 · 传 subject/topic 过滤查看卡片，或传 id 看单条</div>${groups}`,
    )
  }
  return demoPage(
    `${subject || "知识"}速查`,
    `<h1>${esc(subject || matched[0].subject)}速查${topic ? ` · ${esc(topic)}` : ""}</h1><div class="sub">共 ${matched.length} 条</div>${matched.map((e) => entryCard(e, matched.length === 1)).join("")}`,
  )
}

/* ---------- 模板注册表 ---------- */

export const DEMO_TEMPLATES: DemoTemplate[] = [
  {
    id: "quiz",
    usage:
      '通用可作答练习卷（出题练习/错题重做首选）：{ title?, studentName?, questions: [{q 题干, options? 2~6 个选项(单选), answer 答案(有 options 须为选项原文), explain? 批改后展示的解析}] }（≤50 题）——自动批改、判对错、展示解析与得分',
    render: quiz,
  },
  {
    id: "mental_math",
    usage:
      '口算闯关（计时挑战，页面内随机出题）：{ ops? ["add","sub","mul","div"]（默认加减）, digits? 1|2|3 位数(默认 2), count? 题量(默认 20, ≤100), seconds? 秒(默认 60), studentName?, title? }',
    render: mentalMath,
  },
  {
    id: "column",
    usage:
      '竖式计算演示（演示非练习，进位/借位/部分积自动标注）：{ a, b 非负整数(a≥b 减法), op "add"|"sub"|"mul" }',
    render: column,
  },
  {
    id: "function_graph",
    usage:
      '函数图像（自动坐标轴/网格/关键点标注）：{ fn "linear"|"quadratic"|"inverse"|"absolute", a?, b?, c?, xMin?, xMax? }——y=ax+b / y=ax²+bx+c / y=a/x / y=a|x+b|+c（linear 标轴交点、quadratic/absolute 标顶点）',
    render: functionGraph,
  },
  {
    id: "geometry",
    usage: '几何示意图（静态标注图）：{ preset "pythagorean" 勾股定理 | "triangle_height" 三角形的高 | "circle" 圆的要素 | "parallel" 平行线截角 }',
    render: geometry,
  },
  {
    id: "fraction",
    usage:
      '分数可视化（饼图/条形分格，涂色表示分数，附约分与百分数；可传第二分数对比大小）：{ numerator, denominator, shape? "pie"|"bar"(默认 pie), numerator2?, denominator2? }',
    render: fraction,
  },
  {
    id: "flashcards",
    usage: '记忆翻卡（点击翻面、标记认识/需复习并统计——拼音/单词/公式/古诗通用）：{ title?, items: [{front, back}] }（≤60 张）',
    render: flashcards,
  },
  {
    id: "reference",
    usage:
      '内置公式与定理速查库（静态知识、零 token 展示）：{ subject? 学科（数学/物理/化学/英语）, topic? 主题或名称子串（如 圆、浮力、方程式、时态）, id? 单条 }——不传 = 索引目录页；传过滤 = 该范围速查卡（公式大字+要点易错，部分定理带图示与证明思路）；讲公式/定理/给学生做速查表时用。覆盖：小学数学（平面/立体图形、运算律、单位换算）、中学数学（乘法公式、求根与韦达、均值不等式、函数、勾股/内角和/圆周角、解三角形、统计）、物理（速度密度、压强、浮力、功功率、杠杆、欧姆/电功率、热学、机械能）、化学（摩尔、溶液、常考方程式、质量守恒）、英语（五大时态表）',
    render: reference,
  },
]

export const DEMO_TEMPLATE_IDS = DEMO_TEMPLATES.map((t) => t.id)
