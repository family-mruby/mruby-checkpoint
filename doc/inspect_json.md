# 像を読んだ JSON (inspect JSON) 版 1

> 状態: P5 で実装済 | 更新: 2026-11-03 | 読み手は `tools/lib/checkpoint_image.rb` ただ 1 つ。形の検査は `tools/lib/checkpoint_schema.rb`

checkpoint の像 (`doc/format_v1.md`) を、形式を知らなくても読める JSON にしたもの。CLI
(`tools/checkpoint_inspect.rb`)・1 ファイルの HTML・2 つの像の差はすべてこの JSON から作る。
後で作る GUI (Asterism の console でも端末のアプリでも) は、この文書と JSON だけで作れることを
目標にする。

```
ruby tools/checkpoint_inspect.rb x.ckpt --json [--pretty] [--no-code]   # 像 1 つ
ruby tools/checkpoint_inspect.rb diff a.ckpt b.ckpt --json               # 2 つの像の差 (§9)
```

HTML (`--html`) には、命令列を除いた JSON (`--no-code` と同じ) が
`<script type="application/json" id="checkpoint-data">` に `{"docs": [JSON, ...], "diff": 差か null}`
の形で入っている (`</` は `<\/` と書く)。ページから JSON だけ取り出して使ってよい。

## 1. 版

- 先頭の `inspect_json` が版 (いまは 1)。差の JSON は `inspect_diff` (いまは 1)。
- 欄を足すだけなら版は上げない。読む側は知らない欄を読み飛ばすこと。欄の意味を変える・欄を
  消すときに版を上げる。
- 像の形式の版は `header.format` (いまは 1)。読み手は v1 の像だけを読む。

## 2. 2 種類の欄

- **像のままの欄**: 像の記録を、順も中身もそのまま JSON にしたもの。番号は像の中の番号
  (§3)。シンボルは名前に、irep の番号はそのまま。
- **導いた欄**: 読み手が像全体から計算したもの。形式を知らなくても絵が描けるようにするための
  欄で、どれも像のままの欄から計算し直せる。下の表の「導いた欄」の列に挙げる。

| 場所 | 導いた欄 |
|---|---|
| `ireps[]` | `hash_ok`、`file`、`line` |
| `objects[]` | `label`、`class_name`、`refs`、`path`、`temporary` |
| `contexts[]` | `role`、`task`、`fiber_objects`、`current`、`key`、`backtrace` |
| `contexts[].frames[]` | `irep`、`file`、`line`、`location`、`key`、`self`、`locals`、`captured` |
| `scheduler.tasks[]` | `main`、`name_text`、`running`、`host`、`queue_index`、`key` |
| 全体 | `roots`、`warnings` |

## 3. 番号と値

