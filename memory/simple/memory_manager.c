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

// Dynamic Areas.
// They may be shared between cores, or not.
// My initial implementation will allocate a multiple of megabytes for each
// DA, to simplify memory management, for the time being. Later, they will
// allow page-size allocation, by associating a L2TT with each one that
// needs it.
// In the mean time, I will allocate megabytes, and lie about the real size.
// This is proof of concept code; if I can get multiple independent cores
// working with the Wimp and Filing Systems, it should show that the approach
// has merit.

#include "inkernel.h"

// Very, very simple implementation: one block of contiguous physical memory for each DA.
// It will break very quickly, but hopefully demonstrate the principle.

struct DynamicArea {
  int32_t  number:12;
  uint32_t permissions:3;
  bool shared:1; // Visible to all cores at the same location (e.g. Screen)
  uint32_t reserved:16;

  uint32_t virtual_page;
  uint32_t start_page;
  uint32_t pages;
  uint32_t requested_pages; // This is the lie we tell any code that asks.
  uint32_t handler_routine;
  uint32_t workarea;
  DynamicArea *next;
};

void Initialise_system_DAs()
{
  // This isn't set in stone, but first go:
  // (It kind-of shadows Kernel.s.osinit)

  // System Heap (per core, initially zero sized, I don't know what uses it)
  // RMA (shared, but not protected)
  // Screen (shared, not protected)
  // Font cache (shared, not protected) - not created by FontManager
  // Sprite Area (per core)

  // Create a Relocatable Module Area, and initialise a heap in it.

  uint32_t initial_rma_size = natural_alignment;
  svc_registers regs;

  claim_lock( &shared.memory.dynamic_areas_setup_lock ); 

// While we're hacking like crazy, let's allocate far too much memory for RO kernel workspace...
// See comments to GSTrans in swis.c

// This is the mechanism used by Kernel SWIs to return to callers, not normal modules...

uint32_t memory;
    // Forgot that there may not be any memory to allocate, yet...
do {
  for (int i = 0; i < 1000; i++) { asm ( "" ); }
  memory = Kernel_allocate_pages( natural_alignment, natural_alignment );
} while (memory == 0xffffffff);

MMU_map_at( (void*) 0xfaf00000, memory, natural_alignment );
memset( (void*) 0xfaf00000, '\0', natural_alignment );
uint32_t slvk[] = {     0xe38ee201, // orr     lr, lr, #0x10000000      SLVK_setV
                        0x638ee201, // orrvs   lr, lr, #0x10000000      SLVK_testV
                        0xe49df004  // pop     {pc} (ldr pc, [sp], #4)  SLVK
                        };
memcpy( (void*) 0xfaff3358, slvk, sizeof( slvk ) );

  if (shared.memory.dynamic_areas == 0) {
    // First core here (need not be core zero)
    uint32_t RMA = -1;

    // But there may not be any memory to allocate, yet...
    while (-1 == RMA) {
      RMA = Kernel_allocate_pages( natural_alignment, natural_alignment );
    }

    MMU_map_shared_at( &rma_heap, RMA, initial_rma_size );
    asm ( "dsb sy" );

    shared.memory.rma_memory = RMA;

    regs.r[0] = 0;
    regs.r[1] = (uint32_t) &rma_heap;
    regs.r[3] = initial_rma_size;

    if (!do_OS_Heap( &regs )) {
      for (;;) { asm ( "wfi" ); }
    }
    // RMA heap initialised, can call rma_allocate

    { // RMA
      DynamicArea *da = rma_allocate( sizeof( DynamicArea ), &regs );
      if (da == 0) goto nomem;
      da->number = 1;
      da->permissions = 7; // rwx
      da->shared = 1;
      da->virtual_page = ((uint32_t) &rma_base) >> 12;
      da->start_page = shared.memory.rma_memory >> 12;
      da->pages = initial_rma_size >> 12;
      da->requested_pages = da->pages;
      da->next = shared.memory.dynamic_areas;
      shared.memory.dynamic_areas = da;
    }

    { // Font Cache - allow module to create it? - No: it creates it, then resizes it, and isn't MP safe
      uint32_t memory = Kernel_allocate_pages( natural_alignment, natural_alignment );
      DynamicArea *da = rma_allocate( sizeof( DynamicArea ), &regs );
      if (da == 0) goto nomem;
      da->number = 4;
      da->permissions = 6; // rw-
      da->shared = 1;
      da->virtual_page = 0x30000000 >> 12; // FIXME I don't know how DAs get allocated to virtual addresses!
      da->start_page = memory >> 12;
      da->pages = natural_alignment >> 12;
      da->requested_pages = 128; // c.f. default_os_byte (FontSize CMOS byte)
      da->next = shared.memory.dynamic_areas;
      shared.memory.dynamic_areas = da;

      MMU_map_shared_at( (void*) (da->virtual_page << 12), da->start_page << 12, da->pages << 12 );
    }

    asm ( "dsb sy" );
  }
  else {
    // Map the shared areas into core's virtual memory map
    MMU_map_shared_at( &rma_heap, shared.memory.rma_memory, initial_rma_size );
    asm ( "dsb sy" );

    DynamicArea *da = shared.memory.dynamic_areas;
    while (da != 0) {
      if (da->number != 1) // RMA already mapped
        MMU_map_shared_at( (void*) (da->virtual_page << 12), da->start_page << 12, da->pages << 12 );
      da = da->next;
    }
    asm ( "dsb sy" );
  }

  release_lock( &shared.memory.dynamic_areas_setup_lock );

  // Now the non-shared DAs, can be done in parallel

  { // "Free Pool" - hopefully obsolete, expected by WindowManager init, at least
  // TODO Add names, handlers to DAs
    extern uint32_t free_pool;

    DynamicArea *da = rma_allocate( sizeof( DynamicArea ), &regs );
    if (da == 0) goto nomem;
    da->number = 6;
    da->permissions = 6; // rw-
    da->shared = 0;
    da->virtual_page = ((uint32_t) &free_pool) >> 12;
    da->pages = 256;
    da->start_page = Kernel_allocate_pages( da->pages << 12, da->pages << 12 ) >> 12;

    da->next = workspace.memory.dynamic_areas;
    workspace.memory.dynamic_areas = da;

    MMU_map_at( (void*) (da->virtual_page << 12), da->start_page << 12, da->pages << 12 );
  }

  { // System heap, one per core (I think)
    extern uint32_t system_heap;

    DynamicArea *da = rma_allocate( sizeof( DynamicArea ), &regs );
    if (da == 0) goto nomem;
    da->number = 0;
    da->permissions = 6; // rw-
    da->shared = 0;
    da->virtual_page = ((uint32_t) &system_heap) >> 12;
    da->pages = 256;
    da->start_page = Kernel_allocate_pages( da->pages << 12, da->pages << 12 ) >> 12;

    da->next = workspace.memory.dynamic_areas;
    workspace.memory.dynamic_areas = da;

    MMU_map_at( (void*) (da->virtual_page << 12), da->start_page << 12, da->pages << 12 );

    regs.r[0] = 0;
    regs.r[1] = da->virtual_page << 12;
    regs.r[3] = da->pages << 12;

    if (!do_OS_Heap( &regs )) {
      for (;;) { asm ( "wfi" ); }
    }
  }
  return;

nomem:
  asm ( "bkpt 1" );
}

