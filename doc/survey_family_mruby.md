# Family mruby へ持ち込むための調査: 原理検証と実アプリの隔たり

> 状態: 調査済 | 更新: 2026-11-03 | mruby-checkpoint (P5b まで) と Family mruby (fmruby-core、feature/linux-native) の間にある隔たりを、VM とビルド・アプリの作り・エンジン・gem と C のデータ・端末のセーフポイント・転送に分けて棚卸しし、最初のデモ (1 本の mruby アプリを 2 つの Family mruby の間で運ぶ) までの段と、決めることを並べる

## 0. 読み方

- パスは、fmruby-core のものは `fmruby-core/...`、このリポジトリのものはリポジトリの根からの相対で書く。
- 根拠の強さを書き分ける。
  - **[確認]**: コードや設定を読んで確かめた。
  - **[推定]**: 読んだことからの見立て。持ち込むときに確かめる。
  - **[未測]**: 測っていない数字 (見積もり)。
- 見たもの: fmruby-core `a06bb89b` (feature/linux-native)、その中の picoruby の mruby `f56d44ecc`、このリポジトリ `bf8c68e`。
  ビルドも sim も動かしていない (読んだだけ)。数字のうち測ったのは、このリポジトリの 32bit ビルドの
  オブジェクトの大きさと permanent の名前の数だけ。

## 1. まとめ

### 主な隔たり

1. **ウィンドウを持つアプリは、今は書き出しを断られる** [確認]。窓のあるアプリは必ず `@gfx`
   (`FmrbGfx`、C のデータ `mrb_gfx_data_type`) を持ち、別表に登録が無い型は断る決まりなので。Asterism に
   つないだアプリも、セッション (`zrb_session_type`) を持つので同じ。まず運べるのは、窓も通信も
   持たないアプリ (ヘッドレス) だけ。
2. **アプリはほとんどの時間を C の関数 `_spin` の中で過ごし、イベントは C から `mrb_funcall` で呼ばれる**
   [確認]。`_spin` の中 (メッセージ待ち) にはセーフポイントが来ず、`on_event`・`on_control` の中は
   C の境目なので断られる。止める要求は `_spin` を起こして抜けさせ、その外で書く形にする必要がある
   (§3.2)。アプリが自分で移動を命じる API も、`on_event` から呼べば断られる。
3. **時分割の仕組みが違う** [確認]。Family mruby は tick を VM の外 (mruby-task の FreeRTOS の HAL) で数え、
   切り替えの要求を別のスレッドから、または命令の境目から直接 `task.switching` に書く。`patches/0002`
   の「要求を数える」はこの書き込みを通らないので、HAL 側にも同じ手当てが要る。タイムスライスの残りも
   HAL の側の数 (`tick_countdown`) が決めていて、イメージに無い。
4. **patch の形が違う** [確認]。Family mruby は mruby のファイルを丸ごと置き換える (`lib/patch`) ので、
   `patches/0001`・`0002` は当てられない。`vm.c` の同じマクロ (`RETURN_IF_TASK_STOPPED`) に
   Family mruby も自前の時分割を入れている。`0003` は Family mruby に既に同じ直しがある。
5. **gem の作りが 1 プロセス 1 VM に寄っている** [確認]。静的な表 (`states[8]`、`made_funcs[64]` の
   6,404 バイト) と、比べる関数のための静的変数 (`sort_mrb`) がある。Family mruby は 1 プロセスに最大
   32 の VM が別々の FreeRTOS のタスク (2 コア) で動き、静的な領域は ESP32 では内蔵 RAM に載る。
6. **VM の外にアプリの分身が多い** [確認]。窓の位置・大きさ・重なり・キャンバスの中身、スプライトの
   画像、購読しているトピック、開いているファイル、MIDI の予定、Spinel の gem の実体、エディタの文書、
   ソケット、Zenoh のセッション。どれもイメージに入らず、着いた先で作り直すか、諦めるかを決める必要がある (§3.3、§5)。
7. **時刻を絶対値で持つ所がある** [確認]。`FmrbApp#set_timer` は `Machine.board_millis` (起動からの
   ミリ秒) を ivar に持つ。端末が違えば起動からの時間も違うので、着いた先でタイマが狂う。mruby-task の
   tick は「新しい VM の tick を書いたときの値にする」で既に解いているが、`board_millis` は VM の外の時計。
8. **転送と遠隔起動の道具は部品しかない** [確認]。Asterism はオブジェクトの公開と呼び出し・値の置き場・
   参加と離脱の通知を持つが、塊を運ぶ仕組みも、別のノードでアプリを起動する口も無い。zenoh-pico の
   既定の組み立て直しの上限は 4 KB (`Z_FRAG_MAX_SIZE 4096`)。

### 最初のデモまでの段 (詳しくは §8)

| 段 | 内容 | 何が分かるか |
|---|---|---|
| F0 | 持ち込みの土台 (patch の統合、gem の組み込み、静的領域の除去、普段の速さと内蔵 RAM の測定) | 普段のコストが無いこと |
| F1 | ネイティブ版の 1 プロセスの中で、ヘッドレスのアプリを書き出し、同じ端末で別のアプリとして続ける | Family mruby の起動の形 (プール・Task・HAL・`_spin`) で通ること |
| F2 | ネイティブ版の 2 プロセスの間を /tmp のファイルで | 別のプロセス、時計のつなぎ方 |
| F3 | 窓を持つアプリ (`FmrbGfx`・キャンバス・購読の作り直し、アプリの再描画の hook) | VM の外の資源の作り直し |
| F4 | ビルドの違う端末の間 (sim ⇔ ネイティブ、ブラウザ版 (32bit) ⇔ ネイティブ) | 別の gem の組み合わせ・ポインタ幅 |
| F5 | Asterism で運ぶ (分割して送り、着いた先で起動) | ネットワーク越しの移動 |
| F6 | 実機 (Tab5、その後 S3) | PSRAM の使い方、内蔵 RAM 0、ESP32 での時間 |

### 決めること (詳しくは §9。先頭がおすすめ)

D1 最初の組み合わせ (ネイティブ ⇔ ネイティブ) / D2 gem の置き場所 (このリポジトリを固定点で取り込む) /
D3 patch の持ち方 (Family mruby の丸ごとの patch に混ぜる) / D4 載せるビルド (ネイティブと sim から) /
D5 止める地点 (`_spin` を起こして外で書く、のちに配りを Ruby へ) / D6 アプリの側の約束 (申告制、
hook の名前は既存と別) / D7 C のデータの dump の置き場所 (各 gem) / D8 VM の外の資源 (記述子で作り直す +
アプリの再描画) / D9 `board_millis` (VM ごとのずれで続ける) / D10 タイムスライスの残り (Family mruby
では満タン) / D11 イメージと作業の領域 (/tmp と、VM のプールの外の PSRAM) / D12 基準を取る範囲 (申告した
アプリだけ) / D13 転送 (Asterism の公開したオブジェクトに分けて送る) / D14 段取りを持つ所 (C とサービスの
アプリ) / D15 移動元のアプリの終わらせ方 (着いた側の返事のあと) / D16 Spinel のアプリ (対象外のまま)。

