// See telemetry.h.
#include "telemetry.h"
#include "runtime.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_t = SOCKET;
static const sock_t kNoSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
static const sock_t kNoSocket = -1;
#endif

namespace telemetry {
namespace {

sock_t g_sock = kNoSocket;
sockaddr_in g_dest{};
std::map<std::string, float> g_keys;
std::map<std::string, float> g_defaults;   // the game's own (set_default)
bool g_log = false;

float key(const char* k, float fallback) {
    auto it = g_keys.find(k);
    if (it != g_keys.end()) return it->second;
    it = g_defaults.find(k);
    return it == g_defaults.end() ? fallback : it->second;
}

void read_file() {
    FILE* f = fopen("telemetry.txt", "rb");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char k[64];
        double v;
        if (line[0] == '#' || sscanf(line, "%63s %lf", k, &v) != 2) continue;
        g_keys[k] = (float)v;
    }
    fclose(f);
}

// SimRacingStudio's custom-game packet, version 102: Python's struct format
// "3sI50s50s50sfffifffffffffffIIII" in its native layout, which on every platform this runs
// on puts a byte of padding after the 3-byte tag and two after the strings. 236 bytes,
// little-endian. The suspension and terrain fields describe wheels, which a hull has not
// got; they are left at zero.
constexpr size_t kSrsSize = 236;
void put_str(uint8_t* p, size_t at, size_t n, const char* s) { strncpy((char*)p + at, s, n - 1); }
void put_u32(uint8_t* p, size_t at, uint32_t v) { memcpy(p + at, &v, 4); }
void put_f32(uint8_t* p, size_t at, float v) { memcpy(p + at, &v, 4); }

void send_srs(const Motion& m) {
    uint8_t p[kSrsSize] = {};
    memcpy(p, "api", 3);
    put_u32(p, 4, 102);
    put_str(p, 8, 50, GCN_GAME_TITLE);
    put_str(p, 58, 50, "");
    put_str(p, 108, 50, "");
    put_f32(p, 160, m.speed_kmh);
    put_f32(p, 164, m.rpm);
    put_f32(p, 168, m.max_rpm);
    put_u32(p, 172, (uint32_t)m.gear);
    put_f32(p, 176, m.pitch);
    put_f32(p, 180, m.roll);
    put_f32(p, 184, m.yaw);
    put_f32(p, 188, m.sway_velocity);  // lateral velocity
    put_f32(p, 192, m.sway);           // lateral acceleration
    put_f32(p, 196, m.heave);          // vertical acceleration
    put_f32(p, 200, m.surge);          // longitudinal acceleration
    sendto(g_sock, (const char*)p, (int)kSrsSize, 0, (const sockaddr*)&g_dest, sizeof(g_dest));
}

}  // namespace

bool configure(const char* spec) {
    read_file();
    std::string host = "127.0.0.1";
    int port = 33001;
    if (spec && *spec) {
        std::string s(spec);
        const size_t colon = s.rfind(':');
        if (colon == std::string::npos) {
            host = s;
        } else {
            host = s.substr(0, colon);
            port = atoi(s.c_str() + colon + 1);
        }
    }
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
#endif
    g_dest.sin_family = AF_INET;
    g_dest.sin_port = htons((uint16_t)port);
    if (port <= 0 || port > 65535 || inet_pton(AF_INET, host.c_str(), &g_dest.sin_addr) != 1) {
        fprintf(stderr, "telemetry: '%s' is not an IPv4 host:port\n", spec);
        return false;
    }
    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock == kNoSocket) return false;
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(g_sock, FIONBIO, &nb);
#endif
    g_log = key("log", 0.0f) != 0.0f || getenv("GCN_TELEMETRY_LOG");
    fprintf(stderr, "telemetry: SimRacingStudio API packets to %s:%d\n", host.c_str(), port);
    return true;
}

bool enabled() { return g_sock != kNoSocket; }

float get(const char* k, float fallback) { return key(k, fallback); }

void set_default(const char* k, float value) { g_defaults[k] = value; }

