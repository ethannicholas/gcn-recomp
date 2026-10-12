// High-level emulation of the JAudio synthesizer ucode, the DSP program JSystem's audio
// library runs in place of AX: its mail protocol, so far. It renders silence.
//
// The protocol, as the CPU side drives it:
//
// - At boot the ucode sends DSP_INIT (interrupting) and then a handshake mail, F3551111,
//   which the library's task-init callback waits for.
// - A command arrives as a count mail (1..0x7FFF) followed by that many words. The first
//   word's top byte is the opcode, with bit 7 set; the rest are its arguments. Each command
//   is acknowledged with DSP_SYNC (interrupting) and F355xxxx, xxxx being the top half of
//   the command word: the library matches that against the command it is waiting for.
// - Opcode 1 tells the ucode how many voices a frame has (the low half of the word).
// - Opcode 2 renders frames: their count in bits 16-23, the output volume in the low half,
//   then the left and right output buffers. It is not acknowledged. Voices are rendered as
//   the CPU allows them: a zero mail and then a sync mail, (group << 16) | flags, releases
//   voices 16*group..16*group+15, a set bit (bit 15 first) for each voice to mix. When a
//   frame's voices are all done the ucode sends DSP_SYNC and F355FF00, on which the CPU
//   sends the next frame's syncs; after the last frame, DSP_FRAME_END, on which the CPU
//   answers CDD10003, and only then are further commands carried out.
// - CDD10001 hands the DSP to another ucode (the boot ROM mails that load it follow),
//   CDD10002 resets to the boot ROM, CDD10000 halts.
#include "../runtime.h"
#include "dsp_hle.h"
#include "jaudio.h"
#include <cstring>
#include <deque>
#include <vector>

namespace {

enum : uint32_t {
    DSP_INIT = 0xDCD10000, DSP_RESUME = 0xDCD10001, DSP_SYNC = 0xDCD10004, DSP_FRAME_END = 0xDCD10005,
    HANDSHAKE = 0xF3551111, ACK = 0xF3550000, FRAME_DONE = 0xF355FF00,
};

// Fingerprints (dsp_hle.cpp's ucode_hash) of the builds of this ucode that speak the
// protocol above.
constexpr uint32_t kHashes[] = {0x2FCDF1EC};

enum class State { Waiting, Command, Sync, Halted };
State g_state;
std::vector<uint32_t> g_batch;
uint32_t g_batch_len;
std::deque<std::vector<uint32_t>> g_pending;
bool g_can_execute;

// Rendering.
bool g_rendering;
uint32_t g_frames, g_frame;
uint32_t g_voices_per_frame, g_voice, g_released;
uint16_t g_sync_flags[16];
uint32_t g_out_left, g_out_right;
uint16_t g_out_volume;

void ack(uint32_t cmd) {
    dsp_send_mail(DSP_SYNC, true);
    dsp_send_mail(ACK | (cmd >> 16), false);
}

void finish_frame() {
    // Each frame fills its share of the output buffers. Silence, until there is a mixer.
    uint32_t bytes = g_out_right > g_out_left ? (g_out_right - g_out_left) / g_frames : 0;
    for (uint32_t base : {g_out_left, g_out_right}) {
        uint32_t a = base + g_frame * bytes;
        if (uint32_t n = dma_fit("jaudio output", a, bytes)) memset(phys_ptr(a), 0, n);
    }
}

// Renders as far as the released voices allow.
void render() {
    while (g_frame < g_frames) {
        while (g_voice < g_voices_per_frame) {
            if (g_voice >= g_released) return;
            g_voice++;
        }
        finish_frame();
        g_frame++;
        g_voice = 0;
        g_released = 0;
        dsp_send_mail(DSP_SYNC, true);
        dsp_send_mail(FRAME_DONE, false);
    }
    g_rendering = false;
    g_can_execute = false;
    dsp_send_mail(DSP_FRAME_END, true);
}

void execute(const std::vector<uint32_t>& cmd) {
    uint32_t op = (cmd[0] >> 24) & 0x7F;
    auto arg = [&](size_t i) { return i < cmd.size() ? cmd[i] : 0u; };
    switch (op) {
    case 0x01:
        g_voices_per_frame = cmd[0] & 0xFFFF;
        LOG(LOG_DSP, "jaudio: %u voices, buffers %08X %08X %08X %08X", g_voices_per_frame, arg(1), arg(2), arg(3), arg(4));
        ack(cmd[0]);
        break;
    case 0x02:
        g_frames = (cmd[0] >> 16) & 0xFF;
        g_out_volume = (uint16_t)cmd[0];
        g_out_left = arg(1) & 0x01FFFFFF;
        g_out_right = arg(2) & 0x01FFFFFF;
        g_frame = g_voice = g_released = 0;
        g_rendering = true;
        render();
        break;
    default:
        LOG(LOG_DSP, "jaudio: command %02X (%zu words) not implemented", op, cmd.size());
        ack(cmd[0]);
        break;
    }
}

void run_pending() {
    while (g_can_execute && !g_rendering && !g_pending.empty()) {
        std::vector<uint32_t> cmd = std::move(g_pending.front());
        g_pending.pop_front();
        execute(cmd);
    }
}

}  // namespace

bool jaudio_is_ucode(uint32_t hash) {
    for (uint32_t h : kHashes)
        if (h == hash) return true;
    return false;
}

void jaudio_boot(bool resume) {
    g_state = State::Waiting;
    if (resume) {
        dsp_send_mail(DSP_RESUME, true);
        return;
    }
    g_pending.clear();
    g_can_execute = true;
    g_rendering = false;
    dsp_send_mail(DSP_INIT, true);
    dsp_send_mail(HANDSHAKE, false);
}

JAudioNext jaudio_mail(uint32_t mail) {
    switch (g_state) {
    case State::Halted:
        LOG(LOG_DSP, "jaudio: mail %08X while halted", mail);
        return JAudioNext::Stay;
    case State::Command:
        g_batch.push_back(mail);
        if (g_batch.size() == g_batch_len) {
            g_pending.push_back(std::move(g_batch));
            g_batch.clear();
            g_state = State::Waiting;
            run_pending();
        }
        return JAudioNext::Stay;
    case State::Sync: {
        g_state = State::Waiting;
        uint32_t group = (mail >> 16) & 0xF;
        g_sync_flags[group] = (uint16_t)mail;
        g_released = (group + 1) * 16;
        if (g_rendering) render();
        return JAudioNext::Stay;
    }
    case State::Waiting:
        break;
    }
    if (mail & 0x80000000u) {
        if ((mail >> 16) != 0xCDD1) {
            LOG(LOG_DSP, "jaudio: unexpected mail %08X", mail);
            return JAudioNext::Stay;
        }
        switch (mail & 0xFFFF) {
        case 1: g_state = State::Halted; return JAudioNext::NewUcode;
        case 2: g_state = State::Halted; return JAudioNext::Reset;
        case 3: g_can_execute = true; run_pending(); return JAudioNext::Stay;
        default: g_state = State::Halted; return JAudioNext::Stay;
        }
    }
    if (mail == 0) {
        g_state = State::Sync;
    } else {
        g_batch.clear();
        g_batch_len = mail & 0x7FFF;
        g_state = State::Command;
    }
    return JAudioNext::Stay;
}
