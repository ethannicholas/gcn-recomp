#pragma once
#include <vector>
#include "render.h"

namespace gx {
// Upper bound on the EFB's supersampling, so that a mistyped digit in vr.txt asks for a
// framebuffer that is merely large rather than one the device cannot allocate at all. 8
// is 5120x4224, past anything either view has a use for.
constexpr int kMaxInternalScale = 8;

// Where compiled shaders are remembered between runs, so that the next run builds them
// all before the game starts instead of at the moment each is first needed. Set before
// render_init; nullptr (the default) builds every shader on first use.
void render_set_shader_cache(const char* path);
void render_init(int internal_scale);

// Re-scale the EFB between frames. Call only when a whole frame is about to be drawn:
// it discards the EFB's contents and every texture an EFB copy has produced.
void render_set_internal_scale(int scale);

void render_set_window_size(int w, int h);

// Redirect the finished frame somewhere other than the window framebuffer, e.g. a VR
// swapchain image. 0 restores the default.
void render_set_output_fbo(unsigned fbo);

bool render_execute(Batch& b);

// Frames presented so far, for tracing what a display frame is actually showing.
uint32_t present_count();


// ---- VR ----
// Set the eye projection, the eye's transform relative to the game's camera, and the
// frame the flat 2D elements are painted on (render_hud_frame).
void render_set_vr_eye(const float proj[16], const float view[16], const float hud[16]);

// Builds that frame: a quad `dist` game units in front of the game's camera, `scale` of
// the vertical field of view tall, its centre `height` game units above the forward axis,
// tilted back by `pitch_rad` (0 standing vertical). `tan_half_fovy` is tan of half that
// field. Both eyes must be given the same frame, or there is nothing for them to fuse
// into.
void render_hud_frame(float dist, float tan_half_fovy, float scale, float height,
                      float pitch_rad, float out[16]);

// Fold the stereo view part of the way back into theater. At 0 an eye shows the game's own
// frame flat on `panel` -- a matrix from the game's clip space to a rectangle in the eye's
// space, built like render_hud_frame -- cropped to it and black around it, which is what
// theater looks like from that eye. At 1 (or more) it is ordinary stereo. Every vertex
// moves between the two in step with its own disparity; see morph_chain. Applies to the
// eye passes that follow, until set again.
void render_set_vr_morph(float t, const float panel[16]);

// Rotate the world back by the game's chase-camera pitch, so the sea comes out level with
// the room instead of sloping. Applies to world geometry only: the HUD frame is placed in
// the headset's own space and stays where it is put. 0 renders what the game draws.
void render_set_world_pitch(float pitch_rad);

// A game's depth layers in stereo, for a game that confines layers to bands of the depth
// buffer through the viewport's z range. A perspective draw confined to depths from
// `background_from` to 1 is background, drawn at infinity: turned with the head, the same in
// both eyes. One confined to depths no further than `foreground_to` is foreground -- a weapon
// or a visor, modelled large and far off because a flat picture shows only angular size --
// and is scaled towards the camera by `foreground_scale`, keeping its angular size in each
// eye. 0 turns either off, which is the default.
void render_set_depth_layers(float background_from, float foreground_to, float foreground_scale);

// The game's own eye. The renderer knows how to re-project a batch for an eye and how to
// take the chase camera's pitch out of the world; where else an eye might stand -- on the
// player's vehicle, say -- is a fact about a game, and comes from a hook the game project
// installs. While first person is on (render_set_first_person) the hook is called once per
// frame, before anything is drawn from the batch, with the off-screen passes already
// marked in `offscreen` (one byte per command, as the renderer skips them) and the
// renderer's frame count, which a hook can use to notice a gap. It fills in `out` and
// returns true, or returns false to leave the eye with the chase camera for that frame:
//   view_to_eye  what world geometry goes through before the eye's view, in place of the
//                world-pitch rotation (column-major 4x4)
//   hud_to_eye   what the HUD frame goes through before the eye's view: the identity, or
//                a rotation that keeps it upright as the view tilts (column-major 4x4)
//   hide         one byte per command, 1 to leave that draw out (the player's own model,
//                say); may be left empty. A hidden draw's textures are kept alive.
// Applies to the eye passes only; the flat view is untouched.
struct EyeOverride {
    float view_to_eye[16];
    float hud_to_eye[16];
    std::vector<uint8_t> hide;
};
using EyeHook = bool (*)(const Batch& b, const std::vector<uint8_t>& offscreen, uint32_t frame, EyeOverride& out);
void render_set_eye_hook(EyeHook hook);
void render_set_first_person(bool on);

// Draw one eye's view of a batch into `fbo`. Both eyes share the vertex buffer, the
// CPU-side transform and any render-to-texture results, so only uniforms and draw
// calls are repeated. Pass do_copies for the first eye only.
bool render_execute_eye(Batch& b, unsigned fbo, int w, int h, bool do_copies);

// The flat frame as a stereo pair for a theater panel, like a 3D film: the batch drawn
// twice, into `fbo_l` then `fbo_r`, with every perspective draw seen from half `eye_sep`
// to the side of the game's camera and its frustum sheared so that the depth at which it
// is `panel_width` wide has no disparity -- the panel's own plane, so the panel is a
// window -- and points at infinity have the eyes' full separation. Both in game units.
// Orthographic draws, the 2D elements, are the same in both and sit on the panel. The
// batch is uploaded once; the second pass redoes the EFB copies from its own EFB. The
// frame dumps get `_l` and `_r` suffixes. Returns whether a frame was presented.
bool render_execute_stereo_pair(Batch& b, unsigned fbo_l, unsigned fbo_r, float eye_sep,
                                float panel_width);
// In a stereo pair, a perspective draw the game confines to a band of the depth buffer no
// deeper than `band` (its HUD layer, say) is drawn on the panel itself, with no disparity,
// as a film's subtitles are; a draw with no band at all (a zero-width z range) is not
// affected. 0, the default, pins nothing.
void render_set_panel_band(float band);
extern bool g_cull_swap;
extern const char* g_dump_dir;
extern int g_dump_every;
}  // namespace gx
