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
#include "trivial_display.h"

// Very, very simple implementation: one block of contiguous physical memory for each DA.
// It will break very quickly, but hopefully demonstrate the principle.

struct DynamicArea {
  uint32_t number;
  uint32_t permissions:3;
  bool shared:1; // Visible to all cores at the same location (e.g. Screen)
  uint32_t reserved:28;

  uint32_t virtual_page;
  uint32_t start_page;
  uint32_t pages;               // Implementation actually allocates MiBs at the moment
  uint32_t actual_pages;        // This is how many there really are
  uint32_t handler_routine;
  uint32_t workarea;
  DynamicArea *next;
};

static inline void InitialiseHeap( void *start, uint32_t size )
{
  register uint32_t code asm( "r0" ) = 0;
  register void *heap asm( "r1" ) = start;
  register uint32_t initial_size asm( "r3" ) = size;
  asm ( "svc %[swi]" : : "r" (code), "r" (heap), "r" (initial_size), [swi] "i" (OS_Heap | 0x20000) : "lr", "memory" );
}

void Initialise_system_DAs()
{
  // This isn't set in stone, but first go:
  // (It kind-of shadows Kernel.s.osinit)

  // System Heap (per core, initially zero sized, I don't know what uses it)
  // RMA (shared, but not protected)
  // Screen (shared, not protected)
  // Font cache (shared, not protected) - not created by FontManager
  // Sprite Area (per core)
  // RamFS DA, expected by WindowManager

  // Create a Relocatable Module Area, and initialise a heap in it.

  uint32_t initial_rma_size = 2 * natural_alignment;

  bool reclaimed = claim_lock( &shared.memory.dynamic_areas_setup_lock ); 
  assert( !reclaimed ); // No question, only entered once

// While we're hacking like crazy, let's allocate far too much memory for RO kernel workspace...
// See comments to GSTrans in swis.c

  //uint32_t memory = Kernel_allocate_pages( natural_alignment, natural_alignment );
  //assert( memory != -1 );

  if (shared.memory.dynamic_areas == 0) {
    // First core here (need not be core zero)
    uint32_t RMA = Kernel_allocate_pages( initial_rma_size, natural_alignment );
    assert( RMA != -1 );

    shared.memory.rma_memory = RMA;

    MMU_map_shared_at( &rma_heap, RMA, initial_rma_size );
    asm ( "dsb sy" );

    InitialiseHeap( &rma_heap, initial_rma_size );

    // RMA heap initialised, can call rma_allocate

    { // RMA
      DynamicArea *da = rma_allocate( sizeof( DynamicArea ) );
      if (da == 0) goto nomem;
      da->number = 1;
      da->permissions = 7; // rwx
      da->shared = 1;
      da->virtual_page = ((uint32_t) &rma_base) >> 12;
      da->start_page = shared.memory.rma_memory >> 12;
      da->pages = initial_rma_size >> 12;
      da->actual_pages = initial_rma_size >> 12;
      da->next = shared.memory.dynamic_areas;
      da->handler_routine = 0;
      shared.memory.dynamic_areas = da;
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

WriteS( "Free pool" ); NewLine;
    DynamicArea *da = rma_allocate( sizeof( DynamicArea ) );
    if (da == 0) goto nomem;
    da->number = 6;
    da->permissions = 6; // rw-
    da->shared = 0;
    da->virtual_page = ((uint32_t) &free_pool) >> 12;
    da->pages = 256;
    da->actual_pages = da->pages;
    da->start_page = Kernel_allocate_pages( da->actual_pages << 12, 1 << 12 ) >> 12;
    da->handler_routine = 0;

if (da == workspace.memory.dynamic_areas) asm ( "bkpt 6" );
    da->next = workspace.memory.dynamic_areas;
    workspace.memory.dynamic_areas = da;

    MMU_map_at( (void*) (da->virtual_page << 12), da->start_page << 12, da->pages << 12 );
  }

  { // System heap, one per core (I think)
    // COMPLETELY WRONG! FIXME.
    // This is where system variables are stored by the legacy code
    // UtilityModule requires its presence on initialisation
    extern uint32_t system_heap;

    // This call is not happening!
    DynamicArea *da = rma_allocate( sizeof( DynamicArea ) );
    da = rma_allocate( sizeof( DynamicArea ) );
    if (da == 0) goto nomem;
    da->number = 0;
    da->permissions = 6; // rw-
    da->shared = 0;
    da->virtual_page = ((uint32_t) &system_heap) >> 12;
    da->pages = 256;
    da->actual_pages = da->pages;
    da->start_page = Kernel_allocate_pages( da->actual_pages << 12, 1 << 12 ) >> 12;
    da->handler_routine = 0;

if (da == workspace.memory.dynamic_areas) asm ( "bkpt 6" );
    da->next = workspace.memory.dynamic_areas;
    workspace.memory.dynamic_areas = da;

    MMU_map_at( (void*) (da->virtual_page << 12), da->start_page << 12, da->pages << 12 );

    InitialiseHeap( (void*) (da->virtual_page << 12), da->pages << 12 );
  }
  return;

nomem:
  asm ( "bkpt 11" );
}

static DynamicArea *find_DA( uint32_t n )
{
#ifdef DEBUG__WATCH_DYNAMIC_AREAS
WriteS( "Looking for DA " ); WriteNum( n ); NewLine;
#endif
  DynamicArea *da = workspace.memory.dynamic_areas;
  while (da != 0 && da->number != n) {
    da = da->next;
  }
  if (da == 0) {
    da = shared.memory.dynamic_areas;
    while (da != 0 && da->number != n) {
      da = da->next;
    }
  }
  return da;
}

static inline bool Error_UnknownDA( svc_registers *regs )
{
  static error_block error = { 261, "Unknown dynamic area" };
  regs->r[0] = (uint32_t) &error;
  return false;
}

bool do_OS_ChangeDynamicArea( svc_registers *regs )
{
// https://www.riscosopen.org/forum/forums/11/topics/16963?page=1#posts-129122
#ifdef DEBUG__WATCH_DYNAMIC_AREAS
  WriteS( "Resizing DA " ); WriteNum( regs->r[0] ); WriteS( " caller " ); WriteNum( regs->lr ); NewLine;
#endif

  if (regs->r[0] == 6) {
    // "Free pool" no longer a real DA
    // OK, it appears, from the description in PRM5a-38, that the free pool
    // is the mechanism used to increase and decrease the size of the task
    // slot (application space).
    // For now, just log it and pretend to work
    // Is claiming UpCall 257 the difference between red and green sliders
    // in the task manager? No, there's a DA flag for that (as well?)
#ifdef DEBUG__FREE_POOL
    int32_t resize_by = (int32_t) regs->r[1];
    WriteS( "Free pool: " ); if (resize_by < 0) { WriteS( "-" ); WriteNum( -resize_by ); } else { WriteNum( resize_by ); } NewLine;
#endif
    regs->r[1] = 0; // "Moved"
    return true;
  }


  DynamicArea *da = find_DA( regs->r[0] );
  int32_t resize_by = (int32_t) regs->r[1];
  int32_t resize_by_pages = resize_by >> 12;

  if (da == 0) {
    return Error_UnknownDA( regs );
  }

  if (resize_by == 0) { // Doing nothing
#ifdef DEBUG__WATCH_DYNAMIC_AREAS
  WriteS( "Resizing DA by 0 bytes" ); NewLine;
  WriteS( "What if this call is to ensure the callbacks are called?" ); NewLine;
#endif
    return true;
  }

  if (0 != (resize_by & 0xfff)) {
    resize_by_pages ++;
  }

#ifdef DEBUG__WATCH_DYNAMIC_AREAS
  WriteS( "Resizing DA " ); WriteNum( regs->r[0] ); WriteS( " from " ); WriteNum( da->pages<<12 ); WriteS( " by " ); WriteNum( resize_by_pages << 12 ); WriteS( " (actual = " ); WriteNum( da->actual_pages << 12 ); WriteS( ")" ); NewLine;
#endif

  if (resize_by_pages < 0 && (-resize_by_pages > da->pages)) {
#ifdef DEBUG__WATCH_DYNAMIC_AREAS
    WriteS( "Shrinking DA as much as possible" ); NewLine;
#endif
    resize_by_pages = -da->pages;      // Attempting to reduce the size as much as possible
    resize_by = resize_by_pages << 12;
  }

  if (da->actual_pages != 0 && (da->pages << 12) + resize_by > (da->actual_pages << 12)) {
    static error_block error = { 999, "DA maximum size exceeded" };
    regs->r[0] = (uint32_t) &error;
    asm ( "bkpt 21" );
    return false;
  }

  if (da->handler_routine != 0 && resize_by < 0) {
#ifdef DEBUG__WATCH_DYNAMIC_AREAS
WriteS( "  Pre-shrink " ); WriteNum( resize_by_pages << 12 );
#endif
    register uint32_t code asm ( "r0" ) = 2;
    register uint32_t shrink_by asm ( "r3" ) = -resize_by_pages << 12;
    register uint32_t current_size asm ( "r4" ) = da->pages << 12;
    register uint32_t page_size asm ( "r5" ) = 4096;
    register uint32_t workspace asm ( "r12" ) = da->workarea;
    register uint32_t error asm( "r0" );
    register int32_t permitted asm ( "r3" );
    asm ( "blx %[preshrink]"
        : "=r" (error)
        , "=r" (permitted)
        : "r" (code)
        , "r" (shrink_by)
        , "r" (current_size)
        , "r" (page_size)
        , "r" (workspace)
        , [preshrink] "r" (da->handler_routine) 
        : "lr" );
    if (error != 2) { // pre-shrink code
      regs->r[0] = error;
      return false;
    }
    permitted = -permitted; // FIXME: Non-page multiples
    resize_by_pages = permitted >> 12;
    resize_by = resize_by_pages << 12;
#ifdef DEBUG__WATCH_DYNAMIC_AREAS
WriteS( " permitted = " ); WriteNum( resize_by_pages << 12 ); NewLine;
#endif
  } 

  if (da->handler_routine != 0 && resize_by >= 0) {
#ifdef DEBUG__WATCH_DYNAMIC_AREAS
Write0( "  Pre-grow +" ); WriteNum( resize_by_pages << 12 ); Write0( ", from " ); WriteNum( da->pages << 12 ); Write0( ", routine: " ); WriteNum( da->handler_routine ); NewLine;
#endif
    register uint32_t code asm ( "r0" ) = 0;
    register uint32_t page_block asm ( "r1" ) = 0xbadf00d;
    register uint32_t pages asm ( "r2" ) = resize_by_pages;
    register uint32_t grow_by asm ( "r3" ) = resize_by_pages << 12;
    register uint32_t current_size asm ( "r4" ) = da->pages << 12;
    register uint32_t page_size asm ( "r5" ) = 4096;
    register uint32_t workspace asm ( "r12" ) = da->workarea;
    register uint32_t error asm( "r0" );
    asm ( "blx %[pregrow]"
        : "=r" (error)
        : "r" (code)
        , "r" (page_block)
        , "r" (pages)
        , "r" (grow_by)
        , "r" (current_size)
        , "r" (page_size)
        , "r" (workspace)
        , [pregrow] "r" (da->handler_routine)
        : "lr" );
    if (error != 0) {
      regs->r[0] = error;
      return false;
    }
  } 

  if (da->start_page == 0) {
#ifdef DEBUG__WATCH_DYNAMIC_AREAS
WriteS( " Allocate" );
#endif
    // Give everything a MiB, for now
    uint32_t memory = Kernel_allocate_pages( natural_alignment, natural_alignment );
    da->start_page = memory >> 12;
    da->actual_pages = natural_alignment >> 12;
#ifdef DEBUG__WATCH_DYNAMIC_AREAS
WriteS( " Allocated " ); WriteNum( memory ); NewLine;
#endif
    if (da->shared) {
      MMU_map_shared_at( (void*) (da->virtual_page << 12), da->start_page << 12, da->actual_pages << 12 );
    }
    else {
      MMU_map_at( (void*) (da->virtual_page << 12), da->start_page << 12, da->actual_pages << 12 );
    }
#ifdef DEBUG__WATCH_DYNAMIC_AREAS
WriteS( " Mapped " ); WriteNum( da->virtual_page << 12 ); NewLine;
#endif
  }

  da->pages = da->pages + resize_by_pages; // Always increased (or decreased) to the next largest page

  if (da->handler_routine != 0 && resize_by >= 0) {
#ifdef DEBUG__WATCH_DYNAMIC_AREAS
WriteS( " Post-grow" );
#endif
    register uint32_t code asm ( "r0" ) = 1;
    register uint32_t page_block asm ( "r1" ) = 0xbadf00d;
    register uint32_t pages asm ( "r2" ) = resize_by_pages;
    register uint32_t grown_by asm ( "r3" ) = resize_by_pages << 12;
    register uint32_t current_size asm ( "r4" ) = da->pages << 12;
    register uint32_t page_size asm ( "r5" ) = 4096;
    register uint32_t workspace asm ( "r12" ) = da->workarea;
    register uint32_t error asm( "r0" );
    asm ( "blx %[postgrow]"
        : "=r" (error)
        : "r" (code)
        , "r" (page_block)
        , "r" (pages)
        , "r" (grown_by)
        , "r" (current_size)
        , "r" (page_size)
        , "r" (workspace)
        , [postgrow] "r" (da->handler_routine) 
        : "lr" );
    if (error != 1) { // Changed
      regs->r[0] = error;
      return false;
    }
  }

  if (da->handler_routine != 0 && resize_by < 0) {
#ifdef DEBUG__WATCH_DYNAMIC_AREAS
WriteS( " Post-shrink" );
#endif
    register uint32_t code asm ( "r0" ) = 3;
    register uint32_t grown_by asm ( "r3" ) = resize_by_pages << 12;
    register uint32_t current_size asm ( "r4" ) = da->pages << 12;
    register uint32_t page_size asm ( "r5" ) = 4096;
    register uint32_t workspace asm ( "r12" ) = da->workarea;
    register uint32_t error asm( "r0" );
    asm ( "blx %[postgrow]"
        : "=r" (error)
        : "r" (code)
        , "r" (grown_by)
        , "r" (current_size)
        , "r" (page_size)
        , "r" (workspace)
        , [postgrow] "r" (da->handler_routine) 
        : "lr" );
    if (error != 3) { // Changed
      regs->r[0] = error;
      return false;
    }
  }

  {
    // Service_MemoryMoved
    register uint32_t code asm( "r1" ) = 0x4e;
    asm ( "svc %[swi]"
        :
        : [swi] "i" (OS_ServiceCall | Xbit)
        , "r" (code)
        : "lr", "cc", "memory" );
  }

  regs->r[1] = resize_by_pages << 12;

  return true;
}

bool do_OS_ReadDynamicArea( svc_registers *regs )
{
#ifdef DEBUG__WATCH_DYNAMIC_AREAS
  WriteS( "Reading DA " ); WriteNum( regs->r[0] ); WriteS( ", caller " ); WriteNum( regs->lr ); NewLine;
#endif

  if (regs->r[0] == 0xffffffff) {
    // Special case, PRM 5a-43
    TaskSlot *slot = TaskSlot_now();
    regs->r[0] = 0x8000;
    regs->r[1] = slot == 0 ? 0x8000 : TaskSlot_Himem( slot );
    if (0 != regs->r[1]) regs->r[1] = regs->r[1] - 0x8000;
    regs->r[2] = 0x1fff8000;

    return true;
  }

  DynamicArea *da = find_DA( regs->r[0] & ~(1 << 7) );

  if (da != 0) {
    bool max_size_requested = 0 != (regs->r[0] & (1 << 7));
    if (max_size_requested) {
      regs->r[2] = da->actual_pages << 12;
    }
    regs->r[0] = da->virtual_page << 12;
    regs->r[1] = da->pages << 12;
#ifdef DEBUG__WATCH_DYNAMIC_AREAS
  WriteS( "DA Address " ); WriteNum( regs->r[0] ); NewLine;
  WriteS( "DA Size " ); WriteNum( regs->r[1] ); NewLine;
  if (max_size_requested) {
    WriteS( "DA Max Size " ); WriteNum( regs->r[2] ); NewLine;
  }
#endif
    return true;
  }
  // FIXME Bit 7

  if (regs->r[0] == 6) {
    // "Free pool"
#ifdef DEBUG__FREE_POOL
    WriteS( "Reading Free Pool" ); NewLine;
#endif
    regs->r[0] = 0xbadbad00;
    regs->r[1] = 0xbaadbaad;
    return true;
  }

  return Error_UnknownDA( regs );
}

static char *da_name( DynamicArea *da )
{
  return (char*) (da+1);
}

bool do_OS_DynamicArea( svc_registers *regs )
{
  bool result = true;

  enum { New, Remove, Info, Enumerate, Renumber, NewInfo = 24 };

  bool reclaimed = claim_lock( &shared.memory.dynamic_areas_lock );
  assert( !reclaimed ); // No question, only entered once

  if (shared.memory.last_da_address == 0) {
    // First time in this routine
    extern uint32_t dynamic_areas_base;
    shared.memory.last_da_address = (uint32_t) &dynamic_areas_base;
    shared.memory.user_da_number = 256;
  }

  switch (regs->r[0]) {
  case New:
    { // Create new Dynamic Area
      // Create, size 0 (with callback)
      // Grow to minimum of requested size and maximum size (with callbacks)
      const char *name = (void*) regs->r[8];

#ifdef DEBUG__WATCH_DYNAMIC_AREAS
  WriteS( "New DA " ); WriteNum( regs->r[1] ); WriteS( " caller " ); WriteNum( regs->lr ); NewLine;
  WriteNum( regs->r[6] ); WriteS( " " ); WriteNum( regs->r[7] ); WriteS( " " ); Write0( regs->r[8] ); NewLine;
#endif
      DynamicArea *da = rma_allocate( sizeof( DynamicArea ) + strlen( name ) + 1 );
      if (da == 0) goto nomem;

      strcpy( da_name( da ), name );

      int32_t number = regs->r[1];
      if (number == -1) {
        number = shared.memory.user_da_number++;
        regs->r[1] = number;
#ifdef DEBUG__WATCH_DYNAMIC_AREAS
  WriteS( "DA Allocated number " ); WriteNum( regs->r[1] ); NewLine;
#endif
      }
      da->number = number;

      int32_t max_logical_size = regs->r[5];
      if (max_logical_size == -1) {
        max_logical_size = 16 << 20; // FIXME, but 16 sounds OK for anything written in olden times
        regs->r[5] = max_logical_size;
      }

      int32_t va = regs->r[3];
      if (va == -1) {
        va = shared.memory.last_da_address;
        shared.memory.last_da_address += max_logical_size;
// FIXME: Remove:
shared.memory.last_da_address += (natural_alignment-1);
shared.memory.last_da_address &= ~(natural_alignment-1);
        regs->r[3] = va;
      }

      da->virtual_page = va >> 12;
      da->handler_routine = regs->r[6];
      da->workarea = regs->r[7];
      if (da->workarea == -1) {
        da->workarea = da->virtual_page << 12;
      }

      da->permissions = 6; // rw- FIXME: There's also privileged only...
      // Only non-shared? Depends on module being shared? TODO
      da->shared = 0;
      da->pages = 0;            // Initial state, allocated and expanded by OS_ChangeDynamicArea
      da->start_page = 0;       // Initial state, allocated and expanded by OS_ChangeDynamicArea
      da->actual_pages = 0;     // Initial state, allocated and expanded by OS_ChangeDynamicArea
      da->next = workspace.memory.dynamic_areas;
      workspace.memory.dynamic_areas = da;

#ifdef DEBUG__WATCH_DYNAMIC_AREAS
  WriteS( "DA " ); WriteNum( da->number ); Space; WriteNum( da->virtual_page << 12 ); Space; WriteNum( da->start_page << 12 ); NewLine;
#endif
      if (regs->r[2] > 0) {
        register uint32_t number asm ( "r0" ) = da->number;
        register uint32_t r2 asm ( "r1" ) = regs->r[2];
        error_block *error;

        asm volatile ( "svc %[swi]"
                   "\n  movvc %[error], #0"
                   "\n  movvs %[error], r0"
                   : [error] "=r" (error)
                   : [swi] "i" (OS_ChangeDynamicArea | Xbit)
                   , "r" (number)
                   , "r" (r2)
                   : "lr", "cc", "memory" );

        if (error != 0) {
          regs->r[0] = (uint32_t) error;
          return false;
        }
      }

      {
        // Service_DynamicAreaCreate 5a-50
        register uint32_t code asm( "r1" ) = 0x90;
        register uint32_t area asm( "r2" ) = da->number;
        asm ( "svc %[swi]"
            :
            : [swi] "i" (OS_ServiceCall | Xbit)
            , "r" (code), "r" (area)
            : "lr", "cc", "memory" );
      }

      break;
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
        da = rma_allocate( sizeof( DynamicArea ) );
        if (da == 0) goto nomem;
        da->number = 2;
        da->permissions = 6; // rw-
        da->shared = 1;
        da->virtual_page = ((uint32_t) &frame_buffer) >> 12;
        da->start_page = regs->r[1] >> 12;
        da->pages = regs->r[2] >> 12;
        da->actual_pages = da->pages;
        da->next = shared.memory.dynamic_areas;
        da->handler_routine = 0;
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
      // Note: Used by original RamFS
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
        result = Error_UnknownDA( regs );
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
  case 27:
    {
      WriteS( "Lying to the Wimp (probably) about free memory" );
      regs->r[2] = 500;
    }
    break;
  case Enumerate:
    {
      uint32_t area = regs->r[1]; 
      DynamicArea *da = shared.memory.dynamic_areas;

      if (area == -1) {
        regs->r[1] = da->number;
      }
      else {
        while (da != 0 && da->number != area) {
          da = da->next;
        }

        if (da != 0) da = da->next;
        if (da == 0) da = workspace.memory.dynamic_areas;

        while (da != 0 && da->number != area) {
          da = da->next;
        }

        if (da != 0) da = da->next;
        if (da != 0) regs->r[1] = da->number;
        else regs->r[1] = -1;
      }
    }
    break;
  case NewInfo:
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
        result = Error_UnknownDA( regs );
      }
      else {
/*
R0 	Preserved
R1 	Preserved
R2 	Current size of area, in bytes
R3 	Base logical address of area
R4 	Area flags
R5 	Maximum size of area in bytes
R6 	Current physical size of area, in pages*
R7 	Maximum physical size of area, in pages*
R8 	Pointer to name of area 

 * Different from Info
*/
        regs->r[2] = da->pages << 12;
        regs->r[3] = da->start_page << 12;
        regs->r[4] = 0; // FIXME
        regs->r[5] = da->pages << 12; // FIXME
        regs->r[6] = da->pages;
        regs->r[7] = da->pages;
        regs->r[8] = (uint32_t) "Need to name DAs"; // FIXME
      }
    }
    break;
  default:
    {

WriteS( "OS_DynamicArea " ); WriteNum( regs->r[0] ); WriteS( " " ); WriteNum( regs->r[1] ); NewLine;
for (;;) { asm ( "bkpt 78" ); }
      static error_block error = { 0x997, "Cannot do anything to DAs" };
      regs->r[0] = (uint32_t) &error;
      result = false;
    }
    break;
  }

  if (!reclaimed) release_lock( &shared.memory.dynamic_areas_lock );

  return result;

nomem:
  for (;;) { asm ( "bkpt 31" ); }
  if (!reclaimed) release_lock( &shared.memory.dynamic_areas_lock );
  return false;
}

