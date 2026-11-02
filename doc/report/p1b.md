# P1b 報告: mruby-checkpoint への改名、save / suspend、C の境目の中は例外

> 状態: 実装済・検収待ち | 更新: 2026-11-02 | 指示は `doc/instruction_p1b.md`

## 結果

- 名前を ghost から checkpoint へ改めた。リポジトリの中に ghost の語が残るのは、
  経過の記録 (`doc/report/p1.md`・`doc/instruction_p1.md`。冒頭に改名の 1 行を
  足した) と、指示書 P1b と、`doc/plan.md` の「Ghost の名前はデモの層にだけ残す」
  の決めごとだけ。
- Ruby の API は `Checkpoint.save(path)` (書き出して動き続ける) と
  `Checkpoint.suspend(path)` (書き出してプロセスを終える、P1 の `Ghost.hop`)。
  どちらもドライバの `checkpoint resume` で呼び出しの次の行から続く。
- 呼び出しの戻り値で 2 つの側を見分ける: 書いた側は `:saved`、復元した側は
  `:resumed`。
- C の関数が呼んだブロックの中で要求すると、その時点で `Checkpoint::Error` が上がる。
  rescue すればそのまま続く。先送りはやめた。
- 形式は変えていない。先頭のマジックだけ `MRGH` → `MRCK` に改め、版は v0 のまま。
- `rake test` 110 項目、`rake test:asan` 110 項目、`rake test:stress` 110 項目、
  すべて通過。

## 変えた名前

| P1 | P1b |
|---|---|
| `mrbgems/mruby-ghost` | `mrbgems/mruby-checkpoint` |
| `include/mruby/ghost.h` | `include/mruby/checkpoint.h` |
| `src/ghost*.c`・`ghost_internal.h` | `src/checkpoint*.c`・`checkpoint_internal.h` |
| モジュール `Ghost`、`Ghost::Error`、`Ghost::Box` | `Checkpoint`、`Checkpoint::Error`、`Checkpoint::Box` |
| `Ghost.hop(path)` | `Checkpoint.suspend(path)` (と新しい `Checkpoint.save(path)`) |
| `Ghost.resumed?` | `Checkpoint.resumed?` (意味は同じ: イメージから復元した状態なら真) |
| C の `mrb_ghost_*`、`MRB_GHOST_*`、内部の `ghost_*`・`GHOST_*` | `mrb_checkpoint_*`、`MRB_CHECKPOINT_*`、`checkpoint_*`・`CHECKPOINT_*` |
| `mrb_ghost_set_hop_handler`、`mrb_ghost_hop_handler` | `mrb_checkpoint_set_handler`、`mrb_checkpoint_handler` (引数に種類 `enum mrb_checkpoint_kind` を足した) |
| `MRB_GHOST_MODE_HOP` | `MRB_CHECKPOINT_MODE_WRITE` |
| ドライバ `ghost run [--no-hop] …` / `ghost resume …` | `checkpoint run [--ignore] …` / `checkpoint resume [--inproc] …` |
| `build_config/ghost.rb`、環境変数 `GHOST_GEM`・`GHOST_TARGETS`・`GHOST_DEBUG` | `build_config/checkpoint.rb`、`CHECKPOINT_GEM`・`CHECKPOINT_TARGETS`・`CHECKPOINT_DEBUG` |
| `tools/ghost_inspect.rb` | `tools/checkpoint_inspect.rb` |
| 拡張子 `.ghost` (`.gitignore` も) | `.ckpt` |
| 形式の先頭のマジック `MRGH` | `MRCK` |
| サンプルのマーカー `# ghost-test: hop-after` | `# checkpoint-test: suspend-after` (と新しい `save-after`) |
| `rake bench` の変種 `bench-ghost` | `bench-checkpoint` |

patch (`patches/0001-vm-safepoint-hook.patch`) はコメントの `mruby-ghost` の語だけ
改めた。`vm.c` に入るコードは P1 と同じ。

