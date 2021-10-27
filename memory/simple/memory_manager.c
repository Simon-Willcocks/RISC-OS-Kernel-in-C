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

///// DEBUG code only.

static inline void initialise_frame_buffer()
{
// I want to get the screen available early in the programming process, it will properly be done in a module
uint32_t volatile * const gpio = (void*) 0xfff40000;
uint32_t volatile * const mbox = (void*) 0xfff41000;
MMU_map_device_shared_at( (void*) gpio, 0x3f200000, 4096 );
MMU_map_device_at( (void*) mbox, 0x3f00b000, 4096 );

gpio[2] = (gpio[2] & ~(3 << 6)) | (1 << 6); // Output, pin 22
      gpio[0x28/4] = (1 << 22); // Clr
extern startup boot_data;

uint32_t volatile *mailbox = &mbox[0x220];
static const uint32_t __attribute__(( aligned( 16 ) )) volatile tags[26] =
{ sizeof( tags ), 0, // 0x00028001, 8, 0, 1, 3, 0x00028001, 8, 0, 2, 3,
          // Tags: Tag, buffer size, request code, buffer
          0x00040001, // Allocate buffer
          8, 0, 2 << 20, 0, // Size, Code, In: Alignment, Out: Base, Size
          0x00048003, // Set physical (display) width/height
          8, 0, 1920, 1080,
          0x00048004, // Set virtual (buffer) width/height
          8, 0, 1920, 1080,
          0x00048005, // Set depth
          4, 0, 32,
          0x00048006, // Set pixel order
          4, 0, 0,    // 0 = BGR, 1 = RGB
0 };
extern uint32_t va_base;
mailbox[8] = 8 | (boot_data.final_location + (uint32_t) tags - (uint32_t) &va_base);
uint32_t count = 0;
uint32_t const toggle = (1 << 26);
while ((mailbox[6] & (1 << 30)) != 0) {
  if (((++count) & (toggle - 1)) == 0) {
    if ((count & toggle) != 0) {
      gpio[0x1c/4] = (1 << 22); // Set
    }
    else {
      gpio[0x28/4] = (1 << 22); // Clr
    }
  }
}
      gpio[0x1c/4] = (1 << 22); // Set
uint32_t s = (tags[5] & ~0xc0000000);

shared.memory.TEMPORARY_screen = s;
asm ( "dsb sy" );
}

static void fill_rect( uint32_t left, uint32_t top, uint32_t w, uint32_t h, uint32_t c )
{
  extern uint32_t frame_buffer;
  uint32_t *screen = &frame_buffer;

  for (uint32_t y = top; y < top + h; y++) {
    uint32_t *p = &screen[y * 1920 + left];
    for (int x = 0; x < w; x++) { *p++ = c; }
  }
}

///// End DEBUG code




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

  if (shared.memory.dynamic_areas == 0) {
    // For some reason this doesn't work on real hardware if the SLVK is set up first
    // Lack of memory? IDK.
    initialise_frame_buffer();
  }
// While we're hacking like crazy, let's allocate far too much memory for RO kernel workspace...
// See comments to GSTrans in swis.c

// This is the mechanism used by Kernel SWIs to return to callers, not normal modules...

uint32_t memory = Kernel_allocate_pages( natural_alignment, natural_alignment );
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

    { // Screen (currently a hack FIXME)
      extern uint32_t frame_buffer;

      DynamicArea *da = rma_allocate( sizeof( DynamicArea ), &regs );
      if (da == 0) goto nomem;
      da->number = 2;
      da->permissions = 6; // rw-
      da->shared = 1;
      da->virtual_page = ((uint32_t) &frame_buffer) >> 12;
      da->start_page = shared.memory.TEMPORARY_screen >> 12;
      da->pages = (8 << 20) >> 12; // Allows access to slightly more RAM than needed, FIXME (1920x1080x4 = 0x7e9000)
      da->next = shared.memory.dynamic_areas;
      shared.memory.dynamic_areas = da;

      MMU_map_shared_at( (void*) (da->virtual_page << 12), da->start_page << 12, da->pages << 12 );
show_word( 10, 10, workspace.core_number, Yellow );

// Cache info...
int y = 20;
uint32_t reg;
asm ( "mrc p15, 0, %[reg], c0, c1, 4" : [reg] "=r" (reg) ); show_word( 10, y, reg, Yellow ); y += 10;
asm ( "mrc p15, 0, %[reg], c0, c1, 5" : [reg] "=r" (reg) ); show_word( 10, y, reg, Yellow ); y += 10;
asm ( "mrc p15, 0, %[reg], c0, c1, 6" : [reg] "=r" (reg) ); show_word( 10, y, reg, Yellow ); y += 10;
asm ( "mrc p15, 0, %[reg], c0, c1, 7" : [reg] "=r" (reg) ); show_word( 10, y, reg, Yellow ); y += 10;
asm ( "mrc p15, 0, %[reg], c0, c2, 6" : [reg] "=r" (reg) ); show_word( 10, y, reg, Yellow ); y += 10;
asm ( "mrc p15, 0, %[reg], c0, c3, 6" : [reg] "=r" (reg) ); show_word( 10, y, reg, Yellow ); y += 10;

asm ( "mrc p15, 1, %[reg], c0, c0, 0" : [reg] "=r" (reg) ); show_word( 10, y, reg, Blue ); y += 10;
clean_cache_to_PoC();
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

  // fill_rect( (100 + 100 * workspace.core_number), 10, 64, 100, 0xff00ff00 );

  // Now the non-shared DAs, can be done in parallel

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
  fill_rect( (100 + 100 * workspace.core_number), 10, 64, 100, 0xffff0000 );
  for (;;) { asm ( "wfi" ); }
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
      register uint32_t result asm ( "r0" );
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
      for (;;) { asm( "wfi" ); }
    }
    break;
  default:
    {
      static error_block error = { 0x997, "Cannot do anything to DAs" };
      regs->r[0] = (uint32_t) &error;
      result = false;
    }
    break;
  }

  release_lock( &shared.memory.dynamic_areas_lock );

  return result;

nomem:
  for (;;) { asm ( "wfi" ); }
  release_lock( &shared.memory.dynamic_areas_lock );
  return false;
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
#define H(n) (150 + (n * 250))

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
  fill_rect( 100 + n + W, 4 * n, 84, 6, c ); \
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
