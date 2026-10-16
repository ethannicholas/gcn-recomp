// See input_map.h.
#include "input_map.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
// After the standard headers: SDL_endian.h trips over clang's intrinsics otherwise.
#include <SDL.h>

namespace {

enum class Target {
    A, B, X, Y, Z, Start, DUp, DDown, DLeft, DRight, L, R,
    StickX, StickY, CStickX, CStickY,
    StickLeft, StickRight, StickUp, StickDown,
    CStickLeft, CStickRight, CStickUp, CStickDown,
    Count
};

struct TargetName { const char* name; Target t; };
const TargetName kTargets[] = {
    {"A", Target::A}, {"B", Target::B}, {"X", Target::X}, {"Y", Target::Y}, {"Z", Target::Z},
    {"start", Target::Start},
    {"dpad_up", Target::DUp}, {"dpad_down", Target::DDown},
    {"dpad_left", Target::DLeft}, {"dpad_right", Target::DRight},
    {"L", Target::L}, {"R", Target::R},
    {"stick_x", Target::StickX}, {"stick_y", Target::StickY},
    {"cstick_x", Target::CStickX}, {"cstick_y", Target::CStickY},
    {"stick_left", Target::StickLeft}, {"stick_right", Target::StickRight},
    {"stick_up", Target::StickUp}, {"stick_down", Target::StickDown},
    {"cstick_left", Target::CStickLeft}, {"cstick_right", Target::CStickRight},
    {"cstick_up", Target::CStickUp}, {"cstick_down", Target::CStickDown},
};

bool bipolar(Target t) { return t >= Target::StickX && t <= Target::CStickY; }

enum class Kind { Key, Button, Hat, Axis };
enum class Half { Whole, Plus, Minus };

struct Binding {
    int line = 0;
    Target target = Target::A;
    Kind kind = Kind::Key;
    SDL_Scancode key = SDL_SCANCODE_UNKNOWN;
    std::string device;  // as written, "#n" removed
    int nth = 1;         // which of the devices with that name
    int index = 0;       // button, hat or axis number
    uint8_t hat_dir = 0;
    Half half = Half::Whole;
    bool invert = false;
    float deadzone = 0.0f, gain = 1.0f, threshold = -1.0f;
    SDL_Joystick* joy = nullptr;  // resolved whenever a device comes or goes
};

std::string g_path;
std::vector<Binding> g_bindings;
std::vector<SDL_Joystick*> g_open;  // every joystick present, in the order it arrived

bool same_name(const char* a, const std::string& b) {
    if (!a || strlen(a) != b.size()) return false;
    for (size_t i = 0; i < b.size(); i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return true;
}

// "Name #2" -> ("Name", 2)
void split_nth(const std::string& s, std::string& name, int& nth) {
    name = s;
    nth = 1;
    const size_t h = s.rfind(" #");
    if (h == std::string::npos || h + 2 >= s.size()) return;
    for (size_t i = h + 2; i < s.size(); i++)
        if (!isdigit((unsigned char)s[i])) return;
    name = s.substr(0, h);
    nth = atoi(s.c_str() + h + 2);
}

// How a device would be written in the file: its name, and "#n" if it is not the first
// of that name.
std::string device_label(SDL_Joystick* j) {
    const char* name = SDL_JoystickName(j);
    std::string n = name ? name : "?";
    int nth = 0;
    for (SDL_Joystick* o : g_open) {
        if (same_name(SDL_JoystickName(o), n)) nth++;
        if (o == j) break;
    }
    if (nth > 1) n += " #" + std::to_string(nth);
    return n;
}

SDL_Joystick* find_device(const std::string& name, int nth) {
    int seen = 0;
    for (SDL_Joystick* j : g_open)
        if (same_name(SDL_JoystickName(j), name) && ++seen == nth) return j;
    return nullptr;
}

void resolve() {
    for (auto& b : g_bindings)
        if (b.kind != Kind::Key) b.joy = find_device(b.device, b.nth);
}

// Splits a line into words: "quoted strings" are one word, '=' is a word of its own, and
// '#' outside quotes ends the line.
bool tokenize(const char* s, std::vector<std::string>& out, std::string& err) {
    while (*s) {
        if (isspace((unsigned char)*s)) { s++; continue; }
        if (*s == '#') break;
        if (*s == '=') { out.emplace_back("="); s++; continue; }
        if (*s == '"') {
            const char* e = strchr(s + 1, '"');
            if (!e) { err = "no closing quote"; return false; }
            out.emplace_back(s + 1, e);
            s = e + 1;
            continue;
        }
        const char* e = s;
        while (*e && !isspace((unsigned char)*e) && *e != '=' && *e != '#' && *e != '"') e++;
        out.emplace_back(s, e);
        s = e;
    }
    return true;
}

bool parse_float(const std::string& s, float& v) {
    char* end;
    v = strtof(s.c_str(), &end);
    return !s.empty() && *end == 0;
}

bool parse_int(const std::string& s, int& v) {
    if (s.empty()) return false;
    for (char c : s)
        if (!isdigit((unsigned char)c)) return false;
    v = atoi(s.c_str());
    return true;
}

bool parse_line(const std::vector<std::string>& w, Binding& b, std::string& err) {
    if (w.size() < 3 || w[1] != "=") { err = "expected <pad control> = <source>"; return false; }
    bool found = false;
    for (const auto& t : kTargets)
        if (!SDL_strcasecmp(t.name, w[0].c_str())) { b.target = t.t; found = true; }
    if (!found) { err = "unknown pad control \"" + w[0] + "\""; return false; }
    size_t i = 2;
    if (w[i] == "key") {
        if (i + 1 >= w.size()) { err = "key: which key?"; return false; }
        b.kind = Kind::Key;
        b.key = SDL_GetScancodeFromName(w[i + 1].c_str());
        if (b.key == SDL_SCANCODE_UNKNOWN) { err = "unknown key \"" + w[i + 1] + "\" (--list-controls prints the names)"; return false; }
        i += 2;
    } else {
        split_nth(w[i], b.device, b.nth);
        if (i + 2 >= w.size()) { err = "expected \"<device>\" button|hat|axis <n>"; return false; }
        const std::string& what = w[i + 1];
        std::string num = w[i + 2];
        if (what == "button") b.kind = Kind::Button;
        else if (what == "hat") b.kind = Kind::Hat;
        else if (what == "axis") {
            b.kind = Kind::Axis;
            if (!num.empty() && (num.back() == '+' || num.back() == '-')) {
                b.half = num.back() == '+' ? Half::Plus : Half::Minus;
                num.pop_back();
            }
        } else { err = "expected button, hat or axis after the device, not \"" + what + "\""; return false; }
        if (!parse_int(num, b.index)) { err = "\"" + w[i + 2] + "\" is not a " + what + " number"; return false; }
        i += 3;
        if (b.kind == Kind::Hat) {
            if (i >= w.size()) { err = "hat: which direction (up, down, left, right)?"; return false; }
            const std::string& d = w[i++];
            if (d == "up") b.hat_dir = SDL_HAT_UP;
            else if (d == "down") b.hat_dir = SDL_HAT_DOWN;
            else if (d == "left") b.hat_dir = SDL_HAT_LEFT;
            else if (d == "right") b.hat_dir = SDL_HAT_RIGHT;
            else { err = "hat direction \"" + d + "\" is not up, down, left or right"; return false; }
        }
    }
    while (i < w.size()) {
        const std::string& o = w[i++];
        if (o == "invert") { b.invert = true; continue; }
        float* dst = o == "deadzone" ? &b.deadzone : o == "gain" ? &b.gain : o == "threshold" ? &b.threshold : nullptr;
        if (!dst) { err = "unknown option \"" + o + "\""; return false; }
        if (i >= w.size() || !parse_float(w[i], *dst)) { err = o + " needs a number"; return false; }
        i++;
    }
    if (b.deadzone < 0.0f || b.deadzone >= 1.0f) { err = "deadzone must be from 0 to below 1"; return false; }
    if (b.threshold < 0.0f) b.threshold = (b.target == Target::L || b.target == Target::R) ? 0.85f : 0.5f;
    return true;
}

// A binding's value: -1 to +1 onto a whole stick axis, 0 to 1 onto anything else.
float value(const Binding& b, const uint8_t* keys) {
    float v = 0.0f;
    switch (b.kind) {
    case Kind::Key: v = keys[b.key] ? 1.0f : 0.0f; break;
    case Kind::Button: v = b.joy && SDL_JoystickGetButton(b.joy, b.index) ? 1.0f : 0.0f; break;
    case Kind::Hat: v = b.joy && (SDL_JoystickGetHat(b.joy, b.index) & b.hat_dir) ? 1.0f : 0.0f; break;
    case Kind::Axis: {
        if (!b.joy) return 0.0f;
        float a = std::clamp(SDL_JoystickGetAxis(b.joy, b.index) / 32767.0f, -1.0f, 1.0f);
        if (b.invert) a = -a;
        if (b.half == Half::Plus) v = std::max(a, 0.0f);
        else if (b.half == Half::Minus) v = std::max(-a, 0.0f);
        else if (bipolar(b.target)) v = a;
        else v = (a + 1.0f) * 0.5f;
        break;
    }
    }
    const float m = std::fabs(v);
    if (m <= b.deadzone) return 0.0f;
    const float s = (m - b.deadzone) / (1.0f - b.deadzone) * b.gain;
    return std::copysign(std::min(s, 1.0f), v);
}

uint8_t stick_byte(float v) { return (uint8_t)std::clamp(128 + (int)std::lround(v * 100.0f), 0, 255); }

void print_targets() {
    fprintf(stderr, "Pad controls:");
    for (const auto& t : kTargets) fprintf(stderr, " %s", t.name);
    fprintf(stderr, "\n");
}

}  // namespace

bool input_map_load(const char* path) {
    g_path = path;
    g_bindings.clear();
    FILE* f = fopen(path, "rb");
    if (!f) return true;
    char line[512];
    int n = 0;
    bool ok = true;
    while (fgets(line, sizeof(line), f)) {
        n++;
        std::vector<std::string> w;
        std::string err;
        Binding b;
        b.line = n;
        if (tokenize(line, w, err) && (w.empty() || parse_line(w, b, err))) {
            if (!w.empty()) g_bindings.push_back(b);
            continue;
        }
        fprintf(stderr, "%s:%d: %s; line ignored\n", path, n, err.c_str());
        ok = false;
    }
    fclose(f);
    fprintf(stderr, "%s: %zu binding%s\n", path, g_bindings.size(), g_bindings.size() == 1 ? "" : "s");
    if (!ok) print_targets();
    resolve();
    return ok;
}

bool input_map_binds_index(int device_index) {
    const char* name = SDL_JoystickNameForIndex(device_index);
    for (const auto& b : g_bindings)
        if (b.kind != Kind::Key && same_name(name, b.device)) return true;
    return false;
}

void input_map_event(const SDL_Event& e) {
    if (e.type == SDL_JOYDEVICEADDED) {
        SDL_Joystick* j = SDL_JoystickOpen(e.jdevice.which);
        if (!j || std::find(g_open.begin(), g_open.end(), j) != g_open.end()) return;
        g_open.push_back(j);
        const std::string label = device_label(j);
        resolve();
        for (const auto& b : g_bindings)
            if (b.joy == j) { fprintf(stderr, "%s: \"%s\" connected\n", g_path.c_str(), label.c_str()); break; }
    } else if (e.type == SDL_JOYDEVICEREMOVED) {
        SDL_Joystick* j = SDL_JoystickFromInstanceID(e.jdevice.which);
        auto it = std::find(g_open.begin(), g_open.end(), j);
        if (it == g_open.end()) return;
        for (const auto& b : g_bindings)
            if (b.joy == j) { fprintf(stderr, "%s: \"%s\" disconnected\n", g_path.c_str(), device_label(j).c_str()); break; }
        g_open.erase(it);
        SDL_JoystickClose(j);
        resolve();
    }
}

void input_map_apply(PadState& p) {
    if (g_bindings.empty()) return;
    const uint8_t* keys = SDL_GetKeyboardState(nullptr);
    float v[(int)Target::Count] = {};
    bool pressed[(int)Target::Count] = {};
    for (const auto& b : g_bindings) {
        const float x = value(b, keys);
        float& acc = v[(int)b.target];
        if (bipolar(b.target)) acc += x;
        else acc = std::max(acc, x);
        if (x > b.threshold) pressed[(int)b.target] = true;
    }
    static const struct { Target t; uint16_t bit; } kButtons[] = {
        {Target::A, PAD_A}, {Target::B, PAD_B}, {Target::X, PAD_X}, {Target::Y, PAD_Y},
        {Target::Z, PAD_Z}, {Target::Start, PAD_START},
        {Target::DUp, PAD_UP}, {Target::DDown, PAD_DOWN}, {Target::DLeft, PAD_LEFT}, {Target::DRight, PAD_RIGHT},
        {Target::L, PAD_L}, {Target::R, PAD_R},
    };
    for (const auto& k : kButtons)
        if (pressed[(int)k.t]) p.buttons |= k.bit;
    auto trig = [&](Target t, uint8_t& out) {
        const uint8_t x = (uint8_t)std::lround(v[(int)t] * 255.0f);
        out = std::max(out, x);
    };
    trig(Target::L, p.trig_l);
    trig(Target::R, p.trig_r);
    // A stick is the file's only while the file pushes it, as the default mapping's is.
    auto stick = [&](Target ax, Target neg, Target pos, uint8_t& out) {
        const float x = std::clamp(v[(int)ax] + v[(int)pos] - v[(int)neg], -1.0f, 1.0f);
        if (x != 0.0f) out = stick_byte(x);
    };
    stick(Target::StickX, Target::StickLeft, Target::StickRight, p.stick_x);
    stick(Target::StickY, Target::StickDown, Target::StickUp, p.stick_y);
    stick(Target::CStickX, Target::CStickLeft, Target::CStickRight, p.cstick_x);
    stick(Target::CStickY, Target::CStickDown, Target::CStickUp, p.cstick_y);
}

// ---------------------------------------------------------------------------
// --list-controls

namespace {

struct AxisWatch {
    SDL_Joystick* joy;
    int axis;
    int16_t rest;
    uint32_t settle_until;  // the rest position is taken from the first readings
    bool out = false;  // reported, and not yet back at rest
};

// A device's first moments are its starting state, not someone pressing it: a switch
// that is on, an axis read before it has settled.
const uint32_t kSettleMs = 300;

std::string quoted_key(SDL_Scancode sc) {
    std::string n = SDL_GetScancodeName(sc);
    return n.find(' ') != std::string::npos ? "\"" + n + "\"" : n;
}

}  // namespace

int input_map_probe() {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    // A window, for the keyboard: keys reach only a focused window. Joysticks are read
    // whether or not it has focus.
    SDL_Window* win = SDL_CreateWindow("Press or move a control (Escape quits)", SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED, 560, 120, 0);
    printf("Press a key or a button, or move an axis well away from where it rests, and the\n"
           "source to bind it with is printed: put it after \"<pad control> =\" in controls.txt.\n"
           "Escape, with this window in front, or closing it quits.\n\n");
    fflush(stdout);
    std::vector<AxisWatch> axes;
    std::vector<std::pair<SDL_JoystickID, uint32_t>> settling;  // device, until
    auto settled = [&](SDL_JoystickID id) {
        for (const auto& [d, until] : settling)
            if (d == id) return SDL_TICKS_PASSED(SDL_GetTicks(), until) != 0;
        return true;
    };
    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT: running = false; break;
            case SDL_KEYDOWN:
                if (e.key.repeat) break;
                if (e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) { running = false; break; }
                printf("key %s\n", quoted_key(e.key.keysym.scancode).c_str());
                break;
            case SDL_JOYDEVICEADDED: {
                SDL_Joystick* j = SDL_JoystickOpen(e.jdevice.which);
                if (!j || std::find(g_open.begin(), g_open.end(), j) != g_open.end()) break;
                g_open.push_back(j);
                settling.push_back({SDL_JoystickInstanceID(j), SDL_GetTicks() + kSettleMs});
                printf("device \"%s\": %d axes, %d buttons, %d hats\n", device_label(j).c_str(),
                       SDL_JoystickNumAxes(j), SDL_JoystickNumButtons(j), SDL_JoystickNumHats(j));
                for (int a = 0; a < SDL_JoystickNumAxes(j); a++) {
                    int16_t rest = 0;
                    if (!SDL_JoystickGetAxisInitialState(j, a, &rest)) rest = SDL_JoystickGetAxis(j, a);
                    axes.push_back({j, a, rest, SDL_GetTicks() + kSettleMs});
                }
                break;
            }
            case SDL_JOYDEVICEREMOVED: {
                SDL_Joystick* j = SDL_JoystickFromInstanceID(e.jdevice.which);
                auto it = std::find(g_open.begin(), g_open.end(), j);
                if (it == g_open.end()) break;
                printf("device \"%s\" removed\n", device_label(j).c_str());
                axes.erase(std::remove_if(axes.begin(), axes.end(), [&](const AxisWatch& w) { return w.joy == j; }), axes.end());
                g_open.erase(it);
                SDL_JoystickClose(j);
                break;
            }
            case SDL_JOYBUTTONDOWN: {
                SDL_Joystick* j = SDL_JoystickFromInstanceID(e.jbutton.which);
                if (j && settled(e.jbutton.which)) printf("\"%s\" button %d\n", device_label(j).c_str(), e.jbutton.button);
                break;
            }
            case SDL_JOYHATMOTION: {
                SDL_Joystick* j = SDL_JoystickFromInstanceID(e.jhat.which);
                static const struct { uint8_t bit; const char* name; } kDirs[] = {
                    {SDL_HAT_UP, "up"}, {SDL_HAT_DOWN, "down"}, {SDL_HAT_LEFT, "left"}, {SDL_HAT_RIGHT, "right"}};
                for (const auto& d : kDirs)
                    if (j && settled(e.jhat.which) && (e.jhat.value & d.bit)) printf("\"%s\" hat %d %s\n", device_label(j).c_str(), e.jhat.hat, d.name);
                break;
            }
            default: break;
            }
        }
        // An axis is reported once it has gone a quarter of its range from where it
        // rested, and again only after coming back.
        for (auto& w : axes) {
            const int cur = SDL_JoystickGetAxis(w.joy, w.axis), d = cur - w.rest;
            if (!SDL_TICKS_PASSED(SDL_GetTicks(), w.settle_until)) {
                w.rest = (int16_t)cur;
                continue;
            }
            if (w.out) {
                if (abs(d) < 6000) w.out = false;
                continue;
            }
            if (abs(d) < 16000) continue;
            w.out = true;
            const std::string dev = "\"" + device_label(w.joy) + "\"";
            if (w.rest < -24000)
                printf("%s axis %d             (rests low: a pedal or lever)\n", dev.c_str(), w.axis);
            else if (w.rest > 24000)
                printf("%s axis %d invert      (rests high: a pedal or lever)\n", dev.c_str(), w.axis);
            else
                printf("%s axis %d%c            (this direction alone; \"axis %d\" for both, as a stick or wheel)\n",
                       dev.c_str(), w.axis, d > 0 ? '+' : '-', w.axis);
        }
        fflush(stdout);
        SDL_Delay(10);
    }
    if (win) SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