## 2. VM とビルドの違い

### 2.1 mruby の版と patch

mruby の版は同じ (`f56d44ecc`) [確認]。違うのは、その上に重ねたもの。

Family mruby の重ね方: `fmruby-core/lib/patch/picoruby-mruby/lib/mruby/` のファイルを、Rakefile が
picoruby の submodule の中へ**丸ごと上書き**する (submodule は直接編集しない決まり、
`fmruby-core/CLAUDE.md`) [確認]。

| ファイル | Family mruby の変更 [確認] | mruby-checkpoint の patch | 重なり |
|---|---|---|---|
| `src/vm.c` | 自前の時分割 (`MRB_TASK_TICK_SELF_SUPPLY` のとき `RETURN_IF_TASK_STOPPED` の頭で `TASK_SELF_TICK`、`RETRY_TRY_BLOCK` で数え直し)。U2 の直し (`OP_RETURN` の `keep_context` の判定を消す) | 0001 (セーフポイントの hook、遅い側を関数に、`L_TASK_CHECK`)、0003 (U2) | **0001 は同じマクロを書き換える**。手で混ぜる。0003 は Family mruby の直しと同じ中身 (注釈 1 行が違うだけ) なので要らない |
| `mrbgems/mruby-task/src/task.c` | U4 の取り込み (main の Task の値をマーク)、スケジューラのループの頭で HAL の tick を受け取る (`mrb_hal_task_take_pending_ticks`、自前の時分割なら `mrb_hal_task_account_ticks`) | 0002 (`switching_ = TRUE` を `task_request_switch` に) | 別の場所。混ぜられる |
| `mrbgems/mruby-task/src/gc.c` | `GC.step` を足した | なし | なし |
| `mrbgems/mruby-task/ports/freertos/task_hal.c` (Family mruby が足したもの) | tick を VM の外で数え、切り替えの要求を `vm->mrb->task.switching = TRUE` で直接書く (`vm_deliver_switch`) | なし | **0002 の数がここを通らない** (§2.3) |
| `include/mruby.h`、`mrbgems/mruby-task/include/task.h`、`src/task_queue.c` | Family mruby は変えていない | 0002 (`switch_requests` を `mrb_task_state` に、`task_request_switch` のマクロ、キューの起こし方) | Family mruby の patch の対象が 3 つ増える |

- 0002 は `mrb_state` の大きさを変える (`mrb_task_state` に 4 バイト) [確認]。Family mruby は rake の側と
  CMake の側でヘッダを同じに保ち、起動時に `mrb_state` の大きさを照らし合わせている
  (`picorb_abi_mrb_state_size`、`fmruby-core/main/app/fmrb_app.c`) [確認]。同じヘッダを両方が読むので
  食い違いは出ない見込み [推定]。
