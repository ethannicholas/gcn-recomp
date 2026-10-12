// Relocatable modules: the SDK's .rel files, linked by OSLink wherever the game allocates.
//
// The recompiler compiles each module at a canonical address (recomp/rel.py lays them out
// above RAM) and the game links its own copy wherever it likes, which may differ from run
// to run. Two things tie the two together:
//
//   * Every instruction whose immediate the linker would patch with an address in a module
//     (the HA/HI/LO halves of a lis/addi or lis/lwz pair) is generated as an expression on
//     g_rel_base, the section's current base, so it computes what the linker would have
//     written. Branches between functions need nothing: the canonical link resolved them,
//     and each is a C call. Data the linker relocates is in guest memory as the game
//     linked it.
//   * A code address the game holds -- a callback, a vtable entry, a jump table's target --
//     is where the game linked the module. rels_canon maps it to the canonical address,
//     which the function table and the module's jump tables are keyed by.
//
// The generated code calls rels_linked after every successful OSLink/OSLinkFixed and
// rels_unlinked before every OSUnlink, so both stay exactly as the game's module list.
#include <atomic>

#include "recomp.h"
#include "runtime.h"

uint32_t g_rel_base[64 * REL_MAX_SECTIONS];

namespace {

// The executable sections currently linked: [base, base + size) maps to canon.
struct Live { uint32_t base, size, canon, module, info; };
constexpr int MAX_LIVE = 128;
Live g_live[MAX_LIVE];
std::atomic<int> g_live_count{0};

int module_index(uint32_t id) {
    for (uint32_t k = 0; k < g_recomp_rel_count; k++)
        if (g_recomp_rels[k].id == id) return (int)k;
    return -1;
}

void forget(uint32_t info) {
    int n = g_live_count.load(std::memory_order_acquire);
    for (int i = 0; i < n; i++)
        if (g_live[i].info == info) g_live[i].size = 0;
}

}  // namespace

void rels_linked(uint32_t info) {
    const uint32_t id = LD32(info);
    const int k = module_index(id);
    if (k < 0) {
        LOG(LOG_OS, "OSLink: module %u at %08X was not recompiled\n", id, info);
        return;
    }
    if (k >= 64) fatal("rels: too many modules");
    forget(info);
    // After a link the header's section table pointer and each section's offset are
    // absolute; the low bit of an offset marks an executable section.
    uint32_t table = LD32(info + 0x10);
    if (table < 0x80000000u) table += info;
    const RecompRel& m = g_recomp_rels[k];
    for (uint32_t i = 0; i < m.count; i++) {
        const RecompRelSection& s = m.sections[i];
        const uint32_t base = LD32(table + s.index * 8) & ~1u;
        g_rel_base[k * REL_MAX_SECTIONS + s.index] = base;
        if (!s.exec) continue;
        int n = g_live_count.load(std::memory_order_relaxed);
        int slot = -1;
        for (int j = 0; j < n; j++)
            if (!g_live[j].size) { slot = j; break; }
        if (slot < 0) {
            if (n == MAX_LIVE) fatal("rels: too many linked sections");
            slot = n;
        }
        g_live[slot] = {base, s.size, s.canon, (uint32_t)k, info};
        if (slot == n) g_live_count.store(n + 1, std::memory_order_release);
    }
    LOG(LOG_OS, "OSLink: %s (module %u) at %08X\n", m.name, id, info);
}

void rels_unlinked(uint32_t info) {
    const int k = module_index(LD32(info));
    forget(info);
    if (k >= 0) {
        for (uint32_t s = 0; s < REL_MAX_SECTIONS; s++) g_rel_base[k * REL_MAX_SECTIONS + s] = 0;
        LOG(LOG_OS, "OSUnlink: %s at %08X\n", g_recomp_rels[k].name, info);
    }
}

uint32_t rels_canon(uint32_t a) {
    int n = g_live_count.load(std::memory_order_acquire);
    for (int i = 0; i < n; i++) {
        const Live& l = g_live[i];
        if (a - l.base < l.size) return l.canon + (a - l.base);
    }
    return a;
}