- オブジェクト・irep・文脈・Task は、像の中の表の 1 から始まる番号で指す (配列の添字 + 1)。
  像ごとに振り直されるので、2 つの像の間で番号を比べてはいけない (比べるときは `path` と
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
  `{"text": "..."}` (オブジェクトでは `text` の欄)、正しくなければ `{"hex": "..."}`。名前
  (シンボル・ファイル名・関数名) は常に文字列で、UTF-8 でないバイトは `\xHH` と書く。

## 4. 全体

| 欄 | 中身 |
|---|---|
| `inspect_json` | 版 (1) |
| `source` | `{file, bytes}`: 像のファイル名 (道筋なし) と大きさ |
| `header` | `{magic, format, implementation, bytecode, integer_bits, float_bits, utf8, pointer_bits}` (形式 §3 のヘッダ) |
| `symbols` | シンボル表 (名前の配列。像の中の番号 i は添字 i−1) |
| `ireps` | irep 表 (§5) |
| `objects` | オブジェクト表 (§6) |
| `changes` | 変更表 (§6.3) |
| `contexts` | 文脈の表 (§7) |
| `current_context` | 書き出したとき動いていた文脈の番号 |
| `scheduler` | スケジューラと Task の表 (§7.3) |
| `sections` | 任意の節 (§7.4) |
| `roots` | 根の一覧 (§8)。導いた欄 |
| `warnings` | 注意の文の配列 (知らない節、irep のハッシュが合わない、根から届かないオブジェクトなど)。正常な像では空 |
| `error` | 壊れた像のときだけ `{offset, where, message}` (§10)。ふだんは `null` |
| `omitted` | (あれば) 省いた欄。HTML では `["ireps.iseq"]` |

## 5. irep (`ireps[]`)

| 欄 | 中身 |
|---|---|
| `id`、`kind` | 番号と種類: `carried` (運ぶ)・`permanent` (ハッシュだけ)・`permanent_carried` (mrblib のもので中身も運ぶ)。`kind_code` は像の値 (0・1・2) |
| `hash` | 内容のハッシュ (16 進 16 桁) |
| `nlocals`、`nregs`、`ilen` | 局所変数の数 (self を含む)、レジスタの数、命令列のバイト数 |
| `catch` | 例外処理の表 `[{type: "rescue"/"ensure", begin, end, target}]` |
| `pool` | 定数プール。各要素は `{"string": {text か hex}}`・`{"int": 整数}`・`{"float": …}`・`{"bigint_hex": "..."}` |
| `syms` | 命令が使うシンボルの名前 (無しは null) |
| `children` | 子の irep の番号 |
| `locals` | 局所変数の名前 (R1 から。無名は null) |
| `iseq` | 命令列 (16 進)。`--no-code` と HTML では省く |
| `bytes` | 像の中でこの記録が占めるバイト数 |
| `hash_ok` | (導いた) 中身から計算し直したハッシュが `hash` と合うか。permanent (中身なし) には無い |
| `file`、`line` | (導いた) `DBG0` の行番号から: 最初のファイルと、最初に現れる行。行番号が無ければ null |

## 6. オブジェクト (`objects[]`)

### 6.1 どのオブジェクトにもある欄

| 欄 | 中身 |
|---|---|
| `id` | 番号 |
| `kind` | 種類の名前 (下の表)。`kind_code` は像の種類の値 |
| `bytes` | 像の中でこの記録が占めるバイト数 |
| `label` | (導いた) 短い表示 (`"c1:1"`、`[1, 2]`、`Hash(3)`、`#<Foo>`、`Task "c1"`、`Proc (x.rb:12)`)。permanent は名前を読みやすくしたもの (`T` は `main`、`S:Foo` は `#<Class:Foo>`) |
| `class_name` | (導いた。クラス・モジュール・特異クラス・取り込みクラス・origin と permanent のクラスだけ) クラスの名前 |
| `refs` | (導いた) このオブジェクトが指すオブジェクト `[[辺の名前, 番号], ...]`。図の辺。名前は ivar・定数 (`@a`、`::Foo`)、要素 `[0]`、Hash の値 `{:key}` と鍵 `(key 0)`、Set の要素 `<0>`、メソッド `#name`、`superclass`、`upper`、`env`、`target_class`、`begin`・`end`・`message`・`backtrace`・`value`、Task の `name`・`value`・`waits on`、最後にクラス `class` |
| `path` | (導いた) 根からの道筋 (§8)。permanent は `=名前` |
| `temporary` | (導いた) 変数でない一時レジスタからしか届かないなら true |

permanent (`permanent`、`permanent_proc`) 以外は、続けて `class` (クラスのオブジェクトの番号) と
`frozen` を持つ。

### 6.2 種類ごとの欄

| `kind` | 欄 |
|---|---|
| `permanent` | `name` (形式 §8 の名前: `Object`、`T`、`S:T`、`M:Kernel#loop`、`V:Task::Queue@WAIT_RETRY` …) |
| `permanent_proc` | `name` (`M:` の名前)、`irep` (枠が動かしている mrblib の irep) |
| `object` | `ivars` |
| `string` | `text` か `hex`、`binary` (`String#b` の印) |
| `array` | `items` (値の配列) |
| `hash` | `default` `{has_default, proc}`、`pairs` (`[鍵, 値]` の配列)、`ivars` |
| `range` | `begin`、`end`、`exclusive`、`uninitialized` |
| `exception` | `message`、`backtrace`、`ivars` |
| `class`・`module`・`singleton_class` | `superclass`、`instance_type`、`flags` `{no_allocate, inherited, prepended, has_method_table}`、`ivars` (定数・クラス変数・内部の印 `__classname__` など)、`methods` |
| `iclass` | `superclass` (`class` が取り込んだモジュール) |
| `proc` | `lambda`、`orphan`、`scope`、`body` (`{kind: "irep", irep}`・`{kind: "cfunc", name, aspec}`・`{kind: "alias", method}`)、`upper`、`env` (`{kind: "env"/"class", ref}` か null) |
| `env` | `method`、`length`、`block_reg`、`visibility`、`on_stack`、スタック上なら `context`・`position` (その文脈の `stack` の添字。その枠の R0)、切り離し済みなら `values` |
| `data` | `type` (`Time`・`Task::Queue` …。空は未初期化)、`data_hex` (中身のバイト)、`decoded` (知っている型なら中身を欄にしたもの: Time は `{sec, nsec, zone}`、Task::Queue は `{closed}` など)、`ivars` |
| `bigint` | `negative`、`hex`、`value` (10 進の文字列) |
| `struct` | `items`、`ivars` |
| `set` | `uninitialized`、`items`、`ivars` |
| `rational` | `numerator`、`denominator` (10 進の文字列) |
| `complex` | `real`、`imaginary` (`{"float": …}`) |
| `break` | `tag` (`break`・`jump`・`stop`)、`frame`、`value` |
| `random` | `state` (16 進)、`seed`、`ivars` |
| `origin` | `superclass`、`has_methods`、`methods` |
| `task` | `task` (Task の表の番号)、`ivars` |
| `fiber` | `context` (文脈の番号。null は未初期化) |

`ivars` は `[[名前, 値], ...]` (像の順)。`methods` の各要素は `{name, visibility, kind}` で、`kind`
が `proc` なら `proc` (番号)、`cfunc` なら `cfunc` (関数の名前) と `aspec`、`undef` は欄なし。

### 6.3 変更表 (`changes[]`)

permanent なものへの変更 (形式 §9)。各要素は `kind` (`define_method`・`remove_method`・
`set_variable`・`remove_variable`・`set_superclass`・`set_global`・`remove_global`・
`random_state`・`class_flags`・`contents`) と `kind_code`、それに種類ごとに `target` (permanent の
オブジェクト)・`method`・`name`・`value`・`superclass`・`state`・`seed`・`prepended`・`frozen`・
`type`・`text`/`hex`・`items`・`pairs`。プログラムが作ったトップレベルのクラスは `Object` への
`set_variable`、トップレベルの `def` は `Object` への `define_method` として現れる。

## 7. 実行の状態

### 7.1 文脈 (`contexts[]`)

| 欄 | 中身 |
|---|---|
| `id`、`state` | 番号と状態 (`created`・`running`・`resumed`・`suspended`・`transferred`・`terminated`。`state_code` は像の値) |
| `vmexec` | C から resume された印 |
| `prev` | この文脈を resume した文脈 (戻る先) |
| `fiber` | この文脈の Fiber のオブジェクト |
| `stack` | スタック (レジスタの並び。値の配列) |
| `frames` | 枠 (§7.2)。**下 (一番外) から上** の順 |
| `role` | (導いた) `root` (プログラムの本体)・`task`・`fiber`・`unknown` |
| `task` | (導いた) この文脈の Task の番号 |
| `fiber_objects` | (導いた) この文脈を指す Fiber のオブジェクト |
| `current` | (導いた) 書き出したとき動いていた文脈か |
| `key` | (導いた) 像をまたいで同じ文脈を指す名前: `root`、`task "名前"` (名前の無い Task は `task @ファイル:行`)、`fiber @ファイル:行` (Fiber の本体のブロックの場所)。重なれば `#2` を付ける |
| `backtrace` | (導いた) この文脈で例外を上げたら mruby が出すバックトレース (**上から**)。`"x.rb:13:in deep"` の形 |

### 7.2 枠 (`frames[]`)

| 欄 | 中身 |
|---|---|
| `index` | 文脈の中の位置 (0 が一番外) |
| `kind` | `ruby`、または `cfunc` (文脈の切り替えを待っている C の関数の枠。`cfunc` にその名前: `F:Fiber#resume`・`F:S:Task#run` など) |
| `method` | メソッド名 (トップレベルやブロックでは null のことがある) |
| `proc` | 動いている Proc のオブジェクト |
| `reg_base` | この枠の R0 の `stack` の添字 |
| `pc` | 命令列の中の位置 (null は無し) |
| `argc`、`kwargc`、`cci`、`visibility`、`block`、`env` | 形式 §10 のまま (`env` は `{kind: "env"/"class", ref}`) |
| `irep` | (導いた) 動いている irep |
| `file`、`line` | (導いた) いま動いている (または呼び出しから戻るのを待っている) 行。pc の 1 つ前の命令の行 |
| `location` | (導いた) バックトレースの 1 行 (`file:line:in method`)。mruby がバックトレースに出さない枠は null |
| `key` | (導いた) 文脈の `key` + `/番号:メソッド` (`root/1:work`) |
| `self` | (導いた) R0 の値 |
| `locals` | (導いた) 局所変数 `[{name, reg, value}]` (無名は name が null) |
| `captured` | (導いた) ブロックが外から見ている変数。外側へ 1 段ずつ `[{env, method, on_stack, context, frame, variables: [{name, value}]}]`。`on_stack` なら、その変数は文脈 `context` の枠 `frame` のレジスタそのもの |

`location` と `backtrace` は mruby の `backtrace.c` と同じ規則で作る: Ruby の枠は行番号があれば
`file:line`、メソッド名があれば `:in 名前` を付ける。C の関数の枠は、名前があれば、その下で
いちばん近い Ruby の枠の場所に `:in 名前` を付ける。行が引けなければ `(unknown):0`。
試験 (`tools/test_inspect.rb`) で、mruby 自身のバックトレースと一致することを確かめている。

### 7.3 スケジューラ (`scheduler`)

`{switching, loop, tick_ms, tick, next_wakeup, tasks, running_task, host_task}`。`next_wakeup` は
残りの tick (無ければ null)。`running_task`・`host_task` は Task の番号。

Task (`tasks[]`):

| 欄 | 中身 |
|---|---|
| `id`、`object`、`context` | 番号、Task のオブジェクト、文脈 (main の Task は null) |
| `state` | `DORMANT`・`READY`・`RUNNING`・`WAITING`・`SUSPENDED` (`state_code`) |
| `queue` | `dormant`・`ready`・`waiting`・`suspended`、どの列にもいない main の Task は null |
| `priority`、`timeslice` | 優先度 (小さいほど先)、時間の割り当ての残り (tick) |
| `name`、`value` | 名前と戻り値 (値) |
| `wait` | 待ちの理由: `{reason: "sleep", ticks_left}`・`{reason: "join", task}`・`{reason: "queue", queue, ticks_left}`、無ければ null。終わった Task では最後の待ち |
| `main`、`running`、`host` | (導いた) main の Task か、書き出したとき動いていたか、埋め込む側の Task か |
| `name_text` | (導いた) 名前の文字列 (main の Task は `"main"`) |
| `queue_index` | (導いた) 列の中の順 (0 から。この順に動く) |
| `key` | (導いた) 像をまたいで同じ Task を指す名前: `"名前"`、名前が無ければ `@ファイル:行` |

### 7.4 任意の節 (`sections[]`)

各要素は `tag` と `offset` (像の中のバイト位置) を持つ。

- `DBG0`: `ireps: [{irep, files: [{file, start, end, lines: [[pc, 行], ...]}]}]` (行が変わる位置)。
- `GCS0`: `disabled`・`generational`・`step_per_allocation`・`scheduler_steps`・`interval_ratio`・
  `step_ratio`・`step_limit`・`malloc_threshold`・`debt_limit` (64 bit の値は 10 進の文字列)。
- 知らない節: `unknown: true`、`bytes`、`head_hex` (先頭 64 バイト)、`tail_hex`。形式は節に長さを
  持たせていないので、その印から最後の `END0` の手前までを中身とみなす (後ろの節も含まれうる)。
  `warnings` にも出る。

## 8. 根と道筋

オブジェクトの番号は像ごとに違うので、像をまたいで「同じもの」を指すには、根からの道筋
(`path`) を使う。

**根** (`roots[]`、`{key, kind, refs, temps, context, frame, task}`) は、この順に並ぶ:

1. `global`: 大域変数 (`$count`)
2. `variable`: permanent なクラスの定数・ivar (`Object::Box`、`Foo.@x`)
3. `method`: permanent なクラスに足したメソッド (`Object#work`)
4. `superclass`・`contents`: 上位の付け替え、組み込みのものの中身
5. `frame`: 枠。**一番外の枠から** (どの文脈も 0 番、次にどの文脈も 1 番 …)。`refs` は名前の
   ある局所変数・`self`・`&block`・`env`/`target_class`・`proc`、`temps` は変数でない一時
   レジスタ (`r5`)
6. `task`: Task (`refs` はそのオブジェクト・名前・値・待っている Queue)
7. `fiber`: Fiber のオブジェクト

**道筋**は、まず全部の根の `refs` から幅優先でたどり、1 つのオブジェクトには最初に届いた道筋を
付ける (近い方、同じ近さなら根の順で先の方)。その後で `temps` から、まだ届いていないものに付ける
(それらは `temporary: true`)。permanent のものは `=名前`。どこからも届かないもの (ふつうは無い) は
`?#番号` で、`warnings` に数が出る。

道筋の書き方: 根の `key` に辺の名前をつなぐ。`[0]`・`{:k}`・`#m`・`::C`・`<0>` はそのまま、それ以外
(`@x`、`superclass`、`upper` …) は `.` を挟む。例:

```
$count
Object::Box
root/1:work.h{:new}
root/1:work.box.@items
task "c1"/3:pop.self.@items
```

一番外の枠から並べるのは、外の枠ほど長く生きるので、そこを通る道筋の方が 2 つの像の間で
変わりにくいため。一時レジスタを後に回すのも同じ理由 (一時レジスタは命令ごとに中身が変わる)。

## 9. 2 つの像の差 (`diff --json`)

| 欄 | 中身 |
|---|---|
| `inspect_diff` | 版 (1) |
| `a`、`b` | 2 つの像の `source` |
| `same` | 差がまったく無ければ true |
| `header` | 違うヘッダの欄 `[{field, a, b}]` (64 bit と 32 bit の書き手の像は `pointer_bits` だけが違う) |
| `variables` | 変更表 (大域変数・定数・足したメソッド) の違い `[{key, a, b}]`。値は表示の文 |
| `contexts` | 文脈ごと (`key` で対応) `[{key, change, a, b, fields, frames}]`。`change` は `same`・`changed`・`added`・`removed`。`fields` は状態などの違い。`frames` は下からの枠の比べ (下) |
| `tasks` | Task ごと (`key` で対応) の違い `[{key, change, a, b, fields}]` (状態・列と順・優先度・割り当ての残り・待ち・値・動いていたか) |
| `scheduler` | 時計・次に起きる時刻・動いていた Task・今の文脈などの違い `[{field, a, b}]` |
| `objects` | `{changed, added, removed, moved}` (下) |

**枠の比べ**: 下から、メソッドと irep のハッシュが同じ間は同じ枠とみなす。同じ枠は `changed`
(行・pc・局所変数・self の違い) か `same`。そこから上は A にだけある枠が `returned`、B にだけある
枠が `called`。局所変数は、同じ値 (スカラーは値、オブジェクトは道筋) でも中身が変わっていれば
`contents_changed: true`。

**オブジェクトの比べ**:

- 同じ道筋で同じ種類のものを対応させ、欄 (クラス・要素・ivar・Hash の組 …。参照は相手の道筋で
  比べる) が違えば `changed` (`{path, a, b, kind, label_a, label_b, temporary, fields: [{field, a, b}]}`)。
- 片方にだけある道筋のうち、中身 (参照を除いた欄) が同じで、その中身を持つものが両方に 1 つずつ
  しかないものは `moved` (`{a_path, b_path, a, b, label, temporary}`): 別の変数に移っただけのもの。
- 残りが `added` (B だけ) と `removed` (A だけ)。
- `temporary` が true のものは一時レジスタからしか届かないもの。CLI と HTML は別の見出しに分ける。

**対応の付け方の限界**:

- 道筋が変わると、同じオブジェクトでも別物に見える。上の `moved` で拾えるのは、中身が同じで、
  その中身が A と B でそれぞれ 1 つしかないときだけ。中身も道筋も変わったもの (別の変数に移して
  から書き換えた Array など) は `removed` + `added` になる。
- 道筋は最初に届いた経路なので、同じオブジェクトにより近い経路ができる (または消える) と
  道筋が変わる。
- Hash の鍵がオブジェクトのとき、道筋は鍵の並びの位置 (`(key 0)`) に依る。鍵の並びが変わると
  別物に見える。
- 枠は下から同じメソッド・同じコードの間だけ対応させる。途中の枠が入れ替わると、そこから上は
  全部 `returned` と `called` になる。
- Task は名前で、名前の無い Task は本体の場所で対応させる。同じ場所から作った名前の無い Task が
  複数あると、表の順 (`#2`) で対応させるので、列の順が変わると取り違えうる。
- 時刻・object_id のように往復で正当に変わるものは、像には出ない (像に object_id は無い)。

## 10. 壊れた像

読み手は、像の形式に合わない所で止まり、`{offset, where, message}` を返す。`offset` はバイト
位置、`where` は記録の場所 (`objects > #12 (hash)`、`contexts > 2 > frame 3`、`sections > DBG0`
など)。読める限りの記録は検査する: 長さの足りない記録、知らない種類・タグ、表の外を指す番号、
指す先の種類の食い違い (Proc の環境が Env でない、など)、`END0` の後の余り。

- `--json` は、そこまで読めた分と `error` を出し、終了の値は 1。
- それ以外の表示は、`broken image at byte N (where): message` を出して終了の値 1。
- 記録はどれも正しいが、組み合わせが合わない (導いた欄が計算できない) ときは `offset` が null、
  `where` が `derived fields`。

## 11. GUI を作るときに使う欄

| 見せるもの | 使う欄 |
|---|---|
| 呼び出しの枠 | `contexts[]` を `current` を先に。各文脈の `frames` を逆順 (上から)。行は `location` (null なら `file:line`)、変数は `locals`・`self`・`captured`、C の関数で待っている枠は `kind == "cfunc"` と `cfunc` |
| オブジェクトのつながり | 起点は `roots[]` (`refs` と `temps`) かオブジェクト。辺は `objects[].refs`、箱の文字は `label`、permanent (`kind` が `permanent`・`permanent_proc`) は名前だけの印。中身の欄は §6.2 |
| 2 つの像の差 | §9 の JSON。オブジェクトへは `a`・`b` の番号で飛ぶ |
| Task と Fiber | `scheduler.tasks[]` を `queue` ごとに `queue_index` の順に並べる。待ちは `wait`、動いていたものは `running`。文脈の鎖は `contexts[].prev` |

形が合っているかは `tools/lib/checkpoint_schema.rb` (Ruby の標準ライブラリだけ) で確かめられる:
`CheckpointSchema.check(doc)` は食い違いの一覧を返す。
