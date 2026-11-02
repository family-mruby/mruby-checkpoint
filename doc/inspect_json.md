# イメージを読んだ JSON (inspect JSON) 版 1

> 状態: P5 で実装済 | 更新: 2026-11-03 | リーダーは `tools/lib/checkpoint_image.rb` ただ 1 つ。形の検査は `tools/lib/checkpoint_schema.rb`

checkpoint のイメージ (`doc/format_v1.md`) を、形式を知らなくても読める JSON にしたもの。CLI
(`tools/checkpoint_inspect.rb`)・1 ファイルの HTML・2 つのイメージの差はすべてこの JSON から作る。
後で作る GUI (Asterism の console でも端末のアプリでも) は、この文書と JSON だけで作れることを
目標にする。

```
ruby tools/checkpoint_inspect.rb x.ckpt --json [--pretty] [--no-code]   # イメージ 1 つ
ruby tools/checkpoint_inspect.rb diff a.ckpt b.ckpt --json               # 2 つのイメージの差 (§9)
```

HTML (`--html`) には、命令列を除いた JSON (`--no-code` と同じ) が
`<script type="application/json" id="checkpoint-data">` に `{"docs": [JSON, ...], "diff": 差か null}`
の形で入っている (`</` は `<\/` と書く)。ページから JSON だけ取り出して使ってよい。

## 1. 版

- 先頭の `inspect_json` が版 (いまは 1)。差の JSON は `inspect_diff` (いまは 1)。
- フィールドを足すだけなら版は上げない。読む側は知らないフィールドを読み飛ばすこと。フィールドの意味を変える・フィールドを
  消すときに版を上げる。
- イメージの形式の版は `header.format` (いまは 1)。リーダーは v1 のイメージだけを読む。

## 2. 2 種類のフィールド

- **イメージそのままのフィールド**: イメージの記録を、順も中身もそのまま JSON にしたもの。番号はイメージの中の番号
  (§3)。シンボルは名前に、irep の番号はそのまま。
- **導出フィールド**: リーダーがイメージ全体から計算したもの。形式を知らなくても絵が描けるようにするための
  フィールドで、どれもイメージそのままのフィールドから計算し直せる。下の表の「導出フィールド」の列に挙げる。

| 場所 | 導出フィールド |
|---|---|
| `ireps[]` | `hash_ok`、`file`、`line` |
| `objects[]` | `label`、`class_name`、`refs`、`path`、`path_root`、`temporary` |
| `contexts[]` | `role`、`task`、`fiber_objects`、`current`、`key`、`backtrace`、`activity` |
| `contexts[].frames[]` | `irep`、`file`、`line`、`location`、`key`、`not_started`、`placeholder`、`self`、`self_register`、`locals`、`captured` |
| `scheduler.tasks[]` | `main`、`name_text`、`running`、`host`、`queue_index`、`key` |
| 全体 | `roots`、`warnings` |

## 3. 番号と値

- オブジェクト・irep・コンテキスト・Task は、イメージの中の表の 1 から始まる番号で指す (配列の添字 + 1)。
  イメージごとに振り直されるので、2 つのイメージの間で番号を比べてはいけない (比べるときは `path` と
  `key`。§8)。「無し」は `null`。
- **値** (レジスタ・変数・要素に入るもの) は次のどれか:

  | JSON | 意味 |
  |---|---|
  | `null` | nil |
  | `true` / `false` | true / false |
  | 数 | Integer (±(2^53−1) の内側) |
  | `{"int": "123…"}` | それより大きい 64 bit の Integer (10 進の文字列) |
  | `{"float": 1.5}` | Float。無限と非数は `{"float": "Infinity"}`・`"-Infinity"`・`"NaN"` |
  | `{"sym": "name"}` | Symbol |
  | `{"ref": 12}` | オブジェクト 12 |
  | `{"undef": true}` | VM の内部の「値なし」(レジスタにだけ現れる) |

- 文字列の中身 (String のオブジェクト、プールの文字列) は、UTF-8 として正しければ
  `{"text": "..."}` (オブジェクトでは `text` のフィールド)、正しくなければ `{"hex": "..."}`。名前
  (シンボル・ファイル名・関数名) は常に文字列で、UTF-8 でないバイトは `\xHH` と書く。

