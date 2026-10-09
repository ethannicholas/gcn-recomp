# Graphics back end notes

The renderer is an **OpenGL 3.3 core profile** back end behind a GX front end that runs
vertex decoding, transform, lighting and texgen on the CPU, and generates one fragment
shader per TEV configuration. On macOS the system framework is linked directly; elsewhere a
vendored glad loader resolves the entry points.

## Threads

Four threads share a frame, besides the renderer's:

- **The guest** runs the game, and skims the GX command stream as the game writes it
  (`gx::skim`, `gx/cmd.cpp`): it walks the commands, copies each complete one into a queue
  with display lists inlined, and keeps the frame protocol itself. At a draw-done it waits
  for the front end to catch up and then raises the signal, at the same guest instruction
  as when everything ran inline, so replays stay exact. A PE token is raised at once, and
  the wait moves to where the game reads the token register: what a token promises is
  only that the GPU has got that far by the time the game looks. It also counts frames at
  the display copy, which scripted input and the guest checks key on.

  A game that sets a token after every skinned model and reads it back straight away still
  waits at each read for the front end to decode everything drawn before it, which by
  then is the whole world. Two things cut that. The guest hands commands over in 4 KB
  chunks rather than 64 KB, so the front end has usually started on what is waited for
  (and each side wakes the other only when it is asleep: a notify per chunk was a futex
  syscall each time on Android). And a game can turn on draw-sync lag
  (`gx::set_draw_sync_lag`): a token read returns the latest token issued before the last
  frame boundary -- a GPU a frame behind, as real hardware usually is -- which the front end
  has nearly always passed, so the read does not wait; a game that reads one over and over
  without issuing another is waiting for the GPU to free something, and after eight reads
  gets the latest. Both are functions of what the guest did, so replays stay exact.
  `GCN_GX_TOKEN_EAGER=1` waits at every token as before; `GCN_GX_TOKEN_LAG=0` or `1`
  overrides the game. `GCN_STALLS=1` prints how long each thread waits on the others.
- **The front end** (`gx/fifo.cpp`) runs `gx::process` on the queue: register loads,
  vertex decoding, pixel-state snapshots, textures. Vertex arrays and textures are read when
  it reaches the draw, as the hardware reads them.
- **Two transform workers** (`XfDraw` in `gx/xf.cpp`) transform and light each submitted
  frame from per-draw snapshots of the XF state, copied a sixteen-word block at a time as
  it changes. A draw's plan -- texgens, channels -- and the cached matrices and lights are
  kept while the snapshot they came from is the same one, since a room's display lists are
  thousands of four-vertex draws under one set of registers.

**Latency.** Each queue between the threads fills when the stage after it is the slow one,
and the picture is then as many frames behind the guest -- and behind the sound, which
follows the guest -- as the queues hold. The front end's was bounded only at 8 MB, which
is dozens of frames of a light scene, and the render queue held eight: a map screen whose
renderer was the slow stage trailed the controls by most of a second. Now the guest may be
one frame ahead of the front end (`kFeMaxFrames`, waited for at the end of a frame), the
transform holds one frame and the render queue one: three frames from guest to screen at
worst, at the frame rate two of each gave. `GCN_STALLS` prints the figure.

Because the front end reads vertex arrays and textures when it gets to them, what it reads
of memory the guest rewrites every frame depends on how far behind the guest it is. Frames
are byte-identical run to run, but a change to the pipeline's timing can move a handful
of pixels where such data is drawn: a first-person game's HUD and visor edges moved by up
to 61 of 255 at a few hundred pixels when the transform got faster, and matched exactly
with the front end inline (`GCN_GX_SYNC=1`) either way.

`GCN_GX_SYNC=1` runs
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
- **Face culling is tracked as GL has it, not as GX asks** (`g_gl_cull`). GL culls polygons
  only, so a line or a point leaves it alone; turning it off around every line made a 3D
  map that alternates filled faces with outlines toggle it 5,600 times a frame, a quarter
  of the render thread on a Quest 3 (9.4 ms a pass to 6.8). The point size likewise is sent
  only when a draw of points needs it.

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

## Texture lifetimes and EFB copies

**Who decides a texture is dead.** The front end decodes a texture when the game first
references it and never sends it again while its cache holds it; the renderer keeps the GL
texture. Each used to age textures on its own clock -- the front end by game frames since
the game last referenced it, the renderer by frames since a draw last bound it -- and they
disagree: in stereo, draws the trimmed flat pass and the eyes leave out reference textures
the renderer never binds. A texture could go from the renderer while the front end still
held it, and every draw from then on sampled nothing: HUD text drawn as black rectangles,
a little more of it as a session went on. Now only the front end decides
(`Batch::dead_textures`): what it evicts, and the variants it drops when the game rewrites
a texture, the renderer deletes a batch later. The renderer ages only EFB copies' textures,
which it makes itself.

**A copy keeps its id.** A copy's texture id is kept per (address, size, format), not per
address: a game that copies several targets of different sizes through one scratch buffer
every frame -- a reflection, then a depth copy of the scene, then a sliver of it -- had
each copy replace the last at that address, so every copy was new again the next frame
and the renderer allocated and later freed five GL textures a frame, a third of a stereo
flat pass. The lookup by address still resolves to the latest copy there; only the id a
repeated copy is given is remembered.

**Copies read back in another format.** An EFB copy is kept as RGBA and sampled as it is,
so the copy shader writes it as the format the game will read it back as. The two-channel
copies (RG8, GB8) are read as IA8 -- the first byte alpha, the second intensity -- and a
16-bit depth copy is RG8. A fog volume indexes a ramp with two of those through indirect
texturing, which takes its offsets from alpha, blue and green; handed (high, middle, low)
it took the low byte for the middle and drew a bright line across the fog wherever a byte
wrapped. Depth copies are also always sampled nearest (`GlTex::depth`): a filter averages
packed depths a byte at a time, and at a higher internal scale a full-screen quad no longer
lands on texel centres. `GCN_EYELOG` prints each copy's format and whether it is of depth.
A depth copy's coordinate is also snapped in the shader to the centre of the texel GX
would read (`u_texsnap`, set in `apply_state` for the maps holding one): a game reads such
a copy back with a quad whose coordinates run from texel centre to texel centre over
exactly as many pixels, which at internal scale 1 leaves the last row's coordinate within
1/256 of a texel edge, where the GPU's own subtexel rounding reads the next texel. A fog
volume drawn in horizontal chunks, each copied and read back that way, showed a faint line
along every chunk's edge at scale 1 and none at scale 2. The snap is from the arithmetic,
not yet from a frame of that game; it leaves every frame without depth copies unchanged.

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

A foreground layer also keeps the game's own depth inside its band (`u_vr` 4 in the vertex
shader): the depth the flat view writes, from the camera's position and the game's
projection, instead of the eye's. A HUD's band can be 1/512 of the buffer, and the eye's
depth -- its near plane a few centimetres out -- squeezed into that cannot tell apart
surfaces closer than a large fraction of a unit at the distance a game models a HUD at: a
3D map 16 units out, its faces and outlines a few hundredths of a unit apart, broke into
stripes in two frames of every six. The game's projection was made for that band. The
order between layers is the bands' either way, and within one layer the camera's depth and
an eye's differ only by the eye's offset. `GCN_EYE_FGDEPTH=0` writes the eye's depth again.

`GCN_DRAWLOG=<frame>` prints each draw's band and view-space box, which is how a game's
layers are found. In stereo the flat pass is trimmed (see above) and logs only what it
draws; `GCN_EYE_FULLFLAT=1` makes it list the whole frame.
