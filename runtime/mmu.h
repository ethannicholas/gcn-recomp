// The Gekko's page tables, for a game that maps memory with them. See mmu.cpp.
#pragma once
#include <cstdint>

// The page table base register (mtsdr1), from hle_mtspr.
void mmu_set_sdr1(uint32_t v);