bool do_OS_ChangeDynamicArea( svc_registers *regs )
{
  static error_block error = { 0x999, "Cannot change size of DAs" };
  regs->r[0] = (uint32_t) &error;
  return false;
}

bool do_OS_ReadDynamicArea( svc_registers *regs )
{
  DynamicArea *da = workspace.memory.dynamic_areas;
  while (da != 0 && da->number != regs->r[0]) {
    da = da->next;
  }
  if (da == 0) {
    da = shared.memory.dynamic_areas;
    while (da != 0 && da->number != regs->r[0]) {
      da = da->next;
    }
  }
  if (da != 0) {
    regs->r[0] = da->virtual_page << 12;
    regs->r[1] = da->pages << 12;
    return true;
  }
  // FIXME Bit 7

  switch (regs->r[0]) {
  case 6:
    {
      regs->r[0] = 0x80000000;
      regs->r[1] = 0;
      return true;
    }
  default:
    {
    static error_block error = { 0x999, "Unknown DA" };
    regs->r[0] = (uint32_t) &error;
    return false;
    }
  }
  return true;
}

static char *da_name( DynamicArea *da )
{
  return (char*) (da+1);
}

bool do_OS_DynamicArea( svc_registers *regs )
{
  bool result = true;

  enum { New, Remove, Info, Enumerate, Renumber };

  claim_lock( &shared.memory.dynamic_areas_lock );

  if (shared.memory.last_da_address == 0) {
    // First time in this routine
    shared.memory.last_da_address = 0x30000000; // FIXME I don't know how DAs get allocated to virtual addresses!
    shared.memory.user_da_number = 256;
  }

  switch (regs->r[0]) {
  case New:
    { // Create new Dynamic Area
      const char *name = (void*) regs->r[8];

      DynamicArea *da = rma_allocate( sizeof( DynamicArea ) + strlen( name ) + 1, regs );
      if (da == 0) goto nomem;

      strcpy( da_name( da ), name );

      int32_t number = regs->r[1];
      if (number == -1) {
        number = shared.memory.user_da_number++;
      }
      da->number = number;

      int32_t max_logical_size = regs->r[5];
      if (max_logical_size == -1) max_logical_size = 128 << 20; // FIXME

      int32_t va = regs->r[3];
      if (va == -1) {
        va = shared.memory.last_da_address;
        shared.memory.last_da_address += max_logical_size;
      }

      da->handler_routine = regs->r[6];
      da->workarea = regs->r[7];
      if (da->workarea == -1) {
        da->workarea = da->virtual_page << 12;
      }

      da->permissions = 6; // rw-
      da->shared = 1;
      da->virtual_page = shared.memory.last_da_address >> 12;


      {
      // Should probably use OS_ChangeDynamicArea for this, move the code there FIXME
      // No support for bit 8 set
      uint32_t initial_size = regs->r[2];
      register uint32_t code asm ( "r0" ) = 0;
      register uint32_t grow_by asm ( "r3" ) = initial_size;
      register uint32_t current_size asm ( "r4" ) = 0;
      register uint32_t page_size asm ( "r5" ) = 4096;
      register uint32_t workspace asm ( "r12" ) = da->workarea;
      asm goto ( "blx %[pregrow]"
             "\n  bvs %l[do_not_grow]"
          : // asm goto does not allow output as at gcc-10, although the documentation pretends it does
          : "r" (code)
          , "r" (grow_by)
          , "r" (current_size)
          , "r" (page_size)
          , "r" (workspace)
          , [pregrow] "r" (da->handler_routine)
          : /* clobbers */
          : do_not_grow );

      // assuming r0 = 0x56534552!
      }
      uint32_t memory = Kernel_allocate_pages( natural_alignment, natural_alignment );
      da->start_page = memory >> 12;

      da->pages = natural_alignment >> 12;
      da->next = shared.memory.dynamic_areas;
      shared.memory.dynamic_areas = da;
      shared.memory.last_da_address += 128 << 20; // FIXME Making this up as I go along...

      MMU_map_shared_at( (void*) (da->virtual_page << 12), da->start_page << 12, da->pages << 12 );

      {
      // Should probably use OS_ChangeDynamicArea for this, move the code there FIXME
      // No support for bit 8 set
      uint32_t initial_size = regs->r[2];
      register uint32_t code asm ( "r0" ) = 1;
      register uint32_t growth asm ( "r3" ) = initial_size;
      register uint32_t current_size asm ( "r4" ) = da->pages << 12;
      register uint32_t page_size asm ( "r5" ) = 4096;
      register uint32_t workspace asm ( "r12" ) = da->workarea;
      asm ( "blx %[postgrow]"
          :
          : "r" (code)
          , "r" (growth)
          , "r" (current_size)
          , "r" (page_size)
          , "r" (workspace)
          , [postgrow] "r" (da->handler_routine) );
      }
      break;
do_not_grow:
      for (;;) { asm( "bkpt 1\nwfi" ); }
    }
    break;
  case 30:
    { // Screen. Creates DA 2, at R1, size R2, returns virtual address in R1
      // Virtual address is always at frame_buffer (set in rom.script)

      // TODO: Remove existing DA, if any, allow resizing, etc.
      // This is not going to work with more than one thread, unless data abort maps it...
      extern uint32_t frame_buffer;

      DynamicArea *da = shared.memory.dynamic_areas;
      while (da != 0 && da->number != 2) {
        da = da->next;
      }

      if (da == 0) {
        da = rma_allocate( sizeof( DynamicArea ), regs );
        if (da == 0) goto nomem;
        da->number = 2;
        da->permissions = 6; // rw-
        da->shared = 1;
        da->virtual_page = ((uint32_t) &frame_buffer) >> 12;
        da->start_page = regs->r[1] >> 12;
        da->pages = regs->r[2] >> 12;
        da->next = shared.memory.dynamic_areas;
        shared.memory.dynamic_areas = da;
      }

      // Could be mapped in when used, by searching DAs in data_abort
      // Should probably have XN. TODO
      MMU_map_shared_at( (void*) (da->virtual_page << 12), da->start_page << 12, da->pages << 12 );

      regs->r[1] = (uint32_t) &frame_buffer;
    }
    break;
  case Info:
    {
      DynamicArea *da = shared.memory.dynamic_areas;
      while (da != 0 && da->number != regs->r[1]) {
        da = da->next;
      }
      if (da == 0) da = workspace.memory.dynamic_areas;
      while (da != 0 && da->number != regs->r[1]) {
        da = da->next;
      }

      if (da == 0) {
WriteS( "OS_DynamicArea " ); WriteNum( regs->r[0] ); WriteS( " " ); WriteNum( regs->r[1] ); NewLine;
        static error_block error = { 0x997, "Unknown Dynamic Area" };
        regs->r[0] = (uint32_t) &error;
        result = false;
      }
      else {
/*
R0 	Preserved
R1 	Preserved
R2 	Current size of area, in bytes
R3 	Base logical address of area
R4 	Area flags
R5 	Maximum size of area in bytes
R6 	Pointer to dynamic area handler routine, or 0 if no routine
R7 	Pointer to workspace for handler
R8 	Pointer to name of area 
*/
        regs->r[2] = da->pages << 12;
        regs->r[3] = da->start_page << 12;
        regs->r[4] = 0; // FIXME
        regs->r[5] = da->pages << 12; // FIXME
        regs->r[6] = 0; // FIXME
        regs->r[7] = 0; // FIXME
        regs->r[8] = (uint32_t) "Need to name DAs"; // FIXME
      }
    }
    break;
  default:
    {

WriteS( "OS_DynamicArea " ); WriteNum( regs->r[0] ); WriteS( " " ); WriteNum( regs->r[1] ); NewLine;
for (;;) { asm ( "wfi" ); }
      static error_block error = { 0x997, "Cannot do anything to DAs" };
      regs->r[0] = (uint32_t) &error;
      result = false;
    }
    break;
  }

  release_lock( &shared.memory.dynamic_areas_lock );

  return result;

nomem:
  for (;;) { asm ( "bkpt 1" ); }
  release_lock( &shared.memory.dynamic_areas_lock );
  return false;
}

