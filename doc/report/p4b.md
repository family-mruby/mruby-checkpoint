# P4b 報告: 時間の割り当ての残り、失われる Task.pass、mrblib の行番号

> 状態: 実装済・検収待ち | 更新: 2026-11-03 | 指示は `doc/instruction_p4b.md`

## 結果

- **時間の割り当て**: 走っていた Task は、像に記した残りの tick から続ける (既定。切り替えは
  無くした)。C の読み手も SabiRuby の読み手も同じ。`--quantum 300` の試験一式 (種 1〜5、両方の
  起動の形、既定の設定) で、往復なしとの食い違いは **0** (P4 では Task の形で 2 ファイル)。
- **失われる `Task.pass`**: mruby-task に patch を足し (`patches/0002-task-switch-requests.patch`)、
  スケジューラが切り替えを頼むたびに数を 1 つ進める (`mrb->task.switch_requests`)。checkpoint は
  要求を立てたときの数と安全点での数を比べる。P4 の「スケジューラの様子の差で見分ける」作りは
  消した (控えの確保と比べる処理がなくなった)。`vm.c` の patch は変えていない。
- 穴を塞いだことを示す試験 `tools/targeted.rb` (`rake test` の中): 外からの要求のすぐ次の命令が
  `Task.pass` になる点を、試験の道具の新しい選び方 `--at-send pass` で全部の pass の前に作る。
  P4 の作りでは 8 点すべてで pass が失われ、2 つの Task の順が `a0 a1 a2 a3 b0 …` になる (往復
  なしは `a0 b0 a1 b1 …`)。P4b では両方の起動の形・同じプロセスと別のプロセスの 4 通りとも、
  往復なしと同じ。
- **`mrb_checkpoint_request_here` は消した**。P4 でこれが要ったのは、要求と安全点の間の 1 命令が
  スケジューラを動かすと像の bit0 が曖昧になるためだった。数で見分けられるので要らない。試験の
  道具は P3 と同じ「次の境目で書く」(`mrb_checkpoint_request`) に戻し、長い版を流した (食い違い 0)。
- **mrblib の行番号**: 書く側は `DBG0` に、運ぶ mrblib の irep の行の表も書く。そのために mrblib
  を行番号つき (`mrbc -g`) で作るようにした (このビルドの mrblib はこれまで行番号を持たなかった)。
  ファイル名はソースの木からの相対 (`mrblib/array.rb`、`mrbgems/mruby-task/mrblib/task.rb`)。
  `tools/checkpoint_inspect.rb` は枠ごとの `file:line` を出すようにした。SabiRuby は mrblib の
  irep も中身から作るので、像の行番号がそのまま枠に付き、復元の後のバックトレースに
  `mrblib/array.rb:20:in each` が出る (新しいサンプル `lines_mrblib`)。
- 長い版の途中で、P3 からの不具合を 1 つ見つけて直した: 書き出しが Rational などのメソッドを呼んで
  呼び出しの枠の表 (ci) が伸び (realloc)、安全点から戻った VM が解放済みの枠を使い続けていた
  (Task の形の `rational.rb` で SIGSEGV)。`vm.c` の patch で、安全点の判定の遅い側の後に枠を
  取り直す (§4)。狙った試験 `test/targeted/ci_grow.rb` を足した。
- 命令数 (valgrind): mruby-task の patch は、普段のベンチマーク 5 本で差 0.0003% 以下、Task を多く
  切り替えるベンチマークで +0.073%。`vm.c` の patch の取り直しは、bm_ao_render で +0.115%、他は
  ±0.0001% 以下 (§費用)。
- 長い版 (種 1〜20、両方の起動の形): 往復なしと食い違った実行は **0** (§試験の数字)。

## 作ったもの・変えたもの

