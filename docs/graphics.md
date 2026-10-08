# Graphics back end notes

The renderer is an **OpenGL 3.3 core profile** back end behind a GX front end that runs
vertex decoding, transform, lighting and texgen on the CPU, and generates one fragment
shader per TEV configuration. On macOS the system framework is linked directly; elsewhere a
vendored glad loader resolves the entry points.

## Shader cache

Each TEV configuration is compiled the first time a draw uses it, mid-frame. Every key
compiled is appended to `saves/shaders.bin`, and the next run builds all of them before the
game boots. Deleting the file is always safe. The startup line reports what happened:

```
[shaders] 29 programs from cache (0 from binaries, 29 compiled) in 44 ms
```

## Stereo scaffolding

`gx/render_gl.cpp` and `gx/shadergen.cpp` carry the stereo machinery written for Wave Race:
`render_execute_eye`, the theater/stereo morph, the world-pitch rotation, and the eye
grab/refraction handling. The desktop frontend runs it through `--eye`. Where the eye stands
beyond the chase camera is the game's business: `render_set_eye_hook` takes a per-frame
callback that returns a view transform, a HUD transform and the draws to leave out, and
bluestorm-recomp's `runtime/first_person.cpp` is the one that finds a jet ski and puts the eye
on it. Nothing here knows what a hull is.
