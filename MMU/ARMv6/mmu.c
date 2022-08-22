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

#include "inkernel.h"

// 4GiB (32-bit) address range
// 16KiB -> 4096 (12 bits) of 1MiB sections
// 256 4KiB pages in 1MiB => L2TT is 256 words; 1024 bytes

// I could declare various constants, L1TT_size, etc., but I think that they would simply
// obscure what's going on. This code is very much tied to specific hardware that deals in
// bits, bytes, pages and megabytes.

// There will be one primary L1TT shared between all cores, entries can be copied from it
// in case of memory faults on shared virtual addresses.
// Each other core will maintain a similar L1TT, but with the core- and 
// application-specific memory areas differing.
// There will be a cache of L2TTs for cores to use.

extern int va_base;
extern int rom_size;

extern int app_memory_limit;

/*
Both level 1 and 2 translation tables, if an entry has the least significant
two bits both zero, the rest of the entry is ignored by the MMU (and attempts
to access that memory raises an exception).

That means any invalid entry may be a word-aligned pointer to something.

Maybe even a routine to handle the problem?

A configuration flag (where?) can be set to raise an exception on any TLB miss.

This means that switching away from a particular TaskSlot (ASID) can be performed
in two steps. Set the flag when moving away from that TaskSlot, then if the next
TLB miss is not in that slot, clear out the translation table and reset the flag.

Some code might like to configure a virtual memory range to be some combination
of, for example:

* Global/Local
* Expand as needed (kernel allocates new memory)
* Page, Section or Large Page mappable
* Executable

Actions:
  Check global tables (default)
  Check TaskSlot (0x8000..0x1fffffff, plus maybe private DAs)
  Allocate kernel memory and zero whole page
  Allocate kernel memory and zero first eight bytes
  Allocate kernel memory and initialise as...

The downside of this is that we need virtual addresses for the translation
tables, to update them.
Solution: a TaskSlot to manage memory.
Solution: a shared memory area with used and unused L2TTs
*/

typedef struct {
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
} l1tt_section_entry;

typedef struct {
  uint32_t type1:2;
  uint32_t SBZ1:1;
  uint32_t NS:1;
  uint32_t SBZ2:1;
  uint32_t Domain:4;
  uint32_t P:1;
  uint32_t page_table_base:22;
} l1tt_table_entry;

typedef bool (*fault_handler)( uint32_t address, uint32_t type );

typedef union l1tt_entry {
  uint32_t raw;
  uint32_t type:2; // 0 = handler, 1 = Page table, 2 = Section (or supersection), executable, 3 = S PXN
  l1tt_table_entry table;
  l1tt_section_entry section;
  fault_handler handler;
} l1tt_entry;

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
  uint32_t type:2; // 0 = handler, 1 = large page, 2 = small executable page, 3 = small data page
  bool (*handler)( uint32_t address, uint32_t type );
} l2tt_entry;

typedef struct Level_one_translation_table {
  l1tt_entry entry[4096];
} Level_one_translation_table; // One entry per MiB

typedef struct Level_two_translation_table {
  l2tt_entry entry[256];
} Level_two_translation_table; // One entry per 4KiB, for 1MiB

extern Level_one_translation_table l1_translation_tables[2];
extern Level_two_translation_table l2_translation_tables[];

static Level_one_translation_table *const Global_L1TT = &l1_translation_tables[0];
static Level_one_translation_table *const Local_L1TT = &l1_translation_tables[1];
static Level_two_translation_table *const L2TTs = l2_translation_tables;

typedef union {
  void *rawp;
  uint32_t raw;
  struct {
    uint32_t offset:12;
    uint32_t page:8;
    uint32_t section:12;
  };
  struct {
    uint32_t section_offset:20;
  };
} arm32_ptr;

static Level_two_translation_table *find_table_from_l1tt_entry( l1tt_entry l1 )
{
  assert( l1.type == 1 );

  // TODO this can be speeded up if the memory is allocated in 64k chunks, etc.

  // Global L2TT consists of page entries for the top MiB.
  uint32_t physical_table_page = l1.table.page_table_base >> 2;

  Level_two_translation_table *l2tt = L2TTs; // The global level 2 TT FIXME Do we know this?

  int l2start = (((uint32_t) L2TTs) >> 12) & 0xff;
  int i = l2start;
  while (l2tt->entry[i].type == 3 // TODO support large data pages
      && l2tt->entry[i].page_base != physical_table_page) {
    i++;
  }
  i -= l2start;

  // Four tables per page
  return l2tt + (i << 2) + (l1.table.page_table_base & 3);
}

