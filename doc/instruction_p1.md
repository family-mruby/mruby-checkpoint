# 実装指示書 P1: 形式 v0、同じプロセスの往復、64bit ⇔ 32bit

`doc/plan.md` を先に読む (方針はそこが正)。この段はスコープの 1-3。

## 作るもの

1. **ビルド**: `vendor/mruby` (`f56d44ecc`、触らない) を使い、このリポジトリの
   build_config で 2 本作る。
   - 64bit (ホストの gcc) と 32bit (`-m32`。`gcc -m32` は使えることを確認済)。
   - 値の表現は Family mruby に合わせる: `MRB_NO_BOXING`、`MRB_INT64`、
     `MRB_UTF8_STRING`。gem は mruby の標準の gembox に、mruby-task
     (hal-posix-task) と下の mruby-ghost を足す。
   - 置き場所は `build/` (git に入れない)。入口は `Rakefile` (`rake build`、
     `rake test`)。
2. **mruby 本体への patch** (要るときだけ): `patches/` に置き、ビルドの前に当てる
   (submodule の中を直接コミットしない)。何を・なぜ当てたかを report に書く。
   上限の目安は安全点への相乗りと、書き出しに要る最小の口だけ。
3. **gem `mrbgems/mruby-ghost`**: 直列化と復元。
   - Ruby からの口の案: `Ghost.hop(path)` で状態をファイルに書き出してプロセスを
     終える。別のプロセスで `ghost resume path` すると、`hop` の次の行から続く。
     `hop` は cfunc の呼び出し自体が C の境目なので、どう止めるか (要求の印を立てて
     次の安全点で止める、`hop` の呼び出しの ci を特別に扱って戻り値を復元の側で
     入れる、など) を考えて選び、理由を report に。
   - 復元は新しい `mrb_state` (`mrb_open` 済み) の上に行う。`mrb_open` と gem の
     初期化が作るもの (組み込みのクラス・モジュール・定数・cfunc) は**名前で
     引き当てる** (permanent)。運ぶのはプログラムが作ったものと、permanent な
     クラスへの変更分 (足したメソッド・定数・ivar)。
   - 対応する型 (v0): nil/true/false、Integer、Float、Symbol、String、Array、Hash、
     Range、Object (ivar)、ユーザ定義の Class / Module (メソッドは irep)、Proc と Env
     (ブロック・クロージャ)、Exception。callinfo と VM スタック (メソッドの途中・
     ブロックの中で止まれること)。Fiber・Task は余裕があれば、無ければ拒否。
   - 対応しない型 (RData など、cfunc の別表に無いもの) を見つけたら、どのオブジェクト
     のどの型かを出して書き出しを拒否する。
   - 形式は 64bit と 32bit で同じ (エンディアン・整数の幅を固定)。先頭に形式の版と
     mruby の版。
4. **ドライバ** `ghost` (C の小さなコマンド): `ghost run prog.rb` と
   `ghost resume file`。
5. **形式の仕様書** `doc/format_v0.md`: 意味論の層で書く (C の構造体の名前に
   頼らない)。レコードの型の一覧、参照の番号、permanent の引き当て、callinfo の
   表し方 (irep の ID + 命令のオフセット + レジスタの窓)、irep の ID の決め方。
   SabiRuby の実装者が読んで復元を書ける粒度で。

## 試験 (`rake test`)

- サンプル `test/samples/*.rb` を、往復なしで最後まで動かした出力と、途中で
  `hop` して復元して続けた出力を比べ、完全に一致すること。組み合わせ:
  同じプロセスの新しい `mrb_state`、64→64、64→32、32→64、32→32。
- サンプルは最低: カウンタ (`hop` の次の行から数え続ける)、メソッドの途中・
  ブロックの中・`each` の中 (C の境目を挟む所。拒否されるか先送りされるかを確かめる)、
  クロージャが外の局所変数を書き換える、ユーザ定義のクラスとインスタンス、
  組み込みクラスへのメソッドの追加 (`class String; def ...`)、Hash を鍵ごと復元、
  例外の rescue の中、対応しない型を持つときの拒否。
- ベンチマーク: `vendor/mruby/benchmark` のいくつかを、patch の前後 (と gem の
  有無) で比べ、差を report に書く (目安 1% 以内)。

## 触ってよい範囲

- このリポジトリ (`tmp/mruby-ghost`) の中だけ。`vendor/mruby` は submodule の
  ままで、中身は patches/ から当てる形にする。
- Family mruby (fmruby-core・graphics-audio)、SabiRuby のリポジトリは読むだけ
  (参考にしてよい)。

## 止まる条件

- sudo やパッケージの追加が要るとき。
- mruby 本体に大きな改造 (VM の命令の実行・GC の作りを変える) が要ると分かったとき。
- 見立てと違う事実 (安全点に相乗りできない、など) が出たとき。
- 方式の選択で、plan.md の方針から外れる必要が出たとき。

## 守ること

- コミットはこのリポジトリの `main` に (英文、Co-Authored-By: Claude Opus 5.5
  <noreply@anthropic.com>)。push しない。文書に手元の絶対パスやユーザ名を書かない。
- `build/` は git に入れない。終わったら build/ は残してよい (このリポジトリ専用)。
- report は `doc/report/p1.md`: 選んだ方式と理由 (`hop` の止め方、permanent の
  引き当て、Env)、当てた patch、試験の結果、ベンチマークの差、見立てと違った点、
  撤回した仮説、残件 (P2 の SabiRuby の側で要りそうなこと)。
