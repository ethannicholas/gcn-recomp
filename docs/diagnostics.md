# Diagnostics and measurement

How to drive a game without a controller, record and replay a run, measure the port, and
find out which guest code or which draw is responsible for something. Environment variables
are `GCN_*`; none of this is needed to play.

## Input logging and replay

Every run of the desktop frontend records the controller state the guest polled, keyed by
the poll's sequence number, into `saves/inputs/<timestamp>/inputs.txt`, with a snapshot of
the memory card beside it. The path is printed at startup. `--no-input-log` turns it off;
`--input-log=<dir>` chooses the directory.

`--replay=<dir>` plays a log back: the guest reads the logged controller instead of the live
one until the log runs out, then the live controller takes over. The run uses a scratch copy
of the logged card (`memcard_replay.raw` in the log directory), so the real card is never
touched and the replay sees the same saves the original did. A replay is still recorded as a
new log, so a route can be extended by replaying it and then playing on.

A replay is exact. Guest time is virtual (see "The clock" below): it is a function of what
the guest executes, not of the host's clock, so the Nth pad poll of the replay is the same
instant in the game as the Nth poll of the recording, every timed event (retrace, DVD
completion, decrementer) lands on the same instruction, and the RTC is started from the
recording's start time, which the log carries. A replay therefore reproduces the recording
bit for bit, including a crash at its end, unless the runtime itself has a nondeterministic
path -- which is then the bug to find. `--fast` runs a replay as fast as the host allows;
the game still sees 60 Hz.

Logs recorded before the virtual clock (version 1, keyed by presented frame) still load and
replay approximately, as they always did.

The log format is text: a header (version, game, start time and its epoch), then one line
per change of a channel's state --
`poll frame chan connected buttons stick_x stick_y cstick_x cstick_y trig_l trig_r`, in
hex -- and, when the run ended in a way the runtime saw (a normal exit, a fault, a failed
guest check or an interrupt), a final `# end <poll> <frame>`. A replay holds the last
state up to that end before handing over to the live pad; without the marker it hands over
at the last change, which can be well before where the run stopped, so a log that lacks
one (a run killed from outside) can be given one by hand. Lines can be edited or written
by hand; `frame` is for the reader.

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
before it goes near a headset. `--first-person` puts the eye wherever the game project's eye
hook says (`render_set_eye_hook` in `runtime/gx/render_gl.h`: the renderer asks the game, per
frame, for a view transform and the draws to leave out, and a game without a hook keeps the
chase camera). With `--dump-dir`/`--dump-every` the eye's frames are written as `eye_NNNNN.png`.

## On a headset

A game built with `ANDROID_PACKAGE` has two device-side tools besides the app.

`<game>_egl` is the GL ES renderer on a headless EGL pbuffer, run over `adb shell` from a
directory holding the image (`game.iso` or `game.ciso`). `--dump-dir`/`--dump-every`,
`--seconds=N`, `--frames=N` and `--scale=N` work as on the desktop. `--eye` renders through
the stereo path instead, with the VR settings the headset uses (the game's defaults, then
`./vr.txt` if present): `--eyes=2` renders both eyes, `--eye-size=WxH` and `--msaa=N` match
the headset's target, `--eye-yaw=deg`/`--eye-pitch=deg` turn the head (dump at two yaws:
anything that does not move is head-locked), and `--first-person` with `--fp-window=a-b`
calls the game's `set_first_person` hook. `GCN_EYE_GPU=1` times the eye passes,
`GCN_EYE_MORPH=0,0.5,1` dumps the theater/stereo morph at those points beside the flat
frame, and `GCN_STEREOLOG=1` prints the game's `wants_stereo` answer whenever it changes.
The desktop build honours `GCN_STEREOLOG` too, numbering frames as its dumps are, so a
game's stereo hook can be checked against a replayed route without a device.
`GCN_RAMSNAP=<dir>` (with `GCN_RAMSNAP_EVERY`, `GCN_RAMSNAP_RANGE=a-b`) writes the low 8 MB
of guest RAM beside the frames, for finding the state such a hook reads.
`GCN_THEATER_STEREO=<metres>` draws the flat frame as the stereo pair the headset hangs on
its panel with `theater_stereo` (see "The theater panel as a stereo pair" in `graphics.md`),
the eyes that far apart; the dumps come as `frame_NNNNN_l.png` and `_r.png`, and the
desktop window shows the right eye's.

