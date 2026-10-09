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

## The theater panel as a stereo pair

A game's 2D views -- menus, maps, a third-person camera -- are drawn by the game as 3D
scenes with its own projection, so the theater panel can show them like a 3D film rather
than a photograph: `render_execute_stereo_pair` draws the flat frame twice, each pass with
every perspective draw seen from half the eyes' separation to one side of the game's
camera, and the headset frontend hangs the two images on the same panel as two quad layers
with `eyeVisibility` left and right (`theater_stereo` in vr.txt). Orthographic draws are
untouched and sit on the panel.

The per-draw change is in the projection alone, because vertices arrive in the camera's view
space: moving the view by `e` shifts clip x by `-P00*e` (P's translation column), and a
shear of `e / halfw` per unit of depth (P's x-from-z term) brings a point back to where it
was at the depth `D = P00 * halfw`, where the frustum is as wide as the panel. So the panel
is a window: a point `D` out sits on it, nearer ones stand in front, and a point at infinity
has the eyes' own separation, whatever the panel's distance or the draw's field of view.
Each draw converges on its own `D`, which keeps a scene composed from several fields of view
consistent with how the flat frame composes it.

Two settings shape it. `panel_band` puts a perspective draw the game confines to a band
of the depth buffer no deeper than that on the panel itself, with no disparity, as a
film's subtitles are: a HUD layer drawn close in front of the camera would otherwise stand
in front of the panel. A draw with no band at all (a zero-width z range, depth off) is
left with its depth. `theater_depth` scales the separation the pair is drawn with: 1 is
true to the game's scale, less flattens everything towards the panel.

The second pass reuses the uploaded vertices and textures (`execute_batch`'s `again`) and
redoes the EFB copies from its own EFB, so a game that samples a copy of its own frame gets
each eye's. What it does not do is keep two EFBs: the second pass starts from the first's
leftovers, as the next frame would, and a game that relies on what the EFB held at the end
of the last frame sees the other eye's. Nothing seen so far does. Cost is a second flat pass,
about twice theater's; the eye path is not involved, so the panel keeps the compositor's
reprojection. `GCN_THEATER_STEREO=<metres>` runs it on the desktop and in the harness,
with the frame dumps in `_l`/`_r` pairs.

## What the render thread spends a stereo frame on

A stereo frame is the batch drawn three times: a flat pass into the EFB, then each eye
(`render_execute_eye`). The vertex buffer and the CPU transform are shared; what repeats is
the state applications and the draw calls, and on a mobile driver those are most of the
render thread. Three things keep them down, each with a switch that restores the old
behaviour for comparison:

- **The flat pass is trimmed** (`flat_pass_trim`). In stereo it exists only to produce the
  render-to-texture copies the eyes sample, since an eye never draws into the EFB and the
  picture the flat pass paints is never shown. So it stops after the last copy something
  will read: the off-screen passes (reflections, sprite sheets) are drawn in full, and the
  main scene only as far as a copy that is read as copied. A whole-frame copy or a
  spray-sized grab the eye substitutes its own grab for does not count as read; a copy with
  no reader this frame does, since a game may sample it in a later one. For a first-person
  game that is nearly the whole main scene left out. `GCN_EYE_FULLFLAT=1` draws it all.
  Eye frames in play are byte-identical with and without the trim; a cinematic's are
  not quite, a handful of pixels a frame off by up to three of 255, the same pixels every
  run, so something the full flat pass leaves behind still reaches the eye there. The
  uniform shadows are not it (the old apply path shows the same pixels) and nor are the
  copies, which are never left out. Not found yet; `GCN_EYE_FULLFLAT=1` is the control.
- **Uniforms are shadowed per program** (`UniformShadow`). A uniform keeps its value in its
  program across switches, so a group whose inputs match what that program already holds is
  not re-uploaded, whether or not the program just changed. Two generation counters say when
  a record is stale: one for the EFB scale, one for the pass and the eye's matrices.
  Before this every program switch re-uploaded a dozen groups. `GCN_APPLYSTATS=1` prints
  what each pass actually sent.
- **The texture and the sampler of a unit are bound separately**, since the texture changes
  at nearly every draw and the sampler almost never.

The vertex buffer is uploaded once a frame and was, for a while, the largest single cost of
the render thread. A `GpuVertex` has room for eight texture coordinates of three floats,
124 bytes, and a draw uses about two: a frame of a hundred thousand vertices was twelve
megabytes through `glBufferData`, 5-7 ms of the render thread on a Quest 3. So the GPU is
given only each vertex's leading `packed_stride(n)` bytes, n the draw's texture coordinate
count (`Batch::packed`): one region of the buffer per count, each starting at a multiple of
its stride so that it can be indexed in vertices of that size, and a vertex array per
count over it (`g_vaos[set][n]`), with `glDrawElementsBaseVertex` and the command's
`base_vertex` to find the draw's vertices in its region. A coordinate past the count reads
the attribute's constant value, (0, 0, 1), which is what the transform writes into the
unused ones. The transform workers pack each vertex as they finish it, while it is in
cache (`layout_packed` lays the regions out first); `GCN_XF_SYNC` builds pack the whole
batch at the end. `Batch::verts` stays whole for the renderer's own CPU-side reads. The
same frame went from twelve megabytes to about four and the upload to about 0.5 ms --
more than the size alone accounts for, so the driver was also slow with the large buffer.
Frames are byte-identical.

And on GL ES the eye's depth attachment is invalidated at the end of each eye pass
(`glInvalidateFramebuffer`): on a tiled GPU a depth buffer left valid is written out of
tile memory to RAM, and at a headset's eye size with multisampling that is tens of
megabytes per eye per frame for a buffer nothing reads. `GCN_EYE_KEEPDEPTH=1` leaves it.

The next lever, not taken, is `GL_OVR_multiview2`: both eyes from one set of draw calls.

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