形式の版は v0 のままにした。中身の並びは何も変えておらず、変わったのは先頭のマジック
だけ。P1 のイメージを外に出した所は無いので、読み替えの互換は持たせていない (P1 のイメージは
「not a checkpoint image」で断られる)。

## save の戻り値の扱い

止め方は P1 と同じ: `save` / `suspend` はフラグを立てて戻り、呼び出し命令が終わった
直後のセーフポイントで hook が書き出す。この時点で呼び出しの戻り値はもう呼んだ側の
レジスタに入っている。それを使う。

1. `save` / `suspend` の C の関数は、自分のフレームの始まり (= 呼んだ側の R[a]、戻り値が
   入る所) のスタック上の位置と、呼んだ側のフレームの深さを控え、`:saved` を返す。
2. セーフポイントの hook は、いまのフレームがその深さで、控えた位置に `:saved` があることを
   確かめ (違えば `Checkpoint::Error`)、そこを `:resumed` に置き換えて書き出し、
   すぐ `:saved` に戻す。
3. よってイメージの中ではその呼び出しの値が `:resumed` で、復元した側は特別な扱い
   なしに `:resumed` を受け取る。書いた側は `:saved` のまま続く。

形式には何も足していない (レジスタの中身が違うだけ)。`doc/format_v0.md` §10 に
この約束を書いた。読む側 (P2 の SabiRuby) は何もしなくてよい。

`suspend` も同じ形にした。書いた側はふつう終わるので値を見ないが、自前の handler
が戻った場合 (埋め込み側の使い方) は `:saved` が返る。

`--ignore` (往復なしの基準の実行、P1 の `--no-hop`) では何も書かず、「本当に動かした
とき続く側が見る値」を返す: `save` は `:saved`、`suspend` は `:resumed`。基準の出力と
往復した出力をそのまま比べられるようにするため。

`Checkpoint.resumed?` も残した。これは状態ごと (イメージから復元した `mrb_state` なら真)
で、復元した側がまた `save` すると、その戻り値は `:saved` で `resumed?` は真になる。
「この呼び出しのどちらの側か」は戻り値で、「この状態が復元されたものか」は
`resumed?` で見る。

`Checkpoint.send(:save, …)` と `Checkpoint.method(:save).call(…)` でも、書いた側
`:saved`・復元した側 `:resumed` になることを手で確かめた (どちらも戻り値の位置が
直接の呼び出しと同じになる)。

## C の境目の中は例外にした

`save` / `suspend` の C の関数の中で、いまのフレームからルートのフレームまで callinfo をたどり、
`cci` が 0 でないフレーム (C から VM に入ったフレーム) があれば、その時点で
`Checkpoint::Error` を上げる。`task_across_c_boundary()` (vm.c の static 関数) と
同じ判定を gem の側に持った。理由には C の関数の名前を入れる (C から入ったフレームの
1 つ下のフレームのメソッド名と、それが見つかったクラス):

```
Checkpoint.suspend inside a block called from C (Array#sort!): the C frame cannot be carried
Checkpoint.save inside a block called from C (Array#initialize): the C frame cannot be carried
```

- `save` / `suspend` 自身のフレームは VM から呼ばれた C の関数のフレームで、`cci` は 0。
- Ruby で書かれた反復 (`each`・`times`・`map` など mrblib のもの) は C の境目に
  ならず、P1 と同じくその場で止まる。
- この判定が通れば、要求のあとの最初の命令の境目で必ず hook が呼ばれる (例外の
  途中でもなく、C の境目もない)。先送りは起きなくなった。P1 の「決めてほしいこと
  2」(抜けないまま終わると何も起きない) はこれで無くなった。
- Fiber の中の要求と C の境目の中の要求は、呼んだ場所だけで決まる断りなので、
  `--ignore` でも同じく例外にする (基準の実行と往復の実行で出力がそろう)。
  書き出してみて分かる断り (運べない型など) は P1 のまま、次の境目で上がる。
- `save` の既定の handler でファイルが書けないときは `Checkpoint::Error` を上げる
  (`suspend` は P1 と同じく終了の状態 2 で終わる)。handler が例外で抜けてもイメージの
  バッファを解放するよう、hook の中で handler の呼び出しを `MRB_TRY` で包んだ。

