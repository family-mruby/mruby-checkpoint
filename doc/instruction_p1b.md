# 実装指示書 P1b: mruby-checkpoint への改名と口の整理

P1 は検収済み (`doc/report/p1.md`)。この段は名前と口を整えるだけで、形式・方式は
変えない。

## 決まったこと (2026-11-02 ユーザ)

- 汎用の機能の名前は **`mruby-checkpoint`** (Linux の CRIU と同じ言葉。保存・移動・
  巻き戻しを全部含む)。Ghost の名前は、この上に作るデモの層 (別のボードへの移動・
  シングルトン) にだけ残す。このリポジトリからは消す。
- Ruby の口:
  - `Checkpoint.save(path)`: 状態を書き出して**動き続ける**。復元した側も `save` の
    次の行から続く。(どちらの側かを見分ける手段が要るなら、`save` の戻り値で区別する
    案を考えて report に。例: 書いた側は `:saved`、復元した側は `:resumed`。)
  - `Checkpoint.suspend(path)`: 状態を書き出して**プロセスを終える** (P1 の
    `Ghost.hop` と同じ)。
  - どちらも、ドライバの `resume` で次の行から続く。
- C の境目の中 (`sort { }` など C の関数が呼ぶブロックの中) で `save` / `suspend` を
  頼んだら、**頼んだ時点で例外** (`Checkpoint::Error`、理由つき) にする。先送りは
  やめる。Ruby で書かれた繰り返し (`each`・`times`) の中は今まで通りその場で行う。
- 誤り: `Ghost::Error` → `Checkpoint::Error`。

## やること

- gem `mrbgems/mruby-ghost` → `mrbgems/mruby-checkpoint`、モジュール・C の関数名・
  ドライバのコマンド (`ghost` → `checkpoint`。`checkpoint run` / `checkpoint
  resume`)、`tools/ghost_inspect.rb` などの道具、サンプル、Rakefile、build_config、
  patch の中のコメント (`mruby-ghost` の語)、README・`doc/plan.md`・
  `doc/format_v0.md` の名前を追従させる。拡張子 `.ghost` も改める (`.ckpt` など、
  .gitignore も)。
- 形式の先頭の印 (magic) に ghost 由来の文字があれば改め、形式の版の扱い (v0 のまま
  か) を report に書く。
- `doc/report/p1.md` は経過の記録なので書き換えない (冒頭に「この段の後で
  mruby-checkpoint に改名した」と 1 行だけ足す)。`doc/instruction_p1.md` も同じ。
- 試験: 既存のサンプルを新しい口に直し、次を足す。
  - `save` (続行) の往復: 書いた側と復元した側の両方の出力を、往復なしと比べる。
  - C の境目の中の `save` / `suspend` が、頼んだ時点で `Checkpoint::Error` になり、
    rescue して続けられること。
- `rake test` (ASan と GC_STRESS の両方も) が通ること。
- ベンチマークの道具 (`tools/bench.rb`) は名前の追従だけ。速さの判定は
  「ホストでは valgrind の命令数、時間は ESP32 で」に改める (`doc/report/p1.md` の
  追加の調査の結論) ので、`doc/plan.md` の受け入れ条件の速さの行をそう直す。

## 触ってよい範囲・守ること

- このリポジトリの中だけ。`vendor/mruby` は触らない。
- `build/` の中の古いビルドは消して作り直してよい (フォルダ名が変わったので)。
- コミットは `main` に (英文、Co-Authored-By: Claude Opus 5.5
  <noreply@anthropic.com>)。push しない。文書に手元の絶対パスやユーザ名を書かない。
- report は `doc/report/p1b.md`: 変えた名前の一覧、`save` の戻り値の扱い、例外に
  した所の実装、試験の結果、残件。