| 場所 | 中身 |
|---|---|
| `patches/0001-vm-safepoint-hook.patch` | 安全点の判定の遅い側 (`L_TASK_CHECK` と、switch で振り分ける版の `RETURN_IF_TASK_STOPPED`) で、hook の後に `ci = mrb->c->ci` と取り直す (§4)。速い側は変えていない |
| `patches/0002-task-switch-requests.patch` (新) | `mrb_task_state` に `switch_requests` を足す (`include/mruby.h`)。mruby-task の `task.h` に `task_request_switch(mrb)` (数を進めて `switching` を立てる)。`task.c`・`task_queue.c` の `switching_ = TRUE` 14 か所をこれに替える。join を待つ Task を起こす所の「印がもう立っていれば優先度を比べない」という省略を外す (下) |
| `mrbgems/mruby-checkpoint/src/checkpoint.c` | `mark_switch` は数と「印がもう立っていたか」を控えるだけ。`sched_switch` は数を比べるだけ。スケジューラの様子の控え (P4 の `snap_*`) と `mrb_checkpoint_request_here` を消した。試験の口 `__pass_and_save` の pass も数える |
| `mrbgems/mruby-checkpoint/src/checkpoint_task.c` | 続けるとき、時間の割り当てを満タンにしない。`mrb_checkpoint_keep_timeslice` を消した |
| `mrbgems/mruby-checkpoint/src/checkpoint_dump.c` | `DBG0` に permanent な irep (mrblib) の行の表も書く |
| `build_config/checkpoint.rb` | mrblib を `mrbc -g` で。mrbc をソースの木の根で走らせ、ファイル名を相対にする (`Dir.chdir` でなく起動の引数で。rake は並列に動くため)。`CHECKPOINT_VIRTUAL_CLOCK=1` (仮想の時計だけを入れる。命令数を測るため) |
| `mrbgems/mruby-checkpoint-fuzz/tools/checkpoint-fuzz/checkpoint-fuzz.c` | `mrb_checkpoint_request` に戻した。`--at-send NAME` (その名前のメソッドの呼び出しの直前に毎回頼む)。`--keep-timeslice` を消した |
| `tools/targeted.rb`、`test/targeted/task_pass.rb`・`ci_grow.rb` (新) | 狙った点の試験 (上、§4)。`rake test` から流す。`CHECKPOINT_FUZZ64` で ASan の版でも流せる |
| `test/samples/lines_mrblib.rb` (新)、`test/samples/lines.rb` | mrblib の枠を含む像。`lines.rb` の Task の中のブロックは `Array#each` からプログラムのメソッドに替えた (§SabiRuby との違い) |
| `tools/checkpoint_inspect.rb` | 枠ごとの irep と `file:line` |
| `tools/asan.sh` | ASan の版も仮想の時計 (§試験の数字) |
| `tools/fuzz.rb`、`tools/sabiruby_fixtures.rb` | `--keep-timeslice` を消した。`lines_mrblib` も Task の形の像を作る |
| `doc/format_v1.md` | §11 の bit0 と時間の割り当ての残りの意味、§12 の 3、§13 の `DBG0`、§14。バイトの並びは変えていない |

SabiRuby 側 (worktree、ブランチ `feature/checkpoint`): `ext_task::checkpoint_slice` が割り当てを
満タンにしない。`DBG0` の読み手はもともと運んだ irep すべてに行番号を付けていたので変えていない。
試験の像を作り直した (95 個、約 260 KB)。計画の文書に第 4 段。

## 1. 時間の割り当ての残り

C の読み手は組み立てのときに Task の `timeslice` を像の値にしていた (P4 から)。P4 は続ける直前に
満タンに戻していたので、それをやめた。残りが 0 のときも 0 のまま続ける。mruby-task の tick は
`timeslice > 0` のときだけ減らして 0 で切り替えを頼むので、0 の割り当ては tick では終わらず、Task が
自分から譲るまで続く。これは書いた側でも同じ (0 になって頼んだ後で印が消されると起きる。例:
C の関数の境目の中の sleep が印を消す)。SabiRuby も同じにした。1 tick の中のどこまで進んでいたか
(SabiRuby の命令の数、C のタイマの位相) は像に無いので、tick の境目から数え直す。

