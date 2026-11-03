# mruby-checkpoint

[日本語](README.ja.md)

Write the execution state of a running mruby VM (heap, call frames, VM stack,
Tasks and Fibers) to an image, and resume it from the same point in another
`mrb_state`, another process, another architecture (64-bit / 32-bit), or another
VM implementation ([SabiRuby](https://github.com/sabiruby/sabiruby), in Rust).

This is a proof of concept, developed for [Family mruby](https://github.com/family-mruby).

- mruby is `vendor/mruby` (a submodule), pinned to the revision Family mruby uses (`f56d44ecc`).
- The design notes, phase instructions and reports under `doc/` are written in Japanese
  (`doc/plan.md`, `doc/instruction_pN.md`, `doc/report/`). The image format is `doc/format_v1.md`.

## Patches to mruby (the gem alone is not enough)

The gem `mrbgems/mruby-checkpoint` works with an mruby that has these patches applied
(`rake build` applies `patches/` in order).

| Patch | Touches | Purpose |
|---|---|---|
| `0001-vm-safepoint-hook.patch` | `src/vm.c` | A safe-point hook, called from the slow path of the per-instruction check (`RETURN_IF_TASK_STOPPED`). The VM reloads its frame afterwards, since the hook may move the call-info stack. |
| `0002-task-switch-requests.patch` | `include/mruby.h`, mruby-task | The scheduler counts its switch requests, so an external checkpoint request never swallows one (e.g. a `Task.pass`). |
| `0003-vm-return-target-class.patch` | `src/vm.c`, `include/mruby.h` | Removes a stale test in `OP_RETURN`. Merged upstream as mruby/mruby#7731; not needed with later mruby. |

## Usage

```
rake build    # build/mruby/host (64-bit) and build/mruby/host32 (32-bit)
rake test     # test/samples with and without checkpoints, plus the inspector tests
rake test:asan, rake test:stress   # 64-bit under AddressSanitizer (stress: GC at every allocation too)
rake bench    # benchmarks: no patch / patches only / patches + gem
rake sabiruby # build SabiRuby in SABIRUBY_DIR with its `checkpoint` feature
rake build:fuzz               # test builds (build/fuzz) with checkpoint-fuzz, an external trigger
rake fuzz[seed,runs,points]   # stop mruby's own test suite at random safe points, round-trip, compare
rake fuzz:sabiruby[seed,points]   # continue a subset of those on SabiRuby
```

Run a program through the driver, `build/mruby/host/bin/checkpoint run prog.rb`. Inside it:

- `Checkpoint.save("x.ckpt")` writes the state and keeps running; it returns `:saved`.
- `Checkpoint.suspend("x.ckpt")` writes the state and exits the process.
- `checkpoint resume x.ckpt` continues from the line after either call; there the call
  returns `:resumed` (`Checkpoint.resumed?` is true in a restored state). `--inproc`
  resumes in a fresh `mrb_state` of the same process.
- A request inside a block called from C (`sort { }` and the like) raises
  `Checkpoint::Error` at once: a C frame cannot be carried.
- Images can be written inside Fibers and Tasks, including while Tasks wait; the
  restored side continues the context that was running (a Fiber, or a Task and the scheduler).
- `checkpoint run --as-task prog.rb` starts the program the way Family mruby starts an
  app (its body as a Task, then `mrb_task_run`).

```ruby
# counter.rb
n = 0
5.times do
  n += 1
  puts n
  Checkpoint.suspend("counter.ckpt") if n == 2
end
```

```
$ build/mruby/host/bin/checkpoint run counter.rb
1
2
$ build/mruby/host32/bin/checkpoint resume counter.ckpt   # or: sabiruby --resume counter.ckpt
3
4
5
```

Embedders can request an image from outside, without the program's help, with
`mrb_checkpoint_request()` (written at the next safe point; see
`include/mruby/checkpoint.h`). The host's own Task is recorded with
`mrb_checkpoint_set_host_task()` and found again after a restore with
`mrb_checkpoint_host_task()`.

Notes:

- With `SABIRUBY_DIR` (a SabiRuby checkout) or `SABIRUBY_BIN` (a `sabiruby` built with
  the `checkpoint` feature), `rake test` also resumes the images on SabiRuby; without
  either it says it skipped them. SabiRuby's entry point is `sabiruby --resume x.ckpt`.
- `checkpoint names` lists the names of the permanent objects (what another
  implementation must be able to bind; compare with `sabiruby --checkpoint-names`).
- `ruby tools/sabiruby_fixtures.rb DIR` writes SabiRuby's test images (`tests/checkpoint/`).
- The build needs Prism, mruby's parser. It is cloned into `build/prism` at a fixed
  commit (from GitHub by default; `PRISM_SRC=<local clone>` overrides).
- The test builds (`build/fuzz`) run mruby-task on a virtual clock
  (`mrbgems/hal-task-virtual`): ticks advance when the scheduler is idle and when the
  root sleeps (`checkpoint-fuzz --quantum N` also every N instructions).

## Looking inside an image

`tools/checkpoint_inspect.rb` reads an image. There is one reader
(`tools/lib/checkpoint_image.rb`), and every view is built from its JSON (described in
`doc/inspect_json.md`).

```
ruby tools/checkpoint_inspect.rb x.ckpt                 # every record (--iseq for the instructions too)
ruby tools/checkpoint_inspect.rb x.ckpt --stack         # call stacks and locals per context (root, Tasks, Fibers)
ruby tools/checkpoint_inspect.rb x.ckpt --tasks         # the scheduler, Task queues and waits, context chains
ruby tools/checkpoint_inspect.rb x.ckpt --objects [--depth N] [--from PATH|#id]   # the object graph
ruby tools/checkpoint_inspect.rb x.ckpt --json [--pretty] [--no-code]
ruby tools/checkpoint_inspect.rb x.ckpt --html out.html # one self-contained HTML file with all views
ruby tools/checkpoint_inspect.rb diff a.ckpt b.ckpt [--json | --html out.html]   # the difference of two images
```

- Each `--stack` line matches mruby's own backtrace at that point (checked by `rake test`).
- The diff matches objects by their path from the roots (globals, constants, frames,
  Tasks), not by object number; the limits are in `doc/inspect_json.md` §9.
- In the HTML, `#view=stack|tasks|objects|diff|image` selects the view
  (`&focus=<id>` sets the start of the object graph). The page text is Japanese.
- A broken image is reported with the byte offset and the record, and exit status 1.

## License

MIT. See `LICENSE`.
