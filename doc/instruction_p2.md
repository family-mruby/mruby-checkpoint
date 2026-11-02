# 実装指示書 P2: mruby → SabiRuby の片道

`doc/plan.md`、`doc/format_v0.md`、`doc/report/p1.md`・`p1b.md` を先に読む。この段は
スコープの 4: **mruby (C) が `Checkpoint.suspend` / `save` で書いた像を、SabiRuby
(Rust 製の mruby VM) が読んで、次の行から続ける**。逆向き (SabiRuby → mruby) は
この段ではやらない。

## 前提 (確認済み)

- SabiRuby は mruby 4.1 準拠で RITE 04.00 を読む。こちらの mruby は 4.0.0 系
  (`f56d44ecc`)、RITE 04.00。SabiRuby の mrblib は 4.1.0-rc2 で作られている。
- SabiRuby のヒープは `Heap { objs: Vec<HeapObject> }`、参照は `ObjId(u32)`、値は
  `Value` enum (16 バイト)、格納場所は `Slot`。到達可能性 GC がある
  (`docs/design/gc.md`)。Fiber は `docs/design/fibers.md`。
- 作業場所: SabiRuby の worktree `/home/kishima/fmrb/wt/sabiruby-checkpoint`
  (ブランチ `feature/checkpoint`、作者の手元の `main` から切った)。SabiRuby の
  本体の checkout (`~/book/kishima/sabiruby`) は触らない。

## 決まったこと (2026-11-02 ユーザ)

- SabiRuby への変更は Rust で、SabiRuby の中の **feature `checkpoint` (既定 off)** に
  置く。off のときは普段のビルド・速さ・no_std に影響しないこと。
- 計画は SabiRuby の `docs/plans/checkpoint-plan.md` に**作者向けの案**として置く
  (SabiRuby の既存の plans の書き方に合わせる。この段の実装の記録も兼ねてよい)。
- **止まっていたフレームが指す mrblib の irep だけは中身も運び**、他の mrblib は名前
  (`M:Integer#times` など) で引き当てる。SabiRuby の mrblib とハッシュが合わない
  ため。これは mruby (C) 側の書き出しの変更になる (下の 1)。

## やること

1. **mruby (C) 側** (このリポジトリ):
   - 生きたフレームが指す mrblib の irep を、ハッシュでの引き当てに加えて中身も
     書けるようにする (必要な時だけ。既定は今のまま、または常に、どちらがよいかは
     判断して report に)。形式に足すものは `doc/format_v0.md` を直し、形式の版を
     上げるかどうかも report に。
   - 組み込みの名前 (permanent の名前の一覧) を、SabiRuby 側と突き合わせる表を作る
     材料として書き出せるようにする (例: `checkpoint names` で一覧を出す)。
2. **SabiRuby 側** (worktree):
   - `checkpoint` feature の下に像の読み込み (復元) を作る: 形式の検査 → 全オブジェクト
     を確保 → 参照をつなぐ → permanent を名前で引き当て → irep の取り込み (プログラムの
     irep はこちらの `.mrb` の読み込みと同じ道で、mrblib は名前か運ばれた中身で) →
     callinfo・レジスタの窓・Env の組み立て → 続きから実行。
   - SabiRuby の CLI (`sabiruby`) に `--resume FILE` か同等の入口を足す (feature
     on のときだけ)。
   - 名前の対応: こちらの permanent の名前 (`S:Object`、`M:Integer#times`、
     `F:Array#length` など) を SabiRuby の組み込みに引き当てる。足りないものは像を
     拒否し、何が無いかを出す。
   - 命令の集合・意味の差 (4.0.0 系と 4.1) が問題になる所があれば、表にして report に。
3. **試験** (このリポジトリの `rake test` に足す。SabiRuby のビルドが無いときは
   飛ばしてよいが、飛ばしたことを出す):
   - `test/samples` のうち対応できるものを、mruby 64bit・32bit で書いた像を
     SabiRuby で `resume` し、出力が往復なしと一致すること。最低限: カウンタ、
     ユーザ定義のクラス、クロージャ、`times` の中の suspend (mrblib のフレーム)、
     Hash、`save` の戻り値 `:resumed`。
   - 対応できないものは、どれが・なぜかを表に。
   - SabiRuby 側の既存の試験 (README の Verification の節) が feature off と on の
     両方で通ること。no_std の検査 (`tools/check_no_std.sh`) も。

## 触ってよい範囲

- このリポジトリ (`tmp/mruby-checkpoint`) と、SabiRuby の worktree
  (`/home/kishima/fmrb/wt/sabiruby-checkpoint`) だけ。SabiRuby の本体の checkout と
  `main` は触らない。`vendor/mruby` は触らない。
- SabiRuby の Docker の参照 mruby (`kishima/mruby:4.1.0-rc2`) は使ってよい。

## 止まる条件

- SabiRuby の VM の中心 (命令の実行・GC・Value の表現) に大きな改造が要るとき。
  feature の下の追加と、復元に要る最小の口 (内部の構造体を組み立てる関数) までは可。
- 形式を意味論の層で書いたつもりが、C の mruby の内部に依存していて SabiRuby で
  組み立てられない所が見つかったとき (直し方を案にして返す)。
- sudo・パッケージの追加が要るとき。

## 守ること

- コミット: このリポジトリは `main`、SabiRuby は `feature/checkpoint` (英文、
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>)。どちらも push しない。
  SabiRuby は公開のリポジトリなので、文書に手元の絶対パス・ユーザ名・Family mruby の
  発表の話を書かない (技術の話だけ)。
- SabiRuby のコミットの書き方・文書の書き方は、その README の Rules と既存の
  docs に合わせる。
- `target/` は大きいので、終わったら worktree の `target/` を `cargo clean` で消す。
- report は `doc/report/p2.md`: 選んだ方式と理由、SabiRuby に足した口、名前の対応の
  表、命令の差、試験の結果、拒否した型・場面、見立てと違った点、撤回した仮説、
  残件 (逆向きの書き出しで要りそうなこと)。
