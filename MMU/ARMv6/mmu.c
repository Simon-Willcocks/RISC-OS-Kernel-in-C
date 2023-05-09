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
  uint32_t physical_table_page = l1.table.page_table_base >> 2; // Page containing table

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
  asm ( "bkpt %[line]" : : [line] "i" (__LINE__) );
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

  asm ( "bkpt %[line]" : : [line] "i" (__LINE__) );

  return -1;
}

static void map_l2tt_at_section_local( Level_two_translation_table *l2tt, uint32_t section )
{
  l1tt_entry MiB = { .table.type1 = 1, .table.NS = 1, .table.Domain = 0 };

  MiB.raw |= physical_address( l2tt );

  Local_L1TT->entry[section] = MiB;
}

// FIXME Probably want either privileged and global or user and slot-specific.
static const l2tt_entry l2_device = { .XN = 1, .small_page = 1, .B = 0, .C = 0, .TEX = 0, .unprivileged_access = 1, .read_only = 0, .AF = 1 };

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

/*
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
*/
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

// Map a privileged read-write page into the top 1MiB of virtual memory
// Might be better with pages instead of addresses
// Should be readable in usr32, but the new access permissions don't allow
// for it. I hope it's not critical, or we have to make it all r/w! (Better
// to handle read requests in exceptions to return the current values.)
void map_work_area( Level_two_translation_table *l2tt, struct core_workspace *physical )
{
  struct core_workspace *virtual = &workspace;
  uint32_t va = 0xff000 & (uint32_t) virtual;
  uint32_t size_in_pages = (sizeof( struct core_workspace ) + 0xfff) >> 12;
  uint32_t phys = (uint32_t) physical;

  // Writable by (and visible to) this core only, only in privileged modes.
  // XN off for first page, because the vectors are in there, possibly FIQ code, as well.

  // Outer and Inner Write-Back, Read-Allocate Write-Allocate
  l2tt_entry entry = l2_prwx;

  for (int i = 0; i < size_in_pages; i++) {
    l2tt->entry[(va >> 12) + i].raw = entry.raw | (phys + (i << 12));
    entry = l2_prw; // Just the first page is executable (for the hardware vectors & fiq code)
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
  asm ( "bkpt %[line]" : : [line] "i" (__LINE__) );
  return false;
}

static bool last_free_l2tt_table( uint32_t address, uint32_t type )
{
  // Marker, will not be called (because it won't be in an active L2TT
  asm ( "bkpt %[line]" : : [line] "i" (__LINE__) );
  return false;
}

static bool just_allocated( uint32_t address, uint32_t type )
{
  asm ( "bkpt %[line]" : : [line] "i" (__LINE__) );
  return false;
}

static Level_two_translation_table *find_free_table()
{
  Level_two_translation_table *l2tt = l2_translation_tables;
  l2tt_entry old_value;
  l2tt_entry just_allocated_entry = { .handler = just_allocated };
  l2tt_entry free_entry = { .handler = free_l2tt_table };

  do {
    while (l2tt->entry[0].handler != free_l2tt_table
     && l2tt->entry[0].handler != last_free_l2tt_table) { // FIXME: no more available?
      l2tt++;
    }

if (l2tt->entry[0].handler == last_free_l2tt_table) { asm ( "bkpt 4" ); }

    old_value.raw = change_word_if_equal( &l2tt->entry[0].raw, free_entry.raw, just_allocated_entry.raw );
  } while (old_value.handler != free_entry.handler);

  return l2tt;
}

static bool never_happens( uint32_t address, uint32_t type )
{
  // Marker, will not be called (because it won't be in an active L2TT
  asm ( "bkpt %[line]" : : [line] "i" (__LINE__) );
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

  assert( section.type == 1 );

  Level_two_translation_table *l2tt = find_table_from_l1tt_entry( section );

  about_to_remap_memory();

  // WriteS( "Mapping" ); NewLine;
  uint32_t base = (block.virtual_base >> 12) & 0xff;
  entry.page_base = block.physical_base >> 12;

  // FIXME: What if block overruns the end of the table?

  for (uint32_t b = 0; b < block.size >> 12; b++) {
    // WriteS( "Page " ); WriteNum( (b+base) << 12 ); WriteS( " > " ); WriteNum( entry.page_base << 12 ); NewLine;
    l2tt->entry[base + b] = entry;
    entry.page_base++;
  }

  memory_remapped();
}

static bool check_task_slot_l1( uint32_t address, uint32_t type )
{
  WriteS( "Check task slot L1: " ); WriteNum( address ); NewLine;
  asm ( "bkpt %[line]" : : [line] "i" (__LINE__) );
  return true;
}

static bool allocate_legacy_workspace_as_needed( uint32_t address, uint32_t type )
{
  // FIXME What if Kernel_allocate_pages returns nothing? (Block the Task, find some memory)
  arm32_ptr pointer = { .raw = address };

  l1tt_entry section = Local_L1TT->entry[pointer.section];

  assert( section.type == 1 );

  Level_two_translation_table *l2tt = find_table_from_l1tt_entry( section );

  l2tt->entry[pointer.page].raw = l2_prw.raw | Kernel_allocate_pages( 4096, 4096 );

  return true;
}

static bool random_legacy_kernel_workspace_l1( uint32_t address, uint32_t type )
{
  arm32_ptr pointer = { .raw = address };

  Level_two_translation_table *l2tt = find_free_table();

  for (int i = 0; i < number_of( l2tt->entry ); i++) {
    l2tt->entry[i].handler = allocate_legacy_workspace_as_needed;
  }

  map_l2tt_at_section_local( l2tt, pointer.section );

  // TODO Could call the allocate_legacy_workspace_as_needed routine for this address immediately,
  // avoiding another data abort.
  // Should the type field be modified?
  // Does anything need the type field at this level?
  //   These routines provide some RAM at the appropriate address in response to translation faults.
  //   Permission faults could be reported to user code without calling any of these.

  return true;
}

static bool check_task_slot_l2( uint32_t address, uint32_t type )
{
  bool reclaimed = claim_lock( &shared.mmu.lock );
  assert( !reclaimed ); // IDK, seems sus.

  physical_memory_block block = Kernel_physical_address( address );
  if (!reclaimed) release_lock( &shared.mmu.lock );

  if (block.size != 0) {
    map_block( block );
    return true;
  }

  asm ( "bkpt %[line]" : : [line] "i" (__LINE__) );
  return false;
}

static bool allocate_core_specific_zero_page_ram( uint32_t address, uint32_t type )
{
  assert( address < (1 << 20) );
  assert( (type & ~0x8f0) == 7 ); // From experience, not necessarily always the case. 0x800 => write, real hardware may report a non-zero domain

  Write0( "Zero page access " ); WriteNum( address ); Space; WriteNum( type ); NewLine;
  arm32_ptr pointer = { .raw = address };

  assert( workspace.mmu.zero_page_l2tt != 0 );
  assert( pointer.section == 0 );

  workspace.mmu.zero_page_l2tt->entry[pointer.page].raw = l2_prw.raw | Kernel_allocate_pages( 4096, 4096 );

  return true;
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
Write0( __func__ ); Space; Write0( "Zero section access " ); WriteNum( address ); Space; WriteNum( type ); NewLine;

  assert( address < (1 << 20) );
  assert( (type & ~0x8f0) == 5 ); // 0x800 => write. From experience, real hardware may report a non-zero domain
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
  Write0( "Check global l1tt, " ); WriteNum( address ); Space; WriteNum( type ); NewLine;

  arm32_ptr pointer = { .raw = address };

  l1tt_entry global = Global_L1TT->entry[pointer.section];
  Local_L1TT->entry[pointer.section] = global;

  if (global.handler == check_global_l1tt) {
    WriteS( "No memory at this address" ); NewLine;
    return false;
  }

  assert( global.handler != check_global_l1tt );

  return true;
}

static bool check_global_l2tt( uint32_t address, uint32_t type )
{
#ifdef DEBUG__BREAK_ON_UNEXPECTED_FAULT
  uint32_t *p;
  asm ( "push { r4-r11 }\n  mov %[p], sp" : [p] "=r" (p) );
  Write0( "Check global l2tt, " ); WriteNum( address ); Space; WriteNum( type ); NewLine;
  for (int i = 0; i < 32; i++) { WriteNum( p[i] ); if (0 == (i & 3)) NewLine; else Space; }
  asm ( "bkpt %[line]" : : [line] "i" (__LINE__) );
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
  else if (section == 0xfa6)
    result.handler = random_legacy_kernel_workspace_l1;
    // 0xfa600000 is used by the IF command. =GeneralMOSBuffer
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
  shared_workspace *shared_memory = (void*) startup->shared_memory;
  struct MMU_shared_workspace volatile *shared = &shared_memory->mmu;

  // These areas must be set up before enabling the MMU:
  //  Sections covering the OS code at its current physical address
  //  Sections mapping the OS code to its final virtual address (va_base from rool.script)
  //  Page table at the top megabyte especially for the hardware vectors at 0xffff8000 and
  //    for virtual access to the page tables.

  // The first of those will be removed almost immediately.

  // Reminder: all pointers are physical.

  // Space for 64 level 2 translation tables (enough to start dozens of cores)
  static const int initial_tables = 64;
  static const int size = initial_tables * sizeof( Level_two_translation_table );

  shared->physical_l2tts = (void*) pre_mmu_allocate_physical_memory( size, 64 << 10, startup );

  // FIXME: Allocate on 64k boundary and mark it as a Large Page G4-4866 
  //  shared->physical_l2tts = (void*) pre_mmu_allocate_physical_memory( size, 64 << 10, startup );

  Level_one_translation_table *l1tt = shared->global_l1tt;
  Level_two_translation_table *l2tt = shared->physical_l2tts;

  Level_two_translation_table *high_table = l2tt; // Take the first entry as the global kernel l2tt

  for (int i = 1; i < initial_tables-1; i++) {
    l2tt[i].entry[0].handler = free_l2tt_table;
    //l2tt[i].entry[1].handler = &l2tt[i << 10]; // Physical address of free table
    // The rest of the table is left uninitialised, it will be cleared before use.
  }

  l2tt[initial_tables - 1].entry[0].handler = last_free_l2tt_table;

  for (int i = 0; i < number_of( l1tt->entry ); i++) {
    l1tt->entry[i] = default_l1tt_entry( i );
  }

  l1tt_entry MiB = { .table.type1 = 1, .table.NS = 0, .table.Domain = 0 };

  MiB.raw |= (uint32_t) high_table;
  l1tt->entry[0xfff] = MiB; // Top MiB page-addressable

  for (int i = 0; i < number_of( high_table->entry ); i++) {
    high_table->entry[i].handler = check_global_l2tt;
  }

  // Map the global translation tables for all to share
  map_translation_table( high_table, l1tt, Global_L1TT );

  uint32_t va = 0xff000 & (uint32_t) L2TTs;
  uint32_t phys = (uint32_t) high_table;

  l2tt_entry entry = l2_prw;

  for (int i = 0; i < initial_tables / 4; i++) {
    high_table->entry[(va >> 12) + i].raw = entry.raw | (phys + (i << 12));
  }

  shared->global_l2tt = high_table;
  shared->kernel_l2tt = &L2TTs[high_table - shared->physical_l2tts];
}

static bool uninitialised_page_in_stack_section( uint32_t address, uint32_t type )
{
  arm32_ptr pointer = { .raw = address };
  l1tt_entry section = Local_L1TT->entry[pointer.section];

  assert( section.type == 1 ); // Table containing this page

  Level_two_translation_table *l2tt = find_table_from_l1tt_entry( section );

  l2tt->entry[pointer.page].raw = l2_prw.raw | Kernel_allocate_pages( 4096, 4096 );

  // Access to page in stack section that isn't part of a stack yet
  // WriteS( "Expanded stack at " ); WriteNum( address ); NewLine;

  return true;
}

static bool stack_overflow( uint32_t address, uint32_t type )
{
  // Access to safety page below a system stack
  asm ( "bkpt %[line]" : : [line] "i" (__LINE__) );
  return false;
}

static bool stack_underflow( uint32_t address, uint32_t type )
{

show_tasks_state();
  // Access to safety page above a system stack
  asm ( "bkpt %[line]" : : [line] "i" (__LINE__) );
  return false;
}

static void initialise_l2tt_for_system_stack( Level_two_translation_table *l2tt )
{
  for (int i = 0; i < number_of( l2tt->entry ); i++) {
    l2tt->entry[i].handler = uninitialised_page_in_stack_section;
  }
}

static void setup_stack_pages( uint32_t *top, uint32_t *lim )
{
  // Allocates two pages per privileged stack, one at the top, the other at the bottom.

  uint32_t limit = (uint32_t) lim;
  uint32_t mask = ~(limit - 1);
  uint32_t base = ((uint32_t) top) & mask;

  arm32_ptr top_ptr = { .rawp = top };
  arm32_ptr base_ptr = { .raw = base };

  assert ( top_ptr.section == base_ptr.section );

  l1tt_entry section = Local_L1TT->entry[top_ptr.section];

  Level_two_translation_table *l2tt;

  switch (section.type) {
  case 0: // Unused
    {
      l2tt = find_free_table();

      initialise_l2tt_for_system_stack( l2tt );

      l1tt_entry MiB = { .table.type1 = 1, .table.NS = 1, .table.Domain = 0 };

      MiB.raw |= physical_address( l2tt );

      Local_L1TT->entry[top_ptr.section] = MiB;
    }
    break;
  case 1: // Existing table
    asm ( "bkpt 666" ); // Untested, should allow multiple stacks in one section, e.g. 124KiB each
    l2tt = find_table_from_l1tt_entry( section );
    break;
  default:
    l2tt = 0; // Avoid compiler warning
    asm ( "bkpt 667" );
    assert ( false ); // Bad stack configuration
  }

  uint32_t page = top_ptr.page; // The page above the stack

  l2tt->entry[page--].handler = stack_underflow; // Tried to pop too much, or possibly just a random address

  l2tt->entry[page].raw = l2_prw.raw | Kernel_allocate_pages( 4096, 4096 );

/* No longer the way to go
  while (page > base_ptr.page) {
    l2tt->entry[--page].handler = allocate_stack_as_needed;
  }
*/
  if (top_ptr.page != base_ptr.page) {
    // For the SharedCLibrary           FIXME needed? or just allocate_stack_as_needed?
    l2tt->entry[base_ptr.page].handler = stack_overflow; // Tried to push too much, or possibly just a random address
    l2tt->entry[base_ptr.page].raw = l2_prw.raw | Kernel_allocate_pages( 4096, 4096 );
  }
}

static void clear_svc_stack_area();

void Initialise_privileged_mode_stacks()
{
  extern uint32_t stack_limit; // Not really a pointer!
  extern uint32_t svc_stack_top;
  //extern uint32_t undef_stack_top;
  //extern uint32_t abt_stack_top;
  extern uint32_t irq_stack_top;
  extern uint32_t fiq_stack_top;

  // These require the l2tt tables to be directly mapped locally, there's no 
  // abort stack set up yet.
  // The SVC stack is slot-specific
  {
    Level_two_translation_table *l2tt;

    l2tt = find_free_table();

    l1tt_entry MiB = { .table.type1 = 1, .table.NS = 1, .table.Domain = 0 };

    MiB.raw |= physical_address( l2tt );

    arm32_ptr top_ptr = { .rawp = &svc_stack_top };

    Local_L1TT->entry[top_ptr.section] = MiB;

    // Tried to pop too much, or possibly just a random address?
    l2tt->entry[top_ptr.page].handler = stack_underflow;

    clear_svc_stack_area();
  }

  // FIXME: These can all be made very small...
  // These modes will simply store the task state and tell another task
  // to deal with the problem.
  // setup_stack_pages( &undef_stack_top, &stack_limit );
  // setup_stack_pages( &abt_stack_top, &stack_limit );
  setup_stack_pages( &irq_stack_top, &stack_limit );
  setup_stack_pages( &fiq_stack_top, &stack_limit );
}

static void __attribute__(( noreturn, noinline )) go_kernel()
{
  // Break before make
  about_to_remap_memory();

  // Remove the mapping for virtual == physical address for the ROM
  // We are running in virtual memory now, so adr and function addresses will match

  uint32_t rom = ((uint32_t) &va_base) >> 20;
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
#ifdef SINGLE_CORE
if (ws->core_number > 0) { 
#elif defined SHOW_TASKS
if (ws->core_number != 0 && ws->core_number != 3) { 
#else
if (ws->core_number > 3) {  // Max cores for HD display
#endif
  BOOT_finished_allocating( ws->core_number, startup );
  for (;;) { asm ( "wfi" ); }
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

  int test = 0;
  for (int i = 0; i < (uint32_t) &rom_size; i+= (1 << 20)) {
    l1tt->entry[(start + i) >> 20].raw = rom_sections.raw | ((physical + i) & 0xfff00000);

    // Also where the code currently is...
    l1tt->entry[(physical + i) >> 20].raw = rom_sections.raw | ((physical + i) & 0xfff00000);
    test++;
  }
  assert ( test == 5 );

  // Our core-specific work areas, in our core-specific L2TT
  map_work_area( l2tt, ws );
  map_translation_table( l2tt, l1tt, Local_L1TT );
  map_shared_work_area( l2tt, shared_memory );

  ws->mmu.kernel_l2tt = &L2TTs[l2tt - shared->physical_l2tts];

  // Done: Remove device page at 0xfff00000 pointing to gpio
  // Wow! This line threw an exception because the compiler put the constant (0x33) into a ROM location
  // then used a pointer to it to access it as raw (I guess), trying to access 0xfc018xxx, which is
  // out of RAM. (This is only a problem before the MMU is activated and the code is running where we've
  // told the compiler and linker it is.) This particular line will be removed soon, anyway.
  //    *** Leave the comment here, just in case ***
  // Symptom was the code getting stuck back in boot.c with the wrong address for `states'!
  // l2tt->entry[0].raw = l2_device.raw | 0x3f200000;
  // The following does work, but isn't needed (except for debugging this code!)
  // l2tt_entry dev = l2_device;
  // l2tt->entry[0].raw = dev.raw | 0x3f200000;

  asm ( "  dsb sy" );

  // This version doesn't use TTBR1; there's enough memory in everything,
  // these days. (Any future 64-bit version should, though).
  asm ( "mcr p15, 0, %[ttbcr], c2, c0, 2" : : [ttbcr] "r" (0) );
  // 0x48 -> Inner and Outer write-back, write-allocate cacheable, not shared
  // (per core tables)
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

// noinline attribute is required so that stack space is allocated for any local variables.
// There is no need for this routine to examine the fault generating instruction or the registers.
static bool __attribute__(( noinline )) handle_data_abort()
{
  uint32_t fa = fault_address();
  uint32_t ft = data_fault_type();

  // Real hardware appears to fill in a value for Domain which may not be
  // zero. Domain errors should never happen, and when they do should be
  // handled at this level. The fault type will not be 5 or 7.
  if ((ft & ~0x8f0) != 7
   && (ft & ~0x8f0) != 5) {
    uint32_t *stack;
    asm ( "mov %[sp], sp" : [sp] "=r" (stack) );
    WriteNum( &stack[9] );
    WriteS( "Fault type: " ); WriteNum( ft ); WriteS( " @ " ); WriteNum( fa ); WriteS( " address " ); WriteNum( stack[9] ); NewLine;
    return false;
  }
  about_to_remap_memory();

  fault_handler handler = find_handler( fa );

  bool result = handler( fa, ft );

  memory_remapped();

  return result;
}

#define MMU
#include "trivial_display.h"

void __attribute__(( naked, optimize( 0 ), noreturn )) Kernel_default_data_abort()
{
  // TODO If data aborts start to need other tasks to fill in the missing memory,
  // e.g. from a file, this will have to copy the save_context bit from
  // Kernel_default_irq. At the moment, this only deals with missing memory, not
  // permission faults.
  // Second thoughts: usually the handler will resolve the problem, sometimes it
  // will be a failure, other times it will want to replace the running task with
  // a task that can, say, read data from disc, in which case the rest of the
  // context can be saved in this routine.

  asm volatile (
        "  sub lr, lr, #8"
      "\n  srsdb sp!, #0x17 // Store return address and SPSR"
      "\n  push { "C_CLOBBERED" }"
      );

  if (!handle_data_abort()) {
register uint32_t *sp asm ( "r13" );
uint32_t *stack = sp;
show_word( workspace.core_number * 100+ 960, 20, stack[6], Blue );
uint32_t *ss = (void*) ((~0xff000 & (uint32_t) sp) | 0xff000);
show_word( workspace.core_number * 100+ 960, 30, ss[-1], Yellow );
show_word( workspace.core_number * 100+ 960, 40, ss[-2], Yellow );
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

static void clear_app_area()
{
  Level_two_translation_table *l2tt;

  if (Local_L1TT->entry[0].type == 1) {
    l2tt = find_table_from_l1tt_entry( Local_L1TT->entry[0] );

    for (int i = 0x8000 >> 12; i < 0x100000 >> 12; i++) {
      l2tt->entry[i].handler = check_task_slot_l2;
    }
  }

  for (int i = 1; i < (((uint32_t)&app_memory_limit) >> 20); i++) {
    if (Local_L1TT->entry[i].type == 1) {
      asm ( "bkpt %[line]" : : [line] "i" (__LINE__) ); // Free the l2tt.
    }

    Local_L1TT->entry[i].handler = check_task_slot_l1;
  }
}

static void clear_pipes_area()
{
  extern char pipes_base;
  extern char pipes_top;
  uint32_t base = (uint32_t) &pipes_base;
  uint32_t top = (uint32_t) &pipes_top;

  for (int i = base >> 20; i < top >> 20; i++) {
    if (Local_L1TT->entry[i].type == 1) {
      asm ( "bkpt %[line]" : : [line] "i" (__LINE__) ); // Free the l2tt.
    }

    Local_L1TT->entry[i].handler = check_task_slot_l1;
  }
}

static void clear_svc_stack_area()
{
  Level_two_translation_table *l2tt;

  extern uint32_t svc_stack_top;
  arm32_ptr top_ptr = { .rawp = &svc_stack_top };

  assert( Local_L1TT->entry[top_ptr.section].type == 1 );

  // This will always have been previously initialised
  l2tt = find_table_from_l1tt_entry( Local_L1TT->entry[top_ptr.section] );

  uint32_t entry = top_ptr.page;
  while (entry > 0 && l2tt->entry[--entry].handler != check_task_slot_l2) {
    l2tt->entry[entry].handler = check_task_slot_l2;
  }
  entry = 0; // SharedCLibrary workspace (yuk!)
  while (entry < top_ptr.page && l2tt->entry[entry].handler != check_task_slot_l2) {
    l2tt->entry[entry++].handler = check_task_slot_l2;
  }
}

void MMU_switch_to( TaskSlot *slot )
{
  bool reclaimed = claim_lock( &shared.mmu.lock );
  assert( !reclaimed ); // IDK, seems sus.

  // Write0( "Switching to slot " ); WriteNum( slot ); Space; WriteNum( TaskSlot_asid( slot ) ); NewLine;
  // FIXME Only clear what's used
  // FIXME deal with slots that go over the first MiB
  // Note: My idea is to try to keep memory as contiguous as possible, and have
  // two or possibly three sub-MiB translation tables for the first MiB (bottom_MiB_tt)
  // and the slot's top MiB (and possibly the one below it, in case a task regularly
  // modifies its memory by small amounts above and below a MiB boundary).

  // TODO record which slot was last active, configure all translation table
  // walks to cause an exception, then only clear the table if the slot is
  // not the last active one.
  //   asm ( "mcr p15, 0, %[ttbcr], c2, c0, 2" : : [ttbcr] "r" (0x10) );
  // (Set bit PD0) - get a fault on TLB miss

  // Note: remember which slot most recently updated the slot-specific areas
  // of the translation tables, so switching away and back can be done without
  // doing anything as long as there's no TLB miss in the meantime. TODO

  // These are the areas that TaskSlots are known to update with non-Global
  // entries.
  clear_app_area();
  clear_pipes_area();
  clear_svc_stack_area();
  

  // Set CONTEXTIDR
  asm ( "mcr p15, 0, %[asid], c13, c0, 1" : : [asid] "r" (TaskSlot_asid( slot )) );

  if (!reclaimed) release_lock( &shared.mmu.lock );

  clean_cache_to_PoC();
}

static void map_at( void *va, uint32_t pa, uint32_t size, bool shared ) 
{
// Too early Write0( __func__ ); Space; WriteNum( va ); Space; WriteNum( pa ); Space; WriteNum( size ); NewLine;
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

    // FIXME FIXME FIXME this is horrible. The console task in the HAL needs to be able to read this
    // It will go away when the standard pipe mapping code is written.
    uint32_t v = (uint32_t) va;
    extern uint32_t debug_pipe;
    uint32_t p = (uint32_t) &debug_pipe;
    if (kernel_memory && (v >= p && v < p + 16*1024)) kernel_memory = false;

    l2tt_entry entry = kernel_memory ? l2_prw : l2_urwx;

    entry.S = shared ? 1 : 0;

    entry.page_base = (pa >> 12);

    Level_two_translation_table *l2tt;

    switch (l1tt->entry[section].type) {
    case 0: // Unused
      {
        l2tt = find_free_table();

        initialise_l2tt_for_section( l2tt, section );

        if (section == 0) {
          assert( workspace.mmu.zero_page_l2tt == 0 );
          workspace.mmu.zero_page_l2tt = l2tt;
        }

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
      Write0( __func__ ); Write0( ", Address already allocated to a MiB section (or supersection) " ); WriteNum( pointer.raw ); NewLine;
      asm ( "bkpt 17" : : [x] "r" (l1tt->entry[section]) );
      __builtin_unreachable();
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
    // Delay the breakpoint until the frame buffer is initialised (hopefully)
    for (int i = 0; i < 80000000; i++) asm ( "svc 0xff" );
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
  if (size < natural_alignment) {
    // FIXME Horrible hack; map_at needs changing
    for (int i = 0; i < size; i+= 4096) {
      map_at( (void*) (i + (uint32_t) va), pa + i, 4096, true );
    }
  }
  else {
    map_at( va, pa, size, true );
  }
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
    WriteNum( pa ); WriteS( " mapped at " ); WriteNum( va ); NewLine;
  }
  else {
    for (;;) { asm ( "bkpt 10" ); }
  }

  memory_remapped();
}