static bool fault_on_existing_section( uint32_t address, uint32_t type )
{
  uint32_t *p;
  asm ( "push { r4-r11 }\n  mov %[p], sp" : [p] "=r" (p) );
  Write0( "Fault on existing section, " ); WriteNum( address ); Space; WriteNum( type ); NewLine;
  for (int i = 0; i < 32; i++) { WriteNum( p[i] ); if (0 == (i & 3)) NewLine; else Space; }
  asm ( "bkpt 1" );
  return false;
}

static fault_handler find_handler( uint32_t fa )
{
  arm32_ptr pointer = { .raw = fa };

  l1tt_entry l1 = Local_L1TT->entry[pointer.section];

  if (l1.type == 0) {
    return Local_L1TT->entry[pointer.section].handler;
  }

  if (l1.type == 1) {
    Level_two_translation_table *l2 = find_table_from_l1tt_entry( l1 );

    l2tt_entry l2_entry = l2->entry[pointer.page];

    assert( l2_entry.type == 0 ); // Otherwise we wouldn't be here, right?

    return l2_entry.handler;
  }

  return fault_on_existing_section;
}

static uint32_t physical_address( void *p )
{
  arm32_ptr pointer = { .rawp = p };

  l1tt_entry l1 = Local_L1TT->entry[pointer.section];
  switch (l1.type) {
  case 1: // Table
    {
      Level_two_translation_table *l2 = find_table_from_l1tt_entry( l1 );

      l2tt_entry l2_entry = l2->entry[pointer.page];

      assert( l2_entry.type != 0 ); // Only used for mapped memory

      return (l2_entry.page_base << 12) + pointer.offset;
    }
    break;
  case 2:
  case 3:
    return (l1.section.section_base << 20) + pointer.section_offset;
    break;
  }

  asm ( "bkpt 1" );

  return -1;
}

static void map_l2tt_at_section_local( Level_two_translation_table *l2tt, uint32_t section )
{
  l1tt_entry MiB = { .table.type1 = 1, .table.NS = 1, .table.Domain = 0 };

  MiB.raw |= physical_address( l2tt );

  Local_L1TT->entry[section] = MiB;
}

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

uint32_t code_physical_start()
{
  uint32_t start = (uint32_t) &va_base;
  uint32_t physical;
  asm ( "adr %[loc], code_physical_start" : [loc] "=r" (physical) );
  return start - (((uint32_t) code_physical_start) - physical);
}

// Map a privileged read-write page into the top 1MiB of virtual memory
// Might be better with pages instead of addresses
static void map_translation_table( Level_two_translation_table *l2tt, Level_one_translation_table *physical, void *virtual )
{
  uint32_t va = 0xff000 & (uint32_t) virtual;
  uint32_t phys = (uint32_t) physical;

  // Writable by (and visible to) this core only, only in privileged modes.
  // This must match the TTBR0 settings
  l2tt_entry entry = l2_prw;

  for (int i = 0; i < (sizeof( Level_one_translation_table ) >> 12); i++) {
    l2tt->entry[(va >> 12) + i].raw = entry.raw | (phys + (i << 12));
  }
}

static void map_global_l2tt( Level_two_translation_table *l2tt )
{
  uint32_t va = 0xff000 & (uint32_t) L2TTs;
  uint32_t phys = (uint32_t) l2tt;

  l2tt_entry entry = l2_prw;

  for (int i = 0; i < 2; i++) {
    l2tt->entry[(va >> 12) + i].raw = entry.raw | (phys + (i << 12));
  }
}