// What goes to the platform is built in four stages, each of which on its own would stop a
// class of jerk, so that no single mistake upstream reaches the rider:
//
// 1. Rest and back. A race starting or ending, a pause, the vehicle lost: the motion eases
//    out to rest over `fade_s`, holding its last value on the way, and eases back in. The
//    ease is a smoothstep, so the platform leaves rest and arrives there with no velocity of
//    its own; a linear fade starts it moving at full speed in an instant.
// 2. Discontinuities. A respawn puts the vehicle somewhere else in one frame, so whatever
//    was being sent is crossfaded, over `fade_s` and eased the same way, into the new values
//    rather than stepped to them.
// 3. Yaw washout. A heading is not a position a platform can hold: it runs on for ever and
//    wraps from +180 to -180. What is sent is how far the heading has turned from where it
//    was `yaw_washout_s` ago, more or less -- the turn itself, returning to centre once it is
//    over -- unwrapped, so a wrap is not a turn. A game that sends the platform's own yaw
//    instead (yaw_heading false: a slide's angle, a spin) has it sent as it is.
// 4. Limits. Last of all, after the gains, every channel is clamped to a range and to a rate
//    of change: angles to `max_angle` degrees, pitch and roll to `angle_rate` degrees a
//    second and yaw to `yaw_rate`, accelerations to `max_accel` m/s^2 and `accel_rate`
//    m/s^2 a second. However a value got there -- a crash into a wall reads 7 g for a
//    frame -- this is the most the platform is asked to do. The step between two packets
//    is bounded too, by bounding the time between them, so a stall in the game does not
//    let a step through either.
//
//    The rates are the last guard, not the smoothing. At 60 degrees and 60 m/s^2 a second
//    they were both, and a rider found the motion tame: a wave's chop held the
//    accelerations at the limit four frames in five, slewing back and forth, so that a
//    collision reached the platform as a few m/s^2 and sometimes the wrong way, and a dive
//    off a ramp (the hull pitching at some 300 degrees a second) as a slow lean. Smoothing
//    belongs upstream, where the game knows what is noise; the jerks the limits are here
//    for are the discontinuities stages 1 and 2 already take out. Yaw keeps the slow rate:
//    a heading that swings is the one jerk on a start line that is not yet explained. (A
//    game that sends its own yaw can raise it with set_default.)
//
// Between the gains and the limits, a road texture (5): band-limited noise on heave, pitch and
// roll, as strong as the game's `roughness` and the speed make it, eased in and out with the
// rest of the motion. A platform that sat dead still on a straight felt like no road at all.
// `texture_heave`, `texture_pitch` and `texture_roll` are its RMS at roughness 1 and
// `texture_speed_kmh` or faster (m/s^2 and degrees), `texture_hz` its bandwidth, and
// `texture_gain` scales the lot.
namespace {

enum Ch { kSpeed, kPitch, kRoll, kYaw, kSurge, kSway, kHeave, kSlide, kChannels };

void to_array(const Motion& m, float* v) {
    v[kSpeed] = m.speed_kmh; v[kPitch] = m.pitch; v[kRoll] = m.roll; v[kYaw] = m.yaw;
    v[kSurge] = m.surge; v[kSway] = m.sway; v[kHeave] = m.heave; v[kSlide] = m.sway_velocity;
}

float smoothstep(float t) {
    t = fminf(1.0f, fmaxf(0.0f, t));
    return t * t * (3.0f - 2.0f * t);
}

}  // namespace