bool do_OS_Memory( svc_registers *regs )
{
  switch (regs->r[0] & 0xff) {
  case 10:
    {
    // Free pool lock (as in, affect the lock on the free pool).
    // Bit 8 -> Call is being made by the Wimp
    regs->r[1] = shared.memory.os_memory_active_state;
    shared.memory.os_memory_active_state = 1 - shared.memory.os_memory_active_state;
    return true;
    }
    break;
  default:
    WriteS( "OS_Memory: " ); WriteNum( regs->r[0] ); WriteS( " " ); WriteNum( regs->r[1] ); NewLine;
  }
  return Kernel_Error_UnimplementedSWI( regs );
}


void Kernel_add_free_RAM( uint32_t base_page, uint32_t size_in_pages )
{
  claim_lock( &shared.memory.lock );

  free_block *p = (free_block *) shared.memory.free_blocks;

  while (p->size != 0) {
    p++;
  }

  p->base_page = base_page;
  p->size = size_in_pages;

  release_lock( &shared.memory.lock );
}

static bool aligned( uint32_t b, uint32_t alignment )
{
  return 0 == (b & (alignment - 1));
}

static uint32_t misalignment( uint32_t b, uint32_t alignment )
{
  return alignment - (b & (alignment - 1));
}

// How I think this algorithm will work:
// as memory is allocated, the earlier blocks will become less aligned,
// when a more aligned memory area is needed, there might not be a free_block
// whose base is sufficiently aligned, so the last block will be made into
// an aligned free block by taking the top off an earlier block at the first
// point of alignment.
// In practice, the first free blocks will be least aligned, and the last, most
// aligned.
// This is basically untested, and I'm sure there are dozens of better
// approaches, which I intend to look up later.