bool do_OS_Memory( svc_registers *regs )
{
  // Code calling this SWI is unlikely to be compatible with the C kernel!
  typedef struct {
    uint32_t physical_page;
    uint32_t virtual_address;
    uint32_t physical_address;
  } page_block;

  switch (regs->r[0] & 0xff) {
  case 0:
    {
    // General page block operations
    if (regs->r[0] == ((1 << 9) | (1 << 13))) {
      // Given virtual address, provide physical.
      page_block *blocks = (void*) regs->r[1];
      for (int i = 0; i < regs->r[2]; i++) {
        asm ( "mcr p15, 0, %[va], c7, c8, 0\n  mrc p15, 0, %[pa], c7, c4, 0" 
          : [pa] "=r" (blocks[i].physical_address) 
          : [va] "r" (blocks[i].virtual_address) );
        blocks[i].physical_address = (blocks[i].physical_address & ~0xfff) | (blocks[i].virtual_address & 0xfff);
Write0( "OS_Memory operation " ); WriteNum( regs->r[0] ); Write0( " VA " ); WriteNum( blocks[i].virtual_address ); Write0( " PA " ); WriteNum( blocks[i].physical_address ); NewLine;
      }
    }
    else {
      Write0( "Unsupported OS_Memory operation" ); NewLine;
      for (;;) { asm ( "wfi" ); }
    }
    return true;
    }
    break;
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
    asm ( "bkpt %[line]" : : [line] "i" (__LINE__) );
  }
  return Kernel_Error_UnimplementedSWI( regs );
}