- `lib/patch` は丸ごとの置き換えなので、上流に追従するたびに 3 つ (vm.c・task.c・task_hal.c) +
  0002 で増える 3 つの手当てが要る。U2 の上流 PR (mruby/mruby#7731) が入れば vm.c の差は 1 つ減る。

### 2.2 値の表現と定義

| 端末 | ポインタ | `MRB_INT64` | `MRB_NO_BOXING` | `MRB_UTF8_STRING` | profile | `MRB_USE_DEBUG_HOOK` | 自前の時分割 | `MRB_TICK_UNIT` / スライス |
|---|---|---|---|---|---|---|---|---|
| S3 (Retro) | 32 | あり | あり | あり | CONSTRAINED_BASELINE | あり | `FMRB_TASK_SELF_TICK=1` のときだけ | 5 ms / 10 tick |
| P4 (Tab5、NARYAv4) | 32 | あり | あり | あり | CONSTRAINED_BASELINE | あり | 同上 | 同上 |
| Linux sim | 64 | あり | あり | あり | BASELINE | あり | 同上 | 同上 |
| ネイティブ版 | 64 (amd64 / arm64) | あり | あり | あり | BASELINE | あり | **常に** | 同上 |
| ブラウザ版 | 32 | あり | あり | あり | BASELINE | あり | **常に** | 同上 |

出どころ: `fmruby-core/lib/add/family_mruby_{esp32,esp32p4,linux,native,wasm}.rb`、
`fmruby-core/components/picoruby-esp32/mruby_abi_defines.cmake`、`fmruby-core/lib/patch/picoruby-mruby/mrbgem.rake` [確認]。

- 原理検証の前提 (`MRB_NO_BOXING`・`MRB_INT64`・double・UTF-8、違いはポインタ幅だけ) はどの端末にも
  当てはまる [確認]。`MRB_USE_FLOAT32` も word boxing も無い。`MRB_32BIT` は ESP32 とブラウザ版で明示、
  原理検証の `-m32` では mruby が自動で決める (同じ意味) [確認]。
- profile の違い (メソッドのキャッシュの有無、ヒープのページの大きさ) は論理グラフに出ない [推定]。
- **`MRB_USE_DEBUG_HOOK` が全端末で入っている** [確認] (遠隔デバッガ用。命令ごとに NULL を 1 回見る)。
  原理検証の普段のビルドには無いので、普段の速さの比べ方は Family mruby の定義で測り直す (§2.6)。
- tick の長さとスライスは全端末で同じ (5 ms、10 tick) [確認]。イメージは待ちを残りの tick で運ぶので、
  端末の間で意味が変わらない。

### 2.3 時分割 (case-D と自前の時分割)

[確認] `fmruby-core/lib/patch/picoruby-mruby/lib/mruby/mrbgems/mruby-task/ports/freertos/task_hal.c`:

- 普段の形 (sim・ESP32 の既定): 別の FreeRTOS のタスク (`mruby_tick`) が 5 ms ごとに VM ごとの
  `pending_ticks` を増やし、10 tick ごとに `switch_pending` を立てて、別のスレッドから
  `mrb->task.switching = TRUE` を書く。`mrb_tick()` は VM のスレッドがスケジューラのループの頭で
  まとめて呼ぶ。
- 自前の時分割 (ネイティブ・ブラウザ版の常、ほかは選べる): 命令の境目で 65,536 命令ごとに
  `mrb_hal_task_self_tick` が経過時間を tick にし、同じ所で `switching` を立てる。
- どちらも、タイムスライスの終わりを決めるのは HAL の `tick_countdown` (VM の外、VM ごと) で、
  mruby-task の `t->timeslice` だけではない。

原理検証との差:

1. **切り替えの要求の数** (`patches/0002`)。`vm_deliver_switch` は `task_request_switch` を通らないので、
   外からの checkpoint の要求とタイムスライスの終わりが重なったとき、セーフポイントで「スケジューラも要求した」
   と分からず、そのスライスが長くなる (止まりはしない) [推定]。直すには `vm_deliver_switch` でも数を進める。
   普段の形では別のスレッドからの書き込みなので、数の増やし方 (原子性) も見る [推定]。
2. **タイムスライスの残り**。原理検証は `t->timeslice` の残りを運ぶ (P4b)。Family mruby で次の横取りを決める
   のは HAL の `tick_countdown` なので、残りを正確に運ぶには HAL に読み書きの口が要る [推定]。運ばなくても
   動きは正しく、往復なしと横取りの位置が違うだけ (P4 の報告と同じ性質)。D10。
3. **tick の値**。新しい VM の tick を書いたときの値にする (P4 の仮決め) は、Family mruby の「アプリごとに
   VM を作る」で成り立つ [確認: `create_vm_mruby` が毎回 `mrb_open_with_custom_alloc`]。HAL の側は
   VM の登録で時計を数え始める (`last_self_us` を今に) ので、tick を後から書き換えても困らない [推定]。

### 2.4 アロケータとプール

- アプリは 1 つずつ専用のプールを持ち、estalloc で mruby のヒープ・コンパイラ・`require` の中身を取る。
  伸びない [確認: `fmruby-core/doc/reference/app_memory.md`、`fmruby-core/main/app/fmrb_app.c`
  の `create_vm_mruby`]。大きさは S3 500 KB (大 1 MB)、P4 1 MB (大 2 MB)、sim とブラウザ版 1.5 MB (大 3 MB)、
  置き場所は端末では PSRAM。
- mruby-checkpoint の作業の領域 (書き出しのバッファ、ポインタ → 番号の表、復元の表、基準の名前と snapshot)
  は `mrb_malloc` / `mrb_realloc`、つまり**そのアプリのプール**から取る [確認: `checkpoint_util.c`、
  `checkpoint_perm.c`]。ほぼ満杯のアプリ (麻雀は起動時に 94%、`app_memory.md`) は、書き出そうとしたところで
  NoMemoryError になる [推定]。D11。
- 復元はまっさらなプールに作り直すので、断片化はむしろ減る [推定]。64bit 側は同じ中身で約 1.5 倍の場所を
  取り、プールも 1.5 倍なので、32bit → 64bit で入りきらないことはまれ [推定]。
- 例外: `checkpoint.c` の C のデータの別表・関数の名前の表は素の `malloc` / `realloc` [確認]。Family mruby の
  決まり (`fmrb_mem.h` の関数を使う) から外れ、ESP32 では内蔵 RAM に載りうる。

### 2.5 gem の作りと多重 VM

[確認] このリポジトリの `mrbgems/mruby-checkpoint/src/`:

- `checkpoint.c`: `static checkpoint_state *states[8]` (VM ごとの状態を線形に探す)。Family mruby の
  アプリの枠はネイティブ版・ブラウザ版で 32 (`FMRB_MAX_APPS=32`)。8 では足りない。
- `checkpoint_perm.c`: `made_funcs[64]` (名前 96 バイト + 関数) = 6,404 バイトの静的領域、遅延の初期化
  (`nmade_funcs = -1`)、`qsort` の比べる関数のための `static mrb_state *sort_mrb`。
- 32bit のビルドで測った gem の大きさ (`size`、x86 の -O2): コード約 128 KB、静的領域 (bss) 6,436 バイト。

Family mruby で困ること:

- 複数のアプリが 2 つのコアで同時に起動すると、`sort_mrb` と `made_funcs` の遅延の初期化が競る [推定]。
- 静的領域は ESP32 では内蔵 RAM に入る。内蔵 RAM の増分 0 の決まりに反する (§6.4) [確認: 置き場所の
  属性が無い]。
- 直し方の候補: VM ごとの状態はアプリの文脈 (`fmrb_app_task_context_t`) か VM の中に持つ。全体で 1 つの表は
  初期化を起動時に 1 回 (ロック付き) にし、PSRAM (`EXT_RAM_BSS_ATTR` 相当) か flash の定数に置く。

### 2.6 普段の速さの測り直し

- 受け入れ条件 (普段の速さの差 1% 以内、ホストは valgrind の命令数) は、Family mruby の定義
  (`MRB_USE_DEBUG_HOOK` あり、自前の時分割あり / なし) で測り直す [推定: 命令の境目の作りが違う]。
  自前の時分割では、`RETURN_IF_TASK_STOPPED` の頭に数え下げが 1 つあるので、0001 の速い側 (2 つの読み込みと
  分岐) と並べた形で測る。
- `fmruby-core/doc/reference/vm_speed_measurement.md` の決まり (ホストの時間でなく命令数) と同じやり方で、
  時間は ESP32 で測る。

### 2.7 0001〜0003 の入れ方

- 0001: Family mruby の `vm.c` (丸ごと) に手で混ぜる。`RETURN_IF_TASK_STOPPED` の速い側を
  `TASK_SELF_TICK(mrb); if (mrb_unlikely(switching || STOPPED)) goto L_TASK_CHECK;` の形にし、
  `L_TASK_CHECK` と `task_stop_requested` を足す [推定]。`MRB_USE_VM_SWITCH_DISPATCH` はどの端末の
  設定にも無く、`vm.c` が自分で立てるのは gcc / clang 以外のときだけ [確認]。Family mruby のツールチェーンは
  gcc (xtensa・riscv・ホスト) と clang (emcc) なので、`#else` 側 (computed goto) だけが効く [推定]。
- 0002: `task.c` (丸ごと) に混ぜ、`mruby.h`・`task.h`・`task_queue.c` を新しく `lib/patch` に足す。
  さらに `task_hal.c` の `vm_deliver_switch` (§2.3)。
- 0003: 要らない (同じ直しが入っている)。
- いずれも `lib/` を変えるので、ビルドの前に `rake clean` が要る (`fmruby-core/CLAUDE.md`)。

## 3. アプリの作り

### 3.1 起動から終わりまで

[確認] `fmruby-core/main/app/fmrb_app_spawner.c`、`fmruby-core/main/app/fmrb_app.c`、
`fmruby-core/lib/add/picoruby-fmrb-app/`:

1. 起動の要求 (デスクトップ、シェル、`FmrbApp#request_run`、遠隔の `/app/launch`) → spawner がファイルの
   拡張子で VM の種類を決め (`.rb` → mruby、`.lua`・`.bas`・`.py` → ほかの VM)、同じ名前の `.app.toml`
   から窓の大きさ・全画面・`large_memory`・優先度・スタックなどを読む。
2. `fmrb_app_spawn` が枠 (app id)・プール・FreeRTOS のタスク・メッセージのキューを用意する。
3. アプリのタスクで `create_vm_mruby` (`mrb_open_with_custom_alloc` + HAL への VM の登録) →
   `execute_mruby_script`: ソースならプールの中でコンパイル (組み込みのアプリは flash のバイトコードを
   `mrb_read_irep`)、`mrc_create_task` でプログラムの本体を Task にして、`mrb_task_run` を C から呼ぶ。
   これは原理検証の `checkpoint run --as-task` と同じ形。
4. Ruby の側: `MyApp.new` (`FmrbApp#initialize` が C の `_init` でキャンバスを作り、`@_canvas`・
   `@window_width` などを ivar に入れる。`@gfx = FmrbGfx.new(@_canvas, ...)`) → `start` → `on_create` →
   `main_loop` (`on_update` → `Task.pass` → `_spin(timeout)` → `_run_timers` の繰り返し) → `destroy`。
5. 終わり: `mrb_task_run` から戻り、例外を拾い、`mrb_close`。強制の kill では `mrb_close` をせず、
   開いたファイル・Spinel の gem の実体をカーネルの側で片付ける (`close_owned_files`、
   `release_spinel_gems`)。

### 3.2 フレームの形とセーフポイント

アプリが動いている間のコールスタックは、ほぼ次の 2 つのどちらか [確認: `fmrb-app.rb`、`app.c`]。

```
(a) on_update の中            Task の本体 → start → main_loop → loop (mrblib) のブロック → on_update → ...
(b) イベントの中              ... → main_loop のブロック → _spin (C) → mrb_funcall → _dispatch_event → on_event → ...
```

- (a) は Ruby のフレームだけなので運べる。`main_loop` は毎回 `Task.pass` を呼ぶので、切り替えの判定 (=
  セーフポイントの hook) が少なくとも 1 周に 1 回は通る [確認: `Task.pass` は `task_request_switch` 相当で
  `switching` を立てる]。
- (b) は C の関数 `_spin` が `mrb_funcall` で Ruby を呼んでいるので、C の境目。要求はそこでは受けられない
  (先送り)。`Checkpoint.save` をそこで呼べば `Checkpoint::Error` [確認: 原理検証の決まり]。
- `_spin` がメッセージを待っている間 (`fmrb_msg_receive`、最大 `on_update` が返した時間。サービスでは
  30 秒もある) は命令が進まないので、セーフポイントは来ない [確認]。外から要求しても、`_spin` が戻るまで
  書かれない。
- `_spin` は既に「途中で抜ける」口を持つ: `request_early_update` (`@_spin_break`) [確認]。

止め方の案 (D5):

- **案 A: `_spin` を起こして外で書く**。カーネル (または段取りの係) がアプリのキューに `APP_CONTROL` の
  `cmd = "checkpoint"` を送る → `_spin` が C の中でそれを見て `mrb_checkpoint_request(mrb)` を呼び、
  ループを抜ける → `main_loop` の次のセーフポイントで書く。要求はアプリ自身のタスクの中で出るので、別の
  スレッドから VM の状態を触らない。アプリの協力は要らない。アプリが自分で移動を頼む API (例 `hop`) も、
  その場で書かずに同じ道に乗せる (`on_event` から呼べるように)。
- **案 B: 配りを Ruby に移す**。Spinel のアプリの基底は既に「C は生のメッセージを 1 つ受けるだけ、解いて
  配るのは Ruby」の形 (`fmruby-core/main/app/fmrb_spx_app.h` の `fmrb_spx_app_recv_message`) [確認]。
  mruby の基底も同じ形にすれば、`on_event` の中も C の境目でなくなり、どこでも運べる。費用はメッセージの
  解き方 (MessagePack を Ruby から呼ぶ) の速さと、基底の作り直し。待ちの中 (C) で止まらないのは同じなので、
  起こす仕組みは案 A と同じものが要る。

### 3.3 VM の外でアプリのために持っているもの

[確認] (列挙は `fmrb_app_task_context_t` (`fmruby-core/components/fmrb_common/include/fmrb_app.h`)、
カーネルの Ruby (`fmruby-core/main/prebuild_scripts/kernel/fmrb_kernel.rb`)、各 gem から):

| 持ち主 | 中身 | 着いた先で |
|---|---|---|
| アプリの文脈 (C) | app id (枠)、名前、ファイルのパス、VM の種類、プール、タスク、状態 (RUNNING など)、窓の位置・大きさ・重なり (z)、キャンバスの id (本体・背景・追加)、全画面・高解像度・大きさを変えられるか・角の丸め、`single_instance`・`exclusive_group` の取り分、Spinel の取り分 | 起動の属性として運び、普段の起動と同じ道で作る |
| メッセージのキュー (app id ごと) | 届いて未処理の HID・制御のメッセージ | 捨てる (止める直前に `_spin` が抜けるので、残るのは数件) [推定] |
| カーネル (Ruby) | Pub/Sub の購読の表 (`@subscriptions[topic]` → pid の並び)、窓の一覧とフォーカス | 購読し直す (アプリの ivar に控えるか、hook で) |
| USB の係 | 生の HID レポートの転送先 (`hid_raw_subscribe(slot)` → app id) | 頼み直す |
| 描画の側 (graphics-audio の別プロセス、WROVER、P4 の表示) | キャンバスの絵、スプライトの画像と実体 (整数の id)、タイルの絵 | 作り直し + 描き直し (§5.3) |
| 音の側 | APU の発音中の音、`FmrbMic` の有効化 | 鳴っている音は諦める。マイクは開き直す |
| MIDI の予定 (C の環状の表) | 送り元の pid と絶対時刻 (µs) 付きの未来の音 | 諦める (着いた先で鳴らない) |
| ファイルの HAL | 開いているファイル・ディレクトリ (持ち主のタスクごと) | パスと位置が分かれば開き直す (§5) |
| Spinel の gem の実体 | FFT・raycast・spinel_hello の唯一の実体 (最初のアプリのもの) | 開き直す (取れなければ Ruby の実装に落ちる作りが既にある) |
| エディタの文書 | C の領域 (`POOL_ID_EDITOR_DOC`) の slot | 運ばない (エディタは対象外の見込み) |
| /home・/tmp | `app_state` (`/tmp/<アプリ>.state.toml`)、`app_settings`、アプリのデータ | 移動元の端末に残る。要るなら一緒に送る |
| ネイティブ版だけ | `FmrbApp.linux_*` (Linux の子プロセス)、Wayland の窓 (`wl_*`) | 運べない |

## 4. エンジン (Spinel と mruby の VM)

[確認]:

- 既定 (`fmruby-core/Rakefile`): `FMRB_KERNEL_ENGINE`・`FMRB_APP_ENGINE_DESKTOP`・`FMRB_APP_ENGINE_EDITOR` の
  どれも `mruby`。
- 標準の構成 (方針、このチェックアウトの `.env` もこの形): カーネル = Spinel、デスクトップ = mruby、
  エディタ = Spinel。ネイティブ版の既定 (`NATIVE_ENGINE=standard`、`fmruby-core/rakelib/native.rake`) も同じ。
  互換の構成は全部 mruby。
- 動き方: Spinel のカーネルとエディタ (と `DESKTOP=spinel` のときのデスクトップ) は、AOT で C にしたプログラム
  を `FMRB_VM_TYPE_NATIVE` の枠で動かす (`fmrb_app_run_spinel_vm`)。
- ファイルから起動するアプリ (`.app.rb`) は**常に mruby の VM** (`FMRB_VM_TYPE_MRUBY`)。`.app.toml` で
  エンジンを選ぶ口は無い。組み込みのアプリ (シェル、LogViewer、Monitor、Services、HID Inspector、
  App Store) も mruby の VM (flash のバイトコード)。デスクトップは既定で mruby の VM。
- Lua・BASIC・MicroPython のアプリは拡張子で別の VM。

運べるもの:

- **mruby の VM で動くアプリだけ**。ユーザのアプリはすべてこれに入る。組み込みのアプリも形の上では入る。
- Spinel のプログラム (カーネル、Spinel のエディタ・デスクトップ) は状態が C のスタックと C の構造体に
  あり、イメージにできない。互換の構成のエディタは mruby だが、文書を C の領域に持つ (§3.3) ので、
  別の作り直しが要る。
- mruby のアプリが使う Spinel の gem (FFT、raycast の `:spinel`、spinel_hello) は、アプリから見れば
  VM の外の資源 (§5)。

## 5. gem と C のデータ

### 5.1 入っている gem

[確認] `fmruby-core/lib/add/family_mruby.gembox` と各端末の設定。

| gem | 端末 | C のデータの型 / VM の外の状態 | 分類 |
|---|---|---|---|
| mruby-compiler | 全 | (コンパイルの文脈は VM の外の C) | コンパイルの途中は運べない。合間なら関係しない |
| picoruby-fmrb-io | 全 | `IO` (`mrb_io_type`: HAL のファイルの handle・フラグ・閉じたか)。パスを持たない | 作り直し (パスと位置を足す必要) |
| picoruby-fmrb-filesystem | 全 | `File` (`MRB_TT_CDATA`、IO と同じ型) | 同上 |
| picoruby-machine | 全 | 関数だけ (`board_millis`、`delay_ms` など) | 時計の扱い (§5.4) |
| picoruby-require | 全 | `$LOADED_FEATURES` (グローバル変数の Array) | そのまま運べる (変更表) |
| picoruby-sandbox・picoruby-eval | 全 | `mrb_sandbox_state_type` (コンパイルと実行の状態) | シェルが使う。途中は運べない |
| picoruby-env | 全 | `ENV` (`mrb_env_type`) | permanent。運ばずに着いた先のものを使う |
| picoruby-crc・picoruby-rng | 全 | 関数だけ | 関係しない |
| picoruby-yaml・picoruby-json・picoruby-syntax-highlight | 全 | 関数だけ / 純 Ruby | 関係しない |
| picoruby-fmrb-const | 全 | 定数 (`FmrbConst::BOARD` など、端末ごとに値が違う) | permanent。着いた先の値になる |
| picoruby-fmrb-log・picoruby-fmrb-msgpack | 全 | 関数だけ | 関係しない |
| picoruby-fmrb-kernel | 全 | カーネルへの口 | 関係しない (アプリから使う所はメッセージ) |
| picoruby-fmrb-bmp332 | 全 | 関数だけ | 関係しない |
| picoruby-fmrb-fft・picoruby-fmrb-raycast・picoruby-fmrb-spinel-hello | 全 | Spinel の唯一の実体 (タスクが持ち主) | 開き直す / Ruby の実装へ |
| picoruby-fmrb-app | 全 | `FmrbGfx` (`mrb_gfx_data_type`: 全体の描画の文脈 + キャンバスの id)。スプライトの画像と実体・タイルの絵は描画の側の整数の id。`GfxBlock` の命令は Ruby の Array | 作り直し + 描き直し (§5.3) |
| picoruby-fmrb-editor-core・picoruby-ti | 全 | C の領域の文書の slot、型推論の作業領域 | 運べない (エディタ) |
| picoruby-fmrb-debug | 全 | 遠隔デバッガの hook | 関係しない (止めている間は外す) [推定] |
| picoruby-midi・picoruby-midi-mml | 全 | 純 Ruby | そのまま |
| picoruby-fmrb-midi | 全 | シリアルの MIDI の口、予定の環状の表 (C) | 予定は諦める。口は開き直す |
| picoruby-fmrb-picorabbit | 全 | 純 Ruby (描画の上) | 描画に従う |
| mruby-kernel-ext・string-ext・array-ext・objectspace・metaprog・error・sprintf・math | 全 | なし | そのまま |
| mruby-time | 全 | `Time` | **運べる** (`checkpoint_bundled.c`) |
| mruby-dir | 全 | `Dir` (`mrb_dir_type`、開いたディレクトリ) | 作り直し (パスと位置) |
| mruby-task | 全 | `Task`・`Task::Queue` | **運べる** (P4) |
| picoruby-gpio・rmt・i2c、rx8900、rx8130、bmi270 | S3・P4 (一部の機種) | ピンの番号と周辺機器 | その端末の機器。運べない (着いた先に同じ機器があっても、意味を決めないと開き直せない) |
| picoruby-socket (+ mbedtls)、net-http、net-websocket | P4・sim・ネイティブ (S3 は `FMRB_ASTERISM=1` のときなど) | ソケット・TLS の文脈・サーバ、暗号の途中の状態 | 運べない。つなぎ直しはアプリの仕事 |
| picoruby-asterism-zenoh、picoruby-asterism、picoruby-fmrb-asterism | P4・sim・ネイティブ・ブラウザ版 (S3 は `FMRB_ASTERISM=1`) | `zrb_session_type`・購読・queryable・問い合わせ・トークン。セッションは `Asterism` のモジュールの ivar や `FmrbAsterism::Link` が持つ | 作り直し (つなぎ直して公開・購読をやり直す) |

- **gem の組み合わせは端末で違う** [確認] (ブラウザ版には socket が無い、sim には GPIO が無い、など)。
  permanent は名前で引き当てるので、イメージが**使っている**名前が着いた先にあればよい [推定:
  `doc/format_v1.md` §8 は名前ごとの対応で、全体の一致を求めていない]。無ければ断る。
- mrblib の irep は、止まっていたフレームが指すものだけ中身を運び、ほかは名前で引く (P2 の決め)。
  ファームの版が違えば、着いた先の新しい版のメソッドを使う。

### 5.2 分類

1. **そのまま運べる (書き出しと読み込みが既にある、または純 Ruby)**: Ruby のオブジェクト全般、
   `Task`・`Task::Queue`、`Time`、Regexp など同梱のもの、`$LOADED_FEATURES`、GC の設定 (`GCS0`)、乱数の状態。
2. **中身を書けば運べる (別表に dump / restore を足すだけ)**: 該当は少ない。VM の外に何も持たない C の
   データが対象。Family mruby の gem には今これにあたるものがほぼ無い [確認: `lib/add` の C のデータの型は
   `FmrbGfx` と `IO` の 2 つだけ]。
3. **着いた先で作り直すもの**: `FmrbGfx` とキャンバス、スプライト・タイルの絵、Pub/Sub の購読、
   HID の転送、ファイル・ディレクトリ、Spinel の gem の実体、マイク、MIDI の口、Asterism / Zenoh の
   セッションと公開・購読。
4. **運べないもの**: C の関数のフレームの中 (コンパイル中、`_spin` の中、C が呼んだブロック)、ソケットと
   TLS のつながり、鳴っている途中の音と MIDI の予定、周辺機器 (GPIO・I2C・RMT・センサ)、Linux の子
   プロセスと Wayland の窓、エディタの文書、Spinel のプログラム。

### 5.3 作り直し方の選択肢 (決めてはいない。D8)

| 案 | 中身 | 向く所 | 費用・弱み |
|---|---|---|---|
| (a) 記述子で開き直す | dump は「何を開いていたか」(キャンバスなら窓の大きさ、ファイルならパスと位置と開き方、購読ならトピック) を書き、restore が着いた先で同じものを開いて新しい id に付け替える | ファイル、キャンバス、購読、Spinel の実体 | IO はパスを持っていないので覚えさせる必要。id を ivar に整数で持つ所 (`@_canvas`、スプライトの `@id`) は付け替えの表が要る |
| (b) アプリの hook | 書き出しの前に `on_checkpoint`、着いた先で `on_restore` を呼び、アプリが閉じる・作り直す・描き直す | 描き直し、Asterism のつなぎ直し、ソケット | アプリの協力が要る。**`on_suspend` / `on_resume` は既に別の意味 (全画面での退避) で使っている** [確認: `fmrb-app.rb` の `_handle_system_control`] ので、名前を分ける。予約名の契約 (`fmruby-core/doc/app_model/plan.md`) も直す |
| (c) 基底が肩代わり | `FmrbApp` の基底が購読・タイマ・窓を控え (ivar)、着いた先で自動でやり直し、最後に `on_restore` だけ呼ぶ | Pub/Sub、窓、タイマ | 基底が太る (Spinel の基底にも同じことを足すかは別。Spinel のアプリは運ばない) |
| (d) 遠くの代理 | 移動元の資源を残し、着いた先から Asterism で呼ぶ | 移動元にしか無い機器 (センサ) | 移動元が生きている間だけ。遅い |
| (e) 中身ごと運ぶ | キャンバスの絵を読み戻してイメージに入れる | 描き直せないアプリ | 320×240 の RGB332 で 1 枚 76.8 KB。読み戻せる端末が限られる [推定] |

おすすめの組み合わせは (a) + (c) を骨にして、残りを (b) で埋める形 (D8)。窓のあるアプリは、`FmrbGfx` の
dump に canvas の役 (本体か背景か) を書き、restore で着いた先の新しいキャンバスへ結び、基底が `@_canvas`
などを書き換えてから `on_restore` を呼ぶ。描き直しは、`on_update` が毎回全部描くアプリなら何もしなくてよい。

### 5.4 時計

- mruby-task の tick: 解いてある (§2.3)。
- `Machine.board_millis` (端末の起動からのミリ秒): `set_timer` の期限 (`@_timers` の `:at`)、
  `FmrbAsterism::TableClient`・`Link` の時刻など、ivar に絶対値で残る [確認]。着いた先の起動からの時間が
  短ければタイマは長く待ち、長ければすぐ鳴る。案: VM ごとの「ずれ」を持ち、`board_millis` が
  「端末の時刻 + ずれ」を返す。restore で、書いたときの値から続くようにずれを決める (D9)。
- `Time.now` (壁時計) は端末ごとに正しい値で、合わせない (違って当然のもの)。

## 6. 端末でのセーフポイント、置き場所、大きさ、内蔵 RAM

### 6.1 セーフポイントと Family mruby の時分割

- セーフポイントの hook は「切り替えの要求があり、例外が飛んでおらず、ObjectSpace の走査中でなく、
  C の境目を挟んでいない」命令の境目で呼ばれる (0001) [確認]。Family mruby の時分割はこの判定の手前で
  `switching` を立てるだけなので、hook の呼ばれ方は変わらない [推定]。
- 自前の時分割では、hook の中 (書き出しの最中) も同じ VM のスレッドなので、tick が割り込まない [推定]。
  普段の形 (別のスレッド) では、書き出しの最中に `switching` と `pending_ticks` が増えうる。書き出しは
  VM の中の状態だけを読むので害は無いが、書き出しにかかった時間の tick が着いた先の待ちに含まれない
  (P4 の「移動の時間は待ちに含めない」と同じ) [推定]。
- 止める合図は §3.2 の案 A: アプリ自身のタスクで要求を出す。カーネルのタスクから
  `mrb_checkpoint_request` を直接呼ぶと、スケジューラの様子を控える所 (P4 §スケジューラのフラグ) が別の
  スレッドと競る [推定]。

### 6.2 イメージの置き場所

| 置き場所 | 中身 | 向き |
|---|---|---|
| /tmp (RAM) | S3 は 448 KB、Modern は 8,064 KB、POSIX はホストのディレクトリ [確認: `fmrb_mem_config.h`、`fmrb_tmpfs.h`] | 最初の置き場所に向く。VM の間で大きなものを渡すのは既に「/tmp に書いてパスを渡す」形 (メッセージは 176 バイトまで) |
| PSRAM のバッファ (VM のプールの外) | 書き出しのバッファと表 | アプリのプールを圧迫しない (§2.4) |
| flash の /home | 永続 | 電源を切っても続ける (ハイバネーション) には要る。書き込みは遅く、減る。後回し |
| ネットワークへ直接 | 書きながら送る | 大きいアプリで効く。最初は不要 |

### 6.3 大きさの見積もり

[未測] 原理検証の数字 (カウンタ 375 バイト、最大のサンプル 5.8 KB、mruby の試験一式の途中で 86 KB) からの
見積もり:

- ヘッドレスのカウンタ: 数 KB (アプリの irep + `FmrbApp` の ivar 約 30 個 + Task)。
- 窓のある小さいアプリ: 10〜30 KB。
- 大きいアプリ (麻雀の卓、動いている間のヒープ 0.8 MB / 64bit): 数百 KB。アプリの命令列 (ソース
  約 120 KB 分) が大きい。着いた先に同じ `.app.rb` があれば irep を中身のハッシュで引いて減らせる
  (形式の permanent な irep の引き当てを、アプリの irep に広げる案。未設計)。
- 基準 (permanent の名前の表と snapshot) は**イメージではなく VM ごとの常駐の費用**。このリポジトリの
  既定の gembox で名前 1,611 個、名前の文字だけで 27.5 KB [確認: `checkpoint names`]。Family mruby は gem が
  多いので、1 アプリあたり数十〜100 KB 程度をプールから取る見込み [未測]。S3 の 500 KB のプールには重い。D12。

### 6.4 内蔵 RAM

決まり: 増分 0、できれば減らす (Family mruby の方針)。

| 増えるもの | 置き場所 (今) | 手当て |
|---|---|---|
| gem の静的領域 6,436 バイト (`made_funcs`・`states`) | bss → ESP32 では内蔵 RAM [確認: 属性が無い] | VM ごとの状態はプールへ、全体の表は PSRAM か flash の定数へ |
| 別表・関数の名前の表 (素の `malloc`) | ESP32 の既定の malloc (内蔵になりうる) [推定] | `fmrb_sys_malloc` の PSRAM へ |
| hook の関数ポインタ 1 つ | bss | 4 バイト。ほかを減らして相殺する |
| `mrb_state` の 4 バイト (0002) | アプリのプール (PSRAM) | 内蔵 RAM には響かない |
| 基準・作業の表・イメージ | プール / PSRAM | 内蔵 RAM には響かない |
| gem のコード (32bit で約 128 KB) | flash | 内蔵 RAM には響かない。S3 の小さいパーティション (AtomS3R の 5 MB) では flash の余地を見る |
| `vm.c` の遅い側の関数 | flash (vm.c を IRAM に置く設定は無い) [確認] | 響かない |

測り方は `rake regress:size` (同じコミットの基準との静的な差) と実機の待機時の空き。

## 7. 転送 (Asterism)

[確認] fmruby-core の取り込み (`fmruby-core/lib/add/picoruby-fmrb-asterism/`、`ASTERISM_PIN`、
`PICORUBY_ASTERISM_ZENOH_PIN`) と `fmruby-core/doc/ruby_asterism/`:

- あるもの:
  - Zenoh のセッション、put / subscribe、get / queryable、liveliness、attachment (`asterism-zenoh`)。受信は
    ポーリング (`session.poll`、`each_pending`)。
  - オブジェクトの層: `Asterism.expose(name, obj, methods:, caller:)` でメソッドを公開し、ほかのノードから
    代理 (proxy) で呼ぶ。値は MessagePack (バイト列も送れる)。`caller:` で呼び手 (証明書のノード ID) を
    絞れる。`on_join` / `on_leave`・`on_app_join` / `on_app_leave` で参加と離脱が分かる。
  - Family mruby 側: 接続の設定 (`/home/asterism/config.toml`、証明書の tar)、`FmrbAsterism::Link`
    (つなぎ直しの面倒を見る)、`TableClient`。
  - 開発用の遠隔制御 (`/fs/put`・`/app/launch`、HTTP、無認証、`FMRB_DEV_REMOTE_CTL`) は「送って起動する」が
    既にできるが、開発用で、信頼できる LAN の前提。
- 無いもの:
  - 塊 (数 KB〜数百 KB) を運ぶ仕組み。zenoh-pico の組み立て直しの上限は既定で 4 KB
    (`zenoh_generic_config.h` の `Z_FRAG_MAX_SIZE 4096`。Family mruby のビルドは上書きしていない) [確認]。
    それより大きいメッセージは届かないとみるのが安全 [推定]。分けて送り、着いた先で並べ直す層が要る。
  - 別のノードでアプリを起動する口 (公開された「起動」)。`dev_remote_ctl` を queryable にする案は文書に
    あるだけ (`fmruby-core/doc/ruby_asterism/node_variants.md`)。
- 権限: 誰が誰にアプリを送り込めるかは、Asterism の console の権限の表 (別のプロジェクト asterism-console)
  の話。ここでは扱わない。

組み立ての案 (D13・D14):

1. 受け側のノードで、mruby のサービスのアプリ (受け口) が `Asterism.expose("ckpt", obj, methods: [...],
   caller: ...)` で `begin(id, size, hash, attrs)`・`chunk(id, n, bytes)`・`commit(id)` を公開する。
2. 送り側は止めたアプリのイメージを /tmp から読み、2〜3 KB に分けて呼ぶ (1 回の呼び出しが 4 KB を超えない)。
3. 受け口は /tmp に書き、ハッシュを確かめ、`request_run("/tmp/<id>.ckpt")` 相当で起動を頼む。起動の属性
   (`.app.toml` の中身) はイメージの任意のセクションか、横に置く `.toml` で運ぶ。
4. 受け側の起動が通ったら返事を返し、送り側は元のアプリを終わらせる (D15)。

## 8. 段 (最初のデモまで)

各段の終わりに、`rake regress` (と端末を足した段では `regress:size`) が通ることを条件にする。

| 段 | やること | 分かること |
|---|---|---|
| **F0 土台** | `lib/patch` の `vm.c`・`task.c`・`task_hal.c` に 0001・0002 を混ぜ、`mruby.h`・`task.h`・`task_queue.c` を足す。gem を固定点で取り込み (D2)、まずネイティブ版と sim の設定にだけ入れる (D4)。静的領域と素の `malloc` をなくす。基準は申告したアプリだけ (D12)。Family mruby の定義で普段の命令数を比べる | 入れても普段の速さ・内蔵 RAM・既存の試験が変わらないこと |
| **F1 1 プロセスの中** | ネイティブ版で、ヘッドレスのカウンタのアプリを止めて /tmp に書き、同じ端末で別のアプリとして続ける。止める合図は `_spin` を起こす形 (D5 の案 A)。起動の新しい形 (`.ckpt` を拡張子で見分けて `FMRB_LOAD_MODE_CHECKPOINT` のような読み方にし、VM を開き、基準を取り、`mrb_checkpoint_restore`・`mrb_checkpoint_continue`) と、書き出し側の終わり方 (`suspend` でプロセスを終えず、アプリを普段の終わり方で閉じる) | Family mruby の起動の形 (プール、estalloc、`mrc_create_task` の Task、HAL の tick、app id の付け替え) でイメージが通ること。`_spin` の中からでも止められること |
| **F2 2 プロセス** | ネイティブ版を 2 つ (同じビルド) 動かし、片方のイメージをもう片方で続ける (ファイルを手で渡す)。`board_millis` のずれ (D9) | 別のプロセス・別の起動時刻で、Task の待ちとアプリのタイマが続くこと |
| **F3 窓** | `FmrbGfx` の dump / restore (gem の側に登録の口)、キャンバスの作り直しと id の付け替え、購読のやり直し、`on_checkpoint` / `on_restore` (D6・D8)。窓の位置・大きさなどの属性をイメージと一緒に運ぶ | 窓を持つ普通のアプリが、着いた先で同じ見た目に戻ること |
| **F4 ビルドの違う端末** | sim (ESP-IDF の Linux、別の gem の組み合わせ) ⇔ ネイティブ、ブラウザ版 (32bit) ⇔ ネイティブ | permanent の名前の引き当てが Family mruby の gem の差で崩れないこと、32bit ⇔ 64bit |
| **F5 Asterism** | 受け口のサービスと分けて送る層 (§7)。まずネイティブ ⇔ ネイティブ、次にネイティブ ⇔ ブラウザ版 | ネットワーク越しに、送り込みから起動までが通ること。止まっている時間の実測 |
| **F6 実機** | Tab5 (PSRAM、8 MB の /tmp、ESP32 での普段の時間、内蔵 RAM 0)。その後 S3 (500 KB のプール、448 KB の /tmp) | 端末でも普段のコストが無く、移せること |

最初のデモ (1 本の mruby アプリを 2 つの Family mruby の間で運ぶ) は F2 で形になり (ヘッドレス)、
F3 で窓が付き、F5 でネットワーク越しになる。ネイティブ版は 1 プロセスで docker が要らず、MCP の
`native_*` で動かせ、sim と並べて動かせるので、最初の場に向く。

## 9. 決めること

それぞれ先頭がおすすめ。

1. **D1 最初の組み合わせ**
   - (a) ネイティブ ⇔ ネイティブ (同じビルド) **← おすすめ**: 1 プロセスで速く回せ、ビルドの違いを後に
     回せる。自前の時分割が常に入っていて、tick が 1 つのスレッドに閉じる。
   - (b) sim ⇔ ネイティブ: 両方 64bit だが、別のビルド・別の gem・別の時分割 (普段の形) を同時に相手にする。
   - (c) ブラウザ版 ⇔ ネイティブ: 32bit ⇔ 64bit が最初から入る。ブラウザ版はファイルの出し入れが遠回り。
2. **D2 gem の置き場所**
   - (a) このリポジトリを固定点 (`*_PIN`) で取り込み、`rake setup` が picoruby の木へ写す
     (`ASTERISM_PIN` と同じ形) **← おすすめ**: 素の mruby で試験一式を回す道具がこのリポジトリにあり、
     Family mruby と切り離して試せる。
   - (b) fmruby-core の `lib/add` に写して持つ: 手早いが、2 か所で直すことになる。
   - (c) 上流 (picoruby / mruby) に出す: まだ早い。
3. **D3 patch の持ち方**
   - (a) Family mruby の丸ごとの `lib/patch` に混ぜる (3 ファイル足す) **← おすすめ**: 今の決まりに合う。
   - (b) Family mruby の重ね方を差分 (`patch`) にする: 上流の追従は楽になるが、仕組みの作り直し。
4. **D4 載せるビルド**
   - (a) まずネイティブ版と sim にだけ入れ、端末は F6 で測ってから **← おすすめ**: 内蔵 RAM と flash の
     測定を先にしないで済む。
   - (b) 最初から全端末: 端末の差を早く見られるが、F0 が重くなる。
5. **D5 止める地点**
   - (a) `_spin` を起こして外で書く (§3.2 案 A)。アプリの移動の API もこの道に乗せる **← おすすめ**:
     基底を作り直さずに済み、アプリの協力が要らない。
   - (b) 配りを Ruby に移す (案 B): `on_event` の中でも運べる。費用の測定が要る。(a) の後の段で考える。
   - (c) 両方を最初から。
6. **D6 アプリの側の約束**
   - (a) 申告制 (`.app.toml` に `movable = true` のような印)。hook は `on_checkpoint` / `on_restore` のように
     既存の `on_suspend` / `on_resume` と別の名前 **← おすすめ**: 運べないものを持つアプリを黙って壊さない。
     基準の常駐の費用 (D12) も申告したアプリだけになる。
   - (b) どのアプリも運べる (断るのは書き出しのとき): 「アプリの協力なしに」に近いが、基準の費用を全アプリが払う。
7. **D7 C のデータの dump / restore の置き場所**
   - (a) 各 gem の C に置き、gem の初期化で別表に登録する **← おすすめ**: 型 (`mrb_data_type`) は static で
     gem の中にしか見えない (`FmrbGfx` も `IO` も)。P3 の親の仮決め (同梱の gem の写しを gem 側へ移すか)
     もこの形にそろえる。
   - (b) mruby-checkpoint の中に集める: 型を外から見せる変更が gem 全部に要る。
8. **D8 VM の外の資源**
   - (a) 記述子で開き直す + 基底が肩代わり + 最後にアプリの `on_restore` (§5.3 の (a)+(c)+(b)) **← おすすめ**。
   - (b) アプリの hook だけ: 作りは小さいが、アプリごとに書くことが多い。
   - (c) 遠くの代理: 機器が移動元にしか無いときの別案として残す。
9. **D9 `board_millis`**
   - (a) VM ごとのずれを持ち、着いた先でも書いたときの値から続ける **← おすすめ**: mruby-task の tick と
     同じ考え方。アプリを直さずに済む。
   - (b) `on_restore` でアプリが直す。
   - (c) そのまま (タイマが狂うのを認める)。
10. **D10 タイムスライスの残り (Family mruby)**
    - (a) 着いた先では満タンから (HAL の数は運ばない) **← おすすめ**: 動きは正しく、HAL に口を足さずに済む。
      往復なしとの完全一致は原理検証の側 (仮想の時計) で確かめてある。
    - (b) HAL に読み書きの口を足して残りを運ぶ。
11. **D11 イメージと作業の領域**
    - (a) イメージは /tmp、書き出しのバッファと表は VM のプールの外の PSRAM **← おすすめ**: ほぼ満杯の
      アプリも書ける。
    - (b) 今のまま (アプリのプール): 作りは変えないが、大きいアプリで書けない。
    - (c) ネットワークへ直接流す: F5 の後に考える。
12. **D12 基準 (permanent の名前と snapshot) を取る範囲**
    - (a) 申告したアプリ (と、復元で開く VM) だけ **← おすすめ**: 常駐の費用を払うアプリを絞れる。
    - (b) 全アプリ: どのアプリでも後から運べる。
    - (c) 名前の表をファームごとに作って flash に置き、VM ごとには番地の表だけを持つ: 費用は最小だが、
      作りの変更が大きい。測ってから考える。
13. **D13 転送の形**
    - (a) 受け口のサービスが Asterism で `begin` / `chunk` / `commit` を公開し、`caller:` で送り手を絞る
      **← おすすめ**: 証明書による相手の確認がそのまま効き、4 KB の上限に収まる。
    - (b) Zenoh の put / get を直接: 速いかもしれないが、確認の層を自分で作る。
    - (c) 開発用の遠隔制御 (HTTP): F2 までの手での確認なら使える。外には出さない。
14. **D14 段取りを持つ所**
    - (a) 止める・起動するは C (`fmrb_app.c` と spawner)、ネットワークは mruby のサービスのアプリ
      **← おすすめ**: カーネルの Ruby を触ると Spinel と mruby の両方の構成で直して確かめる必要がある。
    - (b) カーネル (Ruby) に入れる。
15. **D15 移動元のアプリの終わらせ方**
    - (a) 着いた側が起動できたと返事をしてから、移動元を普段の終わり方で閉じる **← おすすめ**: 途中で
      失敗しても消えない。間は両方にいるので、移動元はその間止めたままにする。
    - (b) 書いたらすぐ閉じる: F1・F2 の手での確認ならこれで足りる。
    - (c) 残す (複製): 別の機能 (fork) として後で考える。
16. **D16 Spinel のアプリ**
    - (a) 対象外のまま **← おすすめ**: 状態が C にあり、この方式では運べない。
    - (b) Spinel 側に別の仕組みを考える。

## 10. 確かめていないこと

- `vm_deliver_switch` と `patches/0002` の数の進め方を混ぜたときの動き (別のスレッドからの書き込み)。
- `t->timeslice` と HAL の `tick_countdown` のどちらが実際に横取りを決めているか (スケジューラのループの頭で
  `mrb_tick` が走るときに RUNNING の Task が残っているか)。
- zenoh-pico の 4 KB を超えるメッセージの実際の振る舞い (捨てるのか、誤りになるのか)。
- Family mruby の gem の組み合わせでの permanent の名前の数と、基準の 1 VM あたりの大きさ。
- 窓のある小さいアプリのイメージの実際の大きさ。
- `mrc_create_task` が作る Task と、`execute_mruby_script` の後始末 (`mrb_vm_ci_env_clear`、コンパイルの
  文脈 `mrc_cc`) を、復元した VM でどう扱うか。
- 同じ `.app.rb` を 32bit と 64bit でコンパイルした irep の中身のハッシュが同じになるか (アプリの irep を
  名前で引く案の前提)。
