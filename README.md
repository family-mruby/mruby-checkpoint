# mruby-ghost

mruby の VM の実行状態 (ヒープ・callinfo・VM スタック) を、動いている途中で
書き出し、別の `mrb_state`・別のプロセス・別のアーキテクチャ (64bit / 32bit)・
別の VM 実装 (SabiRuby) で続きから動かす原理検証。

- mruby は `vendor/mruby` (submodule)。Family mruby が使っている版 (`f56d44ecc`) に
  合わせてある。
- 計画は `doc/plan.md`、段ごとの指示は `doc/instruction_pN.md`、経過は `doc/report/`。

## 使い方

```
rake build    # build/mruby/host (64bit) と build/mruby/host32 (32bit)
rake test     # test/samples を hop あり・なしで比べる
rake bench    # patch なし / patch のみ / patch + gem のベンチマーク
```

- ビルドには mruby の構文解析器 Prism が要る。`build/prism` に固定のコミットで
  clone する (既定の取得元は GitHub。`PRISM_SRC=<手元の clone>` で置き換えられる)。
- `build/mruby/host/bin/ghost run prog.rb` で動かし、`Ghost.hop("x.ghost")` で状態を
  書き出して終わる。`ghost resume x.ghost` で続きから動く (`--inproc` は同じ
  プロセスの新しい `mrb_state` で続ける)。
- 形式は `doc/format_v0.md`、P1 の報告は `doc/report/p1.md`。
