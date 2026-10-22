// 相期 · 界面逻辑（零 C++ UI 代码）
//
// ## 三种卡片，三种"等你"的方式
//
//   notice  提示：只要你知道。读完点「知道了」。
//   choice  选择：必须给一个答案。点选项 → 答复回给 agent。
//   todo    待办：等你去做（可能很久）。做完点「完成」，或标「先放着」。
//
// 三者的差别是**能否被忽略**：
//   - notice 可以「全部忽略」批量清掉（它不承载决策）
//   - choice/todo 不能忽略——那两类的价值就是"必须有结果"，
//     允许忽略等于允许 agent 的请求静默消失（比不弹出来更糟）。
//
// ## 宿主能力（C++ 侧 expose，见 main.cpp）
//
//   app.cards.list()                  → [{id, kind, title, body, options?, …}, …]
//   app.cards.answer(id, value)       → 新列表（选了一项 / 点了知道了 / 标了完成）
//   app.cards.dismiss_all()           → 新列表（只清 notice）
//   app.cards.poll()                  → {cards, peer}；peer 是连接状态
//
// 卡片从哪来、答复往哪去，都在 C++（文件/管道/套接字）。JS 只管呈现与转发——
// 这与 xmlapp 同一条分工：数据留 C++，逻辑留 JS。