// This will not play nicely with freeing pages, but that comes later.
// I anticipate having a linked list of freed blocks, and a page of OS
// memory that I can remap to examine them. With the list structure at
// the start of the freed memory blocks, there will be practically zero OS
// memory overhead.

uint32_t Kernel_allocate_pages( uint32_t size, uint32_t alignment )
{
  uint32_t result = -1;
  uint32_t size_in_pages = size >> 12;
  uint32_t alignment_in_pages = alignment >> 12;

  claim_lock( &shared.memory.lock );

  free_block *p = (free_block *) shared.memory.free_blocks;

  while (p->size != 0
      && (!aligned( p->base_page, alignment_in_pages )
       || p->size < size_in_pages)) {
    p++;
  }

  if (p->size == 0) {
    // Find a big enough block to split, and take the aligned part off into another free block

    free_block *big = (free_block *) shared.memory.free_blocks;
    while (big->size != 0
        && big->size < size_in_pages + misalignment( big->base_page, alignment_in_pages )) {
      big++;
    }

    if (big->size != 0) {
      uint32_t mis = misalignment( big->base_page, alignment_in_pages );
      p->size = big->size - mis;
      p->base_page = big->base_page + mis;
      big->size = mis;
    }
  }

  if (p->size != 0) {
    result = p->base_page << 12;
    p->base_page += size_in_pages;
    p->size -= size_in_pages;

    if (p->size == 0) {
      do {
        *p = *(p+1);
        p++;
      } while (p->size != 0);
    }
  }

  release_lock( &shared.memory.lock );
  return result;
}

