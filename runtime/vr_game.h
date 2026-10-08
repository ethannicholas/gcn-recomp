// What a game tells the headset frontends (android/openxr_main.cpp, android/egl_main.cpp).
//
// The frontends know how to put a frame on a panel, how to draw it per eye, and how to
// morph between the two. When to do which, how big the world is, and where else an eye
// might stand are facts about a game. A game project supplies them by calling
// vr::set_game_hooks from a static initializer in one of its SOURCES (gcn_add_game);
// every hook is optional. This lives in the runtime rather than beside the frontends so
// that the desktop build carries the hooks too: GCN_STEREOLOG=1 there prints what
// wants_stereo answers, against frames that can be dumped and looked at.
#pragma once
#include "vr_config.h"

namespace gx { struct Batch; }

namespace vr {
struct GameHooks {
    // Adjusts the defaults before vr.txt is read over them: the game's units per metre,
    // its camera pitch, where its HUD reads well.
    void (*config_defaults)(VrConfig& c) = nullptr;

    // Called once per game frame with that frame's batch, from the render thread: true to
    // present the world in stereo, false for the theater panel. The view follows a change
    // in the answer, held for two frames so a transient cannot flap it; the left
    // thumbstick click overrides it until the answer next changes. Without this hook the
    // view is the viewer's alone: start_in_stereo, then the click.
    bool (*wants_stereo)(const gx::Batch& b) = nullptr;

    // Turns the game's own alternative eye on or off (installing a gx::EyeHook and
    // calling gx::render_set_first_person, say). Called with on=false whenever the view
    // leaves stereo. Without this hook the right thumbstick click does nothing.
    void (*set_first_person)(bool on, const VrConfig& c) = nullptr;
};

void set_game_hooks(const GameHooks& h);
const GameHooks& game_hooks();

// The defaults with the game's adjustments applied, then `dir`/vr.txt over them.
VrConfig load_config(const std::string& dir);
}  // namespace vr
