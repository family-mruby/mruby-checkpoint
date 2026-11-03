# mruby-checkpoint

[English](README.md)

mruby の VM の実行状態 (ヒープ・callinfo・VM スタック) を、動いている途中で
書き出し、別の `mrb_state`・別のプロセス・別のアーキテクチャ (64bit / 32bit)・
別の VM 実装 (SabiRuby) で続きから動かす原理検証。

- mruby は `vendor/mruby` (submodule)。Family mruby が使っている版 (`f56d44ecc`) に
  合わせてある。
- 計画は `doc/plan.md`、段ごとの指示は `doc/instruction_pN.md`、経過は `doc/report/`。

## mruby への patch (gem だけでは動かない)

gem `mrbgems/mruby-checkpoint` は、mruby の本体に次の patch を当てたものと組で使う
(`rake build` が `patches/` を順に当てる)。

| patch | 変える所 | 何のため |
|---|---|---|
| `0001-vm-safepoint-hook.patch` | `src/vm.c` | セーフポイントの hook。命令の境目の判定 (`RETURN_IF_TASK_STOPPED`) の遅い側から呼び、hook が呼び出しの表を動かしたらフレームを取り直す。ルートのコンテキスト (抜ける Task が無い) で立った切り替えの要求はそこで下ろし、続く命令が遅い側を通り続けないようにする |
| `0002-task-switch-requests.patch` | `include/mruby.h`、mruby-task | スケジューラが切り替えを要求した回数を数える (外からの停止と `Task.pass` が重なっても失わない) |
| `0003-vm-return-target-class.patch` | `src/vm.c`、`include/mruby.h` | `OP_RETURN` の古い判定を消す。上流の mruby/mruby#7731 で取り込まれたので、それ以降の mruby では要らない |

## 使い方

```
rake build    # build/mruby/host (64bit) と build/mruby/host32 (32bit)
rake test     # test/samples を checkpoint あり・なしで比べる
rake test:asan, rake test:stress   # 64bit を AddressSanitizer (stress は GC の負荷も) で
rake bench    # patch なし / patch のみ / patch + gem のベンチマーク
rake sabiruby # SABIRUBY_DIR の SabiRuby を feature checkpoint 付きで作る
rake build:fuzz               # 試験のビルド (build/fuzz): 外から止める checkpoint-fuzz
rake fuzz[seed,runs,points]   # mruby の試験一式をランダムなセーフポイントで止めて往復させ、往復なしと比べる
rake fuzz:sabiruby[seed,points]   # その一部を SabiRuby で続ける
```

- `rake test` には、試験一式の短い版 (シード 1、1 ファイル約 30 点、数秒) と、イメージのリーダーと表示の試験
  (`tools/test_inspect.rb`) も入る。
- 試験一式の比べ方・除外の表は `tools/fuzz.rb` と `test/fuzz_exclude.txt`、結果は
  `doc/report/p3.md`。

- `SABIRUBY_DIR` (SabiRuby の checkout) か `SABIRUBY_BIN` (feature `checkpoint` 付きの
  `sabiruby`) があると、`rake test` はイメージを SabiRuby でも `resume` する (無ければ飛ばして、
  飛ばしたことを出す)。SabiRuby 側の入口は `sabiruby --resume x.ckpt`。
- `checkpoint names` は permanent の名前の一覧を出す (別の処理系の名前の引き当てを確かめる
  材料。`sabiruby --checkpoint-names` に渡す)。
- `ruby tools/sabiruby_fixtures.rb DIR` は SabiRuby の試験用のイメージ (`tests/checkpoint/`) を作る。

- ビルドには mruby の構文解析器 Prism が要る。`build/prism` に固定のコミットで
  clone する (既定の取得元は GitHub。`PRISM_SRC=<手元の clone>` で置き換えられる)。
