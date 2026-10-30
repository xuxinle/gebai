// 册府（CeFu）界面逻辑。改这个文件保存即生效（换树 + 重装逻辑，原子替换）。
//
// 分工很清楚：
//   - 数据在 C++（SQLite + 倒排索引），这里只有**界面状态**（当前选中的册 / 词条 / 检索词）
//   - 每次拿到的是 C++ 给的**快照**，回写一律走 app.cefu.*（不要试图在 JS 里攒一份可变的库）
//
// ## 为什么要整体包一个 IIFE
//
// 热重载重装逻辑时是**在同一个全局作用域里再执行一遍本文件**。顶层 `let state` 会变成
// "redeclaration of 'state'"，重装直接失败（旧逻辑继续跑，改动看起来"没生效"）。
// 包成 `(function(){ 'use strict'; ... })()` 之后，全部变量都在函数作用域里，
// 重装就是一次全新的执行——这是热重载对脚本的唯一硬约束。

(function () {
  'use strict';

let state = {
  category: '',      // 当前册（空 = 全部）
  query: '',         // 检索词
  filter: '',        // 本册内过滤
  entries: [],       // 当前列表（快照）
  selected: null,    // 当前词条全文
  reviewItems: [],   // 整理巡检结果
}

/* ══════════ 工具 ══════════ */

function daysAgo(ms) {
  if (!ms) return '—'
  const d = Math.floor((Date.now() - ms) / 86400000)
  if (d <= 0) return '今天'
  return d + ' 天前'
}

function confBadge(c) {
  if (c === 'verified') return '已确认'
  if (c === 'stale') return '已过期'
  return '待确认'
}

function setStatus(text) {
  $('#status').set({ text })
}

/* ══════════ 渲染：分类 ══════════ */

function renderCategories() {
  const box = $('#cat-list')
  box.clear()

  // 「全部」是一个真实入口：不知道在哪册时要能一次看全
  const all = box.append('Button', { label: '全部词条', variant: state.category === '' ? 'primary' : 'ghost' })
  all.on('click', () => {
    state.category = ''
    renderAll()
  })

  const cats = app.cefu.categories()
  for (const c of cats) {
    const btn = box.append('Button', {
      label: c.name + '（' + c.count + '）',
      variant: state.category === c.name ? 'primary' : 'ghost',
    })
    btn.on('click', () => {
      state.category = c.name
      state.query = ''
      $('#search').set({ value: '' })
      renderAll()
    })
  }
}

/* ══════════ 渲染：列表 ══════════ */

function renderList() {
  const box = $('#entry-list')
  box.clear()

  let items = state.entries
  const f = state.filter.trim().toLowerCase()
  if (f) {
    items = items.filter((e) =>
      (e.title || '').toLowerCase().includes(f) ||
      (e.id || '').toLowerCase().includes(f) ||
      (e.aliases || []).some((a) => a.toLowerCase().includes(f)) ||
      (e.key_line || '').toLowerCase().includes(f))
  }

  const label = state.query
    ? '检索「' + state.query + '」· ' + items.length + ' 条'
    : (state.category ? state.category : '全部词条') + ' · ' + items.length + ' 条'
  $('#list-title').set({ text: label })

  if (items.length === 0) {
    box.append('Text', { text: '没有匹配的词条', text_color: '#8a8f98', font_size: 12 })
    return
  }

  for (const e of items) {
    // 每条卡片：标题 + 一句话结论 + 状态。
    // 给"一句话结论"是因为运维知识最有用的是"第一步该做什么"——列表里就能看出来才叫工具书。
    //
    // 卡片结构：标题行（Text 左对齐 + 右侧一个「›」按钮承担点击）。
    //
    // 为什么不在整张卡片上绑 click：Panel **不参与命中测试**，在它上面 on('click') 永不触发
    // （实测：input.mouse 派发了、日志里也没有 openEntry）——可点区必须是可交互元素。
    // 为什么不用整宽 Button 当标题：按钮内部固定居中布局，text_align 对它无效，
    // 标题会被挤到中间（书页的标题不该居中）。
    const card = box.append('Column', { gap: 2, padding: 7, surface: 'alt', radius: 6 })
    const title_row = card.append('Row', { gap: 4, align_items: 'center' })
    title_row.append('Text', { text: e.title, font_size: 14, font_weight: 'semibold', grow: true })
    const hit = title_row.append('Button', { label: '›', variant: 'ghost', scale: 'small' })
    hit.on('click', () => openEntry(e.id))
    if (e.key_line) {
      card.append('Text', { text: e.key_line, font_size: 12, text_color: '#8a8f98', wrap: true })
    }
    const meta = confBadge(e.confidence) + ' · 用 ' + (e.hits || 0) + ' 次 · ' + daysAgo(e.updated_at)
    card.append('Text', { text: meta, font_size: 11, text_color: '#6b7280' })
  }
}

/* ══════════ 渲染：正文 ══════════ */

function section(parent, title, lines) {
  if (!lines || lines.length === 0) return
  const box = parent.append('Column', { gap: 3 })
  box.append('Text', { text: title, font_size: 12, font_weight: 'semibold', text_color: '#8a8f98' })
  lines.forEach((line, i) => {
    // steps 有序：序号是必要信息（"第一步查什么"和"第三步查什么"不一样）
    const prefix = title === '处理步骤' ? (i + 1) + '. ' : '· '
    box.append('Text', { text: prefix + line, font_size: 13, wrap: true })
  })
}

function renderDetail() {
  const body = $('#detail-body')
  body.clear()

  const e = state.selected
  if (!e) {
    $('#detail-title').set({ text: '从左边选一个词条' })
    $('#detail-meta').set({ text: '' })
    return
  }

  $('#detail-title').set({ text: e.title })
  $('#detail-meta').set({
    text: e.category + ' · ' + e.id + ' · ' + confBadge(e.confidence) +
      ' · 用 ' + (e.hits || 0) + ' 次 · 更新于 ' + daysAgo(e.updated_at) +
      (e.owner ? ' · 负责人 ' + e.owner : ''),
  })

  if (e.aliases && e.aliases.length) {
    body.append('Text', { text: '也叫：' + e.aliases.join('、'), font_size: 12, text_color: '#8a8f98', wrap: true })
  }
  if (e.symptom) {
    const box = body.append('Column', { gap: 3 })
    box.append('Text', { text: '现象', font_size: 12, font_weight: 'semibold', text_color: '#8a8f98' })
    box.append('Text', { text: e.symptom, font_size: 13, wrap: true })
  }
  section(body, '可能原因', e.causes)
  section(body, '处理步骤', e.steps)

  if (e.commands && e.commands.length) {
    const box = body.append('Column', { gap: 3 })
    box.append('Text', { text: '命令', font_size: 12, font_weight: 'semibold', text_color: '#8a8f98' })
    e.commands.forEach((cmd) => {
      // 命令用等宽字体：复制粘贴时最忌讳把 l 看成 1
      box.append('Text', { text: '$ ' + cmd, font_size: 12, monospace: true, wrap: true })
    })
  }
  section(body, '硬事实', e.facts)
  if (e.links && e.links.length) {
    body.append('Text', { text: '相关：' + e.links.join('、'), font_size: 12, text_color: '#8a8f98', wrap: true })
  }
  // 出处必显示：无法溯源的知识等于谣言
  body.append('Text', {
    text: '出处：' + (e.source_kind || '—') + ' / ' + (e.source_ref || '—') +
      ' · 作用域 ' + (e.scope || '—'),
    font_size: 11, text_color: '#6b7280', wrap: true,
  })
}

/* ══════════ 数据动作 ══════════ */

function openEntry(id) {
  try {
    state.selected = app.cefu.get(id)
    renderDetail()
    setStatus('已打开 ' + id)
  } catch (err) {
    setStatus('打开失败：' + err)
  }
}

function refreshList() {
  try {
    if (state.query) {
      state.entries = app.cefu.search(state.query, 20)
      setStatus('检索「' + state.query + '」：' + state.entries.length + ' 条命中')
    } else {
      state.entries = app.cefu.list(state.category)
      setStatus((state.category || '全部') + '：' + state.entries.length + ' 条')
    }
    renderList()
  } catch (err) {
    setStatus('读取出错：' + err)
  }
}

function refreshStats() {
  try {
    const s = app.cefu.stats()
    $('#stats').set({
      text: s.entries + ' 条 · ' + s.categories + ' 册 · 待确认 ' + s.inferred +
        ' · 过期 ' + s.stale + ' · 待整理 ' + s.review_items,
    })
    if (s.error) setStatus('注意：' + s.error)
  } catch (err) {
    setStatus('统计失败：' + err)
  }
}

function renderAll() {
  renderCategories()
  refreshList()
  refreshStats()
}

/* ══════════ 事件 ══════════ */

$('#do-search').on('click', () => {
  state.query = $('#search').value || ''
  state.category = ''
  refreshList()
})

// 回车即检索：检索框里按回车是肌肉记忆，要求去点按钮是没必要的摩擦
$('#search').on('keydown', () => {
  state.query = $('#search').value || ''
  state.category = ''
  refreshList()
})

$('#clear-search').on('click', () => {
  state.query = ''
  state.filter = ''
  $('#search').set({ value: '' })
  $('#filter').set({ value: '' })
  renderAll()
})

$('#filter').on('input', () => {
  state.filter = $('#filter').value || ''
  renderList()
})

$('#review').on('click', () => {
  let items = []
  try {
    items = app.cefu.review()
  } catch (err) {
    setStatus('整理巡检失败：' + err)
    return
  }
  state.reviewItems = items
  const body = $('#detail-body')
  body.clear()
  $('#detail-title').set({ text: '整理建议（' + items.length + ' 项）' })
  $('#detail-meta').set({ text: '知识库不做整理，三个月后会变成垃圾场——这些是当前该做的动作' })

  if (items.length === 0) {
    body.append('Text', { text: '目前没有需要整理的事项。', font_size: 13 })
    return
  }
  const kindName = {
    merge: '重复条目',
    stale: '过期未确认',
    unused: '从未被用到',
    missingAlias: '缺别名',
    categoryDrift: '分类需统一',
  }
  for (const it of items) {
    const card = body.append('Column', { gap: 3, padding: 8, surface: 'alt', radius: 6 })
    card.append('Text', { text: (kindName[it.kind] || it.kind) + (it.ids.length ? '：' + it.ids.join(' / ') : ''), font_size: 13, font_weight: 'semibold' })
    card.append('Text', { text: it.reason, font_size: 12, text_color: '#8a8f98', wrap: true })
    card.append('Text', { text: '→ ' + it.action, font_size: 12, wrap: true })
    if (it.ids.length === 1) {
      const btn = card.append('Button', { label: '去处理', variant: 'ghost', scale: 'small' })
      btn.on('click', () => openEntry(it.ids[0]))
    }
  }
  setStatus('整理巡检：' + items.length + ' 项')
})

$('#confirm').on('click', () => {
  if (!state.selected) {
    setStatus('先选一个词条')
    return
  }
  try {
    app.cefu.confirm(state.selected.id)
    state.selected = app.cefu.get(state.selected.id)
    renderDetail()
    refreshStats()
    setStatus('已标为确认：' + state.selected.id)
  } catch (err) {
    setStatus('确认失败：' + err)
  }
})

/* ══════════ 启动 ══════════ */

renderAll()
setStatus('册府已就绪（' + app.cefu.stats().db_path + '）')

})()
