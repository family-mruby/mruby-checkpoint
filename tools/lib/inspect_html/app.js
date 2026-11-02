// The page of tools/checkpoint_inspect.rb --html: draws the inspect JSON
// (doc/inspect_json.md) embedded in the page. Plain DOM and SVG, no library,
// nothing loaded from outside. The view is kept in the location hash
// (#view=objects&img=0&focus=12) so that a link or a headless browser can
// open one view directly.
// An error is shown on the page (and is what a headless check looks for).
window.addEventListener('error', function (e) {
  var box = document.getElementById('js-error') || document.body.appendChild(document.createElement('pre'));
  box.id = 'js-error';
  box.textContent += 'この表示の不具合: ' + e.message + ' (' + (e.lineno || '?') + ':' + (e.colno || '?') + ')\n';
});
(function () {
  'use strict';
  var DATA = JSON.parse(document.getElementById('checkpoint-data').textContent);
  var DOCS = DATA.docs, DIFF = DATA.diff;
  var SVGNS = 'http://www.w3.org/2000/svg';
  var st = { view: 'stack', img: 0, focus: null, depth: 2, classEdges: false, sel: null, q: '' };
  var cache = [];   // per image: incoming references

  // ---- small helpers ----
  function h(tag, attrs) {
    var e = document.createElement(tag);
    if (attrs) for (var k in attrs) {
      if (k === 'text') e.textContent = attrs[k];
      else if (k === 'cls') e.className = attrs[k];
      else if (k.slice(0, 2) === 'on') e.addEventListener(k.slice(2), attrs[k]);
      else e.setAttribute(k, attrs[k]);
    }
    for (var i = 2; i < arguments.length; i++) add(e, arguments[i]);
    return e;
  }
  function add(e) {
    for (var i = 1; i < arguments.length; i++) {
      var c = arguments[i];
      if (c === null || c === undefined || c === false) continue;
      if (Array.isArray(c)) { c.forEach(function (x) { add(e, x); }); continue; }
      e.appendChild(typeof c === 'string' || typeof c === 'number' ? document.createTextNode(String(c)) : c);
    }
  }
  function s(tag, attrs) {
    var e = document.createElementNS(SVGNS, tag);
    if (attrs) for (var k in attrs) {
      if (k === 'text') e.textContent = attrs[k];
      else if (k === 'cls') e.setAttribute('class', attrs[k]);
      else if (k.slice(0, 2) === 'on') e.addEventListener(k.slice(2), attrs[k]);
      else e.setAttribute(k, attrs[k]);
    }
    return e;
  }
  function cut(t, n) { t = String(t); return t.length > n ? t.slice(0, n - 1) + '…' : t; }
  function doc() { return DOCS[st.img]; }
  function obj(d, id) { return d.objects[id - 1]; }
  function isPerm(o) { return o.kind === 'permanent' || o.kind === 'permanent_proc'; }
  function isRef(v) { return v !== null && typeof v === 'object' && 'ref' in v; }

  function valueText(d, v) {
    if (v === null) return 'nil';
    if (typeof v !== 'object') return String(v);
    if ('ref' in v) return L(obj(d, v.ref));
    if ('sym' in v) return ':' + (v.sym === null ? '?' : v.sym);
    if ('int' in v) return v.int;
    if ('float' in v) return typeof v.float === 'number' ? floatText(v.float) : v.float;
    if ('undef' in v) return 'undef';
    return JSON.stringify(v);
  }
  // ---- words for a person: the JSON's keys and enum values stay inside ----
  var TASK_STATE = { DORMANT: '休止', READY: '実行可能', RUNNING: '実行中', WAITING: '待機中', SUSPENDED: '中断中' };
  var QUEUE = { ready: 'レディキュー', waiting: '待ちキュー', suspended: '中断キュー', dormant: '休止キュー' };
  var ROLE = { root: 'プログラムの本体', task: 'Task', fiber: 'Fiber', unknown: 'コンテキスト' };
  var ACTIVITY = {
    current: '実行中 (ここで書き出した)', not_started: '未開始', yielded: 'Fiber.yield で中断中',
    resuming: 'resume した Fiber を待っている', transferring: '別の Fiber へ transfer した', finished: '終了',
    scheduler: 'C の関数からスケジューラを回している', no_frames: 'フレームなし (埋め込む側がスケジューラを回す)', other: 'そのほか'
  };
  var EDGE = { target_class: '定義先', upper: '外側', env: '環境', 'waits on': '待ち先', 'class': 'クラス', superclass: 'スーパークラス',
    proc: 'Proc', '&block': 'ブロック', message: 'メッセージ', backtrace: 'バックトレース', name: '名前', value: '値', begin: '始め', end: '終わり' };
  var FIELD = {
    activity: '状態', vmexec: 'C から resume された', current: '書き出したとき実行中だった', 'resumed by': 'resume したコンテキスト',
    state: '状態', queue: 'キュー', priority: '優先度', timeslice: 'タイムスライスの残り (tick)', wait: '待ち', value: '値',
    running: '実行中だった', host: '埋め込む側の Task', switching: '切り替えを要求していた', loop: 'スケジューラのループ', tick_ms: '1 tick (ms)',
    tick: '時計 (tick)', next_wakeup: '次に起きるまで (tick)', 'running Task': '実行中だった Task', 'host Task': '埋め込む側の Task',
    'current context': '止まったコンテキスト', magic: 'マジック', format: '形式の版', implementation: '処理系', bytecode: 'バイトコード',
    integer_bits: 'Integer のビット数', float_bits: 'Float のビット数', utf8: 'String は UTF-8', pointer_bits: '書き出し側のポインタ幅'
  };
  var KIND = { permanent: 'permanent (名前だけ)', permanent_proc: 'permanent の Proc (フレームが動かす)', object: 'ふつうのオブジェクト', string: 'String',
    array: 'Array', hash: 'Hash', range: 'Range', exception: '例外', 'class': 'クラス', module: 'モジュール', singleton_class: '特異クラス',
    iclass: '取り込みクラス', proc: 'Proc', env: 'Env (捕まえた変数の箱)', data: 'C のデータ', bigint: '大きな Integer', struct: 'Struct', set: 'Set',
    rational: 'Rational', complex: 'Complex', 'break': 'break の途中', random: 'Random', origin: 'prepend の origin', task: 'Task', fiber: 'Fiber' };
  var IREP_KIND = { carried: '運ぶ', permanent: 'permanent (ハッシュだけ)', permanent_carried: 'mrblib (中身も運ぶ)' };
  // the short text of an object for a person (the JSON's label, with its
  // English words said in Japanese)
  function L(o) {
    var l = o.label;
    switch (o.kind) {
      case 'env': return 'Env (' + (o.method ? o.method + ' の' : 'ブロックの') + '変数)';
      case 'fiber': return o.context ? 'Fiber (コンテキスト ' + o.context + ')' : 'Fiber (まだ初期化されていない)';
      case 'permanent': case 'permanent_proc':
        return l.replace(/ \(method\)$/, ' (メソッド)').replace(/^env of (.*)$/, '$1 の変数の箱');
      case 'iclass': return l.replace(/ \(included\)$/, ' (取り込み)');
      case 'origin': return l.replace(/ \(origin\)$/, ' (prepend の origin)');
      case 'break': return o.tag + ' の途中 (フレーム ' + o.frame + ' へ)';
      default: return l.replace(/ \(not initialized\)/, ' (まだ初期化されていない)');
    }
  }
  function edgeName(l) {
    var m = /^\(key (\d+)\)$/.exec(l);
    return m ? 'キー ' + m[1] : (EDGE[l] || l);
  }
  function taskState(t) { return (TASK_STATE[t.state] || t.state); }
  function queueText(t) { return t.queue ? QUEUE[t.queue] + 'の ' + (t.queue_index + 1) + ' 番目' : 'main の Task (キューに入らない)'; }
  function yesNo(x) { return x === true || x === 'true' ? 'はい' : (x === false || x === 'false' ? 'いいえ' : x); }
  // the name of a context, for a sentence
  function ctxName(d, c) {
    if (c.role === 'root') return 'プログラムの本体';
    if (c.role === 'task') { var t = d.scheduler.tasks[c.task - 1]; return 'Task ' + (t.name_text !== null ? '"' + t.name_text + '"' : t.key); }
    var f = c.frames[0];
    return 'Fiber' + (f && f.file ? ' (' + f.file + ':' + f.line + ')' : ' (コンテキスト ' + c.id + ')');
  }
  // the name of a frame: "Task "c1" の pop" / "本体のトップレベル" / "… のブロック"
  function frameName(d, c, f) {
    var what = f.method ? f.method : (f.index === 0 ? (c.role === 'root' ? 'トップレベル' : '本体') : 'ブロック');
    return ctxName(d, c) + ' の ' + what;
  }
  function rootName(d, r) {
    switch (r.kind) {
      case 'frame': var c = d.contexts[r.context - 1]; return frameName(d, c, c.frames[r.frame]);
      case 'global': return 'グローバル変数 ' + r.key;
      case 'variable': return '定数・クラスの変数 ' + r.key.replace(/^Object::/, '');
      case 'method': return 'メソッド ' + r.key.replace(/^Object#/, '');
      case 'task': return r.key.replace(/^task /, 'Task ');
      case 'fiber': return 'Fiber のオブジェクト';
      default: return r.key;
    }
  }
  // where an object is, for a person: "q、Task "x" のトップレベルのローカル変数"
  function whereText(d, o) {
    if (isPerm(o)) return 'permanent (' + o.name + ')';
    if (o.path_root === null || o.path_root === undefined) return 'ルートから届かない';
    var r = d.roots[o.path_root];
    var rest = o.path.slice(r.key.length).replace(/^\./, '');
    if (r.kind === 'frame') {
      var c = d.contexts[r.context - 1], f = c.frames[r.frame];
      var first = rest.split(/[.\[{<#]/)[0];
      var kind = first === 'self' ? '' : (/^r\d+$/.test(first) ? 'の一時レジスタ' : (f.locals.some(function (l) { return l.name === first; }) ? 'のローカル変数' : ''));
      return rest + '、' + frameName(d, c, f) + kind;
    }
    if (r.kind === 'global' || r.kind === 'variable' || r.kind === 'method') {
      var head = r.kind === 'variable' ? r.key.replace(/^Object::/, '') : (r.kind === 'method' ? r.key.replace(/^Object#/, '') : r.key);
      return head + (rest ? (/^[\[{<#]/.test(rest) ? '' : '.') + rest : '') + '、' + { global: 'グローバル変数', variable: '定数', method: 'メソッド' }[r.kind];
    }
    return (rest || 'それ自身') + '、' + rootName(d, r);
  }
  function waitText(d, t) {
    var w = t.wait;
    if (!w) return null;
    if (w.reason === 'sleep') return 'sleep (残り ' + w.ticks_left + ' tick)';
    if (w.reason === 'join') return (w.task ? d.scheduler.tasks[w.task - 1].key : '?') + ' の終わりを待つ (join)';
    var q = w.queue ? obj(d, w.queue) : null;
    return 'Task::Queue を待つ' + (q ? ' (' + whereText(d, q).split('、')[0] + ')' : '') + (w.ticks_left !== null ? ' 残り ' + w.ticks_left + ' tick' : '');
  }
  // a value of the diff, as a person reads it
  function fieldValue(f, x) {
    if (x === null || x === undefined) return '(なし)';
    if (f === 'wait' && typeof x === 'object') {
      if (x.reason === 'sleep') return 'sleep (残り ' + x.ticks_left + ' tick)';
      if (x.reason === 'join') return x.task + ' の終わりを待つ (join)';
      return 'Task::Queue を待つ (' + x.queue + ')' + (x.ticks_left !== null && x.ticks_left !== undefined ? ' 残り ' + x.ticks_left + ' tick' : '');
    }
    if (f === 'state' && TASK_STATE[x]) return TASK_STATE[x] + ' (' + x + ')';
    if (f === 'activity' && ACTIVITY[x]) return ACTIVITY[x];
    if (f === 'queue') { var m = /^(\w+) #(\d+)$/.exec(x); if (m && QUEUE[m[1]]) return QUEUE[m[1]] + 'の ' + (parseInt(m[2], 10) + 1) + ' 番目'; }
    return String(yesNo(x));
  }

  function floatText(f) { var t = String(f); return /[.eE]|Infinity|NaN/.test(t) ? t : t + '.0'; }
  // a value, objects as links to the object view
  function value(d, v, img) {
    if (!isRef(v)) return h('span', { cls: 'mono', text: valueText(d, v) });
    var o = obj(d, v.ref);
    return objLink(o, img === undefined ? st.img : img);
  }
  function objLink(o, img) {
    return h('a', { cls: 'obj mono', title: whereText(DOCS[img], o) + '\nパス: ' + o.path + '  (#' + o.id + ')',
      onclick: function () { go({ view: 'objects', img: img, focus: o.id, sel: o.id }); } },
      cut(L(o), 70), h('span', { cls: 'muted', text: ' #' + o.id }));
  }

  function incoming(img) {
    if (cache[img]) return cache[img];
    var d = DOCS[img], inc = {};
    d.objects.forEach(function (o) {
      o.refs.forEach(function (r) { (inc[r[1]] = inc[r[1]] || []).push([r[0], o.id]); });
    });
    d.roots.forEach(function (r) {
      r.refs.concat(r.temps || []).forEach(function (x) { (inc[x[1]] = inc[x[1]] || []).push([x[0], r.key]); });
    });
    cache[img] = inc;
    return inc;
  }

  // ---- the location hash ----
  function readHash() {
    location.hash.replace(/^#/, '').split('&').forEach(function (kv) {
      var p = kv.split('='); if (p.length !== 2) return;
      var k = p[0], v = decodeURIComponent(p[1]);
      if (k === 'view') st.view = v;
      else if (k === 'img') st.img = Math.min(DOCS.length - 1, Math.max(0, parseInt(v, 10) || 0));
      else if (k === 'focus') st.focus = /^\d+$/.test(v) ? parseInt(v, 10) : v;
      else if (k === 'sel') st.sel = parseInt(v, 10);
      else if (k === 'depth') st.depth = parseInt(v, 10) || 2;
    });
  }
  function writeHash() {
    var p = ['view=' + st.view, 'img=' + st.img];
    if (st.view === 'objects' && st.focus !== null) p.push('focus=' + encodeURIComponent(st.focus));
    if (st.view === 'objects' && st.sel) p.push('sel=' + st.sel);
    history.replaceState(null, '', '#' + p.join('&'));
  }
  function go(change) {
    for (var k in change) st[k] = change[k];
    if (change.focus !== undefined) graph = null;
    render();
    window.scrollTo(0, 0);
  }

  // ---- the frame of the page ----
  var VIEWS = [['stack', 'コールスタック'], ['tasks', 'Task と Fiber'], ['objects', 'オブジェクト']];
  if (DIFF) VIEWS.push(['diff', '2 つのイメージの差']);
  VIEWS.push(['image', 'イメージの中身']);

  function summary() {
    var d = doc(), hd = d.header, sc = d.scheduler;
    var cur = d.contexts[d.current_context - 1];
    var bits = [d.source.file + ' (' + d.source.bytes + ' バイト)', hd.implementation, hd.pointer_bits + ' bit の書き出し側',
      'オブジェクト ' + d.objects.length, 'コンテキスト ' + d.contexts.length, 'Task ' + sc.tasks.length,
      '止まった所: ' + ctxName(d, cur) + (cur.frames.length ? ' ' + (cur.frames[cur.frames.length - 1].location || '') : '')];
    var e = document.getElementById('summary');
    e.textContent = '';
    add(e, bits.join(' ・ '));
    if (d.warnings.length) add(e, h('span', { cls: 'badge warn', text: '注意 ' + d.warnings.length }));
  }

  function tabs() {
    var nav = document.getElementById('tabs');
    nav.textContent = '';
    VIEWS.forEach(function (v) {
      add(nav, h('button', { cls: st.view === v[0] ? 'on' : '', onclick: function () { go({ view: v[0] }); } }, v[1]));
    });
    if (DOCS.length > 1) {
      add(nav, h('span', { cls: 'sep' }));
      var g = h('span', { cls: 'img' });
      DOCS.forEach(function (d, i) {
        add(g, h('button', { cls: st.img === i ? 'on' : '', title: d.source.file,
          onclick: function () { go({ img: i, focus: null, sel: null }); } }, (i ? 'B: ' : 'A: ') + cut(d.source.file, 24)));
      });
      add(nav, g);
    }
  }

  function render() {
    summary();
    tabs();
    var m = document.getElementById('main');
    m.textContent = '';
    ({ stack: viewStack, tasks: viewTasks, objects: viewObjects, diff: viewDiff, image: viewImage }[st.view] || viewStack)(m);
    writeHash();
  }

  // ---- the call stacks ----
  function ctxTitle(d, c) {
    var t = c.task ? d.scheduler.tasks[c.task - 1] : null;
    var act = c.activity === 'task' ? null : ACTIVITY[c.activity];
    var e = h('h3', { id: 'ctx-' + c.id }, ctxName(d, c), h('span', { cls: 'muted', text: '  コンテキスト ' + c.id + (act && !c.current ? ' ・ ' + act : '') }));
    if (c.current) add(e, h('span', { cls: 'badge cur', text: 'ここで止まった' }));
    if (t && t.running) add(e, h('span', { cls: 'badge run', text: '実行中だった Task' }));
    if (t) add(e, h('span', { cls: 'badge', text: taskState(t) + ' ・ ' + queueText(t) }));
    if (c.prev) add(e, h('span', { cls: 'badge', text: ctxName(d, d.contexts[c.prev - 1]) + ' が resume した' }));
    return e;
  }

  function viewStack(m) {
    var d = doc();
    add(m, h('p', { cls: 'muted' }, 'コンテキスト (プログラムの本体・Task・Fiber) ごとのコールスタック。上が新しいフレームで、バックトレースと同じ並び。' +
      '「C の関数」はコンテキストの切り替えを待っているフレーム。値の青い字はオブジェクトの図へ飛ぶ。'));
    var order = d.contexts.slice().sort(function (a, b) { return (b.current ? 1 : 0) - (a.current ? 1 : 0) || a.id - b.id; });
    order.forEach(function (c) {
      var card = h('div', { cls: 'card' + (c.current ? ' cur' : '') }, ctxTitle(d, c));
      if (!c.frames.length) add(card, h('div', { cls: 'empty', text: c.activity === 'finished' ? '終了 (フレームなし)' : 'フレームなし' }));
      c.frames.slice().reverse().forEach(function (f) { add(card, frameBox(d, c, f)); });
      add(m, card);
    });
  }

  function frameBox(d, c, f) {
    if (f.placeholder) {
      return h('div', { cls: 'frame muted' }, 'フレーム ' + f.index + ': フレーム 0 の写し。Fiber#resume のフレームの代わりに積まれていて、最初の resume から戻るときに外される (中身はフレーム 0 と同じ)');
    }
    var loc = f.location || ((f.file ? f.file + ':' + f.line : '(行なし)') + (f.method ? ':in ' + f.method : ''));
    var head = h('summary', null, h('span', { cls: 'loc mono', text: loc }));
    if (f.kind === 'cfunc') add(head, h('span', { cls: 'badge warn', text: 'C の関数 ' + f.cfunc.replace(/^F:(S:)?/, '').replace('#', f.cfunc.indexOf('F:S:') === 0 ? '.' : '#') + ' で待っている' }));
    else if (f.not_started) add(head, h('span', { cls: 'badge', text: '未開始: ここから始まる' }));
    else if (!f.location) add(head, h('span', { cls: 'badge', text: 'バックトレースには出ない' }));
    add(head, h('span', { cls: 'muted', text: '  フレーム ' + f.index + (f.pc !== null ? ' ・ pc ' + f.pc : '') + (f.irep ? ' ・ irep @' + f.irep : '') }));
    var t = h('table');
    var selfMain = isRef(f.self) && obj(d, f.self.ref).name === 'T';
    if (f.self_register !== undefined) add(t, row('self', h('span', null, value(d, f.self), h('span', { cls: 'muted' }, ' (最初の resume から。それまで R0 には ', value(d, f.self_register), ' が入っている)'))));
    else if (f.method && !selfMain) add(t, row('self', value(d, f.self)));
    f.locals.forEach(function (l) { add(t, row(l.name === null ? 'r' + l.reg : l.name, value(d, l.value))); });
    if (f.block) add(t, row('&block', value(d, { ref: f.block })));
    f.captured.forEach(function (lv) {
      var vars = lv.variables.filter(function (x) { return x.name !== null; });
      if (!vars.length) return;
      var oc = lv.on_stack ? d.contexts[lv.context - 1] : null;
      var where = lv.on_stack ? (lv.frame !== null && lv.frame !== undefined ? frameName(d, oc, oc.frames[lv.frame]) : ctxName(d, oc) + ' のスタック') : 'ブロックが持っている';
      if (lv.on_stack && lv.context === c.id) {
        add(t, row('外の変数', h('span', { cls: 'muted', text: vars.map(function (x) { return x.name; }).join(', ') + ' (' + where + ' の変数そのもの)' })));
      } else {
        vars.forEach(function (x, i) { add(t, row(i ? '' : '外の変数 (' + where + ')', h('span', null, x.name + ' = ', value(d, x.value)))); });
      }
    });
    var det = h('details', { cls: 'frame' }, head, t.childNodes.length ? t : h('div', { cls: 'empty', text: 'ローカル変数なし' }));
    det.open = true;
    return det;
  }
  function row(k, v) { return h('tr', null, h('td', { cls: 'k', text: k }), h('td', null, v)); }

  // ---- Tasks and Fibers ----
  function viewTasks(m) {
    var d = doc(), sc = d.scheduler;
    add(m, h('p', null, 'tick ' + sc.tick + ' (1 tick ' + sc.tick_ms + ' ms) ・ 次に起きる: ' + (sc.next_wakeup === null ? 'なし' : sc.next_wakeup + ' tick 後') +
      ' ・ スケジューラのループ: ' + (sc.loop ? '回っている' : '回っていない') + (sc.switching ? ' ・ 切り替えを要求していた' : '')));
    var byLane = {};
    sc.tasks.forEach(function (t) { var k = t.queue || 'main'; (byLane[k] = byLane[k] || []).push(t); });
    // the lanes that hold a Task (ready always, as the place where Tasks run)
    var lanes = ['ready', 'waiting', 'suspended', 'dormant', 'main'].filter(function (l) { return l === 'ready' || byLane[l]; });
    var names = { ready: QUEUE.ready, waiting: QUEUE.waiting, suspended: QUEUE.suspended, dormant: QUEUE.dormant, main: 'main の Task' };
    var W = 250, H = 96, GX = 18, GY = 16, LW = 120;
    var queues = [];
    sc.tasks.forEach(function (t) { if (t.wait && t.wait.reason === 'queue' && t.wait.queue && queues.indexOf(t.wait.queue) < 0) queues.push(t.wait.queue); });
    var maxN = 1;
    lanes.forEach(function (l) { maxN = Math.max(maxN, (byLane[l] || []).length); });
    var qx = LW + maxN * (W + GX) + 40;
    var width = qx + (queues.length ? W + 20 : 0), height = lanes.length * (H + GY) + GY;
    var svg = s('svg', { width: width, height: height, viewBox: '0 0 ' + width + ' ' + height });
    defs(svg);
    var pos = {};
    lanes.forEach(function (l, i) {
      var y = GY + i * (H + GY);
      svg.appendChild(s('rect', { cls: 'lane', x: 4, y: y - 6, width: qx - 24, height: H + 12, rx: 8 }));
      svg.appendChild(s('text', { cls: 'lanelabel', x: 12, y: y + H / 2 + 4, text: names[l] }));
      (byLane[l] || []).forEach(function (t, k) {
        var x = LW + k * (W + GX);
        pos[t.id] = [x, y];
        var c = t.context ? d.contexts[t.context - 1] : null;
        var g = s('g', { cls: 'task' + (t.running ? ' run' : ''), transform: 'translate(' + x + ',' + y + ')' });
        g.appendChild(s('rect', { width: W, height: H, rx: 6 }));
        g.appendChild(s('text', { cls: 't1', x: 8, y: 18, text: cut(t.main ? 'main' : (t.name_text !== null ? '"' + t.name_text + '"' : t.key), 24) + (t.running ? '  ▶' : '') }));
        g.appendChild(s('text', { cls: 't2', x: 8, y: 35, text: taskState(t) + ' ・ 優先度 ' + t.priority + (c && c.frames.length === 1 && c.frames[0].not_started ? ' ・ 未開始' : '') }));
        g.appendChild(s('text', { cls: 't2', x: 8, y: 51, text: 'タイムスライスの残り ' + t.timeslice + ' tick' }));
        var w = waitText(d, t);
        if (w) g.appendChild(s('text', { cls: 't2', x: 8, y: 67, text: cut((t.state === 'WAITING' ? '待ち: ' : '最後の待ち: ') + w, 34) }));
        var tags = [t.host ? '埋め込む側の Task' : null, t.value !== null && t.value !== undefined && t.state === 'DORMANT' ? '値 ' + cut(valueText(d, t.value), 20) : null].filter(Boolean);
        if (tags.length) g.appendChild(s('text', { cls: 't2', x: 8, y: 83, text: tags.join(' ・ ') }));
        var title = s('title'); title.textContent = t.key + ' (Task ' + t.id + ', コンテキスト ' + (t.context || '-') + ')'; g.appendChild(title);
        if (c) g.addEventListener('click', function () { go({ view: 'stack' }); var e = document.getElementById('ctx-' + c.id); if (e) e.scrollIntoView(); });
        svg.appendChild(g);
      });
    });
    var qpos = {};
    queues.forEach(function (q, i) {
      var y = GY + i * (H / 2 + GY), o = obj(d, q);
      qpos[q] = [qx, y];
      var g = s('g', { cls: 'node', transform: 'translate(' + qx + ',' + y + ')', onclick: function () { go({ view: 'objects', focus: q, sel: q }); } });
      g.appendChild(s('rect', { width: W, height: H / 2, rx: 6 }));
      var wp = whereText(d, o).split('、');
      g.appendChild(s('text', { x: 8, y: 16, text: cut(L(o) + ' (' + wp[0] + ')', 30) }));
      g.appendChild(s('text', { cls: 'kind', x: 8, y: 31, text: cut(wp.slice(1).join('、'), 36) }));
      var qt = s('title'); qt.textContent = whereText(d, o) + '\nパス: ' + o.path; g.appendChild(qt);
      svg.appendChild(g);
    });
    sc.tasks.forEach(function (t) {
      var w = t.wait, a = pos[t.id];
      if (!w || !a || t.state !== 'WAITING') return;
      var b = w.reason === 'join' && w.task ? pos[w.task] : (w.reason === 'queue' && w.queue ? qpos[w.queue] : null);
      if (!b) return;
      if (w.reason === 'queue') hArrow(svg, a[0] + W, a[1] + H / 2, b[0], b[1] + H / 4);
      else arrow(svg, a[0] + W / 2, a[1] + H, b[0] + W / 2, b[1] + (b[1] > a[1] ? 0 : H), '');
    });
    add(m, h('h2', { text: 'スケジューラのキュー' }), h('div', { cls: 'svgbox' }, svg));
    add(m, h('p', { cls: 'muted', text: 'キューの中は左から順番 (この順に動く)。緑の枠線は書き出したときに実行中だった Task。橙の矢印は待っている相手 (join・Task::Queue)。箱を押すとその Task のコールスタックへ。' }));

    // the contexts and who resumed whom
    var cs = d.contexts, CW = 230, CH = 62;
    var cols = Math.max(1, Math.min(4, cs.length));
    var rows = Math.ceil(cs.length / cols);
    var w2 = cols * (CW + 40) + 20, h2 = rows * (CH + 40) + 20;
    var svg2 = s('svg', { width: w2, height: h2, viewBox: '0 0 ' + w2 + ' ' + h2 });
    defs(svg2);
    var cp = {};
    cs.forEach(function (c, i) {
      var x = 20 + (i % cols) * (CW + 40), y = 20 + Math.floor(i / cols) * (CH + 40);
      cp[c.id] = [x, y];
      var t = c.task ? d.scheduler.tasks[c.task - 1] : null;
      var g = s('g', { cls: 'task' + (c.current ? ' cur' : ''), transform: 'translate(' + x + ',' + y + ')',
        onclick: function () { go({ view: 'stack' }); var e = document.getElementById('ctx-' + c.id); if (e) e.scrollIntoView(); } });
      g.appendChild(s('rect', { width: CW, height: CH, rx: 6 }));
      g.appendChild(s('text', { cls: 't1', x: 8, y: 18, text: cut('コンテキスト ' + c.id + ': ' + ctxName(d, c), 30) }));
      g.appendChild(s('text', { cls: 't2', x: 8, y: 36, text: cut((c.activity === 'task' ? 'Task は' + taskState(t) : ACTIVITY[c.activity]) + ' ・ フレーム ' + c.frames.filter(function (f) { return !f.placeholder; }).length, 34) }));
      var top = c.frames.length ? c.frames[0].not_started && c.frames.length <= 2 ? c.frames[0] : c.frames[c.frames.length - 1] : null;
      if (top) g.appendChild(s('text', { cls: 't2', x: 8, y: 53, text: cut((top.not_started ? '始まる所 ' : '') + (top.location || top.cfunc || ''), 34) }));
      svg2.appendChild(g);
    });
    cs.forEach(function (c) {
      if (!c.prev) return;
      var a = cp[c.prev], b = cp[c.id];
      arrow(svg2, a[0] + CW / 2, a[1] + CH, b[0] + CW / 2, b[1] + (b[1] > a[1] ? 0 : CH), 'prev');
    });
    add(m, h('h2', { text: 'コンテキスト (resume のチェーン)' }), h('div', { cls: 'svgbox' }, svg2));
    add(m, h('p', { cls: 'muted', text: '青の枠線は書き出したとき実行中だったコンテキスト (止まった所)。青の矢印は resume したコンテキストから、されたコンテキストへ (prev のチェーン。終わったら戻る先)。' }));

    var tb = h('table', null, h('tr', null, ['Task', '状態', 'キュー', '優先度', 'タイムスライスの残り (tick)', '待ち', '値', 'コンテキスト'].map(function (x) { return h('th', { text: x }); })));
    sc.tasks.forEach(function (t) {
      add(tb, h('tr', null, h('td', { cls: 'mono', text: t.key + (t.running ? ' ▶' : '') + (t.host ? ' (埋め込む側)' : '') }), h('td', { text: taskState(t) }),
        h('td', { text: queueText(t) }), h('td', { text: t.priority }), h('td', { text: t.timeslice }),
        h('td', { text: waitText(d, t) || '-' }), h('td', null, t.value === null ? '-' : value(d, t.value)), h('td', { text: t.context || '-' })));
    });
    add(m, h('h2', { text: 'Task の表' }), sc.tasks.length ? tb : h('div', { cls: 'empty', text: 'Task なし' }));
  }

  function defs(svg) {
    var df = s('defs');
    ['', 'prev'].forEach(function (k) {
      var mk = s('marker', { id: 'arrow' + k, cls: k, viewBox: '0 0 10 10', refX: 9, refY: 5, markerWidth: 7, markerHeight: 7, orient: 'auto' });
      mk.appendChild(s('path', { d: 'M0,0 L10,5 L0,10 z' }));
      df.appendChild(mk);
    });
    svg.appendChild(df);
  }
  function hArrow(svg, x1, y1, x2, y2) {
    var mx = (x1 + x2) / 2;
    svg.appendChild(s('path', { cls: 'arrow', d: 'M' + x1 + ',' + y1 + ' C' + mx + ',' + y1 + ' ' + mx + ',' + y2 + ' ' + x2 + ',' + y2, 'marker-end': 'url(#arrow)' }));
  }
  function arrow(svg, x1, y1, x2, y2, k) {
    var my = (y1 + y2) / 2;
    svg.appendChild(s('path', { cls: 'arrow ' + k, d: 'M' + x1 + ',' + y1 + ' C' + x1 + ',' + (my + 30) + ' ' + x2 + ',' + (my - 30) + ' ' + x2 + ',' + y2, 'marker-end': 'url(#arrow' + k + ')' }));
  }

  // ---- the object graph ----
  var graph = null;   // { start, open: {id: true} }

  function rootByKey(d, key) { for (var i = 0; i < d.roots.length; i++) if (d.roots[i].key === key) return d.roots[i]; return null; }
  function childrenOf(d, id) {
    if (typeof id === 'string') { var r = rootByKey(d, id); return r ? r.refs.concat(r.temps || []) : []; }
    var o = obj(d, id);
    if (isPerm(o)) return [];
    return o.refs.filter(function (r) { return st.classEdges || r[0] !== 'class'; });
  }
  function defaultFocus(d) {
    var cur = d.contexts[d.current_context - 1];
    if (cur.frames.length) return cur.frames[cur.frames.length - 1].key;
    return d.roots.length ? d.roots[0].key : 1;
  }
  function openDepth(d, start, depth) {
    var open = {}, seen = {}, q = [[start, 0]];
    seen[start] = true;
    while (q.length) {
      var x = q.shift();
      if (x[1] >= depth) continue;
      open[x[0]] = true;
      childrenOf(d, x[0]).slice(0, 40).forEach(function (r) { if (!seen[r[1]]) { seen[r[1]] = true; q.push([r[1], x[1] + 1]); } });
    }
    return open;
  }

  function viewObjects(m) {
    var d = doc();
    if (st.focus === null || (typeof st.focus === 'string' && !rootByKey(d, st.focus)) || (typeof st.focus === 'number' && !obj(d, st.focus))) {
      st.focus = defaultFocus(d); graph = null;
    }
    if (!graph || graph.start !== st.focus || graph.img !== st.img) graph = { start: st.focus, img: st.img, open: openDepth(d, st.focus, st.depth) };
    var side = h('div', { cls: 'side' });
    var tools = h('div', { cls: 'tools' },
      h('label', null, '深さ ', h('input', { type: 'number', min: 1, max: 9, value: st.depth, onchange: function (e) { st.depth = parseInt(e.target.value, 10) || 2; graph = null; render(); } })),
      h('label', null, h('input', { type: 'checkbox', onchange: function (e) { st.classEdges = e.target.checked; graph = null; render(); } }), ' クラスへの辺'));
    if (st.classEdges) tools.querySelector('input[type=checkbox]').checked = true;
    var search = h('input', { type: 'text', placeholder: '#番号・パス・表示で探す', value: st.q });
    var list = h('div', { cls: 'list' });
    function fill() {
      list.textContent = '';
      var q = st.q.trim().toLowerCase();
      if (!q) {
        var grp = null;
        var gname = { global: 'グローバル変数', variable: '定数・クラスの変数', method: '足したメソッド', superclass: 'スーパークラスの付け替え', contents: '組み込みの中身', frame: 'フレーム', task: 'Task', fiber: 'Fiber' };
        d.roots.forEach(function (r) {
          if (!r.refs.length && !(r.temps || []).length) return;
          if (r.kind !== grp) { grp = r.kind; add(list, h('div', { cls: 'grp', text: gname[r.kind] || r.kind })); }
          var where = null;
          if (r.kind === 'frame') { var fr = d.contexts[r.context - 1].frames[r.frame]; where = fr.location || (fr.file ? fr.file + ':' + fr.line : fr.cfunc); }
          add(list, h('div', { cls: st.focus === r.key ? 'on' : '', title: r.key, onclick: function () { go({ focus: r.key, sel: null }); } },
            rootName(d, r), where ? h('div', { cls: 'muted mono', text: cut(where, 60) }) : null));
        });
        return;
      }
      var hits = 0;
      for (var i = 0; i < d.objects.length && hits < 200; i++) {
        var o = d.objects[i];
        if (('#' + o.id) === q || o.path.toLowerCase().indexOf(q) >= 0 || o.label.toLowerCase().indexOf(q) >= 0) {
          hits++;
          (function (o) { add(list, h('div', { cls: 'mono', title: o.path, onclick: function () { go({ focus: o.id, sel: o.id }); } }, '#' + o.id + ' ' + cut(L(o), 40), h('div', { cls: 'muted', text: whereText(d, o) }))); })(o);
        }
      }
      if (!hits) add(list, h('div', { cls: 'grp', text: '見つからない' }));
    }
    search.addEventListener('input', function () { st.q = search.value; fill(); });
    fill();
    add(side, h('div', { cls: 'tools' }, search), list);
    var wide = h('div', { cls: 'wide' }, tools, h('div', { cls: 'svgbox' }, drawGraph(d)), h('p', { cls: 'muted', text:
      '箱を押すと先を開く・閉じる (右の欄にそのオブジェクトの中身)。灰色の箱は permanent (VM を開いたときからあるもの。名前で運ばれ、中身はイメージに無い)。点線はすでに描いた箱への辺。' }));
    var detail = h('div', { cls: 'side' }, details(d, st.sel));
    add(m, h('div', { cls: 'split' }, side, wide, detail));
  }

  function drawGraph(d) {
    var W = 180, H = 38, GX = 100, GY = 12, MAXC = 40;
    var col = {}, rows = [], order = [], edges = [];
    var start = graph.start;
    col[start] = 0; rows[0] = [start]; order.push(start);
    var q = [start];
    while (q.length) {
      var id = q.shift();
      if (!graph.open[id]) continue;
      var ch = childrenOf(d, id);
      ch.slice(0, MAXC).forEach(function (r) {
        var c = r[1];
        if (col[c] === undefined) {
          col[c] = col[id] + 1;
          (rows[col[c]] = rows[col[c]] || []).push(c);
          order.push(c); q.push(c);
          edges.push([id, c, r[0], false]);
        } else edges.push([id, c, r[0], true]);
      });
      if (ch.length > MAXC) {
        var more = 'more:' + id;
        col[more] = col[id] + 1;
        (rows[col[more]] = rows[col[more]] || []).push(more);
        edges.push([id, more, '', false]);
      }
    }
    var at = {};
    rows.forEach(function (ids, c) { ids.forEach(function (id, r) { at[id] = [10 + c * (W + GX), 10 + r * (H + GY)]; }); });
    var maxRows = 0; rows.forEach(function (r) { maxRows = Math.max(maxRows, r.length); });
    var width = 20 + rows.length * (W + GX), height = 20 + maxRows * (H + GY);
    var svg = s('svg', { width: width, height: height, viewBox: '0 0 ' + width + ' ' + height });
    edges.forEach(function (e) {
      var a = at[e[0]], b = at[e[1]];
      var x1 = a[0] + W, y1 = a[1] + H / 2, x2 = b[0], y2 = b[1] + H / 2;
      if (e[3] && b[0] <= a[0]) { x2 = b[0] + W / 2; y2 = b[1] + (b[1] > a[1] ? 0 : H); }
      svg.appendChild(s('path', { cls: 'edge' + (e[3] ? ' back' : ''), d: 'M' + x1 + ',' + y1 + ' C' + (x1 + 50) + ',' + y1 + ' ' + (x2 - 50) + ',' + y2 + ' ' + x2 + ',' + y2 }));
      if (e[2]) svg.appendChild(s('text', { cls: 'elabel', x: e[3] ? (x1 + x2) / 2 : x2 - 4, y: e[3] ? (y1 + y2) / 2 - 3 : y2 - 4, 'text-anchor': e[3] ? 'middle' : 'end', text: cut(edgeName(e[2]), 18) }));
    });
    order.concat(Object.keys(at).filter(function (k) { return k.indexOf('more:') === 0; })).forEach(function (id) {
      var p = at[id];
      if (!p) return;
      var g;
      if (typeof id === 'string' && id.indexOf('more:') === 0) {
        var pid = id.slice(5); pid = /^\d+$/.test(pid) ? parseInt(pid, 10) : pid;
        g = s('g', { cls: 'node perm', transform: 'translate(' + p[0] + ',' + p[1] + ')', onclick: function () { go({ focus: pid, sel: typeof pid === 'number' ? pid : null }); } });
        g.appendChild(s('rect', { width: W, height: H, rx: 6 }));
        g.appendChild(s('text', { x: 8, y: 23, text: '… あと ' + (childrenOf(d, pid).length - MAXC) + ' 個 (ここを起点に)' }));
        svg.appendChild(g);
        return;
      }
      if (typeof id === 'string') {
        g = s('g', { cls: 'node root', transform: 'translate(' + p[0] + ',' + p[1] + ')' });
        g.appendChild(s('rect', { width: W, height: H, rx: 6 }));
        var rt = rootByKey(d, id);
        g.appendChild(s('text', { x: 8, y: 16, text: cut(rt ? rootName(d, rt) : id, 28) }));
        g.appendChild(s('text', { cls: 'kind', x: 8, y: 31, text: rt && rt.kind === 'frame' ? (d.contexts[rt.context - 1].frames[rt.frame].location || '') : '' }));
        svg.appendChild(g);
        return;
      }
      var o = obj(d, id), perm = isPerm(o);
      var n = childrenOf(d, id).length;
      g = s('g', { cls: 'node' + (perm ? ' perm' : '') + (st.sel === id ? ' sel' : ''), transform: 'translate(' + p[0] + ',' + p[1] + ')' });
      g.addEventListener('click', function () {
        if (!perm && n) { if (graph.open[id]) delete graph.open[id]; else graph.open[id] = true; }
        st.sel = id;
        render();
      });
      g.appendChild(s('rect', { width: W, height: perm ? 24 : H, rx: perm ? 12 : 6 }));
      g.appendChild(s('text', { x: 8, y: perm ? 16 : 16, text: cut(L(o), 28) }));
      if (!perm) {
        g.appendChild(s('text', { cls: 'kind', x: 8, y: 31, text: '#' + o.id + ' ' + (o['class'] && o.kind !== 'env' ? cut(obj(d, o['class']).class_name || L(obj(d, o['class'])), 20) : KIND[o.kind] || o.kind) }));
        if (n) g.appendChild(s('text', { cls: 'more', x: W - 8, y: 31, 'text-anchor': 'end', text: graph.open[id] ? '−' : '+' + n }));
      }
      var t = s('title'); t.textContent = whereText(d, o) + '\nパス: ' + o.path; g.appendChild(t);
      svg.appendChild(g);
    });
    return svg;
  }

  function details(d, id) {
    var box = h('div', { cls: 'card' });
    if (!id || !obj(d, id)) { add(box, h('div', { cls: 'empty', text: '箱を選ぶと中身が出る' })); return box; }
    var o = obj(d, id), t = h('table');
    var cn = o['class'] && o.kind !== 'env' ? (obj(d, o['class']).class_name || L(obj(d, o['class']))) : null;
    add(box, h('h3', { text: cut(L(o), 60) }), h('div', { cls: 'muted', text: '#' + o.id + (cn ? ' ・ ' + cn + ' のオブジェクト' : '') + (o.frozen ? ' ・ frozen' : '') + ' ・ イメージの中で ' + o.bytes + ' バイト' }));
    add(box, h('div', { text: whereText(d, o) }), h('div', { cls: 'muted mono', text: 'パス: ' + o.path, title: 'ルートからのパス (2 つのイメージの差はこれで対応を取る)' }));
    if (o['class']) add(t, row('クラス', value(d, { ref: o['class'] })));
    var iv = function (list) { (list || []).forEach(function (p) { add(t, row(p[0], value(d, p[1]))); }); };
    switch (o.kind) {
      case 'permanent': case 'permanent_proc':
        add(t, row('名前', h('span', { cls: 'mono', text: o.name })));
        add(t, row('', h('span', { cls: 'muted', text: '読む側の同じ名前のものに結ぶ (中身は運ばない)' })));
        if (o.irep) add(t, row('irep', '@' + o.irep));
        break;
      case 'string':
        add(t, row('中身', h('pre', { text: o.text !== undefined ? o.text : '<' + o.hex + '>' })));
        if (o.binary) add(t, row('', 'binary'));
        break;
      case 'array': case 'struct':
        o.items.forEach(function (v, i) { add(t, row('[' + i + ']', value(d, v))); }); iv(o.ivars); break;
      case 'hash':
        o.pairs.forEach(function (p) { add(t, h('tr', null, h('td', { cls: 'k' }, value(d, p[0])), h('td', null, '=> ', value(d, p[1])))); });
        if (o['default'].has_default) add(t, row('既定値', o['default'].proc ? 'Proc' : 'あり'));
        iv(o.ivars); break;
      case 'set':
        (o.items || []).forEach(function (v, i) { add(t, row('<' + i + '>', value(d, v))); }); iv(o.ivars); break;
      case 'range': add(t, row('始め', value(d, o.begin)), row('終わり', value(d, o.end)), row('終わりを含む', o.exclusive ? 'いいえ' : 'はい')); break;
      case 'exception': add(t, row('メッセージ', value(d, o.message)), row('バックトレース', value(d, o.backtrace))); iv(o.ivars); break;
      case 'class': case 'module': case 'singleton_class': case 'origin':
        if (o.class_name) add(t, row('名前', h('span', { cls: 'mono', text: o.class_name })));
        add(t, row('スーパークラス', o.superclass ? value(d, { ref: o.superclass }) : '-'));
        iv(o.ivars);
        o.methods.forEach(function (mm) { add(t, row('#' + mm.name, mm.kind === 'proc' ? value(d, { ref: mm.proc }) : h('span', { cls: 'mono', text: mm.kind === 'cfunc' ? 'C ' + mm.cfunc : 'undef' }))); });
        break;
      case 'iclass': add(t, row('スーパークラス', o.superclass ? value(d, { ref: o.superclass }) : '-')); break;
      case 'proc':
        var b = o.body;
        add(t, row('本体', h('span', { cls: 'mono', text: b.kind === 'irep' ? 'irep @' + b.irep + (d.ireps[b.irep - 1].file ? ' (' + d.ireps[b.irep - 1].file + ':' + d.ireps[b.irep - 1].line + ')' : '') : (b.kind === 'cfunc' ? 'C ' + b.name : 'alias ' + b.method) })));
        add(t, row('フラグ', [['lambda', 'lambda'], ['orphan', 'return の戻り先がもう無い'], ['scope', 'メソッドの本体']].filter(function (k) { return o[k[0]]; }).map(function (k) { return k[1]; }).join('、') || '-'));
        if (o.upper) add(t, row('外側', value(d, { ref: o.upper })));
        if (o.env) add(t, row(o.env.kind === 'env' ? '環境' : '定義先', value(d, { ref: o.env.ref })));
        break;
      case 'env':
        add(t, row('メソッド', o.method || '-'));
        if (o.on_stack) add(t, row('置き場所', 'コンテキスト ' + o.context + ' のスタックの ' + o.position + ' (そのフレームのレジスタそのもの)'));
        else (o.values || []).forEach(function (v, i) { add(t, row('[' + i + ']', value(d, v))); });
        break;
      case 'data':
        add(t, row('型', o.type || '(まだ無い)'));
        if (o.decoded) Object.keys(o.decoded).forEach(function (k) { add(t, row(k, h('span', { cls: 'mono', text: JSON.stringify(o.decoded[k]) }))); });
        else if (o.data_hex) add(t, row('中身', h('span', { cls: 'mono', text: cut(o.data_hex, 80) })));
        iv(o.ivars); break;
      case 'task':
        var tk = o.task ? d.scheduler.tasks[o.task - 1] : null;
        if (tk) add(t, row('Task', tk.key + ' ・ ' + taskState(tk) + ' ・ ' + queueText(tk)), row('待ち', waitText(d, tk) || '-'));
        iv(o.ivars); break;
      case 'fiber': add(t, row('コンテキスト', o.context ? String(o.context) : '(まだ無い)')); break;
      case 'break': add(t, row('種類', { 'break': 'break', jump: 'jump (ensure を通る)', stop: '止まる' }[o.tag] || o.tag), row('行き先のフレーム', String(o.frame)), row('値', value(d, o.value))); break;
      default:
        [['value', '値'], ['numerator', '分子'], ['denominator', '分母'], ['state', '乱数の状態'], ['seed', 'シード']].forEach(function (k) { if (o[k[0]] !== undefined) add(t, row(k[1], String(o[k[0]]))); });
        iv(o.ivars);
    }
    add(box, t);
    var inc = incoming(st.img)[id] || [];
    if (inc.length) {
      var it = h('table');
      inc.slice(0, 50).forEach(function (r) {
        var rr = typeof r[1] === 'number' ? null : rootByKey(d, r[1]);
        add(it, row(r[0] ? edgeName(r[0]) : '(値)', typeof r[1] === 'number' ? objLink(obj(d, r[1]), st.img) : h('span', { title: r[1], text: rr ? rootName(d, rr) : r[1] })));
      });
      add(box, h('h3', { text: 'ここを指しているもの (' + inc.length + ')' }), it);
    }
    return box;
  }

  // ---- the diff ----
  function viewDiff(m) {
    var df = DIFF, A = DOCS[0], B = DOCS[1];
    var byPath = [{}, {}];
    DOCS.forEach(function (d, i) { d.objects.forEach(function (o) { byPath[i][o.path] = o; }); });
    var wrap = h('div', { cls: 'diff' });
    add(m, wrap);
    if (df.same) { add(wrap, h('p', { text: '差はない。' })); return; }
    var ob = df.objects;
    add(wrap, h('p', null, 'A → B。オブジェクトは番号でなく、ルート (グローバル変数・定数・フレーム・Task) からのパスで対応を取っている。 ',
      h('span', { cls: 'chg', text: '変わった ' + ob.changed.length }), ' ・ ', h('span', { cls: 'add', text: '増えた ' + ob.added.length }), ' ・ ',
      h('span', { cls: 'del', text: '消えた ' + ob.removed.length }), ' ・ 移った ' + ob.moved.length));
    // one side of a change: an object of that image as a link, else the words
    function side(img, field, x, ref, objectFields) {
      if (ref) return objLink(obj(DOCS[img], ref), img);
      if (objectFields && typeof x === 'string' && byPath[img][x]) return objLink(byPath[img][x], img);
      return h('span', { cls: 'mono', text: fieldValue(field, x) });
    }
    function fieldTable(list, objectFields) {
      var t = h('table');
      list.forEach(function (f) {
        add(t, h('tr', null, h('td', { cls: 'k', text: objectFields ? edgeName(f.field) : (FIELD[f.field] || f.field) }),
          h('td', { cls: 'a' }, side(0, f.field, f.a, f.a_ref, objectFields)), h('td', { text: '→' }),
          h('td', { cls: 'b' }, side(1, f.field, f.b, f.b_ref, objectFields))));
      });
      return t;
    }
    function objText(img, id, path) {
      var o = id ? obj(DOCS[img], id) : byPath[img][path];
      return h('a', { cls: 'link', title: 'パス: ' + path, onclick: function () { go({ view: 'objects', img: img, focus: o.id, sel: o.id }); } }, whereText(DOCS[img], o));
    }
    function ctxHead(key) {
      var c = B.contexts.filter(function (x) { return x.key === key; })[0] || A.contexts.filter(function (x) { return x.key === key; })[0];
      return c ? ctxName(B.contexts.indexOf(c) >= 0 ? B : A, c) : key;
    }
    if (df.header.length) add(wrap, h('h2', { text: 'ヘッダ' }), fieldTable(df.header));
    if (df.variables.length) {
      add(wrap, h('h2', { text: 'グローバル変数・定数・メソッド' }), fieldTable(df.variables.map(function (v) {
        return { field: v.key.replace(/^Object::/, '').replace(/^Object#/, '#'), a: v.a, b: v.b, a_ref: v.a_ref, b_ref: v.b_ref };
      })));
    }
    var cs = df.contexts.filter(function (c) { return c.change !== 'same'; });
    if (cs.length) {
      add(wrap, h('h2', { text: 'コールスタック' }));
      cs.forEach(function (c) {
        var card = h('div', { cls: 'card' }, h('h3', { title: c.key }, ctxHead(c.key), c.change !== 'changed' ? h('span', { cls: c.change === 'added' ? 'badge run' : 'badge warn', text: c.change === 'added' ? '増えた' : '消えた' }) : null));
        if (c.fields && c.fields.length) add(card, fieldTable(c.fields));
        (c.frames || []).slice().reverse().forEach(function (f) {
          if (f.change === 'same') return;
          if (f.change === 'called') add(card, h('div', { cls: 'row add mono', text: '+ 呼ばれたフレーム ' + f.index + ': ' + f.b }));
          else if (f.change === 'returned') add(card, h('div', { cls: 'row del mono', text: '− 戻ったフレーム ' + f.index + ': ' + f.a }));
          else {
            add(card, h('div', { cls: 'row chg mono', text: '~ フレーム ' + f.index + ': ' + (f.a === f.b ? f.a : f.a + ' → ' + f.b) + (f.pc ? ' (pc ' + f.pc[0] + ' → ' + f.pc[1] + ')' : '') }));
            var rows = (f.self ? [{ field: 'self', a: f.self[0], b: f.self[1] }] : []).concat(f.locals.map(function (l) {
              return l.contents_changed ? { field: l.name, a: '(中身が変わった)', b: l.b, b_ref: l.b_ref } : { field: l.name, a: l.a, b: l.b, a_ref: l.a_ref, b_ref: l.b_ref };
            }));
            if (rows.length) add(card, fieldTable(rows));
          }
        });
        add(wrap, card);
      });
    }
    if (df.tasks.length) {
      add(wrap, h('h2', { text: 'Task' }));
      df.tasks.forEach(function (t) {
        add(wrap, h('div', { cls: 'card' }, h('h3', null, 'Task ' + t.key, t.change !== 'changed' ? h('span', { cls: 'badge', text: t.change === 'added' ? '増えた' : '消えた' }) : null), t.fields ? fieldTable(t.fields) : null));
      });
    }
    if (df.scheduler.length) add(wrap, h('h2', { text: 'スケジューラ' }), fieldTable(df.scheduler));
    function objectLists(sel, suffix) {
      var ch = ob.changed.filter(sel), ad = ob.added.filter(sel), rm = ob.removed.filter(sel), mv = ob.moved.filter(sel);
      if (ch.length) {
        add(wrap, h('h2', { text: '変わったオブジェクト' + suffix }));
        ch.forEach(function (c) {
          var o = obj(B, c.b), cn = o['class'] ? (obj(B, o['class']).class_name || '') : c.kind;
          add(wrap, h('div', { cls: 'card' }, h('div', null, objText(1, c.b, c.path), h('span', { cls: 'muted', text: '  ' + cn + ' ・ A #' + c.a + ' / B #' + c.b })), fieldTable(c.fields, true)));
        });
      }
      plain('増えたオブジェクト (B だけ)' + suffix, ad, 'add', 1, 'b');
      plain('消えたオブジェクト (A だけ)' + suffix, rm, 'del', 0, 'a');
      if (mv.length) {
        var mt = h('table');
        mv.forEach(function (x) { add(mt, h('tr', null, h('td', null, objText(0, x.a, x.a_path)), h('td', { text: '→' }), h('td', null, objText(1, x.b, x.b_path)), h('td', { cls: 'mono', text: cut(L(obj(DOCS[1], x.b)), 40) }))); });
        add(wrap, h('h2', { text: '置き場所が変わったオブジェクト (中身が同じもの)' + suffix }), mt);
      }
    }
    function plain(title, list, cls, img, idk) {
      if (!list.length) return;
      var t = h('table');
      list.forEach(function (x) { add(t, h('tr', null, h('td', { cls: cls, text: cls === 'add' ? '+' : '−' }), h('td', { cls: 'mono', text: cut(L(obj(DOCS[img], x[idk])), 60) }), h('td', null, objText(img, x[idk], x.path)))); });
      add(wrap, h('h2', { text: title }), t);
    }
    objectLists(function (x) { return !x.temporary; }, '');
    objectLists(function (x) { return x.temporary; }, ' ・ 一時レジスタからしか届かないもの');
  }

  // ---- the image as it is ----
  function viewImage(m) {
    var d = doc(), hd = d.header;
    var t = h('table');
    Object.keys(hd).forEach(function (k) { add(t, row(FIELD[k] || k, String(yesNo(hd[k])))); });
    add(m, h('h2', { text: 'ヘッダ' }), t);
    if (d.warnings.length) {
      add(m, h('h2', { text: '注意' }));
      d.warnings.forEach(function (w) { add(m, h('div', { cls: 'badge warn', text: w })); });
    }
    if (d.error) add(m, h('h2', { text: '壊れている所' }), h('pre', { text: JSON.stringify(d.error, null, 1) }));
    var kinds = {}, kb = {};
    d.objects.forEach(function (o) { kinds[o.kind] = (kinds[o.kind] || 0) + 1; kb[o.kind] = (kb[o.kind] || 0) + o.bytes; });
    var kt = h('table', null, h('tr', null, h('th', { text: '種類' }), h('th', { text: '個数' }), h('th', { text: 'バイト' })));
    Object.keys(kinds).sort(function (a, b) { return kb[b] - kb[a]; }).forEach(function (k) { add(kt, h('tr', null, h('td', { text: KIND[k] || k }), h('td', { text: kinds[k] }), h('td', { text: kb[k] }))); });
    var ib = 0; d.ireps.forEach(function (ir) { ib += ir.bytes; });
    add(m, h('h2', { text: '表' }), h('p', null, 'シンボル ' + d.symbols.length + ' ・ irep ' + d.ireps.length + ' (' + ib + ' バイト) ・ オブジェクト ' + d.objects.length +
      ' ・ 変更 ' + d.changes.length + ' ・ コンテキスト ' + d.contexts.length + ' ・ Task ' + d.scheduler.tasks.length), kt);
    add(m, h('h2', { text: '任意のセクション' }));
    if (!d.sections.length) add(m, h('div', { cls: 'empty', text: 'なし' }));
    d.sections.forEach(function (sec) {
      var text = sec.tag === 'DBG0' ? sec.ireps.length + ' 個の irep の行番号' : (sec.unknown ? '知らないセクション (読み飛ばした): ' + sec.length + ' バイト ' + sec.head_hex :
        (sec.tag === 'GCS0' ? 'GC の設定: ' + Object.keys(sec).filter(function (k) { return k !== 'tag' && k !== 'offset' && k !== 'length'; }).map(function (k) { return k + '=' + sec[k]; }).join(' ') : JSON.stringify(sec)));
      add(m, h('div', { cls: 'mono', text: sec.tag + ' (' + sec.offset + ' バイト目): ' + text }));
    });
    var it = h('table', null, h('tr', null, ['irep', '種類', 'ハッシュ', '場所', 'locals', 'regs', '命令のバイト'].map(function (x) { return h('th', { text: x }); })));
    d.ireps.forEach(function (ir) {
      add(it, h('tr', null, h('td', { text: '@' + ir.id }), h('td', { text: IREP_KIND[ir.kind] || ir.kind }), h('td', { cls: 'mono', text: ir.hash + (ir.hash_ok === false ? ' 合わない' : '') }),
        h('td', { cls: 'mono', text: ir.file ? ir.file + ':' + ir.line : '-' }), h('td', { text: ir.nlocals === undefined ? '-' : ir.nlocals }),
        h('td', { text: ir.nregs === undefined ? '-' : ir.nregs }), h('td', { text: ir.ilen === undefined ? '-' : ir.ilen })));
    });
    add(m, h('h2', { text: 'irep' }), it);
  }

  readHash();
  window.addEventListener('hashchange', function () { readHash(); graph = null; render(); });
  render();
})();