切り替え `mrb_checkpoint_keep_timeslice` は消した。満タンから始めたい埋め込む側は、今のところ
見当たらない (Family mruby はアプリの VM ごとに運ぶ)。残すと、往復なしと違う動きになる口を
既定の外に持つことになる。サブの仮決め 1。

## 2. 失われる Task.pass を塞ぐ

### 原因 (P4 のまま)

外からの要求は、VM が命令の境目ごとに見ている `mrb->task.switching` (bool) を立てて、次の安全点で
hook を呼ばせる。スケジューラも同じ印で切り替えを頼む。要求が立てた後でスケジューラも立てると、
印は 1 つなので、安全点では誰が立てたか分からない。P4 はスケジューラの様子 (動いている Task の
状態・割り当て、待ちの列、準備の列) の差で推し量ったが、`Task.pass` は印を立てるほかに何もしない
ので、差に出ない。

### 比べた案

| 案 | 命令の境目の費用 | 正確さ | 手間・影響 |
|---|---|---|---|
| A. mruby-task が頼みを数える (採用) | 増えない (VM は今の印だけを見る) | 頼みはすべて数に出る。印がもう立っていたかと合わせて、安全点で正確に分かる | mruby-task の patch (数を進める所 14 か所 + 1 つの省略を外す)。`mrb_task_state` に 4 バイト |
| B. 頼みの理由 (pass・sleep・割り当て・起こすなど) を記録する | 増えない | A と同じ (理由は見分けに要らない) | 理由ごとの欄が要る。今は使い道が無い |
| C. VM に専用の印を足す (P4 の決めてほしいこと 4) | 境目ごとに読み込みが 1 つ増える | 正確 | `vm.c` の patch が変わる。普段の速さの条件に響く |
| D. 様子の差で推し量る (P4) | 増えない | pass を見落とす。安全点までに文脈が変わると (要求の後の命令が root の `Task.run` など) 推し量れない | 控えの確保と比べる処理が要る |

A を取った。数は「動いたかどうか」だけを見る (値そのものに意味は無い)。tick の割り込みと VM の
スレッドが同時に数を進めて 1 つ分しか進まないことがあっても、動いたことは分かる。32 bit の数が
要求と安全点の間にちょうど 2^32 回進むと見落とすが、その間は 1 命令か、長くても 1 つの割り当ての
中である。

### 数を進める所

mruby-task が `switching_ = TRUE` と書く所 14 か所 (pass、sleep (時間・無期限)、join、Task を作った
ときと戻したときに優先度が上なら、止めた Task が走っていたとき、自分を終えたとき、tick の割り当て
の終わり、tick で起こしたとき、join を待つ Task を起こしたとき、Queue の push・close・pop の待ち)
をすべて `task_request_switch(mrb)` に替えた。印を消す所 (割り当ての始まりと終わり、root の sleep、
C の境目の中の sleep) は変えていない。

1 か所だけ、書き方を変えた。join を待つ Task を起こす所 (`wake_up_join_waiters`) は、印がもう立って
いれば優先度を比べずに飛ばしていた。外からの要求が印を立てていると、スケジューラが頼んだかどうかが
決まらないので、印に関係なく比べて、上なら頼む (数を進める) ようにした。印が立っていれば立てても
同じなので、mruby-task の振る舞いは変わらない。

`mrb_task_state` の最後に `uint32_t` を足した。64 bit では詰め物の中に入り、`mrb_task_state` (64
バイト)・`mrb_state` の大きさと全部の欄の位置は変わらない。32 bit では `mrb_task_state` が 36 → 40
バイト、`mrb_state` が 4 バイト増える。

### 安全点での見分け

- 要求のとき: 「印がもう立っていたか」と数を控え、印を立てる。
- 安全点で: 印が立っていて、かつ (要求の前から立っていた、または数が動いた) なら、スケジューラが
  頼んでいる。像の bit0 を立て、印を残す (VM はすぐスケジューラへ戻る)。そうでなければ印を消す
  (往復なしと同じ状態)。