(function () {
  'use strict';

  // ── 状态 ────────────────────────────────────────────────────────────────
  let cards = [];
  let peer = { connected: false, name: '' };
  /** 当前是否为轻提示形态（窗口缩小、只显示最新一条）。 */
  let compact = false;
  /** 用户主动展开过（此后不再自动缩回——自动收起是打断）。 */
  let userExpanded = false;
  /** 界面需要重画（所有变更置脏，由 tick 统一渲染）。 */
  let dirty = false;

  // ── 工具 ────────────────────────────────────────────────────────────────
  //
  // 卡片是"重建式渲染"：每次数据变了就整片重建。为什么不用增量更新：
  // 三类卡片的内部结构不同（有选项/有进度/有截止），增量更新要维护三套 diff。
  // 而卡片数量是个位数量级，重建成本可忽略——简单可靠胜过省那点开销。
  // （注意重建时 `.on()` 内置去重，不会像老代码那样累积重复绑定。）

  function badgeText() {
    const n = cards.length;
    if (n === 0) return '暂无待办';
    return n + ' 件等你处理';
  }

  /** 一张卡片的语义色：三类各有自己的调性。 */
  function toneOf(kind) {
    if (kind === 'choice') return { icon: 'info', color: '#2563eb', label: '需要你定' };
    if (kind === 'todo') return { icon: 'clock', color: '#d97706', label: '等你去做' };
    return { icon: 'message', color: '#6b7280', label: '有事告诉你' };
  }

  // ── 渲染 ────────────────────────────────────────────────────────────────

  // 渲染重入闸。
  //
  // 为什么必须要：`render()` 内部会解绑/重建元素，而宿主在结构变化期间可能同步派发事件
  // → 回调里又调 `render()` → 无限递归，最终进程 std::bad_alloc 崩掉
  // （实测症状："已答复"日志连打两次后进程消失，且无任何 JS 异常）。
  // 重入时直接返回：正在进行的这一轮会把最新状态画出来。
  let rendering = false;

  function render() {
    if (rendering) return;
    rendering = true;
    try {
      renderInner();
    } finally {
      rendering = false;
    }
  }

  function renderInner() {
    const list = $('#cards');
    if (!list) return;

    // 轻提示形态：窗口已缩成小尺寸，只显示最新一条（不画主界面）。
    // 这样 notice 类不会把整个工作面板拉出来——只是通知一声。
    if (compact) {
      renderCompact();
      return;
    }

    // ## 自然的写法：清空 + 整片重建
    //
    // 早期版本这里是"槽位复用"（结构只建一次、之后只 set 属性），
    // 那是因为框架当时有两个缺陷：**每次事件都重新解析选择器** + **死绑定不回收**，
    // 连续重建能把进程压到 std::bad_alloc。
    //
    // 那两条已在框架层修掉（选择器缓存 + prune_dead_bindings），
    // 所以这里回到最直白的写法——应用不该为框架缺陷长期背适配。
    list.clear();
    cards.forEach(function (card, i) {
      renderCard(card, i);
    });

    // 内容重建后不必手动调滚动偏移：`scroll_to` 每次都会把现有 offset
    // 夹到新的 `[0, max_scroll]` 里，超界会自动收回来。
    // 保留用户的阅读位置（不无脑滚到底）——卡片按到达顺序排，
    // 正在看上面那张的人不该被抽走位置。

    // 徽标
    $('#badge').set({ text: badgeText() });

    // 形态互斥：展开态下隐藏 compact 区（标题栏一直可见）
    $('#compact').set({ visible: 'false' });

    // 空状态与卡片区互斥（都用 grow，同时可见会各占一半高度）
    $('#empty').set({ visible: cards.length === 0 ? 'true' : 'false' });
    $('#cards').set({ visible: cards.length === 0 ? 'false' : 'true' });

    // 底部连接状态：如实显示（断线与"agent 很安静"必须能区分）
    paintLinkState($('#link-text'), $('#link-icon'));
  }

  /**
   * 该用哪种形态？——**这是"不必每次弹出主界面"的判定处**。
   *
   * 规则（按用户视角倒推）：
   * - 只有 notice（纯告知）且**用户没在看**时才用轻提示；
   * - 一旦有 choice/todo，就必须给足界面（那两类要求用户做出动作，
   *   挤在 360×200 的小窗里连选项都放不下）；
   * - 用户主动展开过之后，不再自动缩回去（人已经决定要看了，
   *   自动收起是打断——那种"手一动就没了"的界面最烦人）。
   */
  function desiredCompact() {
    if (userExpanded) return false;
    if (cards.length === 0) return false;
    // 有任何需要决策/动手的卡片 → 必须展开
    for (const card of cards) {
      if (card.kind === 'choice' || card.kind === 'todo') return false;
    }
    return true;   // 全是 notice
  }

  // ── 轻提示形态：一屏只说一件事 ──────────────────────────────────────────

  function renderCompact() {
    // 主区域全隐：这一形态下只剩 compact 区
    $('#cards').set({ visible: 'false' });
    $('#empty').set({ visible: 'false' });
    // ⚠ 标题栏**不隐藏**：徽标、连接状态、「全部忽略」都住在那里（见 main.cpp）。
    // 以前这里还隐 `#header`/`#footer`，那是 XML 自带的两条冗余带；
    // 现在它们没了，隐藏标题栏等于把连接状态也一起藏了。
    $('#compact').set({ visible: 'true' });

    // 徽标在**两种形态下都要更新**：它住在常驻标题栏里，
    // 只在主界面分支里写会让轻提示形态停在“暂无待办”——
    // 而同一屏下面明明写着“另 5 件”，两处自相矛盾。
    $('#badge').set({ text: badgeText() });

    const card = cards[0];
    if (!card) {
      // 没有卡片时不该停在 compact（否则窗口小小一片、什么都没）
      set_compact(false);
      return;
    }
    const tone = toneOf(card.kind);
    $('#compact-icon').set({ icon: tone.icon, text_color: tone.color });
    $('#compact-kind').set({ text: tone.label, text_color: tone.color });
    $('#compact-title').set({ text: card.title || '（无标题）' });
    // 正文也是 markdown（与主界面同一口径——同一条内容两种形态不应长得不一样）
    if (card.body) {
      $('#compact-body').set({ markdown: card.body, visible: 'true' });
    } else {
      $('#compact-body').set({ visible: 'false' });
    }
    // 还有几件在排队（让用户知道"叫我的不止这一件"）
    $('#compact-more').set({ text: cards.length > 1 ? '另 ' + (cards.length - 1) + ' 件' : '' });

    // 「知道了」只对 notice 有意义（choice/todo 必须给结果 → 引导去展开）
    const isNotice = card.kind === 'notice';
    $('#compact-ok').set({ visible: isNotice ? 'true' : 'false' });
    $('#compact-expand').set({ label: isNotice ? '展开' : '去处理' });

    // 连接状态：这一形态把主界面底部栏整个隐了，所以必需在 compact 区里有一份。
    // 断线时用户必须能看见——否则他会以为 agent 还在盯着。
    paintLinkState($('#compact-link'), $('#compact-link-icon'));
  }

  /**
   * 绘制连接状态：**只用一个图标**。
   *
   * 为何不写文字：常驻面板的横向空间很贵，而“已连接”三个字占的位置
   * 比图标大好几倍，信息量却一样——绿色链子 = 通着，红色警告 = 断了。
   * 具体原因（如“对端已关闭连接”）保留在 `title` 属性里，悬停可见。
   *
   * 为何抽出来：主界面与轻提示各有一份元素，但**判定逻辑必须一致**——
   * 两份各写一遍，早晚会出现“一种形态说已连接、另一种说未连接”。
   */
  function paintLinkState(textHandle, iconHandle) {
    if (peer && peer.connected) {
      iconHandle.set({ icon: 'link', text_color: '#059669' });
      // 文字元素**隐藏**而不是留空：空文本仍占一行行高，会把标题栏顶高。
      textHandle.set({ text: '', visible: 'false' });
    } else {
      iconHandle.set({ icon: 'warning', text_color: '#dc2626' });
      textHandle.set({ text: '', visible: 'false' });
    }
    // 具体状态文字改挂到图标的 hint 上（悬停可见），不占横向空间。
    const detail = (peer && peer.connected)
        ? ('已连接' + (peer.name ? ' · ' + peer.name : ''))
        : (peer && peer.detail ? peer.detail : '未连接');
    iconHandle.set({ hint: detail });
  }

  /**
   * 切换形态（同时把窗口改成对应尺寸）。
   *
   * 为何窗口尺寸也变：轻提示的全部意义就是"不占地方"。
   * 若窗口仍是主界面那么大，只是内容变少，那和"弹主界面"没区别。
   */
  function set_compact(next) {
    if (compact === next) return;
    compact = next;
    try {
      app.window.set_size(compact ? 360 : 460, compact ? 200 : 620);
    } catch (e) {
      log('相期：切换窗口尺寸失败 — ' + e);
    }
    // 切回展开态：只需收起 compact 区（标题栏一直可见）
    if (!compact) {
      $('#compact').set({ visible: 'false' });
    }
    render();
  }

  /** 事件回调里只置脏（不在事件栈上改结构）。 */
  function requestRender() {
    dirty = true;
  }

  // ── 单张卡片：三类的内部结构不同，各建各的 ──────────────────────────────

  function renderCard(card, index) {
    const tone = toneOf(card.kind);
    const sid = 'card-' + index;
    const list = $('#cards');

    list.append('Column', {
      id: sid,
      gap: '8',
      padding: '12',
      surface: 'alt',
      radius: '8',
      border_width: '1',
      border_color: '#e5e7eb',
    });

    // ① 抬头：图标 + 类别 + 来源
    const headId = sid + '-head';
    $('#' + sid).append('Row', { id: headId, gap: '8' });
    $('#' + headId).append('Icon', { icon: tone.icon, size: '16', text_color: tone.color });
    $('#' + headId).append('Text', { size: '12', text: tone.label, text_color: tone.color });
    $('#' + headId).append('Spacer', { grow: 'true' });
    if (card.from) {
      $('#' + headId).append('Text', { size: '11', text: card.from, text_color: '#9ca3af' });
    }

    // ② 标题与正文
    $('#' + sid).append('Text', { size: '15', text: card.title || '（无标题）' });
    if (card.body) {
      // 正文用 **MarkdownView** 而不是 Text：大模型生成的内容天然是 markdown
      //（列表、代码块、强调、表格）。当普通文本画的话，`**粗**`、`- 项`、
      // ```code``` 都会原样露出来。
      //
      // 注：`max_height` 给一个上限，否则一条长回答会把整张卡撑到很高、
      // 把下面全挤出去。超出部分由 MarkdownView 自己滚（它有 scroll 属性）。
      $('#' + sid).append('MarkdownView', {
        id: sid + '-body',
        markdown: card.body,
        max_height: '220',
      });
    }

    // ③ 交互区：按类型分派
    const actId = sid + '-act';
    $('#' + sid).append('Row', { id: actId, gap: '8', padding_y: '4' });

    if (card.kind === 'choice' && card.options) {
      // 选择卡：每个选项一个按钮，**不给「忽略」**——那类的价值就是必须定夺
      card.options.forEach(function (opt, oi) {
        const bid = actId + '-opt-' + oi;
        $('#' + actId).append('Button', {
          id: bid,
          label: opt.label,
          variant: oi === 0 ? 'primary' : 'soft',
          size: 'small',
          hint: opt.hint || '',
        });
        $('#' + bid).on('click', function () {
          submit(card.id, { choice: opt.value });
        });
      });
    } else if (card.kind === 'todo') {
      // 待办卡：「完成了 / 先放着」（后者不是忽略，是"还记着，晚点做"）
      const doneId = actId + '-done';
      const laterId = actId + '-later';
      $('#' + actId).append('Button', { id: doneId, label: '完成了', icon: 'check', variant: 'primary', size: 'small' });
      $('#' + actId).append('Button', { id: laterId, label: '先放着', variant: 'ghost', size: 'small' });
      $('#' + doneId).on('click', function () {
        submit(card.id, { done: true });
      });
      $('#' + laterId).on('click', function () {
        submit(card.id, { snooze: true });
      });
      if (card.due) {
        $('#' + actId).append('Text', { size: '11', text: '截止 ' + card.due, text_color: '#9ca3af' });
      }
    } else {
      // 提示卡：知道就行
      const okId = actId + '-ok';
      $('#' + actId).append('Button', { id: okId, label: '知道了', variant: 'soft', size: 'small' });
      $('#' + okId).on('click', function () {
        submit(card.id, { acknowledged: true });
      });
    }
  }

  // ── 动作 ────────────────────────────────────────────────────────────────

  /** 把答复送回 C++（它会写入答复通道），然后标脏等下一次 tick 渲染。 */
  function submit(id, answer) {
    try {
      cards = app.cards.answer(id, JSON.stringify(answer));
      // ⚠ **不在事件回调里重建界面树**。
      //
      // 这个回调由宿主在派发事件时调用，调用栈里还握着被点元素；
      // 而重建（`clear()` + 数十次 `append`）会当场释放那个元素。
      // 回调返回后宿主手里的引用就成了悬垂——实测表现是进程
      // `std::bad_alloc` 退出，且**没有任何 JS 异常**（日志还停在上一条业务日志，
      // 极易误判成业务代码问题）。
      //
      // 改法：数据变 → 置脏 → 下一次 tick 统一渲染。tick 跑在主循环里，
      // 不在任何事件派发栈上，此时改结构是安全的。
      dirty = true;
      log('相期：已答复 ' + id + ' → ' + JSON.stringify(answer) + ' 剩余=' + cards.length);
    } catch (e) {
      // 答复失败**必须说出来**：静默失败会让卡片停在原地，用户以为点没生效又点一次，
      // 而 agent 那头可能已经收到了第一次。
      log('相期：答复失败 ' + id + ' — ' + e);
      $('#link-text').set({ text: '答复失败：' + e, text_color: '#dc2626' });
    }
  }

  // ── 轮询：卡片由 C++ 侧（agent）投递，JS 定期取 ────────────────────────
  //
  // 为什么用轮询而不是事件推送：控制通道（ScriptBridge）是单向的"JS 调 C++"。
  // C++ 主动调 JS 要走 `bridge.call`，那需要把整个 App 生命周期与脚本宿主
  // 绑在一起。对一个"几百毫秒延迟完全够用"的常驻面板，轮询是最简正确的选择。
  // 若将来要做即时推送，把 tick 换成 bridge.call('on_cards', …) 即可，渲染逻辑不动。

  var POLL_MS = 500;

  function tick() {
    try {
      const snapshot = app.cards.poll();
      const nextCards = snapshot.cards || [];
      const nextPeer = snapshot.peer || { connected: false };

      // 只在真的变了才重渲染：无谓重建会打断用户的鼠标悬停与阅读位置
      const changed =
        nextCards.length !== cards.length ||
        JSON.stringify(nextCards) !== JSON.stringify(cards) ||
        nextPeer.connected !== peer.connected ||
        nextPeer.name !== peer.name;

      if (changed) {
        cards = nextCards;
        peer = nextPeer;
        dirty = true;
      }
    } catch (e) {
      // 轮询失败只降级显示，不打断界面（下次 tick 可能就好了）
      peer = { connected: false, name: '' };
      $('#link-text').set({ text: '轮询失败：' + e, text_color: '#dc2626' });
      return;
    }

    // 统一渲染点：所有变更（数据到 / 用户答复）都在这里落到界面上。
    // 放在 tick 里而不是各回调里，是为了保证**只在主循环、不在事件派发栈上**改结构。
    if (dirty) {
      dirty = false;
      const want = desiredCompact();
      if (want !== compact) set_compact(want);
      else render();
    }
  }

  // ── 启动 ────────────────────────────────────────────────────────────────

  $('#compact-ok').on('click', function () {
    const card = cards[0];
    if (card) submit(card.id, { acknowledged: true });
  });
  $('#compact-expand').on('click', function () {
    userExpanded = true;   // 主动展开后不再自动缩回
    requestRender();       // 由 tick 调 set_compact（不在事件栈上改结构）
  });

  $('#dismiss-all').on('click', function () {
    try {
      cards = app.cards.dismiss_all();
      userExpanded = false;   // 内容清空 → 下次新卡片重新走轻提示
      dirty = true;           // 统一由 tick 渲染（不在事件栈上改结构）
    } catch (e) {
      log('相期：批量忽略失败 — ' + e);
    }
  });


  // 首次立即渲染，然后进入轮询
  tick();
  // ⚠ 霜天脚本宿主**没有** setInterval/setTimeout（QuickJS 裸环境 + 宿主注入的少数 API）。
  // 框架提供的是 every(ms, fn) / after(ms, fn)，由 UiRoot 每帧推进 __run_timers 触发。
  // 写 setInterval 不会报错——只会永远不触发，于是卡片永不刷新，
  // 而现象是"agent 说投了但界面没反应"。这类静默失效在常驻应用里最致命。
  if (typeof every === 'function') {
    every(POLL_MS, tick);
  } else {
    // 真没有定时器就明确说出来，而不是假装在工作
    log('相期：宿主无 every()，卡片不会自动刷新');
    $('#link-text').set({ text: '宿主无定时器：卡片不会自动刷新', text_color: '#dc2626' });
  }

  log('相期就绪：等待 agent 投递卡片');
})();