## 4. 全体

| フィールド | 中身 |
|---|---|
| `inspect_json` | 版 (1) |
| `source` | `{file, bytes}`: イメージのファイル名 (パスなし) と大きさ |
| `header` | `{magic, format, implementation, bytecode, integer_bits, float_bits, utf8, pointer_bits}` (形式 §3 のヘッダ) |
| `symbols` | シンボル表 (名前の配列。イメージの中の番号 i は添字 i−1) |
| `ireps` | irep 表 (§5) |
| `objects` | オブジェクト表 (§6) |
| `changes` | 変更表 (§6.3) |
| `contexts` | コンテキストの表 (§7) |
| `current_context` | 書き出したとき実行中だったコンテキストの番号 |
| `scheduler` | スケジューラと Task の表 (§7.3) |
| `sections` | 任意のセクション (§7.4) |
| `roots` | ルートの一覧 (§8)。導出フィールド |
| `warnings` | 注意の文の配列 (知らないセクション、irep のハッシュが合わない、ルートから届かないオブジェクトなど)。正常なイメージでは空 |
| `error` | 壊れたイメージのときだけ `{offset, where, message}` (§10)。ふだんは `null` |
| `omitted` | (あれば) 省いたフィールド。HTML では `["ireps.iseq"]` |

## 5. irep (`ireps[]`)

| フィールド | 中身 |
|---|---|
| `id`、`kind` | 番号と種類: `carried` (運ぶ)・`permanent` (ハッシュだけ)・`permanent_carried` (mrblib のもので中身も運ぶ)。`kind_code` はイメージの値 (0・1・2) |
| `hash` | 内容のハッシュ (16 進 16 桁) |
| `nlocals`、`nregs`、`ilen` | ローカル変数の数 (self を含む)、レジスタの数、命令列のバイト数 |
| `catch` | 例外処理の表 `[{type: "rescue"/"ensure", begin, end, target}]` |
| `pool` | 定数プール。各要素は `{"string": {text か hex}}`・`{"int": 整数}`・`{"float": …}`・`{"bigint_hex": "..."}` |
| `syms` | 命令が使うシンボルの名前 (無しは null) |
| `children` | 子の irep の番号 |
| `locals` | ローカル変数の名前 (R1 から。無名は null) |
| `iseq` | 命令列 (16 進)。`--no-code` と HTML では省く |
| `bytes` | イメージの中でこの記録が占めるバイト数 |
| `hash_ok` | (導出) 中身から計算し直したハッシュが `hash` と合うか。permanent (中身なし) には無い |
| `file`、`line` | (導出) `DBG0` の行番号から: 最初のファイルと、最初に現れる行。行番号が無ければ null |

## 6. オブジェクト (`objects[]`)

### 6.1 どのオブジェクトにもあるフィールド

| フィールド | 中身 |
|---|---|
| `id` | 番号 |
| `kind` | 種類の名前 (下の表)。`kind_code` はイメージの種類の値 |
| `bytes` | イメージの中でこの記録が占めるバイト数 |
| `label` | (導出) 短い表示 (`"c1:1"`、`[1, 2]`、`Hash(3)`、`#<Foo>`、`Task "c1"`、`Proc (x.rb:12)`)。permanent は名前を読みやすくしたもの (`T` は `main`、`S:Foo` は `#<Class:Foo>`) |
| `class_name` | (導いた。クラス・モジュール・特異クラス・取り込みクラス・origin と permanent のクラスだけ) クラスの名前 |
| `refs` | (導出) このオブジェクトが指すオブジェクト `[[辺の名前, 番号], ...]`。図の辺。名前は ivar・定数 (`@a`、`::Foo`)、要素 `[0]`、Hash の値 `{:key}` とキー `(key 0)`、Set の要素 `<0>`、メソッド `#name`、`superclass`、`upper`、`env`、`target_class`、`begin`・`end`・`message`・`backtrace`・`value`、Task の `name`・`value`・`waits on`、最後にクラス `class` |
| `path` | (導出) ルートからのパス (§8)。permanent は `=名前` |
| `path_root` | (導出) パスの始まりのルート (`roots` の添字。0 から)。permanent とルートから届かないものは null。人に見せる置き場所 (「q、Task "x" の本体のローカル変数」) は、このルートと `path` の残り (ルートの `key` の後ろ) から作る |
| `temporary` | (導出) 変数でない一時レジスタからしか届かないなら true |