// Map a privileged read-write page into the top 1MiB of virtual memory
// Might be better with pages instead of addresses
// Should be readable in usr32, but the new access permissions don't allow
// for it. I hope it's not critical, or we have to make it all r/w! (Better
// to handle read requests in exceptions to return the current values.)
void __attribute__(( optimize (0) )) map_work_area( Level_two_translation_table *l2tt, struct core_workspace *physical )
{
  struct core_workspace *virtual = &workspace;
  uint32_t va = 0xff000 & (uint32_t) virtual;
  uint32_t size = (sizeof( struct core_workspace ) + 0xfff) & ~0xfff;
  uint32_t phys = (uint32_t) physical;

  // Writable by (and visible to) this core only, only in privileged modes.
  // XN off for first page, because the vectors are in there, possibly FIQ code, as well.

  // Outer and Inner Write-Back, Read-Allocate Write-Allocate
  l2tt_entry entry = l2_prwx;

  for (int i = 0; i < size >> 12; i++) {
    l2tt->entry[(va >> 12) + i].raw = entry.raw | (phys + (i << 12));
    entry = l2_prw; // Just the first page is executable (for the hardware vectors)
  }
}

static void map_shared_work_area( Level_two_translation_table *l2tt, shared_workspace *physical )
{
  uint32_t virtual = (uint32_t) &shared;
  uint32_t va = 0xff000 & (uint32_t) virtual;
  uint32_t phys = (uint32_t) physical;
  uint32_t size = (sizeof( shared_workspace ) + 0xfff) & ~0xfff;

  // Writable by (and visible to) all cores, only in privileged modes.
  l2tt_entry entry = l2_prw;
  entry.S = 1;

  for (int i = 0; i < size >> 12; i++) {
    l2tt->entry[(va >> 12) + i].raw = entry.raw | (phys + (i << 12));
  }
}

static bool free_l2tt_table( uint32_t address, uint32_t type )
{
  // Marker, will not be called (because it won't be in an active L2TT
  asm ( "bkpt 1" );
  return false;
}

static bool last_free_l2tt_table( uint32_t address, uint32_t type )
{
  // Marker, will not be called (because it won't be in an active L2TT
  asm ( "bkpt 1" );
  return false;
}

static bool just_allocated( uint32_t address, uint32_t type )
{
  asm ( "bkpt 1" );
  return false;
}

static Level_two_translation_table *find_free_table()
{
  Level_two_translation_table *l2tt = l2_translation_tables;

  do {
    while (l2tt->entry[0].handler != free_l2tt_table
     && l2tt->entry[0].handler != last_free_l2tt_table) { // FIXME: no more available?
      l2tt++;
    }

if (l2tt->entry[0].handler == last_free_l2tt_table) { asm ( "bkpt 4" ); }

  } while (free_l2tt_table != change_word_if_equal( &l2tt->entry[0].raw, (uint32_t) free_l2tt_table, (uint32_t) just_allocated ));

  return l2tt;
}

static bool never_happens( uint32_t address, uint32_t type )
{
  // Marker, will not be called (because it won't be in an active L2TT
  asm ( "bkpt 1" );
  return false;
}

static void map_block( physical_memory_block block )
{
  // All RISC OS memory is RWX.
  // FIXME: Even the stuff that isn't meant to be at the moment... Lowest common denominator
  // All lazily mapped memory is shared (task slots, and the associated storage in the kernel)
  l2tt_entry entry = { .XN = 0, .small_page = 1, .TEX = 0b101, .C = 0, .B = 1, .unprivileged_access = 1, .AF = 1, .S = 1, .nG = 1 };

  arm32_ptr pointer = { .raw = block.virtual_base };

  l1tt_entry section = Local_L1TT->entry[pointer.section];

  Level_two_translation_table *l2tt = find_table_from_l1tt_entry( section );

  about_to_remap_memory();

  WriteS( "Mapping" ); NewLine;
  uint32_t base = (block.virtual_base >> 12) & 0xff;
  entry.page_base = block.physical_base >> 12;

  // FIXME: What if block overruns the end of the table?

  for (uint32_t b = 0; b < block.size >> 12; b++) {
    WriteS( "Page " ); WriteNum( (b+base) << 12 ); WriteS( " > " ); WriteNum( entry.page_base << 12 ); NewLine;
    l2tt->entry[base + b] = entry;
    entry.page_base++;
  }

  memory_remapped();
}

static bool check_task_slot_l1( uint32_t address, uint32_t type )
{
  asm ( "bkpt 1" );
  return false;
}

