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
  it changes. Batches reach the renderer through their queue in the order they were
  made, including a batch with nothing to transform (a copy alone): handed to the renderer
  directly, such a batch overtook the pass before it, still in the queue, and a game that
  clears a strip of the EFB with a copy between its two half-frame passes had the strip
  cleared a pass early -- a band of the last pass's pixels across the middle of the frame. A draw's plan -- texgens, channels -- and the cached matrices and lights are
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

The header holds a version and the size of `ShaderKey`; bump `kCacheVersion` whenever a
record's layout or a key field's meaning changes (a new field changes the size by itself). A
file with any other header is replaced (`[shaders] replacing unrecognised cache`). It used
to be only skipped. The programs compiled after that were then appended behind the old
header, so the file was skipped on every later run as well. From the version bump on
2026-10-12 to 2026-10-13, every run compiled every program in the middle of play, about a
thousand of them at 15-40 ms each on a Quest 3, while the file grew to 60 MB that nothing
read. If the startup line is missing, look for that message.

**Keys are canonical** (`canonicalize_shader_key`): everything `gen_pixel_shader` does not
read is cleared before a key is used -- the konstant selectors and order of stages past the
last, swap tables no stage refers to, an indirect command's matrix and format where no
indirect stage stands behind it, the projection bit of no fog, the comparisons of an alpha
test that always passes. The registers keep whatever a game last wrote to those parts, and
keyed raw a game that sets up only the stages it uses gets a program per leftover: one
cache had 23,673 programs for 2,193 distinct keys (2,108 distinct sources), took 47 s to
restore from binaries on a Quest 3 and held 133 MB. Keys read from an older file are made
canonical too, and duplicates dropped, so the file shrinks on its next rewrite without a
version bump: a canonical key generates the same source as the raw one, so a stored
binary's source hash still matches. That equality is the invariant to keep when the
generator starts reading a new field -- clear nothing it reads. A check over a cache file
(generate each key raw and canonical, compare) is how it was established; the step that
also lowered `num_texgens` to the coordinates used was dropped because it changed dead
declarations in the source.

So many resident programs also slowed every texture the driver allocated. Texture uploads
on ES now go through immutable storage (`glTexStorage2D`) and one pixel unpack buffer per
batch; with the 23,673 programs loaded that took a batch of 47 new textures (5.7 MB) from
390-580 ms to 255 ms, and the first draw from them from about 100 ms to 2. With the 2,193
the same batch took 30-40 ms, and one of 87 (12.4 MB) 54-62 ms against 460 at first.

## Stereo scaffolding

`gx/render_gl.cpp` and `gx/shadergen.cpp` carry the stereo machinery: `render_execute_eye`,
the theater/stereo morph, the world-pitch rotation, and the eye grab/refraction handling. The
desktop frontend runs it through `--eye`. Where the eye stands beyond the game's camera is the
game's business: `render_set_eye_hook` takes a per-frame callback that returns a view
transform, a HUD transform and the draws to leave out, and a game project installs it.
Nothing here knows what the game is.

An eye takes a copy of the whole displayed frame for the game compositing its finished
picture, so it substitutes a grab of its own for it and does not count it as ending an
off-screen pass (`is_fullscreen_copy`). A copy of *depth* is never that, whatever its size:
a grab is of colour. A game that renders its flashlight's shadow map at the screen's size,
from the light, and copies it out as depth with a clear had that pass replayed from each
eye and the eye's colour grab sampled as the map, which lit everything the light fell on a
solid yellow. Depth copies are now render targets like any other: a clearing one ends an
off-screen pass, which the flat pass draws once and the eyes sample.

`render_set_frame_hook` is the other half: a game's look at every batch the renderer is given,
flat or stereo, before anything is drawn from it and before the eye hook, with the off-screen
passes marked. A game that follows something on screen in every view -- its player's vehicle,
for motion telemetry -- does its finding there, and its eye hook can then hand back what the
frame hook already worked out for the same batch. `render_first_person()` tells the frame
hook whether the eye will want it.

**A camera the game turns after the head.** A game that turns its own camera to follow the
viewer's gaze -- so that what the head looks at is on the game's screen, where its targeting
and its 2D overlays reach -- would turn the world under the viewer by the same amount. It says
how far it turned, from the guest thread, with `set_view_turn` (`render.h`): a quaternion in
the eyes' frame. The turn is pinned to where the guest has got in the command stream: the
guest hands over what it has gathered, queues the turn at that position (`g_turns` in
`fifo.cpp`), and the front end applies it as it reaches that chunk (`view_turn_reached` in
`xf.cpp`), into the batch it is building and every one after (`Batch::view_turn`). The first
version applied it on the guest thread, straight into the front end's batch; but the front
end runs up to a frame behind on a thread of its own, so the turn landed on whichever batch
it happened to be building, and now and then a frame of the world was drawn a frame's turn
off -- a single-frame jolt of the world while the camera moved, the HUD unaffected. Found by
replaying a headset session with the eye held still and measuring each frame's shift
against the last. `render_execute_eye` draws each eye through the frontend's view times that
turn (`apply_view_turn`): the world stays where the viewer saw it, and everything the game
places about its camera -- the HUD frame, view-space draws, the sky -- turns with the camera.
The frontends are untouched; the poses they publish (`vr::hand_pose`, `vr::eye_views`) stay
in the unturned frame, and a game takes the turn off them before setting them against its
camera. The morph between the panel and the eyes takes the turn at its panel end too, so a
game keeps it at the identity outside the view it turns.