permanent (`permanent`、`permanent_proc`) 以外は、続けて `class` (クラスのオブジェクトの番号) と
`frozen` を持つ。

### 6.2 種類ごとのフィールド

| `kind` | フィールド |
|---|---|
| `permanent` | `name` (形式 §8 の名前: `Object`、`T`、`S:T`、`M:Kernel#loop`、`V:Task::Queue@WAIT_RETRY` …) |
| `permanent_proc` | `name` (`M:` の名前)、`irep` (フレームが動かしている mrblib の irep) |
| `object` | `ivars` |
| `string` | `text` か `hex`、`binary` (`String#b` のフラグ) |
| `array` | `items` (値の配列) |
| `hash` | `default` `{has_default, proc}`、`pairs` (`[キー, 値]` の配列)、`ivars` |
| `range` | `begin`、`end`、`exclusive`、`uninitialized` |
| `exception` | `message`、`backtrace`、`ivars` |
| `class`・`module`・`singleton_class` | `superclass`、`instance_type`、`flags` `{no_allocate, inherited, prepended, has_method_table}`、`ivars` (定数・クラス変数・内部用の変数 `__classname__` など)、`methods` |
| `iclass` | `superclass` (`class` が取り込んだモジュール) |
| `proc` | `lambda`、`orphan`、`scope`、`body` (`{kind: "irep", irep}`・`{kind: "cfunc", name, aspec}`・`{kind: "alias", method}`)、`upper`、`env` (`{kind: "env"/"class", ref}` か null) |
| `env` | `method`、`length`、`block_reg`、`visibility`、`on_stack`、スタック上なら `context`・`position` (そのコンテキストの `stack` の添字。そのフレームの R0)、切り離し済みなら `values` |
| `data` | `type` (`Time`・`Task::Queue` …。空は未初期化)、`data_hex` (中身のバイト)、`decoded` (知っている型なら中身をフィールドにしたもの: Time は `{sec, nsec, zone}`、Task::Queue は `{closed}` など)、`ivars` |
| `bigint` | `negative`、`hex`、`value` (10 進の文字列) |
| `struct` | `items`、`ivars` |
| `set` | `uninitialized`、`items`、`ivars` |
| `rational` | `numerator`、`denominator` (10 進の文字列) |
| `complex` | `real`、`imaginary` (`{"float": …}`) |
| `break` | `tag` (`break`・`jump`・`stop`)、`frame`、`value` |
| `random` | `state` (16 進)、`seed`、`ivars` |
| `origin` | `superclass`、`has_methods`、`methods` |
| `task` | `task` (Task の表の番号)、`ivars` |
| `fiber` | `context` (コンテキストの番号。null は未初期化) |

`ivars` は `[[名前, 値], ...]` (イメージの順)。`methods` の各要素は `{name, visibility, kind}` で、`kind`
が `proc` なら `proc` (番号)、`cfunc` なら `cfunc` (関数の名前) と `aspec`、`undef` はフィールドなし。

### 6.3 変更表 (`changes[]`)

permanent なものへの変更 (形式 §9)。各要素は `kind` (`define_method`・`remove_method`・
`set_variable`・`remove_variable`・`set_superclass`・`set_global`・`remove_global`・
`random_state`・`class_flags`・`contents`) と `kind_code`、それに種類ごとに `target` (permanent の
オブジェクト)・`method`・`name`・`value`・`superclass`・`state`・`seed`・`prepended`・`frozen`・
`type`・`text`/`hex`・`items`・`pairs`。プログラムが作ったトップレベルのクラスは `Object` への
`set_variable`、トップレベルの `def` は `Object` への `define_method` として現れる。

## 7. 実行の状態

### 7.1 コンテキスト (`contexts[]`)