- 要求と安全点の間で文脈が変わっても (root の `Task.run` で Task に入り、割り当ての始まりで印が
  消され、後で tick が頼む、など) 数で分かる。P4 は「要求のとき Task が動いていなかった」場合を
  一律に「頼んでいない」としていた。
- root で要求の後にスケジューラが印を立てた場合 (root の命令が Task を起こしたなど。root では
  印は使われず、立ったまま残る) も、像にはそのまま bit0 として書かれ、読む側は立てておく。P4 は
  「Task が動いていなかった」としてこれを消していた。

### 穴を塞いだことの試験

`test/targeted/task_pass.rb`: 同じ優先度の Task 2 つが、`Task.pass` だけで交互に 4 回ずつ動き、順を
記録する (仮想の時計で、割り当ては tick で終わらない)。`checkpoint-fuzz --at-send pass` は、
`pass` を呼ぶ命令 (`OP_SEND` など) の直前に毎回要求を立てる。要求と安全点の間の 1 命令がちょうど
その `Task.pass` になる。像は安全点 (pass の呼び出しの次の命令) で書かれ、新しい `mrb_state` (または
別のプロセス) で続く。

| 作り | top-level・同じプロセス | top-level・別のプロセス | Task の形・同じプロセス | Task の形・別のプロセス |
|---|---|---|---|---|
| P4 (966b46f。試験の道具だけ P4b のもの) | 8/8 点書けた、順が `a0 a1 a2 a3 b0 b1 b2 b3` | 同じ | 同じ | 同じ |
| P4b | 8/8 点書けた、往復なしと同じ `a0 b0 a1 b1 a2 b2 a3 b3` | 同じ | 同じ | 同じ |

P4 の作りで流すために、P4 の版を取り出したもの (`git archive 966b46f`) に P4b の試験の道具
(`checkpoint-fuzz.c`) と試験を入れて作った。

### `mrb_checkpoint_request_here` の見直し

P4 で足した理由は「次の境目で書くと、間の 1 命令がスケジューラを動かしたかが曖昧になる」だった。
数で正確に分かるので、その理由は無くなった。P3 の「次の境目で書く」に戻すと、点の位置が P3 と
同じになり、点の位置の決め方を試験の道具の側に持たずに済む。外からの要求の口は
`mrb_checkpoint_request` の 1 つになった。要らないので消した (サブの仮決め 2)。

## 3. mrblib の行番号

- このビルドの mrblib (mruby の `mrblib/*.rb` と gem の `mrblib/`) は、`mrbc` を `-g` なしで
  走らせていたので行番号を持たなかった (`irep->debug_info` が NULL)。`DBG0` に入れるものが無いので、
  `build_config/checkpoint.rb` で `-g` を付けた (`enable_debug` は `MRB_DEBUG` も立てるので使わない)。
  mrbc は渡されたとおりの名前をファイル名にするので、絶対のパスが入る。ソースの木の根で mrbc を
  走らせて相対の名前を渡すようにした。普段のビルド・試験のビルド・ベンチマークのビルドすべて。
- 書く側は、行番号を持つ運ぶ irep をすべて `DBG0` に書く (P4 は permanent なものを除いていた)。
  同じビルドの C の読み手は、名前で引いた自分の mrblib の irep (同じ行番号を持つ) を使う。中身から
  組み立てる読み手 (SabiRuby、別の版の mrblib を持つ C の読み手) は、この行番号を付ける。
- 往復なしの C mruby のバックトレースにも mrblib の行が出るようになった (`lines.out` が変わった:
  `lines.rb:20:in each_of` の行。下)。
- `tools/checkpoint_inspect.rb` の終わりに枠ごとの行を出す:

  ```
  frames:
    context 1 frame 0: - irep @5 pc=145 lines.rb:54
    context 1 frame 1: :each irep @6 pc=52 mrblib/array.rb:20
    context 1 frame 2: - irep @4 pc=14 lines.rb:55
  ```

  (P5 の共通の読み手の元になる。行は pc の 1 つ前の命令のもの。バックトレースと同じ)