#define W (480 * workspace.core_number)
//#define H(n) (150 + (n * 250))
#define H(n) (150)

#define BSOD( n, c ) \
  asm ( "push { r0-r12,lr }" ); \
  register uint32_t addr; \
  asm ( "mov %[addr], lr" : [addr] "=r" (addr) ); \
for (int i = 0; i < 0x8000000; i++) { asm ( "" ); } \
  show_word( 100 + W, 30, workspace.core_number, c ); \
  show_word( 100 + W, 40, addr, c ); \
  register uint32_t *regs; \
  asm ( "mov %[regs], sp" : [regs] "=r" (regs) ); \
  for (int i = 13; i >= 0; i--) { \
    show_word( 100 + W, H(n) + 10 * i, regs[i], c ); \
  } \
  uint32_t reg; \
  asm ( "mrs %[reg], spsr" : [reg] "=r" (reg) ); \
  show_word( 100 + W, H(n) - 72, reg, Yellow ); \
  asm ( "mrs %[reg], sp_usr" : [reg] "=r" (reg) ); \
  show_word( 100 + W, H(n) - 62, reg, Blue ); \
  asm ( "mrs %[reg], lr_usr" : [reg] "=r" (reg) ); \
  show_word( 100 + W, H(n) - 52, reg, Blue ); \
  asm ( "mrs %[reg], lr_svc" : [reg] "=r" (reg) ); \
  show_word( 100 + W, H(n) - 42, reg, Blue ); \
  show_word( 100 + W, H(n) - 32, data_fault_type(), Red ); \
  show_word( 100 + W, H(n) - 22, instruction_fault_type(), Red ); \
  show_word( 100 + W, H(n) - 12, fault_address(), Green ); \
clean_cache_to_PoC(); \
clean_cache_to_PoU(); \
  for (;;) { asm ( "wfi" ); }

void __attribute__(( naked, noreturn )) Kernel_default_prefetch()
{
  // When providing proper implementation, ensure the called routine is __attribute__(( noinline ))
  // noinline attribute is required so that stack space is allocated for any local variables.
  BSOD( 0, Blue );
}

void Kernel_failed_data_abort()
{
  // When providing proper implementation, ensure the called routine is __attribute__(( noinline ))
  // noinline attribute is required so that stack space is allocated for any local variables.
  BSOD( 1, Green );
}

void __attribute__(( naked, noreturn )) Kernel_default_undef()
{
  // When providing proper implementation, ensure the called routine is __attribute__(( noinline ))
  // noinline attribute is required so that stack space is allocated for any local variables.
  BSOD( 2, Yellow );
}

void __attribute__(( naked, noreturn )) Kernel_default_reset() 
{
  // When providing proper implementation, ensure the called routine is __attribute__(( noinline ))
  // noinline attribute is required so that stack space is allocated for any local variables.
  BSOD( 3, Red );
}

void __attribute__(( naked, noreturn )) Kernel_default_irq() 
{
  // When providing proper implementation, ensure the called routine is __attribute__(( noinline ))
  // noinline attribute is required so that stack space is allocated for any local variables.
  BSOD( 4, Blue );
}