A game's eye filter (`render_set_eye_filter`, `EyeFilter` in `render.h`) can call a draw
`EyeFlat`: a 3D model the game composes into its 2D picture -- a character's face in a
window of the HUD -- drawn with a frustum of its own into a viewport the size of that
window. Its positions are in a little scene of its own, so an eye that took it for world
geometry floated it at that scene's depth, out of step with the HUD frame around it, and
uncomfortable to look at. An `EyeFlat` draw is painted on the HUD frame instead, through its
own projection and then its viewport, which is exactly where the flat frame puts it
(`u_vr` 5), clipped to that viewport by the four clip planes the morph uses (an eye's
target has no viewport to bound it), and depth-tested by the depth the game's own projection
gives it, so its parts hide one another as on the screen. The eye's depth buffer is cleared
before the first of them: they sort among themselves, in front of everything, as the HUD
does. `EyeDrawFacts` carries the viewport's rectangle for a filter to tell them by. A model
the game draws through an orthographic projection instead -- a face reflected on a visor --
can be called `EyeFlat` too. It went on the HUD frame anyway, as every ortho draw does, but
there with no depth test, so its parts lay in the order they were drawn: eyes drawn after a
face showed through it. Until 2026-10-15 `EyeFlat` applied only to a perspective draw.

`EyeScreen` is for an orthographic draw over the whole picture: a film on a visor or a lens
(steam, rain, a flash). On the HUD frame such a draw is a panel at the HUD's distance and
size, and its edges show. An `EyeScreen` draw goes on the HUD frame's path (`u_vr` 2, no
depth test), but its matrix maps the game's clip space straight onto the eye's, with no view
or frame between them. It covers the eye's whole field, fixed to the head, enlarged by
`kEyeScreenOverscan` (1.1) so that its edges are out of sight. The cost is magnification:
the game's 4:3 picture is stretched over a field several times as wide, so a film's texture
reads coarser than on a television. The draw log names its layer `eye-screen`. In the morph
between theater and stereo it goes from the panel to that placement, blended in the eye's clip
space (`MorphKind::Screen`), and not to the HUD frame like other 2D draws. A filter can tell
such a film by `EyeDrawFacts`' clip box and `clip_area`. The box alone also catches an
outline drawn into a whole-screen viewport, and a mesh of letterbox bars whose box is the
whole picture.

`EyeRestore` is for a cut-away: a game copies part of the frame (no clear), draws
something, then draws the copy back over the rect it came from, orthographically, at a
chosen depth with a depth test, so that whatever it drew since nearer than that depth is
replaced by the scene that was behind it. On the HUD frame that is a flat picture of the
flat view hanging in the air. Instead the eye grabs its own target where the game copies
(`g_restore_grab`, taken at the copy command itself, since the draws between the copy and
the put-back are the ones to cut), and draws the quad across its whole field (its clip
box stretched to the overscanned field, as `EyeScreen` does), sampling the grab at each
fragment's own position. The grab is what lies under every fragment, so outside what was
drawn since the copy the quad changes nothing, and the cut follows the thing cut wherever
the eye sees it, not the rect the flat view copied. Its depth is the game's in its band,
`b.x + b.y * z` folded into the matrix (an ortho clip w is 1), the same depth a foreground
layer writes under `u_vr` 4, and it keeps the game's depth test and write. That comparison
is only meaningful against draws in a foreground layer; mid-morph (`u_vr` 3 writes the
eye's depth) the draw is left out. The draw log names its layer `eye-restore`.

`EyeFacing` is for an orthographic panel a game moves about its picture by translating
its projection: a window that follows a pointer or, in a headset, the gaze. The HUD frame
is a flat plane fixed to the camera, so such a panel well off the frame's centre is seen
at a slant, a trapezoid. An `EyeFacing` draw goes back to the frame's centre (the
projection's translation, which is where it puts the panel's origin in clip space, is
taken off) and is painted on the HUD frame turned about the camera, yaw then pitch with no
roll, from the direction of the frame's centre to the direction of that point
(`facing_frame`): the same place, as far off as the frame's centre, square on to the
viewer. The draw log names its layer `hud-frame(facing)`.