- 像の大きさ: mrblib の irep を運ぶ像で 1 つあたり数十〜数百バイト増える (SabiRuby の試験の像 95 個で
  約 262 KB。P4 の 91 個は約 240 KB)。

## 4. 安全点から戻った VM が古い枠を使う (P3 からの不具合)

### 起きたこと

長い版の Task の形で、`mruby-rational/rational.rb` の種 16 が最初の点の後に SIGSEGV (状態 139) で
終わった。点を切り出すと 150 番目の命令の要求 1 つで起き、ASan の版では heap-use-after-free:

- 書き出し (`checkpoint_dump` の `w_object`) が、Rational の分子と分母を `mrb_funcall` で取る
  (`numerator`・`denominator`。P3 から。Complex の `real`・`imaginary`、Set の `to_a` も同じ)。
- 呼び出しは今の文脈の枠の表に 1 つ積む (`cipush`)。表が満杯だと realloc で移る。Task の文脈の表は
  4 つ分から始まるので、浅い所でも満杯になる。
- 安全点の hook から戻った `mrb_vm_exec` は、局所変数 `ci` (枠の表の中を指す) を取り直さずに次の
  命令を読んでいた (`insn = *ci->pc`、レジスタは `ci->stack`)。移った後の解放済みの領域を読む。
  普段のビルドでも同じ (試験の道具の hook は関係ない)。

P4 で見つからなかったのは、満杯になる点に当たらなかっただけと見ている (P4 の試験の道具は命令ごとの
hook の中で書いていたが、そこから戻った後も同じく古い `ci` で命令を実行する)。`Checkpoint.save` の
安全点も同じ所を通る。

### 直し方

| 案 | |
|---|---|
| A. VM が hook の後に枠を取り直す (採用) | 安全点の判定の遅い側にだけ `ci = mrb->c->ci` を足す。命令の境目ごとの速い側は変わらない。hook の中で何が動いても (書き出しのメソッドの呼び出し、埋め込む側の handler、断りの報告の関数) 正しい |
| B. 書き出しがメソッドを呼ばない (構造体を直に読む) | Rational・Complex・Set の中身の C の構造体を写すことになる (`checkpoint_bundled.c` と同じ追従が増える)。handler が Ruby を呼べば同じことが起きる |
| C. 書き出しの呼び出しを別の文脈で行う | 枠の表は守れるが、文脈の切り替えと例外の扱いが増える。handler の問題は残る |

hook の約束 (patch の注記) に「メソッドを呼んでよい。VM は判定の後に `mrb->c->ci` から枠を取り直す」
と書いた。`vm.c` の patch が変わったので命令数を比べた (§費用)。

### 試験

`test/targeted/ci_grow.rb`: Task の中で、最初の `mark` が 4 つ目の枠になる (Task のブロック・
`level1`・`level2`・`mark`) ように呼び、Rational を局所変数に持つ。`--at-send mark` で `mark` の
最初の命令で書く。書き出しの `numerator` の呼び出しがちょうど表を伸ばす。

| 作り | ASan の版 (`tools/asan.sh fuzz`、4 通り) | 普段の試験のビルド |
|---|---|---|
| P4 (966b46f) | 4 通りとも heap-use-after-free | 通る (解放済みの領域の中身がたまたま残っている) |
| P4b | 4 通りとも通る | 通る |

(後の点は復元の後で、読む側は枠の表に 16 個の余裕を持たせるので、満杯になるのは最初の点だけ)

## 費用 (valgrind の命令数)

64 bit、mruby-checkpoint の gem なし (patch だけの差を見る)、同じ設定 (mrblib は両方 `-g`)。
「前」は `0001` だけ、「後」は `0001` + `0002`。cachegrind (`--cache-sim=no`) の I refs。