The app has no environment of its own, so `gcn_env.txt` in its files directory
(`/sdcard/Android/data/<package>/files/`), one `KEY=VALUE` per line, is put into it before
the runtime starts; `gcn_input.txt` there holds a `GCN_INPUT` script, and `GCN_REPLAY=<dir>`
in the env file replays an input log relative to that directory. Its log is
`adb logcat -s <game>`, with compositor and game frame counts every five seconds.

## Measuring the guest

`GCN_PROFILE=<file>` (Linux and Android) samples the host's program counters with a profiling
timer while `<game>_bench` or `<game>_egl` runs and writes a histogram at the end; it needs no
perf permissions, which a Quest's shell does not have. The kernel rounds the timer, so the
counts are proportions rather than milliseconds. `GCN_PROFILE_DELAY=<seconds>` starts
sampling later, to profile one stretch of a route. `tools/profile_report.py <file>
<unstripped binary> --addr2line=<llvm-addr2line>` sums it by function, overall and per
thread; with `--symbolizer=<llvm-symbolizer> --outer=1` instead it names each sample by the
outermost function of its inline chain -- the recompiled function a guest load was inlined
into, rather than `LD32`. Symbolise against the binary that was profiled: any other build
puts samples in the wrong functions. Both device tools take `GCN_REPLAY=<dir>` to drive a run
with an input log, and the harness `--fast` to run unpaced.

The GX front end runs on a thread of its own and the vertex transform on worker threads (see
"Threads" in `graphics.md`). `GCN_GX_SYNC=1` puts the front end back on the guest thread,
`GCN_XF_SYNC=1` runs the transform per draw there, and `GCN_XF_THREADS=N` sets how many
threads share the transform of a frame (2 by default).

`GCN_STALLS=1` prints, every 60 frames, the milliseconds per frame each thread spent
waiting on another at each place one can block: the guest on the front end (its queue
full, a draw-done, a draw-sync token read), the front end on the transform, the transform
on the renderer, and how many token reads were answered a frame behind
(`GCN_GX_TOKEN_LAG`, see "Threads" in `graphics.md`). A thread's frame time minus its
stalls is its own work, which is what tells the stage that sets the pace from the ones
waiting on it. `GCN_GX_TOKEN_EAGER=1` makes the guest wait for the front end at every
token, as it once did.

`<game>_bench` runs the game with no graphics, audio or input, unpaced, and reports how fast
the guest advances; `--warmup=N` (default 8) excludes boot from the steady-state figure. Its
last column is the guest's work per presented frame in loop back-edges, the figure the
virtual clock's tick-per-edge constant is tuned against.

## The clock