| フィールド | 中身 |
|---|---|
| `id`、`state` | 番号と、イメージのままの状態 (mruby の `mrb_fiber_state`: `created`・`running`・`resumed`・`suspended`・`transferred`・`terminated`。`state_code` はイメージの値)。意味を持つのは Fiber のコンテキストだけ (下の `activity`) |
| `vmexec` | C から resume されたフラグ |
| `prev` | このコンテキストを resume したコンテキスト (戻る先) |
| `fiber` | このコンテキストの Fiber のオブジェクト |
| `stack` | スタック (レジスタの並び。値の配列) |
| `frames` | フレーム (§7.2)。**下 (一番外) から上** の順 |
| `role` | (導出) `root` (プログラムの本体)・`task`・`fiber`・`unknown` |
| `task` | (導出) このコンテキストの Task の番号 |
| `fiber_objects` | (導出) このコンテキストを指す Fiber のオブジェクト |
| `current` | (導出) 書き出したとき実行中だったコンテキストか |
| `key` | (導出) イメージをまたいで同じコンテキストを指す名前: `root`、`task "名前"` (名前の無い Task は `task @ファイル:行`)、`fiber @ファイル:行` (Fiber の本体のブロックの場所)。重なれば `#2` を付ける |
| `backtrace` | (導出) このコンテキストで例外を上げたら mruby が出すバックトレース (**上から**)。`"x.rb:13:in deep"` の形。未開始のフレームは始まる行、`placeholder` のフレームは入れない |
| `activity` | (導出) このコンテキストがしていること。`current` (書き出したとき実行中だった)・`not_started` (Fiber がまだ resume されていない)・`yielded` (`Fiber.yield` で中断中)・`resuming` (resume した Fiber を待っている)・`transferring` (transfer して離れた)・`finished` (終了)・`task` (Task のコンテキスト: 中身は Task の `state` を見る)・`scheduler` (本体が C の関数 `Task.run`・`Task.pass` でスケジューラを回している)・`no_frames` (本体にフレームが無い: 埋め込む側がスケジューラを回す)・`other` |

**`state` を人に見せない理由**: コンテキストの状態は mruby-fiber しか更新しない。mruby-task は Task のコンテキストを
作るときに `created`、Task が止まるときに `terminated` (`MRB_TASK_STOPPED` は `MRB_FIBER_TERMINATED`
の別名) にするだけで、実行中の Task も `created` のまま。本体のコンテキストは VM を開いたときの `created`
から、Fiber が戻ってきたときに `running` になるだけ。だから本体と Task では `resumed`・`transferred`
(Fiber を待っている) と Task の `terminated` しか意味を持たない。`activity` はそれを踏まえて決める。

### 7.2 フレーム (`frames[]`)

| フィールド | 中身 |
|---|---|
| `index` | コンテキストの中の位置 (0 が一番外) |
| `kind` | `ruby`、または `cfunc` (コンテキストの切り替えを待っている C の関数のフレーム。`cfunc` にその名前: `F:Fiber#resume`・`F:S:Task#run` など) |
| `method` | メソッド名 (トップレベルやブロックでは null のことがある) |
| `proc` | 実行中の Proc のオブジェクト |
| `reg_base` | このフレームの R0 の `stack` の添字 |
| `pc` | 命令列の中の位置 (null は無し) |
| `argc`、`kwargc`、`cci`、`visibility`、`block`、`env` | 形式 §10 のまま (`env` は `{kind: "env"/"class", ref}`) |
| `irep` | (導出) 実行中の irep |
| `file`、`line` | (導出) いま実行中の (または呼び出しから戻るのを待っている) 行。pc の 1 つ前の命令の行。pc が 0 のフレームは、始まる行 (pc 0 の命令の行。ブロックなら `do` の行) |
| `location` | (導出) バックトレースの 1 行 (`file:line:in method`)。mruby がバックトレースに出さないフレームは null。pc が 0 のフレームは始まる行 (mruby 自身は pc の 1 つ前を引くので `(unknown):0` と出す) |
| `key` | (導出) コンテキストの `key` + `/番号:メソッド` (`root/1:work`) |
| `not_started` | (導出) pc が 0 (まだ 1 命令も動いていない) |
| `placeholder` | (導出) まだ resume されていない Fiber のフレーム 1。フレーム 0 の写しで、`Fiber#resume` のフレームの代わりに積まれている。最初の resume で、resume から戻るときに VM が外す (mruby-fiber の `init_fiber` と `vm.c` の C の関数の戻り)。中身はフレーム 0 と同じ |
| `self` | (導出) R0 の値。ただし、まだ resume されていない Fiber のフレームでは、始まるときの self (ブロックの環境の self、無ければ main。mruby-fiber が最初の resume で R0 に入れるもの) |
| `self_register` | (導いた。上の場合だけ) そのときの R0 の値。`Fiber.new` の受け手 (Fiber のオブジェクト) が写されている |
| `locals` | (導出) ローカル変数 `[{name, reg, value}]` (無名は name が null) |
| `captured` | (導出) ブロックが外から見ている変数。外側へ 1 段ずつ `[{env, method, on_stack, context, frame, variables: [{name, value}]}]`。`on_stack` なら、その変数はコンテキスト `context` のフレーム `frame` のレジスタそのもの |

