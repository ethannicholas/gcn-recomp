// See vr_config.h and vr_game.h.
#include "vr_config.h"
#include "vr_game.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

// Messages go to stderr: the headset app pipes it into logcat, and the headless harness
// is run from a shell.
VrConfig vr_config_load(const std::string& dir, const VrConfig& defaults) {
    VrConfig c = defaults;
    const std::string path = dir + "/vr.txt";
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        fprintf(stderr, "no %s, using VR defaults (units_per_metre %.1f)\n", path.c_str(),
                c.units_per_metre);
        return c;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char key[64];
        double val;
        if (line[0] == '#' || sscanf(line, "%63s %lf", key, &val) != 2) continue;
        if (!strcmp(key, "units_per_metre")) c.units_per_metre = (float)val;
        else if (!strcmp(key, "offset_x")) c.offset_x = (float)val;
        else if (!strcmp(key, "offset_y")) c.offset_y = (float)val;
        else if (!strcmp(key, "offset_z")) c.offset_z = (float)val;
        else if (!strcmp(key, "near_m")) c.near_m = (float)val;
        else if (!strcmp(key, "far_m")) c.far_m = (float)val;
        else if (!strcmp(key, "hud_scale")) c.hud_scale = (float)val;
        else if (!strcmp(key, "hud_distance_m")) c.hud_distance_m = (float)val;
        else if (!strcmp(key, "hud_height_m")) c.hud_height_m = (float)val;
        else if (!strcmp(key, "hud_pitch_deg")) c.hud_pitch_deg = (float)val;
        else if (!strcmp(key, "world_pitch_deg")) c.world_pitch_deg = (float)val;
        else if (!strcmp(key, "background_band")) c.background_band = (float)val;
        else if (!strcmp(key, "foreground_band")) c.foreground_band = (float)val;
        else if (!strcmp(key, "foreground_scale")) c.foreground_scale = (float)val;
        else if (!strcmp(key, "theater_scale")) c.theater_scale = (int)val;
        else if (!strcmp(key, "stereo_scale")) c.stereo_scale = (int)val;
        else if (!strcmp(key, "eye_scale")) c.eye_scale = (float)val;
        else if (!strcmp(key, "msaa")) c.msaa = (int)val;
        else if (!strcmp(key, "log_frames")) c.log_frames = val != 0;
        else if (!strcmp(key, "dump_every")) c.dump_every = (int)val;
        else if (!strcmp(key, "start_in_stereo")) c.start_in_stereo = val != 0;
        else if (!strcmp(key, "first_person")) c.first_person = val != 0;
        else if (!strcmp(key, "transition_s")) c.transition_s = (float)val;
        else c.extra[key] = (float)val;
    }
    fclose(f);
    fprintf(stderr,
            "vr.txt: units_per_metre=%.2f offset=(%.2f,%.2f,%.2f) near=%.3fm far=%.0fm "
            "hud=%.3f@%.1fm height=%.2fm pitch=%.1fdeg world_pitch=%.1fdeg layers=%.4f/%.4fx%.2f "
            "scale=%d/%d eyes=%.2fx msaa=%d stereo_at_start=%d first_person=%d transition=%.2fs\n",
            c.units_per_metre, c.offset_x, c.offset_y, c.offset_z, c.near_m, c.far_m,
            c.hud_scale, c.hud_distance_m, c.hud_height_m, c.hud_pitch_deg, c.world_pitch_deg, c.background_band, c.foreground_band, c.foreground_scale,
            c.theater_scale, c.stereo_scale, c.eye_scale, c.msaa, (int)c.start_in_stereo,
            (int)c.first_person, c.transition_s);
    for (const auto& [k, v] : c.extra) fprintf(stderr, "vr.txt: %s=%g (for the game)\n", k.c_str(), v);
    return c;
}

namespace vr {
// Function-local, because the hooks are installed from another translation unit's static
// initializer, which may run before this one's.
static GameHooks& hooks() {
    static GameHooks h;
    return h;
}
void set_game_hooks(const GameHooks& h) { hooks() = h; }
const GameHooks& game_hooks() { return hooks(); }

// Written by the frontend's render thread, read by the guest's; a pose is seven floats, so
// a lock is cheaper than anything cleverer.
static std::mutex g_hand_lock;
static HandPose g_hands[2];
static bool g_hand_valid[2];

void set_hand_pose(Hand h, const HandPose* p) {
    std::lock_guard<std::mutex> l(g_hand_lock);
    g_hand_valid[h] = p != nullptr;
    if (p) g_hands[h] = *p;
}

bool hand_pose(Hand h, HandPose* out) {
    std::lock_guard<std::mutex> l(g_hand_lock);
    if (g_hand_valid[h]) *out = g_hands[h];
    return g_hand_valid[h];
}

VrConfig load_config(const std::string& dir) {
    VrConfig d;
    if (hooks().config_defaults) hooks().config_defaults(d);
    const VrConfig c = vr_config_load(dir, d);
    if (hooks().config_loaded) hooks().config_loaded(c);
    return c;
}
}  // namespace vr