Guest time is virtual by default. Every backward branch in recompiled code counts one
back-edge, and the time base is that count times a constant (`TICKS_PER_EDGE` in
`cpu.cpp`); events are due when the count reaches them, and the SDK's idle spin
(`idle.txt` in the game's tables) jumps the count to the next event instead of iterating.
The host is held to real time by sleeping when virtual time runs ahead; when the host
cannot keep up, the game slows down rather than catching up later. So the game's view of
time depends only on what it executed, which is what makes replays exact; the cost is that
a scene heavier than a frame's budget of back-edges looks to the game like a dropped frame,
at a threshold set by the constant rather than by the real console.

`GCN_CLOCK=host` restores wall-clock time (the ticker thread polls for due events every
200 µs). `GCN_TIMESCALE=N` paces virtual time at N× real time (0: unpaced; `--fast` on
the game, and the benchmark's default). `GCN_CLOCKLOG=1` prints once per virtual second how
virtual and host time compare and the work per presented frame.

`GCN_FRAMETIME=1` prints a line per frame from each thread: the guest's interval between
presents and how much of it the GX front end took, the batch's shape, and the renderer's
time to issue it (`[rt]` for the flat pass, with the texture and vertex uploads' share and
the EFB copies'; in stereo `[eye-rt]` adds, per eye, the flat pass's time and the eye's
own). `GCN_XFSTATS=1` prints, per frame over every 64, what the vertex transform was asked
for: vertices, how many carried a normal, how many had a lit colour or alpha channel,
light evaluations, texgens, and the texgens with a post matrix or an emboss -- which is
what decides where the transform's time goes. `GCN_APPLYSTATS=1` prints every 64th frame, per pass, how
many state applications there were and which groups of GL state each actually sent --
program, projection, viewport, TEV registers, textures, samplers, blend and so on -- which
is what a state application costs on a given game. `GCN_EYE_FULLFLAT=1` and
`GCN_EYE_KEEPDEPTH=1` undo the two stereo savings described in `graphics.md`, for
measuring them.

## Textures

`GCN_TEXLOG=1` prints a line per decoded texture: frame, id, guest address, format, size and
how many top-level texels are not black -- which tells a texture the game never wrote (all
zero) from one the shader mishandles. For an indexed texture it adds the palette entries the
texels use, decoded, and which palette variant this is. `GCN_TEXDUMP=<dir>` writes each
texture's top level as `<dir>/tex_<id>.png`, alpha forced opaque.

The texture cache checks a texture's texels once per frame, assuming a game does not rewrite
a texture between two draws of one frame. `GCN_TEXHASH_ALWAYS=1` checks on every lookup; if
an artifact goes away under it, the game is doing exactly that. (Palettes are not subject to
this: an indexed texture is decoded once per distinct palette its texels see, so a font drawn
black for a shadow and then white in the same frame gets both.)

An EFB copy lives on the GPU and is never written to RAM, so a lookup at its address returns
the copy -- until the RAM there changes. The copy's destination is fingerprinted when it is
made and checked once a frame (every lookup under `GCN_TEXHASH_ALWAYS`); when it differs, the
game has loaded something else into that memory, and the copy is dropped so the texture is
decoded from RAM. Before this a copy outlived its memory: a texture loaded where an old copy's
buffer had been was drawn as that copy -- black, or a stale screen -- for as long as it was in
use, which every lookup prolonged.

## Finding a draw or a function

- `GCN_DRAWLOG=<frame>` lists every draw in one frame with its index; `GCN_DRAW_SKIP=a-b`
  then drops a range of them, to attribute a piece of the image to the draws that made it.
  `GCN_DRAWLOG_VERBOSE=1` adds each draw's TEV setup (stage orders, colour and alpha
  combiners, konst selectors, the colour registers and konsts, alpha test, Z and blend
  modes) and its first vertices with colours and texture coordinates.
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
- `--log-all` enables every log category, including the DSP mailbox and DVD reads;
  `--log=exi,dsp` enables just those (`cpu os hw dvd gx vi si exi dsp ai thr`).
- Build with `-DGCN_GUEST_CHECKS=ON` to call the game project's consistency check (a
  function installed with `debug_set_guest_check`, which inspects the guest's own data
  structures and reports the first thing wrong) at every interrupt poll, after every
  runtime write into guest memory (DVD, ARAM and locked-cache DMA) and at every presented
  frame. The first failure prints what was wrong, where in the runtime it was noticed and
  the thread dump, then exits (`GCN_GUEST_CHECK_CONTINUE=1` to carry on instead). Since a
  replay is exact, the way to a heap corruption is: replay the run with the checks on,
  read the corrupted word's address off the report, replay again on a `-DGCN_WATCH` build
  with `GCN_WATCH_ADDR=<that>` and the store that did it is named. A check that fires
  right after a DMA names the runtime itself.

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