## ドライバの `--inproc`

`suspend` は P1 と同じ (新しい `mrb_state` に復元して続け、古い方には戻らない)。
`save` では、新しい `mrb_state` に復元したものを最後まで動かしてから、書いた側に
戻って続ける。出力は「save までの基準の出力 + 復元した側の出力 + 書いた側の残り」に
なる。同じプロセスの中で 2 つの側を両方試すための形。

## 試験

`tools/run_tests.rb` を新しい API に合わせ、次を足した。

- `save.rb` (新): mrblib の `each` のブロックの中、メソッド 2 段の中で `save` し、
  外のローカル変数を捕まえたラムダもある。次を確かめる。
  - 書いた側 (64・32) の出力が往復なしの基準と完全に一致し、イメージが書かれている。
  - そのイメージを 64・32 で復元した側の出力が `save.resumed.out` と一致する
    (`side resumed resumed?=true` から始まり、残りは基準の save より後と同じ)。
    4 通り (64→64・64→32・32→64・32→32)。
  - `--inproc` (64・32) の出力が「基準の save までの行 + `save.resumed.out` + 基準の
    残り」と一致する。
  - イメージが `tools/checkpoint_inspect.rb` で最後まで読める。
- `c_boundary.rb` (書き直し): `sort { }` の中の `suspend` と `Array.new { }` の中の
  `save` が、要求した時点で `Checkpoint::Error` になり、rescue してブロックが続く
  (最初の比較の直後に理由の行が出る)。C の関数を抜けた後の `suspend` は通り、
  64・32 のすべての組み合わせで往復なしと一致する。
- 既存のサンプルは `Ghost.hop` → `Checkpoint.suspend` などの名前の追従だけ。
  期待の出力で変わったのは、名前を含む行 (`data_box.out`・`refuse.out`) と、
  `closure.rb` の出力の文言 (`before hop` → `before suspend`)。

ASan の版の試験を `rake test:asan` と `rake test:stress` (GC_STRESS も) にした。
`tools/asan.sh` で 64bit の版を作り、`CHECKPOINT_BIN64` で 64bit のドライバを
差し替えて同じ試験を流す (32bit の側は普通の版)。

| | 結果 |
|---|---|
| `rake test` | 110 passed, 0 failed (12 サンプル) |
| `rake test:asan` | 110 passed, 0 failed |
| `rake test:stress` | 110 passed, 0 failed |

ASan の版は最初、`resume` が読んだイメージのバッファの解放漏れを報告した。P1 では
`resume` が必ず `exit` で終わっていたが、`save` の `--inproc` のために「復元して
最後まで動かす」を戻る関数にしたため。解放するよう直した。

`build/` は古いフォルダ名で作られていたので、`build/prism` (固定のコミットの
clone) を残して消し、作り直した。

## ベンチマーク

`tools/bench.rb` は名前の追従だけ (変種 `ghost` → `checkpoint`。冒頭のコメントに
ホストの時間は目安で、判定は valgrind の命令数と ESP32 の時間で行うことを書いた)。
`vm.c` に入るコードは変わっていないので測り直していない。`doc/plan.md` の受け入れ
条件の速さの行を「ホストでは valgrind の命令数、時間は ESP32 で」に改めた。

## 残件

- C の API は handler の型が変わった (種類の引数を足した)。P1 の handler をそのまま
  使うことはできない。使っているのはこのリポジトリのドライバだけ。
- `save` の戻り値の置き換えは「呼んだ側のフレームの深さと、戻り値が入る位置」を控えて
  確かめている。`send`・`Method#call` では位置が合うことを確かめた。位置が合わない
  経路があれば、書き出さずに `Checkpoint::Error` (「the call's value is not where
  the checkpoint expects it」) になる。黙って違う値を入れることはない。
- P3 の「外からの要求でランダムなセーフポイントで止める」では、`save` の呼び出しが無いので
  戻り値の置き換えは起きない。その場合の両側の見分け方 (`resumed?` で足りるか) は
  P3 で決める。
