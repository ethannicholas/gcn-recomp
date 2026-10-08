# Graphics back end notes

The renderer is an **OpenGL 3.3 core profile** back end behind a GX front end that runs
vertex decoding, transform, lighting and texgen on the CPU, and generates one fragment
shader per TEV configuration. On macOS the system framework is linked directly; elsewhere a
vendored glad loader resolves the entry points.

## Threads

Four threads share a frame, besides the renderer's:

- **The guest** runs the game, and skims the GX command stream as the game writes it
  (`gx::skim`, `gx/cmd.cpp`): it walks the commands, copies each complete one into a queue
  with display lists inlined, and keeps the frame protocol itself. At a PE token or a
  draw-done it waits for the front end to catch up and then raises the signal, at the same
  guest instruction as when everything ran inline, so replays stay exact; it also counts
  frames at the display copy, which scripted input and the guest checks key on.
- **The front end** (`gx/fifo.cpp`) runs `gx::process` on the queue: register loads,
  vertex decoding, pixel-state snapshots, textures. Vertex arrays and textures are read when
  it reaches the draw, as the hardware reads them.
- **Two transform workers** (`XfDraw` in `gx/xf.cpp`) transform and light each submitted
  frame from per-draw snapshots of the XF state, copied a sixteen-word block at a time as
  it changes.

Frames are byte-identical either way. `GCN_GX_SYNC=1` runs
the front end inline on the guest thread again, `GCN_XF_SYNC=1` the transform; the
diagnostics that read transformed vertices early select the latter themselves.

Stores to the write-gather pipe's address are appended to its line buffer inline in the
generated code (`gp_store32` in `recomp.h`) rather than through the MMIO decode; a game's
CPU skinning stores every vertex that way.

## Shader cache

Each TEV configuration is compiled the first time a draw uses it, mid-frame. Every key
compiled is appended to `saves/shaders.bin`, and the next run builds all of them before the
game boots. Deleting the file is always safe. The startup line reports what happened:

```
[shaders] 29 programs from cache (0 from binaries, 29 compiled) in 44 ms
```

## Stereo scaffolding

`gx/render_gl.cpp` and `gx/shadergen.cpp` carry the stereo machinery: `render_execute_eye`,
the theater/stereo morph, the world-pitch rotation, and the eye grab/refraction handling. The
desktop frontend runs it through `--eye`. Where the eye stands beyond the game's camera is the
game's business: `render_set_eye_hook` takes a per-frame callback that returns a view
transform, a HUD transform and the draws to leave out, and a game project installs it.
Nothing here knows what the game is.

## Depth bands in an eye

GX has no depth-range call, but a viewport carries a z range, and a game can confine a draw
to a band of the depth buffer with it, layer by layer. The flat path has always applied the
viewport's z terms; an eye now maps its own depth into the draw's band too, which keeps the
game's layering and the eye's depth within each layer. A full-range viewport maps to itself.

Two layers can also be placed differently in stereo, keyed on their band, for a game that
names them (`render_set_depth_layers`; `background_band`, `foreground_band` and
`foreground_scale` in `vr.txt`, usually set by the game's `config_defaults` hook). Both are
off by default.

- **Background** (a band starting at or past `background_band`) is drawn at infinity: turned
  with the head, not moved with it, and the same in both eyes. A sky modelled a short way out
  around the camera otherwise has the disparity of something near. `GCN_EYE_SKY=0` turns this
  off.
- **Foreground** (a band ending at or before `foreground_band`) is scaled towards the camera
  by `foreground_scale`. A game models a weapon or a visor large and far off, because a flat
  picture shows only angular size; scaling about the camera keeps that angular size in each
  eye and brings it nearer, so it reads smaller.

`GCN_DRAWLOG=<frame>` prints each draw's band and view-space box, which is how a game's
layers are found.