| ベンチマーク | 前 | 後 | 差 |
|---|---:|---:|---:|
| bm_fib | 45,932,151,929 | 45,932,152,000 | +71 (+0.0000002%) |
| bm_so_lists | 8,120,754,445 | 8,120,754,661 | +216 |
| bm_ao_render | 56,304,760,745 | 56,304,795,225 | +34,480 (+0.00006%) |
| bm_so_mandelbrot | 12,885,437,849 | 12,885,439,244 | +1,395 |
| bm_mandel_term | 183,235,415 | 183,235,710 | +295 (+0.0002%) |
| Task のベンチマーク (仮想の時計) | 364,531,445 | 364,795,746 | +264,301 (+0.073%) |

Task のベンチマーク (`CHECKPOINT_VIRTUAL_CLOCK=1` のビルド。タイマだと命令数が時間で動くため):
Task 4 つが 8 回に 1 回 `Task.pass` しながら 4 万回まわる、Queue の生産者 (4 回に 1 回 pass) と
消費者 (pop で待つ) で 3 万個、sleep_ms 4 を 300 回する Task、2 つを join で待つ Task。頼みは
約 6 万回で、1 回あたり 4 命令ほど (数を進める読み書き)。普段のベンチマーク (Task なし) は、
`vm.c` も `mrb_state` の並びも変わらないので、差は起動の時の分だけ。

### `vm.c` の patch の取り直し (§4)

「前」は `0001` (P4 のまま) + `0002`、「後」は取り直しを足した `0001` + `0002`。別の回に測ったので、
同じ実行ファイルでも上の表と数十万の違いがある (同じものを 2 回測った差: bm_fib 約 10 万、
bm_ao_render 約 19 万。起動の環境で動く分)。

| ベンチマーク | 前 | 後 | 差 |
|---|---:|---:|---:|
| bm_fib | 45,932,249,669 | 45,932,291,645 | +41,976 (+0.0001%) |
| bm_so_lists | 8,120,786,701 | 8,120,784,937 | −1,764 |
| bm_ao_render | 56,304,981,405 | 56,369,927,051 | +64,945,646 (**+0.115%**) |
| bm_so_mandelbrot | 12,885,430,083 | 12,885,438,944 | +8,861 |
| bm_mandel_term | 183,235,526 | 183,235,673 | +147 |
| Task のベンチマーク (仮想の時計) | 364,795,746 | 364,795,746 | 0 |

bm_ao_render の増えた分はすべて `mrb_vm_exec` の中 (37,321,753,406 → 37,386,693,348、cg_annotate)。
足した命令は遅い側にしか無いが、`ci` に代入する所が増えたことでコンパイラの `mrb_vm_exec` の作り
(レジスタの割り振り) が変わった分と見ている。受け入れ条件 (1% 以内) の中。避けるなら、`vm.c` を
変えずに、書き出しの呼び出しを別の文脈で行う案 (§4 の C) がある (決めてほしいこと 3)。

## 試験の数字

すべて最後の形 (`0001` の取り直し・`0002`・mrblib の `-g`) のビルドで流した。

### サンプルと狙った点 (`rake test`)

| | 結果 |
|---|---|
| `SABIRUBY_DIR=... rake test` | サンプル 685 passed, 0 failed (サンプル 24 本。`lines_mrblib` を足した)、狙った点 8 通りすべて通る、短い版: top-level の形 3,011 点 (書けた 2,860)、Task の形 3,758 点 (3,575)、食い違い 0 |
| `rake test:asan` / `rake test:stress` | 443 passed / 443 passed |

Task の形の短い版の点が P4 (3,012) より多いのは、`mruby-object-ext/object.rb` (U2。往復なしでも
落ちる) の往復なしが命令を数え終える前に落ち、平均の間隔が 1 になって 771 点を試みたため (除外の表の
とおり食い違いには数えない)。

`rake test:stress` は、最初は 9 つ落ちた (`lines`・`fiber_generator` の Task の形、往復なしを含む)。
ASan と割り当てごとの GC で遅くなり、POSIX のタイマが Task の中の Fiber で時間の割り当てを終わらせ、
その Task が止まる (P4 の U3。checkpoint と関係なく、並べて流すと往復なしでも出力が途中で切れる)。
重い試験を同時に流していたので当たった。ASan の版 (`tools/asan.sh`) の mruby-task を仮想の時計に
した (メモリの検査が目的の版で、タイマの振る舞いは普段のビルドの `rake test` が見る)。その後は通る。