void publish(const Motion& in) {
    if (!enabled()) return;
    static auto t_prev = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const float dt = fminf(0.1f, std::chrono::duration<float>(now - t_prev).count());
    t_prev = now;
    const float fade = key("fade_s", 1.0f);
    const float fade_step = fade > 0.0f ? dt / fade : 1.0f;

    // The input, with its heading washed out (3).
    static float held[kChannels];        // the last active input: what rest fades away from
    static bool have_heading = false;
    static float heading_prev, unwrapped, reference;
    if (in.active && !in.yaw_heading) {
        have_heading = false;
        to_array(in, held);
    } else if (in.active) {
        if (!have_heading || in.discontinuity) {
            unwrapped = reference = in.yaw;
            have_heading = true;
        } else {
            float d = in.yaw - heading_prev;
            while (d > 180.0f) d -= 360.0f;
            while (d < -180.0f) d += 360.0f;
            unwrapped += d;
            const float washout = key("yaw_washout_s", 1.5f);
            reference += (unwrapped - reference) * (washout > 0.0f ? 1.0f - expf(-dt / washout) : 1.0f);
        }
        heading_prev = in.yaw;
        to_array(in, held);
        held[kYaw] = unwrapped - reference;
    }

    // Rest and back (1).
    static float rest_t = 0.0f;   // 0 at rest, 1 following the input
    rest_t = in.active ? fminf(1.0f, rest_t + fade_step) : fmaxf(0.0f, rest_t - fade_step);
    const float level = smoothstep(rest_t);
    float want[kChannels];
    for (int c = 0; c < kChannels; c++) want[c] = held[c] * level;

    // Discontinuities (2): crossfade from what was being sent.
    static float out[kChannels];   // what was last sent
    static float cross_from[kChannels];
    static float cross_t = 1.0f;
    if (in.active && in.discontinuity) {
        for (int c = 0; c < kChannels; c++) cross_from[c] = out[c];
        cross_t = 0.0f;
    }
    if (cross_t < 1.0f) {
        cross_t = fminf(1.0f, cross_t + fade_step);
        const float s = smoothstep(cross_t);
        for (int c = 0; c < kChannels; c++) want[c] = cross_from[c] + (want[c] - cross_from[c]) * s;
    }

    // Gains, then limits (4).
    want[kPitch] *= key("pitch_gain", 1.0f);
    want[kRoll] *= key("roll_gain", 1.0f);
    want[kYaw] *= key("yaw_gain", 1.0f);
    want[kSurge] *= key("surge_gain", 1.0f);
    want[kSway] *= key("sway_gain", 1.0f);
    want[kHeave] *= key("heave_gain", 1.0f);

    // Road texture (5): white noise through a one-pole low-pass, normalised to unit RMS.
    {
        static float tex[3];
        static uint32_t seed = 0x2545F491u;
        auto noise = [] {
            seed = seed * 1664525u + 1013904223u;
            return (float)(seed >> 8) * (2.0f / 16777216.0f) - 1.0f;   // uniform in [-1, 1)
        };
        const float a = 1.0f - expf(-dt * 6.2831853f * key("texture_hz", 8.0f));
        const float norm = a > 0.0f ? 1.0f / sqrtf(a / (2.0f - a) / 3.0f) : 0.0f;
        for (float& t : tex) t += (noise() - t) * a;
        const float rough = in.active ? fminf(1.0f, fmaxf(0.0f, in.roughness)) : 0.0f;
        const float at_speed = fminf(1.0f, fabsf(held[kSpeed]) / fmaxf(1.0f, key("texture_speed_kmh", 60.0f)));
        const float amp = rough * at_speed * level * key("texture_gain", 1.0f) * norm;
        want[kHeave] += tex[0] * amp * key("texture_heave", 1.5f);
        want[kPitch] += tex[1] * amp * key("texture_pitch", 0.3f);
        want[kRoll] += tex[2] * amp * key("texture_roll", 0.5f);
    }

    const float max_angle = key("max_angle", 25.0f), angle_rate = key("angle_rate", 120.0f);
    const float yaw_rate = key("yaw_rate", 60.0f);
    const float max_accel = key("max_accel", 15.0f), accel_rate = key("accel_rate", 300.0f);
    const float range[kChannels] = {1000.0f, max_angle, max_angle, max_angle, max_accel, max_accel, max_accel, 10.0f};
    const float rate[kChannels] = {200.0f, angle_rate, angle_rate, yaw_rate, accel_rate, accel_rate, accel_rate, 20.0f};
    for (int c = 0; c < kChannels; c++) {
        const float v = fminf(range[c], fmaxf(-range[c], want[c]));
        const float step = rate[c] * dt;
        out[c] = fminf(out[c] + step, fmaxf(out[c] - step, v));
    }

    Motion m;
    m.active = in.active;
    m.speed_kmh = out[kSpeed]; m.pitch = out[kPitch]; m.roll = out[kRoll]; m.yaw = out[kYaw];
    m.surge = out[kSurge]; m.sway = out[kSway]; m.heave = out[kHeave]; m.sway_velocity = out[kSlide];
    if (in.active) {
        m.rpm = in.rpm;
        m.max_rpm = in.max_rpm;
        m.gear = in.gear;
    }
    send_srs(m);

    if (g_log) {
        static int n;
        static bool was_active;
        const bool change = in.active != was_active || in.discontinuity;
        was_active = in.active;
        if (change)
            fprintf(stderr, "[telemetry] %s\n", in.discontinuity ? "discontinuity: crossfading" : in.active ? "active: easing in" : "rest: easing out");
        if (n++ % 30 == 0)
            fprintf(stderr, "[telemetry] %s %5.1f km/h pitch %+5.1f roll %+5.1f yaw %+6.1f | surge %+6.2f sway %+6.2f heave %+6.2f m/s2 slide %+5.2f m/s | %4.0f rpm\n",
                    in.active ? "active" : "rest  ", m.speed_kmh, m.pitch, m.roll, m.yaw, m.surge, m.sway, m.heave, m.sway_velocity, m.rpm);
    }
}

}  // namespace telemetry