void Kernel_add_free_RAM( uint32_t base_page, uint32_t size_in_pages )
{
  bool reclaimed = claim_lock( &shared.memory.lock );
  assert( !reclaimed );

  free_block *p = (free_block *) shared.memory.free_blocks;

  while (p->size != 0) {
    p++;
  }

  p->base_page = base_page;
  p->size = size_in_pages;

  if (!reclaimed) release_lock( &shared.memory.lock );
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

// Or a red-black tree of free pages, whose maximum size would be a node
// for each of the odd- or even-numbered pages (half allocated, half freed). 

uint32_t Kernel_allocate_pages( uint32_t size, uint32_t alignment )
{
  uint32_t result = -1;
  uint32_t size_in_pages = size >> 12;
  uint32_t alignment_in_pages = alignment >> 12;

  bool reclaimed = claim_lock( &shared.memory.lock );
  assert( !reclaimed ); // IDK, seems sus.

  free_block *p = (free_block *) shared.memory.free_blocks;

  while (p->size != 0
      && (!aligned( p->base_page, alignment_in_pages )
       || p->size < size_in_pages)) {
    p++;
  }

  if (p->size == 0) {
    // Find a big enough block to split, and take the aligned part off into another free block
    // Note: p points to a free entry

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

  if (!reclaimed) release_lock( &shared.memory.lock );

  assert( result != -1 );

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
  uint32_t *sp;
  asm volatile ( "push {r0-r12, r14}\n  mov %[sp], sp" : [sp] "=r" (sp) );
  //Write0( __func__ ); NewLine;
  //for (int i = 0; i < 32; i++) { WriteNum( sp[i] ); if (0 == (i & 3)) NewLine; else Space; }
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

// Begin FPEmulator hack to get Wimp_StartTask to complete
void __attribute__(( noinline )) UndefinedInstruction( uint32_t regs[6], uint32_t instruction )
{
  // This breaks things: 
  // WriteS( "Undefined instruction at " ); WriteNum( regs[5] ); NewLine;

  // This a a do-nothing implementation.
}
// End FPEmulator hack to get Wimp_StartTask to complete

void __attribute__(( naked, noreturn )) Kernel_default_undef()
{
  uint32_t *regs;
  uint32_t instruction;

  // Return address is the instruction following the undefined one,
  // no need to change it.
  asm volatile (
        "srsdb sp!, #0x1b // Store return address and SPSR (UND mode)" 
    "\n  push { "C_CLOBBERED" }"
    "\n  mov %[regs], sp"
    "\n  ldr %[inst], [lr, #-4]"
    : [regs] "=r" (regs)
    , [inst] "=r" (instruction) );

  UndefinedInstruction( regs, instruction );

  asm ( "pop { "C_CLOBBERED" }"
    "\n  rfeia sp! // Restore (modified) execution and SPSR"
      );
}

void __attribute__(( naked, noreturn )) Kernel_default_reset() 
{
  // When providing proper implementation, ensure the called routine is __attribute__(( noinline ))
  // noinline attribute is required so that stack space is allocated for any local variables.
  BSOD( 3, Red );
}