### 長い版 (`ruby tools/fuzz.rb --seed-from 1 --runs 20 --points 200 --isolate`)

| | P4 top-level | P4 Task の形 | P4b top-level | P4b Task の形 |
|---|---:|---:|---:|---:|
| 試みた点 | 407,953 | 420,831 | 407,133 | 419,751 |
| 書けた像 | 390,085 | 402,646 | 389,186 | 401,493 |
| 断った | 17,868 | 18,185 | 17,947 | 18,258 |
| 往復なしと食い違った実行 | 0 | 0 | **0** | **0** |

除外の表に当たったもの (P4b): `core/proc.rb` 4、`mruby-io/io.rb` 12、`mruby-io/file_test.rb` 3、
`mruby-method/method.rb` 1、(Task の形のみ) `mruby-object-ext/object.rb` 3。

断った理由は P4 と同じ種類 (C の枠 5,8xx、プロセスの資源 11,696、U2 の枠が命令列の外 240 (Task の形)、
鍵の hash の副作用 140、名前の付かない permanent 30 前後、C の関数が resume・yield した Fiber 80 前後)。
「合流 (前の要求が待っている)」が 180・229 に戻った (P3 は 590、P4 は 0)。次の境目で書くので、要求の
次の命令が C の関数の呼んだブロックに入ると、安全点がそのブロックを出るまで来ない。断りではない
(その間の点は数えるだけ)。

この長い版の最初の 1 回 (`0001` の取り直しの前) で、Task の形の `mruby-rational/rational.rb` 種 16 が
SIGSEGV で食い違った (§4)。直した後、両方の形を流し直したのが上の数字。

### 時間の割り当て (`--quantum 300`、種 1〜5、既定の設定)

| | P4 (満タン、既定) | P4b (残りを運ぶ、既定) |
|---|---|---|
| top-level の形 | 食い違い 0 | 食い違い 0 (101,631 点、書けた 97,112) |
| Task の形 | 2 ファイル (`lazy.rb` 5 実行、`enumerator.rb` 3 実行) | **食い違い 0** (104,445 点、書けた 99,817) |

### AddressSanitizer

試験のビルドの ASan 版 (`tools/asan.sh fuzz`)、種 1・2、約 100 点、同じプロセスのみ: top-level の形
20,220 点、Task の形 21,504 点、食い違い 0、ASan の報告 0。狙った点の試験も ASan の版で 8 通りとも
通る。

### SabiRuby

| | 結果 |
|---|---|
| サンプルの像 (`rake test` の SabiRuby の部分、両方の形) | すべて続き、出力が一致。`lines_mrblib` は像の行番号で `mrblib/array.rb:20:in each` を出す |
| `tools/fuzz_sabiruby.rb --gems mruby-fiber,mruby-task,mruby-enumerator`、1 本 20 点、種 1〜3 | 2,400 点のうち書けた 2,368、SabiRuby で断った 36 (未対応の種類と鍵の hash の副作用)、除外の表に当たった 1 (`Array#hash`)、続いた残りはすべて一致 (食い違い 0) |
| 同じ、Task の形 (種 1) | 760 点のうち書けた 748、断った 13、続いた 735 はすべて一致 |
| `cargo test --workspace` | feature off 276 passed、on 281 passed |
| `SABIRUBY_GC_STRESS=1 cargo test --features checkpoint --test checkpoint` | 5 passed |
| `tools/check_no_std.sh` / `--features checkpoint` | 両方 no_std OK |

## SabiRuby との違い (この段で分かったもの)