- `build/mruby/host/bin/checkpoint run prog.rb` で動かす。プログラムの中で
  - `Checkpoint.save("x.ckpt")` は状態を書き出して動き続ける。戻り値は `:saved`。
  - `Checkpoint.suspend("x.ckpt")` は状態を書き出してプロセスを終える。
  - `checkpoint resume x.ckpt` で、どちらも呼び出しの次の行から続く。復元した側
    では呼び出しの戻り値が `:resumed` になる (`Checkpoint.resumed?` は復元した
    状態なら真)。`--inproc` は同じプロセスの新しい `mrb_state` で続ける。
  - C の関数が呼んだブロックの中 (`sort { }` など) では、要求した時点で
    `Checkpoint::Error` になる。
  - Fiber・Task の中でも、Task が待っている最中でも書き出せる (P4)。続きは、実行中だった
    コンテキスト (Fiber、または Task とスケジューラ) から。
  - `checkpoint run --as-task prog.rb` は、Family mruby のアプリと同じ起動の形 (プログラムの
    本体を Task にして `mrb_task_run`) で動かす。
  - `Checkpoint.save("x.ckpt", source: true)` (`suspend` も同じ) は、プログラムのソースもイメージに
    入れる (任意のセクション `SRC0`。表示用で、実行には使わない。インスペクタが各フレームの行の前後を出す)。
    `checkpoint run --with-source` は、`source:` を書かない呼び出しの既定を「入れる」にする。既定は入れない。
    埋め込む側は、コンパイルするファイルのソースを `mrb_checkpoint_add_source()` で登録する。
- 形式は `doc/format_v1.md` (v0 は `doc/format_v0.md`、経過として残す)、報告は `doc/report/`
  (`p1.md`・`p1b.md`・`p2.md`・`p3.md`・`p4.md`・`p4b.md`・`p5.md`・`p6.md`)。

## イメージを見る

`tools/checkpoint_inspect.rb` はイメージを読んで見せる (リーダーは `tools/lib/checkpoint_image.rb` の 1 つ。
どの表示もその JSON から作る。JSON の形は `doc/inspect_json.md`)。

```
ruby tools/checkpoint_inspect.rb x.ckpt                 # 全部の記録 (--iseq で命令列も)
ruby tools/checkpoint_inspect.rb x.ckpt --stack         # コンテキスト (本体・Task・Fiber) ごとのコールスタックとローカル変数
ruby tools/checkpoint_inspect.rb x.ckpt --tasks         # スケジューラ、Task のキューと待ち、コンテキストのチェーン
ruby tools/checkpoint_inspect.rb x.ckpt --objects [--depth N] [--from PATH|#番号]   # オブジェクトのつながり
ruby tools/checkpoint_inspect.rb x.ckpt --json [--pretty] [--no-code]
ruby tools/checkpoint_inspect.rb x.ckpt --html out.html # 4 つを見られる 1 ファイル (外の資源を読まない)
ruby tools/checkpoint_inspect.rb diff a.ckpt b.ckpt [--json | --html out.html]   # 2 つのイメージの差
```

- `--stack` の行は、その点で例外を上げたときの mruby のバックトレースと同じ (`rake test` で確かめる)。
  イメージにソースが入っていれば、各フレームの行の前後も出す (止まった行に印。HTML でも同じ)。
- 差はオブジェクトの番号でなく、ルート (グローバル変数・定数・フレーム・Task) からのパスで対応を取る。限界は
  `doc/inspect_json.md` §9。
- HTML は `#view=stack|tasks|objects|diff|image` で開く表示を選べる (`&focus=番号` で
  オブジェクトの図の起点)。
- 壊れたイメージは、どのバイトのどの記録で壊れたかを出して終了の値 1。
- 試験のビルド (`build/fuzz`) の mruby-task は仮想の時計 (`mrbgems/hal-task-virtual`) で動く:
  tick はスケジューラが空いたときと root が sleep したときに進む (`checkpoint-fuzz --quantum N`
  は N 命令ごとにも)。
- gem の初期化はセーフポイントの hook を置くだけ。VM ごとの準備 (状態の表、`Checkpoint` モジュール) は
  `mrb_checkpoint_open()` か、gem を設定する最初の呼び出し (`mrb_checkpoint_baseline()` など) で行う。
  使わない VM は費用を払わない。
- 埋め込む側は `mrb_checkpoint_request()` で、プログラムの協力なしに外から書き出しを要求できる
  (次のセーフポイントで書く。`include/mruby/checkpoint.h`)。間の命令がスケジューラに切り替えを要求したか
  は、mruby-task が要求を数える数で見分ける (`patches/0002-task-switch-requests.patch`)。埋め込む側の Task は
  `mrb_checkpoint_set_host_task()` でイメージに記し、復元の後 `mrb_checkpoint_host_task()` で引く。
