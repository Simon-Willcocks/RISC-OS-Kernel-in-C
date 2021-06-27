/* Copyright 2021 Simon Willcocks
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "kernel.h"

// 4GiB (32-bit) address range
// 16KiB -> 4096 (12 bits) of 1MiB sections
// 256 4KiB pages in 1MiB => L2TT is 256 words; 1024 bytes

// I could declare various constants, L1TT_size, etc., but I think that they would simply
// obscure what's going on. This code is very much tied to specific hardware that deals in
// bits, bytes, pages and megabytes.

extern int va_base;
extern int rom_size;
extern uint32_t translation_tables;

static uint32_t *const L1TT = (&translation_tables);
static uint32_t *const top_MiB_tt = (&translation_tables) + 4096;
// static uint32_t *const bottom_MiB_tt = (&translation_tables) + 4096 + 1024;

typedef union {
  struct {
    uint32_t type2:2;
    uint32_t B:1;
    uint32_t C:1;
    uint32_t XN:1;
    uint32_t Domain:4;
    uint32_t P:1;
    uint32_t AP:2;
    uint32_t TEX:3;
    uint32_t APX:1;
    uint32_t S:1;
    uint32_t nG:1;
    uint32_t zeros:2;
    uint32_t section_base:12;
  };
  uint32_t raw;
} l1tt_section_entry;

typedef union {
  struct {
    uint32_t type1:2;
    uint32_t SBZ1:1;
    uint32_t NS:1;
    uint32_t SBZ2:1;
    uint32_t Domain:4;
    uint32_t P:1;
    uint32_t page_table_base:22;
  };
  uint32_t raw;
} l1tt_table_entry;

typedef union {
  struct {
    uint32_t XN:1;
    uint32_t small_page:1;
    uint32_t B:1;
    uint32_t C:1;
    uint32_t AP:2;
    uint32_t TEX:3;
    uint32_t APX:1;
    uint32_t S:1;
    uint32_t nG:1;
    uint32_t page_base:20;
  };
  uint32_t raw;
} l2tt_entry;

void __attribute__(( noreturn, noinline )) go_kernel()
{
  uint32_t *L1TT = &translation_tables;
  for (int i = 0; i < 64; i++) {
    L1TT[i] = 0;
  }

  Kernel_start();
}

// Map a privileged read-write page into the top 1MiB of virtual memory
// Might be better with pages instead of addresses
static void map_work_area( uint32_t *l2tt, uint32_t physical, void *virtual, uint32_t size )
{
  uint32_t va = 0xff000 & (uint32_t) virtual;

  // Writable by (and visible to) this core only, only in privileged modes.
  // XN off, because the vectors are in there.
  l2tt_entry entry = { .XN = 0, .small_page = 1, .B = 0, .C = 0, .AP = 1, .TEX = 0, .APX = 0, .S = 0, .nG = 0 };

  for (int i = 0; i < (size + 0xfff) >> 12; i++) {
    l2tt[(va >> 12) + i] = entry.raw | (physical + (i << 12));
  }
}

static void map_shared_work_area( uint32_t *l2tt, uint32_t physical, void *virtual, uint32_t size )
{
  uint32_t va = 0xff000 & (uint32_t) virtual;

  // Writable by (and visible to) this core only, only in privileged modes.
  l2tt_entry entry = { .XN = 1, .small_page = 1, .B = 0, .C = 0, .AP = 1, .TEX = 0, .APX = 0, .S = 1, .nG = 0 };

  for (int i = 0; i < (size + 0xfff) >> 12; i++) {
    l2tt[(va >> 12) + i] = entry.raw | (physical + (i << 12));
  }
}

void __attribute__(( noreturn, noinline )) MMU_enter( core_workspace *ws, volatile startup *startup )
{
  ws->mmu.l1tt_pa = (void*) pre_mmu_allocate_physical_memory( 16384, 16384, startup );
  ws->mmu.l2tt_pa = (void*) pre_mmu_allocate_physical_memory( 4096, 4096, startup );

  // OK, got all the memory we need, let the next core roll...
  BOOT_finished_allocating( ws->core_number, startup );

  for (int i = 0; i < 4096; i++) {
    ws->mmu.l1tt_pa[i] = 0;
  }

  for (int i = 0; i < 1024; i++) {
    ws->mmu.l2tt_pa[i] = 0;
  }

  uint32_t start = (uint32_t) &va_base;
  uint32_t physical;
  asm ( "adr %[loc], MMU_enter" : [loc] "=r" (physical) );
  physical = start - (((uint32_t) MMU_enter) - physical);

  // FIXME: permissions, caches, etc.
  l1tt_section_entry rom_sections = { .type2 = 2, .B = 0, .C = 0, .XN = 0, .Domain = 0, .P = 0, .AP = 3, .TEX = 0, .APX = 1, .S = 0, .nG = 0 };

  for (int i = 0; i < (uint32_t) &rom_size; i+= (1 << 20)) {
    ws->mmu.l1tt_pa[(start + i) >> 20] = rom_sections.raw | ((physical + i) & 0xfff00000);

    // Also where the code currently is...
    ws->mmu.l1tt_pa[(physical + i) >> 20] = rom_sections.raw | ((physical + i) & 0xfff00000);
  }

  l1tt_table_entry top_MiB = { .type1 = 1, .NS = 0, .Domain = 0 };

  ws->mmu.l1tt_pa[0xfff] = top_MiB.raw | (uint32_t) ws->mmu.l2tt_pa;

  map_work_area( ws->mmu.l2tt_pa, (uint32_t) ws, &workspace, sizeof( workspace ) );
  map_work_area( ws->mmu.l2tt_pa, (uint32_t) ws->mmu.l1tt_pa, L1TT, 4096 * 4 );
  map_work_area( ws->mmu.l2tt_pa, (uint32_t) ws->mmu.l2tt_pa, top_MiB_tt, 4096 );

  map_shared_work_area( ws->mmu.l2tt_pa, startup->shared_memory, &shared, sizeof( shared ) );

  // This version doesn't use TTBR1; there's enough memory in everything,
  // these days. (Any future 64-bit version should, though).
  asm ( "mcr p15, 0, %[ttbr0], c2, c0, 0" : : [ttbr0] "r" (ws->mmu.l1tt_pa) );
  asm ( "mcr p15, 0, %[dacr], c3, c0, 0" : : [dacr] "r" (1) ); // Only using Domain 0, at the moment, allow access.

  uint32_t tcr;

  // XP, bit 23, 1 = subpage AP bits disabled.
  // I, bit 12, 0 = instruction cache off (temporarily, assuming it was on, anyway)
  MODIFY_CP15_REG( "c1, c0, 0", (1 << 12) | (1 << 23), (1 << 23), tcr );

  // Turn on I, D cache, MMU, set SP, and call Kernel_start
  tcr |= (1 << 12) | (1 << 2) | (1 << 0);

  asm ( "  mcr p15, 0, %[tcr], c1, c0, 0" 
      "\n  mov sp, %[stack]"
      "\n  bx %[kernel]"
      :
      : [tcr] "r" (tcr)
      , [kernel] "r" (go_kernel)
      , [stack] "r" (sizeof( workspace.kernel.svc_stack ) + (uint32_t) &workspace.kernel.svc_stack) );

  __builtin_unreachable();
}

