# The Vision Pro frontend

`visionos/` runs a game standalone on an Apple Vision Pro, with the same frontend core a
Quest runs (`openxr/xr_frontend.cpp`): the theater panel, stereo, the morph between them,
the controllers and everything a game says through `vr::GameHooks`. A game project opts
in with `VISIONOS_BUNDLE_ID` in `gcn_add_game` and builds with CMake's Xcode generator:

```
cmake -S . -B build-visionos -G Xcode -DCMAKE_SYSTEM_NAME=visionOS \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 -DGCN_APPLE_TEAM=<team id>
xcodebuild -project build-visionos/<game>.xcodeproj -target <game> \
    -configuration RelWithDebInfo -sdk xros -allowProvisioningUpdates build
xcrun devicectl device install app --device <id> build-visionos/RelWithDebInfo-xros/<game>.app
```

The game project calls `gcn_enable_languages()` at its top level first (the launcher is
Swift, the frontend Objective-C++).

## Pieces

| File | What it is |
|---|---|
| `App.swift` | The app: a launcher window (is there a disc image, copy one in from Files, Play) and the immersive space whose `CompositorLayer` the game renders into. |
| `visionos_main.mm` | The shell, as `android/openxr_main.cpp` is on a Quest: the log, the GL context, the order the game is brought up in. |
| `xr_runtime.mm` | A minimal OpenXR runtime: exactly the calls the core makes, over CompositorServices, ARKit and GameController. |
| `angle.mm` | GL ES, through ANGLE. |
| `audio_coreaudio.cpp` | The mix out through a RemoteIO unit. |

Everything the app reads and writes is in its Documents folder, which the Files app shows
and `xcrun devicectl device copy to|from --domain-type appDataContainer
--domain-identifier <bundle id>` reaches: the disc image (any `.iso` or `.ciso`), `vr.txt`,
`gcn_env.txt` and `gcn_input.txt` as on the Quest, the memory card, the shader cache, the
input logs and `log.txt`, the last run's output. The log also goes to the unified log
(subsystem `com.gcn-recomp`).

## There is no GL: ANGLE

visionOS has no OpenGL for a native app. `OpenGLES.framework` is in the SDK, but
`EAGLContext` is marked unavailable on visionOS: it is there for iPad apps run in
compatibility. (The first build of this frontend was written against it, and failed to
compile on exactly that.) So the renderer runs on ANGLE's Metal backend, GL ES 3.0, which
is what WebKit's WebGL runs on there.

`tools/build-angle-visionos.sh` builds it from WebKit's copy of ANGLE at a pinned commit,
not Chromium's: WebKit's carries an Xcode project for every Apple platform, where
Chromium's GN build knows iOS and tvOS but not visionOS. The project expects pieces of a
full WebKit build (WebKit's `Configurations/`, the SDK additions in `WebKitLibraries/`, a
clang overlay); the script supplies them. The first configure runs it into
`<build>/angle/` (`GCN_ANGLE_DIR`), and the app embeds `libANGLE-shared.dylib`.

WebKit's build exports ANGLE's entry points under prefixed names, `EGL_Initialize` for
`eglInitialize` and `GL_DrawArrays` for `glDrawArrays`, so as not to collide with the
system's GL. `angle.mm` opens the library from the bundle and maps each standard name to
its export; the glad loader resolves GL through it.

The renderer at GL ES 3.0 (`GCN_GL_ES30`): GLSL `#version 300 es`, and the two things it
takes from 3.2 have fallbacks. `glDrawElementsBaseVertex` comes from
`GL_OES_draw_elements_base_vertex` or the `EXT` one when the context is below 3.2
(`gl_load.cpp`), and failing both, each draw's base vertex is added into its indices as
they are uploaded (`render_gl.cpp`). Pointing the vertex arrays at the base vertex
instead, the first attempt, cannot work: a base vertex is often negative. Clip distances and dual-source blending are extensions the renderer
already checks for.

## The OpenXR runtime

The core is written against OpenXR, and the alternative to emulating the part of it the
core uses was a second core. What a runtime does for the core, `xr_runtime.mm` does the
same way:

- **Frames.** `xrWaitFrame` takes a `cp_frame` through its update phase, waits for the
  compositor's optimal input time and queries the frame's drawables;
  `predictedDisplayTime` is the drawable's presentation time. `xrEndFrame` composites the
  submitted layers into every drawable with one Metal pipeline (a textured unit quad) and
  presents them. A frame `xrWaitFrame` could not get a drawable for has `shouldRender`
  false.
- **The head.** ARKit world tracking: the device anchor at the drawable's trackable anchor
  time. `xrLocateViews` composes it with each `cp_view`'s transform, and takes each eye's
  field from `cp_drawable_compute_projection`.
- **LOCAL** is where the head was when tracking first came up, turned to its heading and
  levelled, which is what OpenXR's LOCAL is. ARKit's origin is on the floor where the space
  opened.
- **Swapchains** are Metal textures (BGRA8), each wrapped as a GL texture through
  `EGL_ANGLE_metal_texture_client_buffer`. The compositing reads them through an sRGB view:
  the renderer writes the game's gamma-encoded colour, and decoding it as it is sampled
  into the sRGB drawable stores what was written, as an sRGB swapchain does on a Quest.
  ANGLE signals a shared event (`EGL_ANGLE_metal_shared_event_sync`) when the renderer's
  work before a frame's compositing is done, and the compositing waits for it on the GPU.
  `xrWaitSwapchainImage` waits for compositing still reading an image.
- **A projection layer** is drawn across each view at infinity. Each image keeps the device
  anchor current when it was released, and a frame showing it gives the drawable that
  anchor, so the compositor reprojects an image from the pose it was rendered at. That is
  what lets the core re-submit the last eye images on a display frame with no new game
  frame, as it does on a Quest.
- **Quad layers** (the theater panel, as a single image or a stereo pair) are drawn into
  each view at their place in LOCAL, with depth, for the same anchor.
- **Input.** Only the Touch profile's suggested bindings are taken. A spatial controller
  (PS VR2 Sense: one stick, A and B, trigger, grip, a menu button) is mapped onto it per
  hand: A and B are Touch's A and B on the right and X and Y on the left, and either menu
  button is the left menu. Its hand comes from ARKit's accessory chirality, its aim pose from
  ARKit's accessory tracking (the `aim` location, predicted for the display time). A
  gamepad is merged as a pad (`Platform::merge_input`), with its stick clicks fed to the
  thumbstick-click actions so the view toggle and the game's left click work on it too.
- **Lifecycle.** Session states are queued as a runtime would send them. When the layer is
  invalidated (the viewer left the space) the session is lost and the core's loop ends.

## Known limits

- **A console reset ends the app.** On a Quest the app relaunches itself into a clean
  process; visionOS gives an app no way to do that, so the viewer opens it again.
- **A projection layer has no depth**, so the compositor reprojects stale eye images for
  rotation only. The eyes' depth buffer could be handed over as the drawable's depth.
- **Foveation is off**, so the eyes render at a uniform resolution.
- Untested on a device as of this writing; what a first run showed belongs here.