`location` と `backtrace` は mruby の `backtrace.c` と同じ規則で作る: Ruby のフレームは行番号があれば
`file:line`、メソッド名があれば `:in 名前` を付ける。C の関数のフレームは、名前があれば、その下で
いちばん近い Ruby のフレームの場所に `:in 名前` を付ける。行が引けなければ `(unknown):0`。
試験 (`tools/test_inspect.rb`) で、mruby 自身のバックトレースと一致することを確かめている。

### 7.3 スケジューラ (`scheduler`)

`{switching, loop, tick_ms, tick, next_wakeup, tasks, running_task, host_task}`。`next_wakeup` は
残りの tick (無ければ null)。`running_task`・`host_task` は Task の番号。

Task (`tasks[]`):

| フィールド | 中身 |
|---|---|
| `id`、`object`、`context` | 番号、Task のオブジェクト、コンテキスト (main の Task は null) |
| `state` | `DORMANT`・`READY`・`RUNNING`・`WAITING`・`SUSPENDED` (`state_code`) |
| `queue` | `dormant`・`ready`・`waiting`・`suspended`、どのキューにもいない main の Task は null |
| `priority`、`timeslice` | 優先度 (小さいほど先)、タイムスライスの残り (tick) |
| `name`、`value` | 名前と戻り値 (値) |
| `wait` | 待ちの理由: `{reason: "sleep", ticks_left}`・`{reason: "join", task}`・`{reason: "queue", queue, ticks_left}`、無ければ null。終了した Task では最後の待ち |
| `main`、`running`、`host` | (導出) main の Task か、書き出したとき実行中だったか、埋め込む側の Task か |
| `name_text` | (導出) 名前の文字列 (main の Task は `"main"`) |
| `queue_index` | (導出) キューの中の順 (0 から。この順に動く) |
| `key` | (導出) イメージをまたいで同じ Task を指す名前: `"名前"`、名前が無ければ `@ファイル:行` |

### 7.4 任意のセクション (`sections[]`)

各要素は `tag`、`offset` (イメージの中のバイト位置)、`length` (中身の長さ。形式 §13) を持つ。

- `DBG0`: `ireps: [{irep, files: [{file, start, end, lines: [[pc, 行], ...]}]}]` (行が変わる位置)。
- `GCS0`: `disabled`・`generational`・`step_per_allocation`・`scheduler_steps`・`interval_ratio`・
  `step_ratio`・`step_limit`・`malloc_threshold`・`debt_limit` (64 bit の値は 10 進の文字列)。
- 知らないセクション: `unknown: true`、`head_hex` (先頭 64 バイト)、`tail_hex` (64 バイトを超えるとき最後の
  16 バイト)。長さぶん読み飛ばし、後ろのセクションは続けて読む。`warnings` にも出る。

## 8. ルートとパス

オブジェクトの番号はイメージごとに違うので、イメージをまたいで「同じもの」を指すには、ルートからのパス
(`path`) を使う。

