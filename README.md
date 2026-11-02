# mruby-ghost

mruby の VM の実行状態 (ヒープ・callinfo・VM スタック) を、動いている途中で
書き出し、別の `mrb_state`・別のプロセス・別のアーキテクチャ (64bit / 32bit)・
別の VM 実装 (SabiRuby) で続きから動かす原理検証。

- mruby は `vendor/mruby` (submodule)。Family mruby が使っている版 (`f56d44ecc`) に
  合わせてある。
- 計画は `doc/plan.md`、段ごとの指示は `doc/instruction_pN.md`、経過は `doc/report/`。