`EyeLateObject` is for a marker a game draws into its world and then covers with a 2D
window that magnifies a copy of the picture around it, as a scanner's or a scope's window
does. On the screen the copy holds the marker, magnified. In an eye the window, on the HUD
frame and drawn without a depth test, covers the eye's own marker, and the copy is of the
flat view, magnified about a point the flat view chose, so the marker in it is somewhere
else or out of the window altogether. Every draw sharing a position matrix with one so
called (as `EyeHideObject` finds a model's other parts) is left out of the flat pass that
feeds the copies, through `g_hide`, and drawn by each eye after everything else in the
frame (`g_late`), with its own depth state against the depth the eye has written: still
hidden behind what stands in front of it, over the window. A frame that also has an
`EyeFlat` draw has had its depth cleared by then, so there the markers lose their
occlusion by the world.

A late object whose origin the game projects inside the box of the frame's facing panels
(`EyeFacing`, the window) is drawn on the panel's plane (`late_panel_view`): scaled about
the viewer, so the same direction and angular size, at the panel's depth. The draw log
names it `late(at-panel)`. Inside the window it reads as part of the window; left at its
own distance it was seen through the window's outline but metres behind it, which is hard
on the eyes. Any other late object is drawn at its modelled distance whatever its band, as
`EyeTrueScale` is (`true-scale`): a game may draw the reticle round the thing a scanner
marks in a band its foreground layer scales towards the camera, only to have it drawn over
everything, and scaled it stood at half the marker's distance.

A filter tells draws apart by what their state says, and that is not always enough: a
texture's content hash, stable for most textures, can differ from one run to the next for
one the game writes as it goes. So a game can also tag its draws by where they come from
(`set_draw_tag` in `render.h`, `PixelState::tag`): on entering one of its functions and on
leaving it, from the guest thread. The tag is pinned to the command stream as a view turn
is (the same queue, `g_turns` in `fifo.cpp`), so the front end, however far behind,
applies it to the states it takes from the draws written after it. It is as exact as the
stream: up to 31 bytes the game wrote before the call can still be in the gather pipe and
reach the front end after the change, so a tag is best changed where some state has been
written since the last draw it should not cover.

`EyeTrueScale` is for a perspective draw in a scaled layer's band (the foreground's or
the HUD's, "Depth bands in an eye") that is placed in the world all the same: a reticle
out where the aim is, a marker on a target, put in a near band only so that nothing in the
world hides it. The layer's scale towards the camera brought it to a fraction of its
distance, in front of what it marks, and the eyes could not fuse the two. It is drawn at
its modelled distance and keeps the band's depth. The draw log names its layer
`true-scale`.

**Post-process passes over the whole frame.** A game can copy the whole frame and draw it
straight back over the screen through a filter, orthographically: false colour, say. In an
eye the copy's lookups are taken over by the eye's grab as for any whole-frame copy, but on
the HUD frame the quad is a smaller picture hanging in front of the view. A game's filter
calls such a draw `EyeScreen`, which puts it across the eye; since the grab is sampled at
each fragment's own position, what it filters is this eye's frame. Two more things follow
from such a pass:

- A frame can copy the whole frame more than once, each copy a later state of it (a filter,
  then more drawn over the filtered frame, then a second copy and filter). The eye grabs at
  the first draw that samples a whole-frame copy, and again at the first such draw after
  every later whole-frame copy. With one grab a frame, the second filter read the frame as
  it was before the second pass drew.
- The grab is the eye's colour, while the game's copy was converted from it, as intensity
  for one. A substituted lookup is converted the same way in the shader (`EfbConv` in
  `shadergen.h`: I4 with its truncation to four bits, I8, IA), so a filter that indexes a
  palette by the copy's intensity gets the index the copy would have given.

## PC VR: OpenXR on desktop GL

`openxr/xr_frontend.cpp` is the OpenXR frontend for every platform; `openxr/xr_desktop.cpp`
binds it to the SDL window's WGL context through `XR_KHR_opengl_enable` (`--vr`), and
`android/openxr_main.cpp` to EGL. A runtime may want a newer context than the renderer does
(Meta's PC runtime wants 4.0, and a driver asked for 3.3 gives exactly 3.3), so the desktop
asks for the newest core profile from 4.6 down and falls back to the renderer's minimum.
Three things differ from GL ES, and each is confined to
`#ifdef GCN_GL_ES` in the frontend or the renderer:

- **The swapchain is sRGB.** The renderer writes the game's gamma-encoded colour. A runtime
  takes a linear format at its word and encodes it again, which washes the picture out, on a
  PC runtime and on a standalone headset alike (the Quest build was given `GL_RGBA8` and was
  too bright from its first build to 2026-10-11; the PC port, compared against the game's own
  window with Meta's `OculusMirror.exe`, is what showed it). So both ask for `GL_SRGB8_ALPHA8`
  first and write it with the encode off, which stores the colour unchanged for the compositor
  to decode as sRGB. On desktop GL that is `GL_FRAMEBUFFER_SRGB`, off by default. On GL ES
  writes to an sRGB image are always encoded unless `EXT_sRGB_write_control` is there to turn
  it off (`glDisable(GL_FRAMEBUFFER_SRGB_EXT)`, in `create_swapchains`, which then clears a
  scratch sRGB target to mid-grey and logs what it reads back: 128 stored as it is, 188
  encoded); without the extension the linear format is taken. The eyes draw straight into the
  swapchain images on ES, and ES refuses `glCopyTexSubImage2D` between an sRGB attachment and
  a linear texture, so the renderer's eye grabs (`grab_eye`) take the encoding of the target
  they copy from and, when sRGB, are read without decoding (`EXT_texture_sRGB_decode`): the
  water and the spray see the same values either way. `GCN_XR_SRGB=0` takes `GL_RGBA8` on
  either platform, to compare the two in a headset.
- **The eyes draw into a target of their own.** There is no
  `GL_EXT_multisampled_render_to_texture` on the desktop, so an eye renders into a
  multisampled RGBA8 renderbuffer (`msaa` samples), is resolved into an RGBA8 texture, and is
  then blitted into the swapchain image. The resolve has its own stop because a multisampled
  blit may not change format, and the swapchain's is sRGB.
- **The eye grab resolves.** The water and the spray copy the eye's own image mid-frame
  (`grab_eye`). `glCopyTexSubImage2D` cannot read a multisampled framebuffer, so on the desktop
  a multisampled target is blitted into the grab texture instead. (On GL ES the copy resolves
  the render-to-texture samples itself.)

The loader is Khronos' OpenXR-SDK at the same release as the Android AAR, fetched as source by
CMake and linked statically (`GCN_OPENXR`, on by default on Windows), so the executable finds
the system's active runtime with nothing beside it. On the machine this was written on,
SteamVR, Virtual Desktop and NVIDIA CloudXR all offered `XR_KHR_opengl_enable`. The
OpenXR-MotionCompensation layer draws its own overlay only into D3D11 and D3D12 sessions, so
with GL that overlay is missing; whether its compensation still works with GL is untested.

## The theater panel as a stereo pair

A game's 2D views -- menus, maps, a third-person camera -- are drawn by the game as 3D
scenes with its own projection, so the theater panel can show them like a 3D film rather
than a photograph: `render_execute_stereo_pair` draws the flat frame twice, each pass with
every perspective draw seen from half the eyes' separation to one side of the game's
camera, and the headset frontend hangs the two images on the same panel as two quad layers
with `eyeVisibility` left and right (`theater_stereo` in vr.txt). Orthographic draws are
untouched and sit on the panel. A screen that reads badly as a pair can be drawn flat, one
image for both eyes: the game's `wants_panel_pair` hook (`runtime/vr_game.h`) is asked for
every panel frame, and the harness and the desktop's `GCN_THEATER_STEREO` follow it too.

The per-draw change is in the projection alone, because vertices arrive in the camera's view
space: moving the view by `e` shifts clip x by `-P00*e` (P's translation column), and a
shear of `e / halfw` per unit of depth (P's x-from-z term) brings a point back to where it
was at the depth `D = P00 * halfw`, where the frustum is as wide as the panel. So the panel
is a window: a point `D` out sits on it, nearer ones stand in front, and a point at infinity
has the eyes' own separation, whatever the panel's distance or the draw's field of view.
Each draw converges on its own `D`, which keeps a scene composed from several fields of view
consistent with how the flat frame composes it. A draw in the game's background band
(`render_set_depth_layers`) takes the shear alone, which gives every point the disparity of
infinity, as the eyes draw that band. Before, a planet modelled tens of units out kept its
modelled depth and looked short of infinity.

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
of the last frame sees the other eye's. Nothing seen so far does. A game that samples a copy
of its own frame *by screen position* -- a water reflection cut from the frame and mapped
back by where each vertex lands -- does suffer: the copy is remade for the second pass from
the shifted camera, but the mapping was computed by the game for its own, so the reflection
lands off; one game turned the pair off for that. Cost is a second flat pass,
about twice theater's; the eye path is not involved, so the panel keeps the compositor's
reprojection. `GCN_THEATER_STEREO=<metres>` runs it on the desktop and in the harness,
with the frame dumps in `_l`/`_r` pairs.

A frontend that gives its views different internal scales (`stereo_scale`, `theater_scale`)
changes the EFB's size on the first frame of the new view. `render_set_internal_scale`
blits the old contents into the new targets. A console frame starts from what the last
display copy left, not from a clear, so new targets left undefined meant the first frame
drew over garbage, and garbage depth fails every test. That frame's first pass came out
empty: when leaving stereo, the left image of the pair was black (or garbage) for one game
frame, and the right image was whole because the left pass's display copy had cleared the
EFB. EFB copies are still dropped at a change of scale, so a copy made before the change
and sampled after it is missing for a frame.

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
a texture, the renderer deletes a batch later. EFB copies' textures go the same way (see
"A copy lives as long as its memory" below).

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
16-bit depth copy is RG8. The two-byte depth formats store the lower of their bytes first,
the reverse of the colour order: Z16 is (middle, top), Z16L (low, middle). A fog volume
indexes a ramp with two Z16 copies through indirect texturing, which takes its column from
the alpha and its row from the intensity, so the ramp's rows are the top byte and its
columns the middle. Written (top, middle), the lookup was transposed: the row became the
middle byte, and since the ramp climbs steeply in its last rows the fog lit up wherever
that byte neared 255, in bright one-pixel lines along contours of depth. The transposed
lookup, emulated from the dumped copies (`GCN_COPYDUMP`) and the ramp, saturated 273
pixels of a chunk where the frame had 253, and with the right order none; the swap removed
the lines. (An earlier fix had the bytes swapped the other
way, which removed coarser lines where the top byte changed, and left these.) Z16L is swapped
by the same rule, unverified by a game. Depth copies are also always sampled nearest (`GlTex::depth`): a filter averages
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
The 24-bit depth the copy shader packs is clamped at 16777215: at the far plane the
rounding `z * 16777215 + 0.5` lands on 16777216 in single precision, whose top byte is 0,
so a cleared depth buffer copied as Z8 read as the nearest depth. A game that renders its
shadow casters into a cleared region from above, copies the depth as Z8 and then darkens
its ground wherever the copy is below 1.0 shadowed the whole ground.

**Copies read through a palette.** A game can copy the frame as intensity and then bind the
same memory as CI4 or CI8 with a TLUT loaded: each texel becomes an index and the palette
turns the picture into false colour. On hardware this is only memory read under another
format; here a copy stays on the GPU in the format it was copied in, and was sampled as it
was, grey. Now `texture_lookup`, finding a copy bound as CI4 over a 4-bit copy (I4, R4) or
CI8 over an 8-bit one, decodes the palette into a 16x1 or 256x1 texture
(`PixelState::tex_pal`), and the shader turns the copy's value into the index and fetches
the palette on unit 8+m (`ShaderKey::efb_pal_mask`). Any other pairing reorders texels
within a block and is still sampled as the copy. Palettes live by their contents and go
when idle, like decoded textures. With this, an I4 or I8 copy also reads back its intensity
in all four components, alpha included, as the formats do: the copy shader used to leave the
frame's alpha in it. The I4 copy also keeps only the top four bits of the 8-bit intensity,
which is the index a CI4 read takes. The verbose draw log prints each map's format, and for
a copy the format it was copied in and whether it is read through a palette
(`fmt7=8(copy 10 pal4)`).

**A copy lives as long as its memory.** A copy is never written to RAM, so one the texture
cache lets go of cannot be had again: the address decodes as whatever RAM holds there. The
cache used to drop copies idle for 240 frames, and the renderer aged their GL textures on
its own clock besides. A game can render a set of textures once -- the sixty frames of a
spinning menu emblem, made while the menu loads -- and show them long after, and they were
all gone before the first was drawn. Now an idle copy goes only when the RAM at its
destination has changed (the game has put something else there) or when more than 512 are
held, oldest first, which still bounds a game that copies dozens of sprites a frame to
rotating addresses. The front end then lists the copy's id with the decoded textures it lets
go of (`Batch::dead_textures`), and the renderer deletes it a batch later; it no longer ages
anything itself.

**Copies a game's CPU reads.** A game that copies the frame out and then moves or reads
the bytes with the CPU needs the copy in RAM, in its texture format, by its next
draw-done. `gx::set_copy_writeback` takes a filter from the game; a copy it accepts is
flagged, the front end hands the batch so far to the renderer and waits for it, and the
renderer reads the copy back and encodes it (whole tiles, edges repeated) at the
destination (`copy_writeback` in `render_gl.cpp`). It encodes RGB565, RGB5A3 and RGBA8 from
colour, and the one-byte (I8, A8, R8, G8, B8; Z8, Z8M, Z8L) and two-byte (IA8, RG8, GB8;
Z16, Z16L) formats from colour or depth: the copy texture already holds each texel as the
game reads it back ("Copies read back in another format" above), so these are only packed,
the one-byte formats in 8x4 tiles and the two-byte ones, alpha byte first, in 4x4. A game
that draws object ids into the EFB and reads a small block of them back with the CPU, to
find what is under the crosshair, uses A8 for the ids and Z16 and Z8L for their depths.
The filter's `dest` is the physical address the copy register holds, not the game's
pointer to it. The guest's draw-done
waits for the front end, so the CPU then finds the bytes there. Each one is a round trip
through the transform and the renderer, which is why it is opt-in and per copy.

## Fog's type

BP 0xF1 holds the fog's C in its low 20 bits, then the projection bit (20: set for an
orthographic fog) and the function, fsel, in bits 21-23 -- as `GXSetFog` packs its
`GXFogType`, `fsel = type & 7` and `proj = type >> 3`. The shader key reads the two back
into that type. Until 2026-10-16 it took bits 20-23 as the type, so the projection bit
landed as the function's lowest bit and the function was read one bit up: exponential fog
(4) came out as no fog at all, linear (2) as exponential and exp2 (5) as an orthographic
linear one. A game that fogs a room with linear fog was drawn too dense, and one whose fog
is exponential (water, say) was clear. `GCN_DRAWLOG_VERBOSE` prints each fogged draw's
type, registers, colour and, for a perspective fog, the start and end distances they make.

## Constant destination alpha

`GXSetDstAlpha` (BP 0x42: the alpha in bits 0-7, enabled by bit 8) makes every draw with
alpha updates on store that constant in the EFB's alpha instead of the TEV's; the TEV's
alpha still decides the alpha test and the blend. The renderer ignored the register, and
every draw stored its own alpha. A game that marks pixels for a later full-screen pass
through the EFB's alpha then marks the wrong ones. One game sets 0xE7 on nearly every
draw and turns it off only for its shadows, whose alpha a copy of the EFB (A8, half size)
then turns into a darkening quad (`4 * (alpha - 0xE7)`). Storing the TEV's alpha left
surfaces whose alpha came out 0xFF -- a large floor panel in front of the camera -- marked
in the shadows' place, so the quad darkened them instead of the shadows, and the copy's box
filter left a pixel-wide band of them undarkened round anything in front -- a bright halo
round the characters -- while nothing cast a shadow.

`dst_alpha_mode` (`shadergen.cpp`) decides it per draw: the shader writes the constant as
its alpha (`u_dstalpha`); when the blend reads source alpha, the TEV's goes out as a second
output and the colour's factors become `GL_SRC1_ALPHA` / `GL_ONE_MINUS_SRC1_ALPHA`, and the
alpha channel's are `GL_ONE, GL_ZERO`. Dual-source blending is core in desktop GL 3.3 and
`GL_EXT_blend_func_extended` on ES; without it, a blended draw that reads source alpha
still stores its own. The verbose draw log prints the register as `dstalpha=`.

## Depth past the viewport's range

GX clips depth before the viewport, at `-w <= z <= 0` in clip space, and clamps the depth
the viewport then makes of what is left to the buffer's 24 bits. A viewport may map depth
well past that range: a far of 10 instead of 1 squeezes the scene into the front tenth of
the buffer and leaves everything farther at the maximum. GL clips after the viewport, at
its own `[-1, 1]`, so it threw away everything past that tenth -- a whole menu backdrop
drawn that way was missing. On desktop GL the renderer now enables `GL_DEPTH_CLAMP`, which
turns off GL's near and far clipping and clamps depth instead, and the vertex shader clips
where GX does through `gl_ClipDistance[4]` and `[5]` (`z + w` and `-z` before the viewport).
Every program writes those two -- the copy and background shaders write 1 -- since an
enabled clip distance a shader leaves unwritten is undefined. The eye paths write GL's own
planes there, which keeps their clipping as it was. GL ES has no depth clamp in core and
keeps GL's clipping. `GXSetClipMode(GX_CLIP_DISABLE)` (XF 0x1005) is recorded in the pixel
state and shown in the draw log as `noclip`, but not yet honoured.

## Z freeze

With gen mode bit 19 set (`GXSetCoPlanar`) a triangle's depth is not its own: GX evaluates
the plane of the last triangle it set up without z freeze, at each pixel. A game sets that
plane with a triangle it culls: one placed at the far end of the frustum, then a sky box
drawn with z freeze, keeps the sky behind planets, distant craft and far terrain whatever
the sky's own geometry. Ignored, the sky wrote its own depth, a hundred units out, and
everything farther failed the depth test behind it. The flat pass now takes the plane of
each triangle draw's last triangle as it goes (`zfreeze_plane` in `render_gl.cpp`: the
vertex shader's own placement, in the target's window coordinates, culled draws included)
and a z freeze program writes `gl_FragDepth` from it (`u_zplane`). Before any plane in a
frame, a z freeze draw keeps its own depth.

An eye takes the plane the same way, from the triangle as the eye sees it
(`zfreeze_plane_eye`): the eye's projection and view, the depth mapped into the draw's band
as the vertex shader maps it, in the eye target's window coordinates. A triangle placed by
the camera (positions in view space, as a sky's reference triangle is) goes through the
eye's view of the camera, as apply_state draws it. Keeping the sky's own depth in an eye
hid everything past the dome: in a flight over a surface only the craft and the HUD were
left.

## Frames rendered in parts

A game whose EFB cannot hold its frame (anti-aliased, the EFB is 640x264) renders it in
parts and copies each to its place in the XFB, with strips to a scratch buffer between
them; the renderer assembles the parts (`Xfb` in `render_gl.cpp`). A batch used to end at
every display copy, so such a frame arrived as five batches, and that broke both headset
paths: the OpenXR frontend takes one batch per display frame, which would have run the game
at a fraction of its speed, and an eye drew every batch as a frame of its own, the last of
them a strip with nothing in it. Now the front end tracks the XFB rows each display copy
writes as the renderer does (`display_copy_ends_frame` in `xf.cpp`) and ends a batch only at
the copy that completes a frame, or one that is a whole frame by itself. Strips to an
address that never fills ride along in the next batch. A game whose display copies are
whole frames sees no change; the flat frames of a game in parts are byte-identical.

Each part draws the same scene through its own window of the frame. An eye has no window,
so it draws the commands up to the first part's display copy and leaves the rest to the
flat pass (`first_part_end`); strips copied before the first part, left over from the
previous frame, have no draws in front of them and do not count. `display_size` takes such
a frame's height from the VI rather than from a part.

## Normals a vertex does not carry, and emboss

A vertex format without a normal, or with a normal but no binormal and tangent, takes the
last ones loaded: the transform unit keeps them, as Dolphin has it. They were left
uninitialised. An emboss texgen also takes the binormal and tangent through the normal
matrix without normalising them -- a game scales them so that the light's direction along
them is the bump's offset in texels -- where the front end normalised them. Between the two,
a bump-mapped hull's second, shifted sample of its own texture landed a sizeable fraction
of the texture away, and its panel lines crawled over it as bright streaks.

## Texture offsets for lines and points

`GXEnableTexOffsets` with the offset `GXSetLineWidth` or `GXSetPointSize` gives (SU_LPSIZE
bits 16-18 and 19-21, per coordinate in SU_SSIZE bits 18 and 19) makes the rasteriser run a
coordinate's s from its value to value + 1/n across a line's width, and s and t across a
point. A game draws antialiased lines this way, from a texture whose alpha is a profile
across u, clear at both edges. GL draws a line one pixel wide with nothing across it, so the
front end moves each such coordinate to the middle of its range when it packs the vertices
(`line_point_tex_offset` in `xf.cpp`). Left at the edge, every line was fully transparent.
Lines are still one GL pixel wide at any width the game asks for.

A point is a sprite, though: GL draws it at the game's size, and a particle textured with a
puff of smoke or a spark needs the whole texture across it. With every fragment sampling the
middle of the range, smoke, dust and sparks came out as flat squares of one texel's colour.
So for a draw of points the fragment shader spreads each such coordinate back out with
`gl_PointCoord` (`u_points`, `u_point_off`, set per draw in `apply_state`; 0 for anything
else, where `gl_PointCoord` is undefined): from the vertex's value at the point's top left to
value + 1/n at its bottom right, as GX does. Points keep their size in EFB pixels in an eye.

## Depth bands in an eye

GX has no depth-range call, but a viewport carries a z range, and a game can confine a draw
to a band of the depth buffer with it, layer by layer. The flat path has always applied the
viewport's z terms; an eye now maps its own depth into the draw's band too, which keeps the
game's layering and the eye's depth within each layer. A full-range viewport maps to itself.
The morph between theater and stereo (`u_vr` 3) maps depth the same way. Until 2026-10-14 it
did not, so for the whole transition a sky modelled tens of units out kept its modelled depth
and hid the world behind it, and pillars and ships came back only as the morph finished.

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
- **HUD** (a band ending at or before `hud_band`, inside the foreground's) is scaled by
  `hud_band_scale` instead, for a game whose HUD is geometry in a band of its own: modelled
  twenty units out, at the weapon's scale it stood ten metres off, a billboard; at a tenth
  it is at arm's length, the same angular size, with the weapon where it was. One scale
  does not suit a piece modelled far nearer than the rest: a flat element a few units out,
  where the rest is some tens, came to a few tens of centimetres from the eyes, painful to
  look at. `hud_push_below` and `hud_push_to` (`render_set_hud_push`, off by default) take a
  state whose nearest vertex in the frame is nearer than the first and scale it about the
  camera, on top of the layer's scale, until that vertex is at the second: the same angular
  size, at the depth of the HUD around it. The draw log names its layer `hud-band(pushed)`.

A perspective draw the game placed in view space itself -- a position matrix with no tilt,
in the camera's own frame (`PixelState::view_space`) -- is by default something held in
front of the camera: it takes the head's transform but not the world's pitch correction or
the game's own eye, so a rig of lights the game hangs before its camera stays there. A game
whose view-space draws are billboards instead -- sprites turned to face the camera but
standing at points in the scene, which is how one marks its item boxes -- says so with
`view_space_world` in `vr.txt` (`render_set_view_space_world`, usually from its
`config_defaults`), and they are drawn as world geometry: their vertices are in the camera's
frame as every world vertex is. Held before the camera, they stayed where the chase camera
saw them while the game's eye moved off into the driver's seat, and hung in the sky above
their boxes.

A game can also leave draws out of the eyes altogether (`vr::GameHooks::eye_filter`, a
`gx::EyeFilter`): the renderer asks it once per pixel state per frame, with what it knows
of the draw -- orthographic or not, indirect texturing, whether it samples an EFB copy and
whether of the whole frame, the depth band, and each texture's content hash
(`TexData::hash`, the same in every run, printed by `GCN_DRAWLOG` and `GCN_TEXLOG`) -- and
it answers keep, hide that state's draws, or hide the whole object: every draw of the
frame sharing a position matrix with one of them, which is how the untextured parts of a
rigid model go with the materials that name it. The flat view is untouched. What a game uses it for: the materials of a helmet model drawn in
the HUD's band, which on a television is the picture's edge and in a headset is a frame in
mid-air; and a screen-space ripple -- a copy of part of the frame drawn back over itself
orthographically through an indirect texture -- which the eyes would paint on the HUD frame
as a distorted square in the air, since an ortho draw has no depth to give it.

A foreground layer also keeps the game's own depth inside its band (`u_vr` 4 in the vertex
shader): the depth the flat view writes, from the camera's position and the game's
projection, instead of the eye's. A HUD's band can be 1/512 of the buffer, and the eye's
depth -- its near plane a few centimetres out -- squeezed into that cannot tell apart
surfaces closer than a large fraction of a unit at the distance a game models a HUD at: a
3D map 16 units out, its faces and outlines a few hundredths of a unit apart, broke into
stripes in two frames of every six. The game's projection was made for that band. The
order between layers is the bands' either way, and within one layer the camera's depth and
an eye's differ only by the eye's offset. `GCN_EYE_FGDEPTH=0` writes the eye's depth again.

That depth is worked out per fragment, not per vertex (`ShaderKey::fg_depth`). The game's
z/w is linear in the camera's screen, not in an eye's: interpolated between the vertices in
an eye turned away from the camera, it is wrong across a face, the more so the wider the
face and the nearer the camera. A weapon held in the hand and looked at from the side, a few
tens of centimetres out, has large faces at exactly that angle, and the error let the parts
inside it win the depth test against its shell, which opened. The vertex shader passes the
game's clip z and w, both linear in the vertex, so their perspective-correct interpolation
is exact, and the fragment shader writes their ratio into the band. Only the programs of
draws an eye gives such a layer write `gl_FragDepth`, so every other draw keeps early depth
testing. `GCN_EYE_FGFRAGDEPTH=0` interpolates the vertices' depth again, for comparing.

`GCN_DRAWLOG=<frame>` prints each draw's band and view-space box, which is how a game's
layers are found. In stereo the flat pass is trimmed (see above) and logs only what it
draws; `GCN_EYE_FULLFLAT=1` makes it list the whole frame.


## The FIFO's watermarks

A game that holds the GP at a breakpoint for a frame while it writes the next keeps
writing until the CP's overflow interrupt suspends the writing thread (the SDK's
`GXCPInterruptHandler` and `__GXOverflowSuspend`), and its underflow interrupt, once the GP
has drained below the low watermark, resumes it. `cp_watermarks` in `gx/fifo.cpp` sets the
status bits whenever the write distance crosses the marks and raises the CP interrupt
through the enable bits the SDK toggles. Without it the writer lapped the 64 KB FIFO: the
lines the GP had not reached were overwritten before it got there, and what it then
parsed was a later frame's vertices.

A FIFO's end register names its last line, not the address past it: the SDK sets it to
`base + size - 4`, and the hardware wraps after the line at the end is written (the CPU's
write pointer) or read (the GP's), as Dolphin has it. Wrapping a line early lost the last
line of a buffer filled to the brim -- which a display list sized by a dry run is: the write
pointer came back to the base with its wrap bit set, `GXEndDisplayList` took that for an
overflow and returned a size of zero, and every call of the list drew nothing.
`cp_end_excl` in `gx/fifo.cpp` is the exclusive end the arithmetic uses.