**ルート** (`roots[]`、`{key, kind, refs, temps, context, frame, task}`) は、この順に並ぶ:

1. `global`: グローバル変数 (`$count`)
2. `variable`: permanent なクラスの定数・ivar (`Object::Box`、`Foo.@x`)
3. `method`: permanent なクラスに足したメソッド (`Object#work`)
4. `superclass`・`contents`: スーパークラスの付け替え、組み込みのものの中身
5. `frame`: フレーム。**一番外のフレームから** (どのコンテキストも 0 番、次にどのコンテキストも 1 番 …)。`refs` は名前の
   あるローカル変数・`self`・`&block`・`env`/`target_class`・`proc`、`temps` は変数でない一時
   レジスタ (`r5`)
6. `task`: Task (`refs` はそのオブジェクト・名前・値・待っている Queue)
7. `fiber`: Fiber のオブジェクト

**パス**は、まず全部のルートの `refs` から幅優先でたどり、1 つのオブジェクトには最初に届いたパスを
付ける (近い方、同じ近さならルートの順で先の方)。その後で `temps` から、まだ届いていないものに付ける
(それらは `temporary: true`)。permanent のものは `=名前`。どこからも届かないもの (ふつうは無い) は
`?#番号` で、`warnings` に数が出る。

パスの書き方: ルートの `key` に辺の名前をつなぐ。`[0]`・`{:k}`・`#m`・`::C`・`<0>` はそのまま、それ以外
(`@x`、`superclass`、`upper` …) は `.` を挟む。例:

```
$count
Object::Box
root/1:work.h{:new}
root/1:work.box.@items
task "c1"/3:pop.self.@items
```

一番外のフレームから並べるのは、外のフレームほど長く生きるので、そこを通るパスの方が 2 つのイメージの間で
変わりにくいため。一時レジスタを後に回すのも同じ理由 (一時レジスタは命令ごとに中身が変わる)。

## 9. 2 つのイメージの差 (`diff --json`)

| フィールド | 中身 |
|---|---|
| `inspect_diff` | 版 (1) |
| `a`、`b` | 2 つのイメージの `source` |
| `same` | 差がまったく無ければ true |
| `header` | 違うヘッダのフィールド `[{field, a, b}]` (64 bit と 32 bit の書き出し側のイメージは `pointer_bits` だけが違う) |
| `variables` | 変更表 (グローバル変数・定数・足したメソッド) の違い `[{key, a, b, a_ref, b_ref}]`。`a`・`b` は表示の文、`a_ref`・`b_ref` は値がオブジェクトならその番号 (それぞれのイメージの中の) |
| `contexts` | コンテキストごと (`key` で対応) `[{key, change, a, b, fields, frames}]`。`change` は `same`・`changed`・`added`・`removed`。`fields` は `activity`・`vmexec`・`current`・`resumed by` の違い。`frames` は下からのフレームの比べ (下)。ローカル変数の違いは `{name, a, b, a_ref, b_ref, contents_changed}` |
| `tasks` | Task ごと (`key` で対応) の違い `[{key, change, a, b, fields}]` (状態・キューと順・優先度・タイムスライスの残り・待ち・値・実行中だったか)。待ち (`wait`) の値は番号を含まない形 `{reason: "sleep", ticks_left}`・`{reason: "join", task: Task の key}`・`{reason: "queue", queue: パス, ticks_left}` |
| `scheduler` | 時計・次に起きる時刻・実行中だった Task・今のコンテキストなどの違い `[{field, a, b}]` |
| `objects` | `{changed, added, removed, moved}` (下) |

**フレームの比べ**: 下から、メソッドと irep のハッシュが同じ間は同じフレームとみなす。同じフレームは `changed`
(行・pc・ローカル変数・self の違い) か `same`。そこから上は A にだけあるフレームが `returned`、B にだけある
フレームが `called`。ローカル変数は、同じ値 (スカラーは値、オブジェクトはパス) でも中身が変わっていれば
`contents_changed: true`。

**オブジェクトの比べ**:

- 同じパスで同じ種類のものを対応させ、フィールド (クラス・要素・ivar・Hash の組 …。参照は相手のパスで
  比べる) が違えば `changed` (`{path, a, b, kind, label_a, label_b, temporary, fields: [{field, a, b}]}`)。
