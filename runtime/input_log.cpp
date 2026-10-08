// Input logging and replay. See input_log.h.
#include "input_log.h"
#include "runtime.h"
#include "platform.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <vector>

uint32_t gx_frames_submitted();

namespace {

struct Entry {
    uint32_t key;  // poll sequence number (v2) or presented frame (v1)
    uint8_t chan;
    PadState s;
};

uint32_t g_poll_seq;  // pad polls so far, all channels

// ---- recording ----
FILE* g_out;
PadState g_last[4];
bool g_have_last[4];
std::mutex g_mutex;

bool copy_file(const std::string& from, const std::string& to) {
    FILE* in = fopen(from.c_str(), "rb");
    if (!in) return false;
    FILE* out = fopen(to.c_str(), "wb");
    if (!out) { fclose(in); return false; }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
    return true;
}

bool same(const PadState& a, const PadState& b) {
    return a.connected == b.connected && a.buttons == b.buttons && a.stick_x == b.stick_x &&
           a.stick_y == b.stick_y && a.cstick_x == b.cstick_x && a.cstick_y == b.cstick_y &&
           a.trig_l == b.trig_l && a.trig_r == b.trig_r;
}

// ---- replay ----
std::vector<Entry> g_replay;     // sorted by key, as logged
size_t g_replay_pos;             // first entry not yet applied
PadState g_replay_state[4];
bool g_replay_loaded;
bool g_replay_by_frame;          // a v1 log
uint32_t g_replay_last_key;

uint32_t replay_key() { return g_replay_by_frame ? gx_frames_submitted() : g_poll_seq; }

void record(int chan, const PadState& s) {
    if (!g_out) return;
    if (g_have_last[chan] && same(g_last[chan], s)) return;
    g_last[chan] = s;
    g_have_last[chan] = true;
    fprintf(g_out, "%u %u %d %d %04X %02X %02X %02X %02X %02X %02X\n", g_poll_seq, gx_frames_submitted(), chan,
            s.connected ? 1 : 0, s.buttons, s.stick_x, s.stick_y, s.cstick_x, s.cstick_y, s.trig_l, s.trig_r);
    // Flushed per line: the point of the log is to survive whatever happens next.
    fflush(g_out);
}

}  // namespace

bool input_log_start(const std::string& dir, const std::string& memcard_path) {
    plat_make_dirs(dir);
    const std::string path = dir + "/inputs.txt";
    g_out = fopen(path.c_str(), "wb");
    if (!g_out) {
        fprintf(stderr, "[input] cannot write %s; not logging input\n", path.c_str());
        return false;
    }
    char stamp[64] = "";
    time_t now = time(nullptr);
    strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S", localtime(&now));
    rtc_set_base(now);
    fprintf(g_out, "# gcn-recomp input log v2\n# game %s  started %s\n# epoch %lld\n", GCN_DISC_ID, stamp, (long long)now);
    fprintf(g_out, "# poll frame chan connected buttons stick_x stick_y cstick_x cstick_y trig_l trig_r\n");
    fprintf(g_out, "# (hex; each line is that channel's state from that pad poll on)\n");
    fflush(g_out);
    if (plat_readable(memcard_path.c_str())) {
        if (!copy_file(memcard_path, dir + "/memcard_a.raw"))
            fprintf(stderr, "[input] could not snapshot %s into the log\n", memcard_path.c_str());
    }
    fprintf(stderr, "[input] logging to %s\n", path.c_str());
    return true;
}


bool input_replay_load(const std::string& dir, std::string& memcard_out) {
    const std::string path = dir + "/inputs.txt";
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        fprintf(stderr, "[input] no %s to replay\n", path.c_str());
        return false;
    }
    char line[256];
    int version = 1;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') {
            long long epoch;
            if (sscanf(line, "# gcn-recomp input log v%d", &version) == 1) continue;
            if (sscanf(line, "# epoch %lld", &epoch) == 1) rtc_set_base((time_t)epoch);
            continue;
        }
        if (line[0] == '\n') continue;
        unsigned key, frame, chan, conn, buttons, sx, sy, cx, cy, l, r;
        if (version >= 2) {
            if (sscanf(line, "%u %u %u %u %x %x %x %x %x %x %x", &key, &frame, &chan, &conn, &buttons, &sx, &sy, &cx, &cy, &l, &r) != 11) continue;
        } else {
            if (sscanf(line, "%u %u %u %x %x %x %x %x %x %x", &key, &chan, &conn, &buttons, &sx, &sy, &cx, &cy, &l, &r) != 10) continue;
        }
        if (chan > 3) continue;
        Entry e;
        e.key = key;
        e.chan = (uint8_t)chan;
        e.s.connected = conn != 0;
        e.s.buttons = (uint16_t)buttons;
        e.s.stick_x = (uint8_t)sx; e.s.stick_y = (uint8_t)sy;
        e.s.cstick_x = (uint8_t)cx; e.s.cstick_y = (uint8_t)cy;
        e.s.trig_l = (uint8_t)l; e.s.trig_r = (uint8_t)r;
        g_replay.push_back(e);
    }
    fclose(f);
    if (g_replay.empty()) {
        fprintf(stderr, "[input] %s holds no input\n", path.c_str());
        return false;
    }
    g_replay_last_key = g_replay.back().key;
    g_replay_by_frame = version < 2;
    for (auto& s : g_replay_state) { s = PadState(); s.connected = true; }
    g_replay_loaded = true;
    memcard_out.clear();
    const std::string snap = dir + "/memcard_a.raw";
    if (plat_readable(snap.c_str())) {
        memcard_out = dir + "/memcard_replay.raw";
        if (!copy_file(snap, memcard_out)) {
            fprintf(stderr, "[input] could not copy the card snapshot; replaying with the live card\n");
            memcard_out.clear();
        }
    }
    fprintf(stderr, "[input] replaying %zu changes over %u %s from %s%s%s\n", g_replay.size(),
            g_replay_last_key, g_replay_by_frame ? "frames" : "polls", path.c_str(),
            memcard_out.empty() ? "" : " (with its card snapshot)",
            g_replay_by_frame ? " (v1 log: keyed by frame, so only approximate)" : "");
    return true;
}

bool input_replay_active() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_replay_loaded && replay_key() <= g_replay_last_key;
}

bool input_pad_poll(int chan, PadState& s) {
    if (chan < 0 || chan > 3) return false;
    std::lock_guard<std::mutex> lk(g_mutex);
    bool replayed = false;
    if (g_replay_loaded) {
        const uint32_t key = replay_key();
        while (g_replay_pos < g_replay.size() && g_replay[g_replay_pos].key <= key) {
            const Entry& e = g_replay[g_replay_pos++];
            g_replay_state[e.chan] = e.s;
        }
        if (key > g_replay_last_key) {
            static bool said;
            if (!said) {
                said = true;
                fprintf(stderr, "[input] replay finished at poll %u (frame %u); live input from here\n", g_poll_seq, gx_frames_submitted());
            }
        } else {
            s = g_replay_state[chan];
            replayed = true;
        }
    }
    record(chan, s);
    g_poll_seq++;
    return replayed;
}