static bool check_task_slot_l2( uint32_t address, uint32_t type )
{
  claim_lock( &shared.mmu.lock );
  physical_memory_block block = Kernel_physical_address( address );
  release_lock( &shared.mmu.lock );

  if (block.size != 0) {
    map_block( block );
    return true;
  }

  asm ( "bkpt 1" );
  return false;
}

static bool allocate_core_specific_zero_page_ram( uint32_t address, uint32_t type )
{
  assert( address < (1 << 20) );
  assert( type == 0x807 ); // From experience, not necessarily always the case

  asm ( "bkpt 1" );
  return false;
}

static void initialise_l2tt_for_section( Level_two_translation_table *l2tt, int section )
{
  if (section == 0) {
    for (int i = 0; i < 8; i++) {
      l2tt->entry[i].handler = allocate_core_specific_zero_page_ram;
    }
    for (int i = 8; i < number_of( l2tt->entry ); i++) {
      l2tt->entry[i].handler = check_task_slot_l2;
    }
  }
  else if (section < (((uint32_t)&app_memory_limit) >> 20)) {
    for (int i = 0; i < number_of( l2tt->entry ); i++) {
      l2tt->entry[i].handler = check_task_slot_l2;
    }
  }
  else {
    for (int i = 0; i < number_of( l2tt->entry ); i++) {
      l2tt->entry[i].handler = never_happens;
    }
  }
}

static bool allocate_core_specific_zero_section( uint32_t address, uint32_t type )
{
  assert( address < (1 << 20) );
  assert( type == 0x805 ); // From experience, not necessarily always the case
  assert( workspace.mmu.zero_page_l2tt == 0 );

  // One-shot per core, claims a L2TT for the bottom MiB of RAM and initialises it.
  Level_two_translation_table *l2tt = find_free_table();

  initialise_l2tt_for_section( l2tt, 0 );

  map_l2tt_at_section_local( l2tt, 0 );
  workspace.mmu.zero_page_l2tt = l2tt;

  return true;
}

static bool check_global_l1tt( uint32_t address, uint32_t type )
{
  asm ( "bkpt 1" );
  return false;
}

static bool allocate_memory_for_SharedCLib( uint32_t address, uint32_t type )
{
  // For SharedCLib, see https://www.riscosopen.org/forum/forums/9/topics/16166?page=5
  arm32_ptr pointer = { .raw = address };

  assert( pointer.section == 0xfff );
  assert( pointer.page == 0 );
  assert( workspace.mmu.kernel_l2tt->entry[0].handler == allocate_memory_for_SharedCLib );

  workspace.mmu.kernel_l2tt->entry[0] = l2_prw;
  workspace.mmu.kernel_l2tt->entry[0].raw |= Kernel_allocate_pages( 4096, 4096 );

  return true;
}

static bool check_global_l2tt( uint32_t address, uint32_t type )
{
#ifdef DEBUG__BREAK_ON_UNEXPECTED_FAULT
  uint32_t *p;
  asm ( "push { r4-r11 }\n  mov %[p], sp" : [p] "=r" (p) );
  Write0( "Check global l2tt, " ); WriteNum( address ); Space; WriteNum( type ); NewLine;
  for (int i = 0; i < 32; i++) { WriteNum( p[i] ); if (0 == (i & 3)) NewLine; else Space; }
  asm ( "bkpt 1" );
#endif

  arm32_ptr pointer = { .raw = address };

  assert( pointer.section == 0xfff );

  l2tt_entry global = shared.mmu.kernel_l2tt->entry[pointer.page];
  workspace.mmu.kernel_l2tt->entry[pointer.page] = global;

  assert( global.handler != check_global_l2tt );

  return true;
}

static l1tt_entry default_l1tt_entry( int section )
{
  l1tt_entry result;
  if (section == 0)
    result.handler = allocate_core_specific_zero_section;
  else if (section < (((uint32_t)&app_memory_limit) >> 20))
    result.handler = check_task_slot_l1;
  else if (section == 0xfff)
    result.handler = never_happens; // Overwritten almost immediately.
  else
    result.handler = check_global_l1tt;
  return result;
}

