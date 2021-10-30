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
static uint32_t *const bottom_MiB_tt = (&translation_tables) + 4096 + 256; // Offsets in words, not bytes!

typedef union {
  struct {
    uint32_t type2:2;
    uint32_t B:1;
    uint32_t C:1;
    uint32_t XN:1;
    uint32_t Domain:4;
    uint32_t P:1;
    uint32_t AF:1;
    uint32_t unprivileged_access:1;
    uint32_t TEX:3;
    uint32_t read_only:1;
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

// AP[2:1] access permissions model
// 
typedef union {
  struct {
    uint32_t XN:1; // If small_page == 1, else must be 1 for large page or 0 for no memory
    uint32_t small_page:1;
    uint32_t B:1;
    uint32_t C:1;
    uint32_t AF:1;
    uint32_t unprivileged_access:1;
    uint32_t TEX:3;
    uint32_t read_only:1;
    uint32_t S:1;
    uint32_t nG:1;
    uint32_t page_base:20;
  };
  uint32_t raw;
} l2tt_entry;

static const l2tt_entry l2_device = { .XN = 1, .small_page = 1, .B = 0, .C = 0, .TEX = 0, .unprivileged_access = 0, .read_only = 0, .AF = 1 };

// static const l2tt_entry l2_write_back_cached = { .TEX = 0b101, .C = 0, .B = 1 };

// User memory is always non-Global (associated with an ASID)

static const l2tt_entry l2_urwx = { .XN = 0, .small_page = 1, .TEX = 0b101, .C = 0, .B = 1, .unprivileged_access = 1, .AF = 1, .S = 0, .nG = 1 };
static const l2tt_entry l2_prwx = { .XN = 0, .small_page = 1, .TEX = 0b101, .C = 0, .B = 1, .unprivileged_access = 0, .AF = 1, .S = 0, .nG = 0 };
static const l2tt_entry l2_prw  = { .XN = 1, .small_page = 1, .TEX = 0b101, .C = 0, .B = 1, .unprivileged_access = 0, .AF = 1, .S = 0, .nG = 0 };

static const l1tt_section_entry l1_urwx = {
        .type2 = 2,
        .TEX = 0b101, .C = 0, .B = 1,
        .XN = 0, .Domain = 0, .P = 0,
        .AF = 1, // The MMU will not cause an exception the first time the memory is accessed
        .unprivileged_access = 1, .read_only = 0 };

static const l1tt_section_entry l1_prwx = {
        .type2 = 2,
        .TEX = 0b101, .C = 0, .B = 1,
        .XN = 0, .Domain = 0, .P = 0,
        .AF = 1, // The MMU will not cause an exception the first time the memory is accessed
        .unprivileged_access = 0, .read_only = 0 };

static const l1tt_section_entry l1_prw = {
        .type2 = 2,
        .TEX = 0b101, .C = 0, .B = 1,
        .XN = 0, .Domain = 0, .P = 0,
        .AF = 1, // The MMU will not cause an exception the first time the memory is accessed
        .unprivileged_access = 1, .read_only = 0, };

static const l1tt_section_entry l1_rom_section = {
        .type2 = 2,
        .TEX = 0b101, .C = 0, .B = 1,
        .XN = 0, .Domain = 0, .P = 0,
        .AF = 1, // The MMU will not cause an exception the first time the memory is accessed
        .unprivileged_access = 1, .read_only = 1, .S = 1, .nG = 0 };

static void __attribute__(( noreturn, noinline )) go_kernel()
{
  // Break before make
  about_to_remap_memory();
  for (int i = 0; i < 64; i++) {
    L1TT[i] = 0;
  }
  memory_remapped();

  l1tt_table_entry MiB = { .type1 = 1, .NS = 0, .Domain = 0 };

  about_to_remap_memory();
  L1TT[0] = MiB.raw | (1024 + (uint32_t) workspace.mmu.l2tt_pa);   // bottom_MiB_tt
  memory_remapped();

  Kernel_start();
}

// Map a privileged read-write page into the top 1MiB of virtual memory
// Might be better with pages instead of addresses
static void map_translation_table( uint32_t *l2tt, uint32_t physical, void *virtual, uint32_t size )
{
  uint32_t va = 0xff000 & (uint32_t) virtual;

  // Writable by (and visible to) this core only, only in privileged modes.
  // This must match the TTBR0 settings
  l2tt_entry entry = l2_prw;

  for (int i = 0; i < (size + 0xfff) >> 12; i++) {
    l2tt[(va >> 12) + i] = entry.raw | (physical + (i << 12));
  }
}

// Map a privileged read-write page into the top 1MiB of virtual memory
// Might be better with pages instead of addresses
static void map_work_area( uint32_t *l2tt, uint32_t physical, void *virtual, uint32_t size )
{
  uint32_t va = 0xff000 & (uint32_t) virtual;

  // Writable by (and visible to) this core only, only in privileged modes.
  // XN off for first page, because the vectors are in there, possibly FIQ code, as well.

  // Outer and Inner Write-Back, Read-Allocate Write-Allocate
  l2tt_entry entry = l2_prwx;

  for (int i = 0; i < (size + 0xfff) >> 12; i++) {
    l2tt[(va >> 12) + i] = entry.raw | (physical + (i << 12));
    entry = l2_prw;
  }
}

static void map_shared_work_area( uint32_t *l2tt, uint32_t physical, void *virtual, uint32_t size )
{
  uint32_t va = 0xff000 & (uint32_t) virtual;

  // Writable by (and visible to) all cores, only in privileged modes.
  l2tt_entry entry = l2_prw;
  entry.S = 1;

  for (int i = 0; i < (size + 0xfff) >> 12; i++) {
    l2tt[(va >> 12) + i] = entry.raw | (physical + (i << 12));
  }
}

static void map_at( void *va, uint32_t pa, uint32_t size, bool shared ) 
{
  uint32_t virt = (uint32_t) va;

  if (naturally_aligned( virt ) && naturally_aligned( pa ) && naturally_aligned( size )) {
    l1tt_section_entry entry = l1_urwx;
    entry.S = shared ? 1 : 0;

    while (size > 0) {
      L1TT[virt / natural_alignment] = entry.raw | pa;
      size -= natural_alignment;
      virt += natural_alignment;
      pa += natural_alignment;
    }
  }
  else if (size == 4096) {
    bool kernel_memory = ((uint32_t) va >= 0xfff00000);
    l2tt_entry entry = kernel_memory ? l2_prw : l2_urwx;

    entry.S = shared ? 1 : 0;

    entry.page_base = (pa >> 12);

    // FIXME: Obviously more areas of memory need finer granularity than just the
    // top and bottom megabytes.
    if ((uint32_t) va >= 0xfff00000) {
      top_MiB_tt[(virt & 0xff000)>>12] = entry.raw;
    }
    else if ((uint32_t) va < (1 << 20)) {
      bottom_MiB_tt[(virt & 0xff000)>>12] = entry.raw;
    }
    else {
      for (;;) { asm ( "wfi" ); }
    }
  }
  else {
    for (;;) { asm ( "wfi" ); }
  }

  memory_remapped();
}

void MMU_map_at( void *va, uint32_t pa, uint32_t size )
{
  map_at( va, pa, size, false );
}

void MMU_map_shared_at( void *va, uint32_t pa, uint32_t size )
{
  map_at( va, pa, size, true );
}

static void map_device_at( void *va, uint32_t pa, uint32_t size, bool shared )
{
  uint32_t virt = (uint32_t) va;
  if (size == 4096) {
    l2tt_entry entry = l2_device;
    entry.S = shared ? 1 : 0;

    entry.page_base = (pa >> 12);
    if ((uint32_t) va >= 0xfff00000) {
      top_MiB_tt[(virt & 0xff000)>>12] = entry.raw;
    }
    else if ((uint32_t) va < (1 << 20)) {
      bottom_MiB_tt[(virt & 0xff000)>>12] = entry.raw;
    }
    else {
      for (;;) { asm ( "wfi" ); }
    }
  }
  else {
    for (;;) { asm ( "wfi" ); }
  }

  memory_remapped();
}

void MMU_map_device_at( void *va, uint32_t pa, uint32_t size )
{
  map_device_at( va, pa, size, false );
}

void MMU_map_device_shared_at( void *va, uint32_t pa, uint32_t size )
{
  map_device_at( va, pa, size, true );
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
  l1tt_section_entry rom_sections = l1_rom_section;

  for (int i = 0; i < (uint32_t) &rom_size; i+= (1 << 20)) {
    ws->mmu.l1tt_pa[(start + i) >> 20] = rom_sections.raw | ((physical + i) & 0xfff00000);

    // Also where the code currently is...
    ws->mmu.l1tt_pa[(physical + i) >> 20] = rom_sections.raw | ((physical + i) & 0xfff00000);
  }

  about_to_remap_memory();

  l1tt_table_entry MiB = { .type1 = 1, .NS = 0, .Domain = 0 };

  ws->mmu.l1tt_pa[0xfff] = MiB.raw | (uint32_t) ws->mmu.l2tt_pa;        // top_MiB_tt

  memory_remapped();

  map_work_area( ws->mmu.l2tt_pa, (uint32_t) ws, &workspace, sizeof( workspace ) );
  map_translation_table( ws->mmu.l2tt_pa, (uint32_t) ws->mmu.l1tt_pa, L1TT, 4096 * 4 );
  map_translation_table( ws->mmu.l2tt_pa, (uint32_t) ws->mmu.l2tt_pa, top_MiB_tt, 4096 );

  map_shared_work_area( ws->mmu.l2tt_pa, startup->shared_memory, (void*) &shared, sizeof( shared ) );

  asm ( "  dsb sy" );

  // This version doesn't use TTBR1; there's enough memory in everything,
  // these days. (Any future 64-bit version should, though).
  asm ( "mcr p15, 0, %[ttbcr], c2, c0, 2" : : [ttbcr] "r" (0) );
  // 0x48 -> Inner and Outer write-back, write-allocate cacheable, not shared (per core tables)
  // This should match the settings in map_work_area
  asm ( "mcr p15, 0, %[ttbr0], c2, c0, 0" : : [ttbr0] "r" (0x48 | (uint32_t) ws->mmu.l1tt_pa) );
  asm ( "mcr p15, 0, %[dacr], c3, c0, 0" : : [dacr] "r" (1) ); // Only using Domain 0, at the moment, allow access.

  uint32_t sctlr;

  asm ( "  mrc p15, 0, %[sctlr], c1, c0, 0" : [sctlr] "=r" (sctlr) );

  sctlr |=  (1 << 23); // XP, bit 23, 1 = subpage AP bits disabled.
  sctlr |=  (1 << 29); // Access Flag enable
  sctlr &= ~(1 << 28); // No TEX remap (VMSAv6 functionality)
  sctlr |=  (1 << 13); // High vectors; there were problems with setting this bit independently, so do it here
  sctlr |=  (1 << 12); // Instruction cache
  sctlr |=  (1 <<  2); // Data cache - N.B. You cannot turn off cache here (for testing), locks will not work
  sctlr |=  (1 <<  0); // MMU Enable

  asm ( "  dsb sy"
      "\n  mcr p15, 0, %[sctlr], c1, c0, 0" 
      "\n  dsb"
      "\n  isb"
      "\n  mov sp, %[stack]"
      "\n  bx %[kernel]"
      :
      : [sctlr] "r" (sctlr)
      , [kernel] "r" (go_kernel) // Virtual (high memory) address
      , [stack] "r" (sizeof( workspace.kernel.svc_stack ) + (uint32_t) &workspace.kernel.svc_stack) );

  __builtin_unreachable();
}

extern void Kernel_failed_data_abort();

static void map_block( physical_memory_block block )
{
  // All RISC OS memory is RWX.
  l2tt_entry entry = l2_urwx;

  about_to_remap_memory();

  if (block.virtual_base < (1 << 20)) {
    // FIXME: more than one page!
    bottom_MiB_tt[(block.virtual_base >> 12) & 0xff] = (block.physical_base | entry.raw);
  }

  memory_remapped();
}

// Taken from swis.h, perhaps integer_context might be more appropriate?
// Or, for aborts, we don't really need to save all the registers, just the scratch registers
typedef struct __attribute__(( packed )) {
  uint32_t r[13];
  uint32_t lr;
  uint32_t spsr;
} svc_registers;

// noinline attribute is required so that stack space is allocated for any local variables.
static void __attribute__(( noinline )) handle_data_abort( svc_registers *regs )
{
  if (0x807 == data_fault_type() && workspace.mmu.current != 0) {
    uint32_t fa = fault_address();
    claim_lock( &shared.mmu.lock );
    physical_memory_block block = Kernel_physical_address( workspace.mmu.current, fa );
    release_lock( &shared.mmu.lock );
    if (block.size != 0) {
      map_block( block );
      return;
    }
  }
/*
  DynamicArea *da = shared.memory.dynamic_areas;

  MMU_map_shared_at( (void*) (da->virtual_page << 12), da->start_page << 12, da->pages << 12 );
*/
  asm volatile ( "ldm sp, { r0-r12, r14 }\n  b Kernel_failed_data_abort" );

  __builtin_unreachable();
}

void __attribute__(( naked, noreturn )) Kernel_default_data_abort()
{
  svc_registers *regs;
  // Without volatile, this may be optimised out because regs is not used.
  asm volatile ( 
        "  sub lr, lr, #8"
      "\n  srsdb sp!, #0x17"
      "\n  push { r0-r12 }"
      "\n  mov %[regs], sp"
      : [regs] "=r" (regs)
      );

  handle_data_abort( regs );

  asm volatile ( "pop { r0-r12 }"
    "\n  rfeia sp!" );

  __builtin_unreachable();
}

void MMU_switch_to( TaskSlot *slot )
{
  claim_lock( &shared.mmu.lock );
  workspace.mmu.current = slot;
  // Set CONTEXTIDR
  asm ( "mcr p15, 0, %[asid], c13, c0, 1" : : [asid] "r" (TaskSlot_asid( slot )) );
  // FIXME: clear out L2TTs, or disable walks until one is needed, then clear them
  release_lock( &shared.mmu.lock );
}

