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
  DynamicArea *next;
};

void Initialise_system_DAs()
{
  // This isn't set in stone, but first go:

  // System Heap (per core, initially zero sized, I don't know what uses it)
  // RMA (shared, but not protected)
  // Screen (shared, not protected)
  // Sprite Area (per core)

  // Create a Relocatable Module Area, and initialise a heap in it.

  uint32_t initial_rma_size = natural_alignment;
  svc_registers regs;

  claim_lock( &shared.memory.dynamic_areas_lock );

  if (shared.memory.dynamic_areas == 0) {
    // First core here (need not be core zero)

    shared.memory.rma_memory = -1;

    // But there may not be any memory to allocate, yet...
    while (-1 == shared.memory.rma_memory) {
      shared.memory.rma_memory = Kernel_allocate_pages( natural_alignment, natural_alignment );
    }

    MMU_map_at( &rma_heap, shared.memory.rma_memory, initial_rma_size );

    regs.r[0] = 0;
    regs.r[1] = (uint32_t) &rma_heap;
    regs.r[3] = initial_rma_size;

    if (!do_OS_Heap( &regs )) {
      for (;;) { asm ( "wfi" ); }
    }
    // RMA heap initialised, can call rma_allocate

    { // RMA
      DynamicArea *da = (void*) rma_allocate( sizeof( DynamicArea ), &regs );
      if (da == 0) goto nomem;
      da->number = 1;
      da->permissions = 7; // rwx
      da->shared = 1;
      da->virtual_page = ((uint32_t) &rma_base) >> 12;
      da->start_page = shared.memory.rma_memory >> 12;
      da->pages = initial_rma_size >> 12;
      da->next = shared.memory.dynamic_areas;
      shared.memory.dynamic_areas = da;
    }

// I want to get the screen available early in the programming process, it will properly be done in a module
uint32_t volatile * const gpio = (void*) 0xfff40000;
uint32_t volatile * const mbox = (void*) 0xfff41000;
MMU_map_at( (void*) gpio, 0x3f200000, 4096 );
MMU_map_at( (void*) mbox, 0x3f00b000, 4096 );

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

    { // Screen (currently a hack FIXME)
      extern uint32_t frame_buffer;

      DynamicArea *da = (void*) rma_allocate( sizeof( DynamicArea ), &regs );
      if (da == 0) goto nomem;
      da->number = 2;
      da->permissions = 6; // rw-
      da->shared = 1;
      da->virtual_page = ((uint32_t) &frame_buffer) >> 12;
      da->start_page = shared.memory.TEMPORARY_screen >> 12;
      da->pages = (8 << 20) >> 12; // Allows access to slightly more RAM than needed, FIXME (1920x1080x4 = 0x7e9000)
      da->next = shared.memory.dynamic_areas;
      shared.memory.dynamic_areas = da;

      MMU_map_at( (void*) (da->virtual_page << 12), da->start_page << 12, da->pages << 12 );

{ uint32_t *s = &frame_buffer;
for (int i = 0; i < 1920*1000; i++) { s[i] = 0xffff0000 + (0xffff >> (4 *workspace.core_number)) ; } }
    }
  }
  else {
    // Map the shared areas into core's virtual memory map
    MMU_map_at( &rma_heap, shared.memory.rma_memory, initial_rma_size );


    DynamicArea *da = shared.memory.dynamic_areas;
    while (da != 0) {
      MMU_map_at( (void*) (da->virtual_page << 12), da->start_page << 12, da->pages << 12 );
      da = da->next;
    }
  }

  release_lock( &shared.memory.dynamic_areas_lock );

{
extern uint32_t frame_buffer;
uint32_t *screen = &frame_buffer;
for (int y = 10; y < 1020; y++) {
for (int i = 0; i < 64; i++) {
  screen[(100 * (workspace.core_number + 1) + i) + y * 1920] = 0xff000000 | (0xfff << (3 * workspace.core_number));
}
}
  // for (;;) { asm ( "wfi" ); }
}

  // Now the non-shared DAs, can be done in parallel

  { // System heap
    extern uint32_t system_heap;

    DynamicArea *da = (void*) rma_allocate( sizeof( DynamicArea ), &regs );
    if (da == 0) goto nomem;
    da->number = 0;
    da->permissions = 6; // rw-
    da->shared = 0;
    da->virtual_page = ((uint32_t) &system_heap) >> 12;
    da->start_page = 0;
    da->pages = 0;
    da->next = workspace.memory.dynamic_areas;
    workspace.memory.dynamic_areas = da;
  }
  return;

nomem:
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
  static error_block error = { 0x998, "Cannot read size of DAs" };
  regs->r[0] = (uint32_t) &error;
  return false;
}

bool do_OS_DynamicArea( svc_registers *regs )
{
  static error_block error = { 0x997, "Cannot do anything to DAs" };
  regs->r[0] = (uint32_t) &error;
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