// No locking, MMU not yet enabled
static Level_two_translation_table *find_free_table_pre_mmu( struct MMU_shared_workspace *shared )
{
  Level_two_translation_table *l2tt = shared->physical_l2tts;

  assert( l2tt != 0 );

  while (l2tt->entry[0].handler != free_l2tt_table
   && l2tt->entry[0].handler != last_free_l2tt_table) { // FIXME: no more available?
    l2tt++;
  }

if (l2tt->entry[0].handler == last_free_l2tt_table) { asm ( "bkpt 4" ); }

  // Not really needed pre-MMU, but a reminder to do the same with STREX later.
  l2tt->entry[0].handler = just_allocated;

  return l2tt;
}

void setup_global_translation_tables( volatile startup *startup )
{
  shared_workspace volatile *shared_memory = (void*) startup->shared_memory;
  struct MMU_shared_workspace *shared = &shared_memory->mmu;

  // These areas must be set up before enabling the MMU:
  //  Sections covering the OS code at its current physical address
  //  Sections mapping the OS code to its final virtual address (va_base from rool.script)
  //  Page table at the top megabyte especially for the hardware vectors at 0xffff8000 and
  //    for virtual access to the page tables.

  // The first of those will be removed almost immediately.

  // Reminder: all pointers are physical.

  // Space for 64 level 2 translation tables (enough to start dozens of cores)
  static const int initial_tables = 16; // 64;
  static const int size = initial_tables * sizeof( Level_two_translation_table );

  // pre_mmu_allocate_physical_memory doesn't support anything but 4k and 1m boundaries atm. FIXME
  shared->physical_l2tts = (void*) pre_mmu_allocate_physical_memory( size, 4 << 10, startup );
if (shared->physical_l2tts == 0) asm ( "bkpt 9" );

  // FIXME: Allocate on 64k boundary and mark it as a Large Page G4-4866 
  //  shared->physical_l2tts = (void*) pre_mmu_allocate_physical_memory( size, 64 << 10, startup );

  Level_one_translation_table *l1tt = shared->global_l1tt;
  Level_two_translation_table *l2tt = shared->physical_l2tts;

  for (int i = 0; i < initial_tables; i++) {
    l2tt[i].entry[0].handler = free_l2tt_table;
    //l2tt[i].entry[1].handler = &l2tt[i << 10]; // Physical address of free table
    // The rest of the table is left uninitialised, it will be cleared before use.
  }

  l2tt[initial_tables - 1].entry[0].handler = last_free_l2tt_table;

  for (int i = 0; i < number_of( l1tt->entry ); i++) {
    l1tt->entry[i] = default_l1tt_entry( i );
  }

  Level_two_translation_table *high_table = find_free_table_pre_mmu( shared );

  l1tt_entry MiB = { .table.type1 = 1, .table.NS = 0, .table.Domain = 0 };

  MiB.raw |= (uint32_t) high_table;
  l1tt->entry[0xfff] = MiB; // Top MiB page-addressable

  high_table->entry[0].handler = allocate_memory_for_SharedCLib;

  for (int i = 1; i < number_of( high_table->entry ); i++) {
    high_table->entry[i].handler = check_global_l2tt;
  }

  // Map the global translation tables for all to share
  map_translation_table( high_table, l1tt, Global_L1TT );
  map_global_l2tt( high_table );

  shared->global_l2tt = high_table;
  shared->kernel_l2tt = &L2TTs[high_table - shared->physical_l2tts];
}

static void __attribute__(( noreturn, noinline )) go_kernel()
{
  // Break before make
  about_to_remap_memory();

  // Remove the mapping for virtual == physical address for the ROM
  // We are running in virtual memory now, so adr and function addresses will match

  uint32_t rom = ((uint32_t) &va_base) >> 20;
  uint32_t size = ((uint32_t) &rom_size) >> 20;
  uint32_t i = 0;

  while (Local_L1TT->entry[i].raw != Local_L1TT->entry[rom].raw) {
    i++;
  }
  if (i == rom) asm ( "bkpt 2" );

  while (Local_L1TT->entry[i].raw == Local_L1TT->entry[rom].raw) {
    Local_L1TT->entry[i] = default_l1tt_entry( i );
    i++;
    rom++;
  }

  memory_remapped();

  Kernel_start();
}

