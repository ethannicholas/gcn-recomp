# Diagnostics and measurement

How to drive a game without a controller, record and replay a run, measure the port, and
find out which guest code or which draw is responsible for something. Environment variables
are `GCN_*`; none of this is needed to play.

## Input logging and replay

Every run of the desktop frontend records the controller state the guest polled, keyed by
presented frame, into `saves/inputs/<timestamp>/inputs.txt`, with a snapshot of the memory
card beside it. The path is printed at startup. `--no-input-log` turns it off;
`--input-log=<dir>` chooses the directory.

`--replay=<dir>` plays a log back: the guest reads the logged controller instead of the live
one until the log runs out, then the live controller takes over. The run uses a scratch copy
of the logged card (`memcard_replay.raw` in the log directory), so the real card is never
touched and the replay sees the same saves the original did. A replay is still recorded as a
new log, so a route can be extended by replaying it and then playing on.

This is not deterministic. The guest runs on wall-clock time, so a replay can land a press a
frame early or late relative to a menu that fades on real time, and a long route can drift.
It is meant to get back to where something happened, not to reproduce it bit for bit. If a
replay diverges, replay it again with `--dump-dir` to see where, and trim or re-record from
there.

The log format is text: a header, then one line per change of a channel's state --
`frame chan connected buttons stick_x stick_y cstick_x cstick_y trig_l trig_r`, in hex.
Lines can be edited or written by hand.

## Scripted input

`GCN_INPUT="frame:BUTTON:duration,..."` presses buttons at given submitted-frame counts; `+`
joins buttons (`A+B`), and `SL SR SU SD` push the control stick. Presses are counted in
presented game frames, not wall-clock. `GCN_INPUT_LOG=1` prints each press as it fires. A
scripted press is recorded in the input log like any other, so a script is one way to make
a log.

## Frame dumps

`--hidden --dump-dir=DIR --dump-every=N` runs with the window hidden and writes every Nth
presented frame as a PNG. `GCN_DUMP_RANGE=a-b` restricts that to a window of frames.
`--headless` runs the guest with no renderer at all.

## The eye preview

`--eye` renders through the stereo path into an offscreen target and shows that in the
window instead of the flat frame: the eye sits where the game's camera is, looking straight
ahead with a 90 degree field, so a change to the VR renderer can be looked at on a desktop
before it goes near a headset. `--first-person[=x,y,z]` puts the eye on the player's vehicle
instead (`render_set_first_person`, with the anchor in game units in the vehicle's frame), and
`GCN_FPLOG=1` prints per frame where the vehicle was found and how many of the rider's draws
were left out. With `--dump-dir`/`--dump-every` the eye's frames are written as `eye_NNNNN.png`.

## Measuring the guest

`<game>_bench` runs the game with no graphics, audio or input and reports how fast the guest
advances; `--warmup=N` (default 8) excludes boot from the steady-state figure. `GCN_TIMESCALE=N`
makes the emulated timebase advance N times faster, so the game tries to run at N× real
time; raise it until the frame rate stops climbing to find a machine's ceiling. It skews
every other emulated timing, so it is a diagnostic only.

`GCN_FRAMETIME=1` prints a line per frame from each thread: the guest's interval between
presents and how much of it the GX front end took, the batch's shape, and the renderer's
time to issue it.

## Textures

`GCN_TEXLOG=1` prints a line per decoded texture: frame, id, guest address, format, size and
how many top-level texels are not black -- which tells a texture the game never wrote (all
zero) from one the shader mishandles. `GCN_TEXDUMP=<dir>` writes each texture's top level as
`<dir>/tex_<id>.png`, alpha forced opaque.

## Finding a draw or a function

- `GCN_DRAWLOG=<frame>` lists every draw in one frame with its index; `GCN_DRAW_SKIP=a-b`
  then drops a range of them, to attribute a piece of the image to the draws that made it.
- `GCN_MTXLOG=<frame>` prints the position matrix and projection of every draw in that frame;
  `GCN_PNMLOG=a-b` lists the position matrices used over a window of frames.
- `GCN_WATCH=a,b,c` (with `GCN_MTXLOG` set) prints those guest addresses every frame;
  `GCN_PEEK=a,b` prints them in the crash dump.
- `GCN_COPYLOG=1` logs EFB copies; `GCN_GXSTATS=1` prints per-frame GX counts.
- `--sample` reports once a second which guest functions each guest thread is in, by
  walking the guest's own stack; names come from the game's `symbols.txt`.
- Build with `-DGCN_TRACE_CALLS=ON` to keep a real guest call stack, printed by the crash
  dump (`GCN_TRACE_DEPTH` sets how many frames). Compiling with `-DGCN_WATCH` adds
  `GCN_COUNT=addr,...` (call counts per function, printed on interrupt) and
  `GCN_WATCH_ADDR=<hex>` (report every change to that word).
- `--log-all` enables every log category, including the DSP mailbox and DVD reads.

## Panics and faults

A game's `OSPanic` should be listed in its `hle.txt`: the replacement prints the message and
a guest backtrace and exits, instead of spinning in `PPCHalt` looking like a hang. The report
goes to stderr, to the crash record (first line only) and to `<save dir>/panic.txt`, which is
the one to read on a device after the fact. A host fault inside guest memory prints the
guest address and every thread's registers.

## The guest heap

The SDK's OSAlloc keeps no record of who owns a cell, so when a game panics on a failed
`OSAllocFromHeap` the message is all there is. Two things fill that in, both in
`runtime/heap_trace.cpp`:

- Every panic report ends with the heaps walked out of guest memory: each heap's size, free
  total, number of free blocks and largest free block, and the allocated total -- which
  answers whether the heap was exhausted or fragmented. Any failed allocation prints the
  same, whether or not the game then panics.
- `GCN_HEAP=1` keeps a table of live cells keyed by the caller's return address and the one
  above it (games reach the allocator through thin wrappers), and prints it grouped by
  caller on any failed allocation or panic. `GCN_HEAP=<frames>` prints it every so many
  presented frames, to watch a trend before the crash.

Both need four one-line patches in the game's `patches.txt`, hooking `OSAllocFromHeap`'s
entry and both of its returns and `OSFreeToHeap`'s entry; the entry hook takes the address
of the SDK's `HeapArray` variable, which is where the report finds the heaps. Each line
keeps the instruction it replaces -- the first instruction of `OSAllocFromHeap` is
`mulli r0,r3,12` and `OSFreeToHeap`'s is `mflr r0`, and the returns are `blr`:

```
<OSAllocFromHeap>      heap_trace_alloc(c, <HeapArray var>); c->r[0] = (uint32_t)((int32_t)c->r[3] * 12);
<its NULL return>      heap_trace_alloc_result(c, 0); RET();
<its normal return>    heap_trace_alloc_result(c, 1); RET();
<OSFreeToHeap>         heap_trace_free(c); c->r[0] = c->lr;
```

`HeapArray` is the small-data word `OSAllocFromHeap` loads first (`lwz r3, off(r13)`), and
`r13` is set by `__start`. Without the hooks the panic report says the heaps were not traced.