- 片方にだけあるパスのうち、中身 (参照を除いたフィールド) が同じで、その中身を持つものが両方に 1 つずつ
  しかないものは `moved` (`{a_path, b_path, a, b, label, temporary}`): 別の変数に移っただけのもの。
- 残りが `added` (B だけ) と `removed` (A だけ)。
- `temporary` が true のものは一時レジスタからしか届かないもの。CLI と HTML は別の見出しに分ける。

**対応の付け方の限界**:

- パスが変わると、同じオブジェクトでも別物に見える。上の `moved` で拾えるのは、中身が同じで、
  その中身が A と B でそれぞれ 1 つしかないときだけ。中身もパスも変わったもの (別の変数に移して
  から書き換えた Array など) は `removed` + `added` になる。
- パスは最初に届いた経路なので、同じオブジェクトにより近い経路ができる (または消える) と
  パスが変わる。
- Hash のキーがオブジェクトのとき、パスはキーの並びの位置 (`(key 0)`) に依る。キーの並びが変わると
  別物に見える。
- フレームは下から同じメソッド・同じコードの間だけ対応させる。途中のフレームが入れ替わると、そこから上は
  全部 `returned` と `called` になる。
- Task は名前で、名前の無い Task は本体の場所で対応させる。同じ場所から作った名前の無い Task が
  複数あると、表の順 (`#2`) で対応させるので、キューの順が変わると取り違えうる。
- 時刻・object_id のように往復で正当に変わるものは、イメージには出ない (イメージに object_id は無い)。

## 10. 壊れたイメージ

リーダーは、イメージの形式に合わない所で止まり、`{offset, where, message}` を返す。`offset` はバイト
位置、`where` は記録の場所 (`objects > #12 (hash)`、`contexts > 2 > frame 3`、`sections > DBG0`
など)。読める限りの記録は検査する: 長さの足りない記録、知らない種類・タグ、表の外を指す番号、
指す先の種類の食い違い (Proc の環境が Env でない、など)、`END0` の後の余り。

- `--json` は、そこまで読めた分と `error` を出し、終了の値は 1。
- それ以外の表示は、`broken image at byte N (where): message` を出して終了の値 1。
- 記録はどれも正しいが、組み合わせが合わない (導出フィールドが計算できない) ときは `offset` が null、
  `where` が `derived fields`。

## 11. GUI を作るときに使うフィールド

| 見せるもの | 使うフィールド |
|---|---|
| コールスタック | `contexts[]` を `current` を先に。コンテキストがしていることは `activity` (Task なら Task の `state`)。未開始のフレームは `not_started`、`placeholder` のフレームは畳む。各コンテキストの `frames` を逆順 (上から)。行は `location` (null なら `file:line`)、変数は `locals`・`self`・`captured`、C の関数で待っているフレームは `kind == "cfunc"` と `cfunc` |
| オブジェクトのつながり | 起点は `roots[]` (`refs` と `temps`) かオブジェクト。辺は `objects[].refs`、箱の文字は `label`、permanent (`kind` が `permanent`・`permanent_proc`) は名前だけのもの。中身のフィールドは §6.2 |
| 2 つのイメージの差 | §9 の JSON。オブジェクトへは `a`・`b` の番号で飛ぶ |
| Task と Fiber | `scheduler.tasks[]` を `queue` ごとに `queue_index` の順に並べる。待ちは `wait`、実行中だったものは `running`。コンテキストのチェーンは `contexts[].prev` |

人に見せる文には、パス (`path`) や `key` をそのまま出さない (内部のキー)。オブジェクトの置き場所は
`path_root` のルート (フレームならそのコンテキストとフレームの名前) と `path` の残りから「q、Task "x" の本体のローカル変数」の
ように組み立て、パスは詳しいフィールドやポインタを重ねたときに出す。HTML の `app.js` (`whereText`) が例。

形が合っているかは `tools/lib/checkpoint_schema.rb` (Ruby の標準ライブラリだけ) で確かめられる:
`CheckpointSchema.check(doc)` は食い違いの一覧を返す。
