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
//
// ## 窗口尺寸不归本文件管
//
// 相期的窗口是**固定的 560×720（逻辑）= 700×900（物理，DPI 1.25）**，
// 启动时由 `main.cpp` 的 `app_options` 一次性定好、运行期永不改变。
// 一次性定好、运行期永不改变。本文件**不调** `app.window.set_size`：
// 曾按“只有 notice 就缩成 420×200 轻提示”自动改尺寸，于是同一块面板有
// 两种宽度——MarkdownView 折行、卡片高、滚动位置全部重排，用户正读的那一屏
// 会被抽走；日志里则表现为画布反复 resize。尺寸是启动参数，不是运行期状态。

(function () {
  'use strict';

  // ── 状态 ────────────────────────────────────────────────────────────────
  let cards = [];
  let peer = { connected: false, name: '' };
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

  /** 一张卡片的语义调性：三类各有自己的 tone（跟主题走，非硬编码色）。
   *
   * choice → primary：需要定夺，最重的事。
   * todo   → warning：时间敏感，拖着有代价。
   * notice → muted：轻告知，读完就走。
   */
  function toneOf(kind) {
    if (kind === 'choice') return { tone: 'primary', label: '需要你定' };
    if (kind === 'todo') return { tone: 'warning', label: '等你去做' };
    return { tone: 'muted', label: '有事告诉你' };
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

    // 徽标：有卡时主色强调、空态弱化（动态信息该比静态标题跳一级）
    $('#badge').set({ text: badgeText(), tone: cards.length > 0 ? 'primary' : 'faint' });

    // 空状态与卡片区互斥（都占同一块 grow 区域，同时可见会各分一半高度）。
    // ⚠ 必须隐 **`#cards-scroll`（外层滚动容器）**，不是 `#cards`：
    //   `#cards` 只是滚动区里的内容列，隐它以后滚动区自己还在布局里，
    //   照样吃掉一半高度——实测现象就是“空态看起来没居中”（它在半屏内居中）。
    const emptyNow = cards.length === 0;
    $('#empty').set({ visible: emptyNow ? 'true' : 'false' });
    $('#cards-scroll').set({ visible: emptyNow ? 'false' : 'true' });
    // 「全部忽略」无卡时隐藏：空态下没有任何可忽略的东西，
    // 留着一个总能点的按钮只会让人以为“有东西被藏起来了”。
    $('#footer-actions').set({ visible: emptyNow ? 'false' : 'true' });

    // 底部连接状态：如实显示（断线与"agent 很安静"必须能区分）
    paintLinkState($('#link-text'), $('#link-icon'));
  }

  /**
   * 绘制连接状态：**只用一个图标**。
   *
   * 为何不写文字：常驻面板的横向空间很贵，而“已连接”三个字占的位置
   * 比图标大好几倍，信息量却一样——绿色链子 = 通着，红色警告 = 断了。
   * 具体原因（如“对端已关闭连接”）保留在 `title` 属性里，悬停可见。
   */
  function paintLinkState(textHandle, iconHandle) {
    if (peer && peer.connected) {
      iconHandle.set({ icon: 'link', tone: 'success' });
      // 文字元素**隐藏**而不是留空：空文本仍占一行行高，会把标题栏顶高。
      textHandle.set({ text: '', visible: 'false' });
    } else {
      iconHandle.set({ icon: 'warning', tone: 'danger' });
      textHandle.set({ text: '', visible: 'false' });
    }
    // 具体状态文字改挂到图标的 hint 上（悬停可见），不占横向空间。
    const detail = (peer && peer.connected)
        ? ('已连接' + (peer.name ? ' · ' + peer.name : ''))
        : (peer && peer.detail ? peer.detail : '未连接');
    iconHandle.set({ hint: detail });
  }

  // ── 单张卡片：三类的内部结构不同，各建各的 ──────────────────────────────

  function renderCard(card, index) {
    const tone = toneOf(card.kind);
    const sid = 'card-' + index;
    const list = $('#cards');

    // 卡片容器：语义表面 + 主题色描边（不再硬编码 #e5e7eb——那在暗色主题上
    // 是一道灰白裂缝，且切主题不跟随）。Content 面在暗色下更沉、亮色下归白，
    // 正是“一张卡”该有的层级。圆角随主题 metrics（内置主题大多 10-14px）。
    list.append('Column', {
      id: sid,
      gap: '10',
      padding: '16',
      surface: 'content',
      radius: '12',
      border_width: '1',
    });

    // ① 抬头：Badge 胶囊（类别）+ 来源。Badge 自带 tone 柔底色，
    // 比“图标+彩字”更有质感且天然跟主题走（feedback 组件的既成能力，不重造）。
    const headId = sid + '-head';
    $('#' + sid).append('Row', { id: headId, gap: '8', align_items: 'center' });
    $('#' + headId).append('Badge', { id: sid + '-kind', text: tone.label, tone: tone.tone });
    $('#' + headId).append('Spacer', { grow: 'true' });
    if (card.from) {
      $('#' + headId).append('Text', { font_size: '11', text: card.from, tone: 'faint', margin_left: '6' });
    }

    // ② 标题与正文：标题加重一档，正文用 muted 退后一层，层级靠色阶而不是字号堆砌。
    //
    // `multiline` 必须显式开：`Text` 的缺省是**单行 + 尾部省略号**，而卡片标题
    // 常常是一整句（「需要飞书授权：令牌属于应用 cli_xxx，与当前应用不一致…」）。
    // 不开多行的后果不是“短一点”，而是**后半句根本看不到**——用户只能看到
    // 一截加省略号的残句，还得靠猜（实测：窗口 496px 内容宽下标题只露出前 30 字）。
    $('#' + sid).append('Text', {
      font_size: '15',
      text: card.title || '（无标题）',
      font_weight: 'semibold',
      multiline: 'true',
    });
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
        // 链接可点（框架能力）：点击直接交给系统浏览器。
        // 卡片正文里的授权/登录链接是“要去做的事”，点不动等于断在半路。
        open_links: 'true',
        // 可拖选 + Ctrl+C（默认就是 true，写出来是让它成为显式约定）：
        // “把这段文字拷走”是卡片的高频动作（命令、单号、号码）。
        selectable: 'true',
      });
    }

    // ③ 交互区：按类型分派。主操作带图标（视觉反馈：纯文字按钮显得单薄），
    // 主/轻操作间留出呼吸。
    // `wrap` 必须显式开：`Row` 缺省是**单行**，排不下时不会折行、而是继续向右
    // 排到容器之外——超出窗口的部分被裁掉，那些按钮**看不见也点不到**
    //（实测：3 个选项 + 复制的行排到 x=815，而窗口只有 560 宽，后两个按钮整块丢失）。
    //
    // 折行需要「父宽有上界」：本行是卡片 `Column` 的子元素，拿到的是确定的内容宽
    //（`ScrollView::arrange` 会在宽度定下来后重测内容），所以这里能真的折。
    const actId = sid + '-act';
    $('#' + sid).append('Row', { id: actId, gap: '8', padding_y: '2', wrap: 'true' });

    if (card.kind === 'choice' && card.options) {
      // 选择卡：每个选项一个按钮，**不给「忽略」**——那类的价值就是必须定夺
      card.options.forEach(function (opt, oi) {
        const bid = actId + '-opt-' + oi;
        $('#' + actId).append('Button', {
          id: bid,
          // 链接项带外链标识：让人一眼看出"按下去会离开相期"
          label: opt.url ? opt.label + ' ↗' : opt.label,
          variant: oi === 0 ? 'primary' : 'soft',
          scale: 'small',
          hint: opt.url ? '在浏览器中打开' : (opt.hint || ''),
        });
        $('#' + bid).on('click', function () {
          // 链接项不是「选项」：它的作用是把人送到浏览器去办事（授权/登录），
          // 点它**不能**提交 {choice:…} —— 那等于替用户宣称"我选了这个"，
          // 而外部系统才真正知道成没成功（授权完成与否由回调告知，不在这里）。
          //
          // 卡片在浏览器完成后由 agent 自己撤（agent 收到外部状态变更 → dismiss），
          // 所以这里不产生 answer，也不把卡从列表里拿掉——用户回来时仍能看到
          // 「去授权」直到 agent 确认完成。
          if (opt.url) {
            openExternal(opt.url);
            return;
          }
          submit(card.id, { choice: opt.value });
        });
      });
    } else if (card.kind === 'todo') {
      // 待办卡：「完成了 / 先放着」（后者不是忽略，是"还记着，晚点做"）
      const doneId = actId + '-done';
      const laterId = actId + '-later';
      $('#' + actId).append('Button', { id: doneId, label: '完成了', icon: 'check', variant: 'primary', scale: 'small' });
      $('#' + actId).append('Button', { id: laterId, label: '先放着', icon: 'clock', variant: 'ghost', scale: 'small' });
      $('#' + doneId).on('click', function () {
        submit(card.id, { done: true });
      });
      $('#' + laterId).on('click', function () {
        submit(card.id, { snooze: true });
      });
      // 截止时间挂到抬头（与来源同侧）：塞在按钮行里会挤压操作区，
      // 且它是元信息不是动作，与 from 同层级更顺。
      if (card.due) {
        const dueId = sid + '-due';
        $('#' + sid + '-head').append('Icon', { id: dueId, icon: 'clock', font_size: '11', tone: 'warning' });
        $('#' + sid + '-head').append('Text', {
          id: dueId + '-t', font_size: '11', text: card.due, tone: 'warning',
        });
      }
    } else {
      // 提示卡：知道就行。带 check 图标与 todo 的「完成了」呼应，
      // 也让纯文字按钮不再单薄（视觉反馈建议）。
      const okId = actId + '-ok';
      $('#' + actId).append('Button', { id: okId, label: '知道了', icon: 'check', variant: 'soft', scale: 'small' });
      $('#' + okId).on('click', function () {
        submit(card.id, { acknowledged: true });
      });
    }

    // ④ 「复制」：把这条内容的**纯文本**放进剪贴板。
    //
    // ## 为何需要一个按钮（正文已经能拖选 + Ctrl+C）
    //
    // 那两条要求用户先知道“这里能选”，而且得先把焦点落到正文上；
    // 而“把这条提醒拷到别处”（贴给同事、存到工单、运行一条命令）
    // 是常驻面板最高频的动作之一。按钮 == 一眼可见的入口。
    //
    // 拷的是**标题 + 正文纯文本**（含链接原地址）：给出去的东西要能独立读懂，
    // 只拷正文时对方看不到“这是什么事”。
    //
    // 靠右摆（Spacer 隔开）：它是辅助动作，不能与“必须给结果”的主操作抢位置。
    {
      const copyId = actId + '-copy';
      $('#' + actId).append('Spacer', { grow: 'true' });
      $('#' + actId).append('Button', {
        id: copyId, label: '复制', icon: 'copy', variant: 'ghost', scale: 'small',
        hint: '把这条内容的文字拷到剪贴板（正文也可直接拖选 / Ctrl+C）',
      });
      $('#' + copyId).on('click', function () {
        copyCard(card);
      });
    }
  }

  /**
   * 把一张卡片拷进剪贴板（纯文本：标题 + 正文 + 链接地址）。
   *
   * 为何不是拷 `body` 原样的 markdown：用户拿到的文本是要**给别人看**的
   * （贴群、贴工单），markdown 标记在那里只会显得乱。而链接必须**留下地址**——
   * 纯文本环境里 `[文字](url)` 里的 `url` 才是可用的那一半。
   */
  function copyCard(card) {
    const parts = [];
    if (card.title) parts.push(card.title);
    if (card.body) parts.push(card.body);
    // 选项里的链接地址单独附在后面：正文里的 `[x](url)` 在纯文本里不再可点。
    if (card.options && card.options.length) {
      const links = card.options.filter(function (o) { return o.url; });
      if (links.length) {
        parts.push(links.map(function (o) { return o.url; }).join('\n'));
      }
    }
    const text = parts.join('\n\n');
    if (!text) return;
    try {
      app.clipboard.set(text);
      log('相期：已复制卡片 ' + card.id + '（' + text.length + ' 字）');
    } catch (e) {
      // 剪贴板失败必须说出来：静默失败会让用户以为拷到了，黏出去是上一次的内容。
      log('相期：复制失败 — ' + e);
      $('#link-text').set({ text: '复制失败：' + e, tone: 'danger', visible: 'true' });
    }
  }

  // ── 动作 ────────────────────────────────────────────────────────────────

  /**
   * 用系统默认浏览器打开链接（授权/登录类卡片用）。
   *
   * 为何要单独一个函数：链接项与普通选项的行为完全不同——
   * 选项是「回答」，链接是「把人送出去办事」。混淆这两者会替用户
   * 声称一个他并未做出的选择。
   *
   * 打不开时**不弹错就完事**：把 URL 显示出来让用户自己复制，
   * 比一句"打开失败"有用。
   */
  function openExternal(url) {
    if (!url) return;
    try {
      const res = app.platform.open_url(url);
      if (res && res.ok) {
        log('相期：已在浏览器中打开 ' + url);
        return;
      }
      log('相期：打开链接失败，请手动复制：' + url);
      copyToClipboard(url);
    } catch (e) {
      log('相期：打开链接异常（' + e + '），请手动复制：' + url);
      copyToClipboard(url);
    }
  }

  /** 尽力把链接放进剪贴板（没有桥能力时静默——不能因为辅助动作弄挂主流程）。 */
  function copyToClipboard(text) {
    try {
      if (app.clipboard && app.clipboard.set) app.clipboard.set(text);
    } catch (e) {
      void e;
    }
  }

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
      $('#link-text').set({ text: '答复失败：' + e, tone: 'danger', visible: 'true' });
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
      $('#link-text').set({ text: '轮询失败：' + e, tone: 'danger', visible: 'true' });
      return;
    }

    // 统一渲染点：所有变更（数据到 / 用户答复）都在这里落到界面上。
    // 放在 tick 里而不是各回调里，是为了保证**只在主循环、不在事件派发栈上**改结构。
    if (dirty) {
      dirty = false;
      render();
    }
  }

  // ── 启动 ────────────────────────────────────────────────────────────────

  $('#dismiss-all').on('click', function () {
    try {
      cards = app.cards.dismiss_all();
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
    $('#link-text').set({ text: '宿主无定时器：卡片不会自动刷新', tone: 'danger', visible: 'true' });
  }

  log('相期就绪：等待 agent 投递卡片');
})();