| 項目 | C mruby (このビルド) | SabiRuby | したこと |
|---|---|---|---|
| mrblib の行番号 | `-g` で作る (この段から) | 行番号なしで作られている (`src/mrblib/core.mrb`) | 像から運んだ mrblib の枠には像の行が付く。復元の後に SabiRuby が新しく呼んだ mrblib のメソッドの枠には行が出ない (C では出る)。`lines.rb` の Task の中のブロックを `Array#each` からプログラムのメソッドに替え、mrblib の枠の行は 1 つの像で済む `lines_mrblib.rb` で確かめる |

## サブの仮決め

1. **`mrb_checkpoint_keep_timeslice` を消した** (§1)。満タンから始める口は使い道が見当たらず、
   往復なしと違う動きを選べる口になる。要るようになれば足し直せる (像には残りがある)。
2. **`mrb_checkpoint_request_here` を消した** (§2)。外からの要求の口は `mrb_checkpoint_request`
   だけ。点の位置は P3 と同じ「頼んだ次の境目 (かその後の最初の安全点)」。
3. **mrblib を `mrbc -g` で作る** (§3)。このリポジトリの全部のビルドで。mrblib の irep に行の表が
   付くぶん、静的なデータが増える: `mrblib.o` と gem の `gem_init.o` の合計 (`size`) で、64 bit は
   text +29.4 KB・data +42.6 KB、32 bit は text +33.2 KB・data +24.8 KB (約 58 KB)。Family mruby で同じことをするかは、持ち込むときに決める (P5 で全部の枠に `file:line` を
   出すなら要る)。
4. **mrblib のファイル名はソースの木からの相対** (§3)。絶対のパスは機械ごとに違い、公開の像に
   入れたくない。mrbc を根で走らせる形にし、mruby の build の側は変えていない
   (`build_config/checkpoint.rb` で `MRuby::Command::Mrbc#run` を包む)。
5. **join を待つ Task を起こす所の省略を外した** (§2)。振る舞いは変わらないが、印が立っている
   ときにも優先度を比べるので、そこで `MRB2TASK` を読む回数が増える。mruby-task は、今の文脈が
   Task の中の Fiber のとき `MRB2TASK` で Task でないものを読む (P4 の上流の不具合と同じ根:
   Fiber の文脈を Task の文脈と見なす)。この読みは patch の前から印が立っていない場合に起きて
   いた。ASan の試験で報告は出ていない (§試験の数字)。
6. **`lines.rb` の Task の中のブロックをプログラムのメソッドに替えた** (§SabiRuby との違い)。
7. **安全点の後の枠の取り直しは `vm.c` で行う** (§4 の A)。命令数は決めてほしいこと 3。
8. **ASan の版 (`rake test:asan`・`test:stress` が使うもの) の mruby-task を仮想の時計にした**
   (§試験の数字)。遅い版で POSIX のタイマが U3 を起こし、結果が流すたびに変わるため。
9. **狙った点は、メソッドの名前で選ぶ** (`--at-send`)。命令の数 (`--at`) で選ぶと、試験一式や
   mrblib が変わるたびに数え直しになる。

## 決めてほしいこと

1. **mrblib を `-g` で作ること** (サブの仮決め 3)。このビルドの 32 bit で静的なデータが約 58 KB
   増える。Family mruby に持ち込むとき、mrblib の行番号のために flash (と、静的なデータが RAM に
   写る分) を払うか。払わないなら、像の mrblib の枠に行は出ない (形式はそのまま。書く側は行番号の
   ある irep だけを書く)。
2. **mruby-task の patch** (`0002`) を、上流に出す形 (切り替えの頼みを数える口) にするか、
   このリポジトリの patch のままにするか。P4 の上流の不具合 U1〜U5 の報告と合わせて決める。
3. **安全点の後に枠を取り直す `vm.c` の変更** (§4) の費用: bm_ao_render で命令数 +0.115% (他は
   ±0.0001% 以下)。このままでよいか、`vm.c` を変えない別の直し方 (書き出しのメソッドの呼び出しを
   別の文脈で行う。埋め込む側の handler が Ruby を呼ぶ場合は約束で禁じる) にするか。おすすめは
   このまま (hook の中で何が動いても正しい)。ESP32 での時間は持ち込むときに測る。