void __attribute__(( noreturn, noinline )) MMU_enter( core_workspace *ws, startup *startup )
{
if (ws->core_number >= 1) {
  BOOT_finished_allocating( ws->core_number, startup );
  for (;;) {}
}
  shared_workspace *shared_memory = (void*) startup->shared_memory;
  struct MMU_shared_workspace *shared = &shared_memory->mmu;

  Level_one_translation_table *l1tt = (void*) pre_mmu_allocate_physical_memory( 16384, 16384, startup );

  Level_two_translation_table *l2tt;

  if (shared->global_l1tt == 0) {
    // First core in
    shared->global_l1tt = l1tt;
    setup_global_translation_tables( startup );
    l2tt = shared->global_l2tt;
  }
  else {
    Level_one_translation_table *global_l1 = shared->global_l1tt;

    assert (l1tt != global_l1);

    *l1tt = *global_l1; // Copy whole table

    l2tt = find_free_table_pre_mmu( shared );

    Level_two_translation_table *global_l2 = shared->global_l2tt;

    *l2tt = *global_l2; // Copy whole table
  }

  // OK, got all the resources we need, let the next core roll...
  BOOT_finished_allocating( ws->core_number, startup );


  // The global L1TT refers to the global L2TT for the top MiB, we need our own, instead.
  l1tt_entry MiB = { .table.type1 = 1, .table.NS = 0, .table.Domain = 0 };

  MiB.raw |= (uint32_t) l2tt;

  l1tt->entry[0xfff] = MiB;

  uint32_t start = (uint32_t) &va_base;
  uint32_t physical = code_physical_start();

  // FIXME: permissions, caches, etc.
  l1tt_entry rom_sections = { .section = l1_rom_section };

  for (int i = 0; i < (uint32_t) &rom_size; i+= (1 << 20)) {
    l1tt->entry[(start + i) >> 20].raw = rom_sections.raw | ((physical + i) & 0xfff00000);

    // Also where the code currently is...
    l1tt->entry[(physical + i) >> 20].raw = rom_sections.raw | ((physical + i) & 0xfff00000);
  }

  // Our core-specific work areas, in our core-specific L2TT
  map_work_area( l2tt, ws );
  map_translation_table( l2tt, l1tt, Local_L1TT );
  map_shared_work_area( l2tt, shared_memory );

  ws->mmu.kernel_l2tt = &L2TTs[l2tt - shared->physical_l2tts];

if (0 && ws->core_number == 0) {
  // This breaks RISC_OSLib; it accesses the page at 0xfff00000
  l2tt->entry[0] = l2_device;
  l2tt->entry[0].raw |= 0x3f200000;

  // Pre MMU:
  uint32_t volatile *gpio = 0x3f200000;
  gpio[2] = (gpio[2] & ~(3 << 6)) | (1 << 6);
  asm volatile ( "dsb" );
  //gpio[0x28/4] = (1 << 22); // Clr
  gpio[0x1c/4] = (1 << 22); // Set
  asm volatile ( "dsb" );

  for (int n = 0; n < 3; n++) {
  for (int i = 0; i < 0x100000; i++) asm ( "" );
  gpio[0x28/4] = (1 << 22); // Clr
  asm volatile ( "dsb" );
  for (int i = 0; i < 0x100000; i++) asm ( "" );
  gpio[0x1c/4] = (1 << 22); // Set
  asm volatile ( "dsb" );
  }
}

  asm ( "  dsb sy" );

  // This version doesn't use TTBR1; there's enough memory in everything,
  // these days. (Any future 64-bit version should, though).
  asm ( "mcr p15, 0, %[ttbcr], c2, c0, 2" : : [ttbcr] "r" (0) );
  // 0x48 -> Inner and Outer write-back, write-allocate cacheable, not shared (per core tables)
  // This should match the settings in map_work_area
  asm ( "mcr p15, 0, %[ttbr0], c2, c0, 0" : : [ttbr0] "r" (0x48 | (uint32_t) l1tt) );
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

#if 0
static void map_block( physical_memory_block block )
{
  // All RISC OS memory is RWX.
  // FIXME: Even the stuff that isn't meant to be at the moment... Lowest common denominator
  // All lazily mapped memory is shared (task slots, and the associated storage in the kernel)
  l2tt_entry entry = { .XN = 0, .small_page = 1, .TEX = 0b101, .C = 0, .B = 1, .unprivileged_access = 1, .AF = 1, .S = 1, .nG = 1 };

  about_to_remap_memory();

  if (block.virtual_base < (1 << 20)) {
    WriteS( "Mapping" ); NewLine;
    uint32_t base = block.virtual_base >> 12;
    for (uint32_t b = 0; b < block.size >> 12; b++) {
      WriteS( "Page " ); WriteNum( (b+base) << 12 ); WriteS( " > " ); WriteNum( block.physical_base + (b << 12) ); NewLine;
      bottom_MiB_tt[base + b] = (block.physical_base + (b << 12)) | entry.raw;
    }
  }

  memory_remapped();
}

// noinline attribute is required so that stack space is allocated for any local variables.
// There is no need for this routine to examine the fault generating instruction or the registers.
static bool __attribute__(( noinline )) handle_data_abort()
{
  uint32_t fa = fault_address();

  uint32_t fault_type = data_fault_type() & ~0x800; // Don't care if read or write

  // Maybe I should care, but permission faults are different...
  if (5 == fault_type || 7 == fault_type) {
WriteS( "Translation fault: va = " ); WriteNum( fa ); NewLine;
uint32_t *p;
asm ( "mov %[p], sp" : [p] "=r" (p) );
for (int i = 15; i < 17; i++) { // Should be faulting instruction and spsr
  WriteNum( p[i] ); Write0( " " );
}
NewLine;
    claim_lock( &shared.mmu.lock );
    physical_memory_block block = Kernel_physical_address( fa );
    release_lock( &shared.mmu.lock );
WriteS( "Data abort physical: " ); WriteNum( block.physical_base ); NewLine;
    if (block.size != 0) {
      map_block( block );
      return true;
    }
  }

/*
  DynamicArea *da = shared.memory.dynamic_areas;

  MMU_map_shared_at( (void*) (da->virtual_page << 12), da->start_page << 12, da->pages << 12 );
*/

  return false;
}
#endif

// noinline attribute is required so that stack space is allocated for any local variables.
// There is no need for this routine to examine the fault generating instruction or the registers.
static bool __attribute__(( noinline )) handle_data_abort()
{
  uint32_t fa = fault_address();
  uint32_t ft = data_fault_type();

  fault_handler handler = find_handler( fa );

  return handler( fa, ft );
}

void __attribute__(( naked, optimize( 0 ), noreturn )) Kernel_default_data_abort()
{
  asm volatile ( 
        "  sub lr, lr, #8"
      "\n  srsdb sp!, #0x17 // Store return address and SPSR"
      "\n  push { "C_CLOBBERED" }"
      );

  if (!handle_data_abort()) {
    // Put the important information somewhere the developer can see it
    register uint32_t fa asm ( "r8" ) = fault_address();
    register uint32_t fault_type asm ( "r9" ) = data_fault_type();
    asm volatile ( "pop { "C_CLOBBERED" }"
               "\n  pop { r0, r1 } // Fault instruction and processor mode"
               "\n  b Kernel_failed_data_abort"
               :
               : "r" (fa)
               , "r" (fault_type) );
  }

  asm volatile ( "pop { "C_CLOBBERED" }"
    "\n  rfeia sp! // Restore execution and SPSR" );

  __builtin_unreachable();
}

void MMU_switch_to( TaskSlot *slot )
{
  claim_lock( &shared.mmu.lock );
  Write0( "Switching to slot " ); WriteNum( slot ); Space; WriteNum( TaskSlot_asid( slot ) ); NewLine;
  // FIXME Only clear what's used
  // FIXME deal with slots that go over the first MiB
  // Note: My idea is to try to keep memory as contiguous as possible, and have
  // two or possibly three sub-MiB translation tables for the first MiB (bottom_MiB_tt)
  // and the slot's top MiB (and possibly the one below it, in case a task regularly
  // modifies its memory by small amounts above and below a MiB boundary).
  for (int i = 0x8000 >> 12; i < 0x100000 >> 12; i++) {
    //bottom_MiB_tt[i] = 0; // FIXME: reproduce  check_task_slot_l2
  }
  // Set CONTEXTIDR
  asm ( "mcr p15, 0, %[asid], c13, c0, 1" : : [asid] "r" (TaskSlot_asid( slot )) );
  // FIXME: clear out L2TTs, or disable walks until one is needed, then clear them
  release_lock( &shared.mmu.lock );

  // This appears to be necessary. Perhaps it should be in MMU_switch_to.
  clean_cache_to_PoC();
}

static void map_at( void *va, uint32_t pa, uint32_t size, bool shared ) 
{
  arm32_ptr pointer = { .rawp = va };
  uint32_t section = pointer.section;

  Level_one_translation_table *l1tt = shared ? Global_L1TT : Local_L1TT;

  if (naturally_aligned( pointer.raw ) && naturally_aligned( pa ) && naturally_aligned( size )) {
    l1tt_entry entry = { .section = l1_urwx };
    entry.section.S = shared ? 1 : 0;

    while (size > 0) {
      l1tt->entry[section].raw = (entry.raw | pa);
      if (shared) Local_L1TT->entry[section].raw = (entry.raw | pa);
      size -= natural_alignment;
      section++;
      pa += natural_alignment;
    }
  }
  else if (size == 4096) {
    bool kernel_memory = ((uint32_t) va >= 0xfff00000);
    l2tt_entry entry = kernel_memory ? l2_prw : l2_urwx;

    entry.S = shared ? 1 : 0;

    entry.page_base = (pa >> 12);

    Level_two_translation_table *l2tt;

    switch (l1tt->entry[section].type) {
    case 0: // Unused
      {
        l2tt = find_free_table();

        initialise_l2tt_for_section( l2tt, pointer.section );

        l1tt_entry MiB = { .table.type1 = 1, .table.NS = shared ? 1 : 0, .table.Domain = 0 };

        MiB.raw |= physical_address( l2tt );

        l1tt->entry[section] = MiB;
        if (shared) Local_L1TT->entry[section] = MiB;
      }
      break;
    case 1: // Existing table
      l2tt = find_table_from_l1tt_entry( l1tt->entry[section] );
      break;
    default: // Address already allocated to a MiB section (or supersection)
      asm ( "bkpt 7" : : [x] "r" (l1tt->entry[section]) );
    }

    l2tt_entry old = l2tt->entry[pointer.page];
    if (old.type == 0) {
      l2tt_entry current;
      current.raw = change_word_if_equal( &l2tt->entry[pointer.page].raw, old.raw, entry.raw );
      if (old.raw != current.raw) {
        asm ( "bkpt 8" ); // Beaten to it by another core
      }
    }
    else if (old.raw != entry.raw)
      asm ( "bkpt 9" ); // Beaten to it by another core, which wrote something else
  }
  else {
#if 0
    Write0( __func__ ); Space; WriteNum( va ); Space; WriteNum( pa ); Space; WriteNum( size ); Space; Write0( shared ? " shared" : " not shared" ); NewLine;
#endif
    for (;;) { asm ( "bkpt 102" ); }
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

void MMU_map_device_at( void *va, uint32_t pa, uint32_t size )
{
  arm32_ptr pointer = { .rawp = va };

  if (size == 4096 && pointer.section == 0xfff) { // FIXME: other areas too?
    l2tt_entry entry = l2_device;

    entry.S = 1;

    entry.page_base = (pa >> 12);

    l2tt_entry *shared_entry = &shared.mmu.kernel_l2tt->entry[pointer.page];
    l2tt_entry old = *shared_entry;
    if (old.type == 0) {
      l2tt_entry current;
      current.raw = change_word_if_equal( &shared_entry->raw, old.raw, entry.raw );
      if (old.raw != current.raw) {
        asm ( "bkpt 8" ); // Beaten to it by another core
      }
    }
    else if (old.raw != entry.raw)
      asm ( "bkpt 90" ); // Beaten to it by another core, which wrote something else

    shared.mmu.kernel_l2tt->entry[pointer.page] = entry;
  }
  else {
    for (;;) { asm ( "bkpt 10" ); }
  }

  memory_remapped();
}

