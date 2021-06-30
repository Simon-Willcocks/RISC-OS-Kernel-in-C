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

startup boot_data = { 0 };

// Simple synchronisation routines to be used before the MMU is
// enabled. They make use of the fact that cores may atomically update
// a word in memory. Each core may write to its own element of the
// array, and read the other elements'.
// They require write access to the "ROM", so only work before the MMU
// is initialised.

enum { CORES_AT_BOOT_START = 1, CORES_RUNNING_AT_NEW_LOCATION };

static inline void at_checkpoint( volatile uint32_t *states, int core, int checkpoint )
{
  states[core] = checkpoint;
  while (states[0] != checkpoint) {}
}

static inline void release_from_checkpoint( volatile uint32_t *states, int checkpoint )
{
  states[0] = checkpoint;
}

static inline void wait_for_cores_to_reach( volatile uint32_t *states, int max_cores, int checkpoint )
{
  int done;
  do {
    done = 1;
    for (int i = 1; i < max_cores; i++) {
      if (states[i] != checkpoint) {
        done = 0;
      }
    }
  } while (!done);
}

// Minimum RAM, to start with. More can be added to pool later, if available.
static const uint32_t top_of_ram = 64 << 20;
extern int rom_size;
static const uint32_t size_of_rom = (uint32_t) &rom_size; // 5 << 20;

static uint32_t relocate_as_necessary( uint32_t start, volatile startup *startup );
uint32_t pre_mmu_allocate_physical_memory( uint32_t size, uint32_t alignment, volatile startup *startup );
void __attribute__(( noreturn, noinline )) pre_mmu_with_stacks( core_workspace *ws, uint32_t max_cores, volatile startup *startup );

void __attribute__(( naked )) locate_rom_and_enter_kernel( uint32_t start );

// The whole point of this routine is to be linked at the start of the execuable, and
// to pass the actual location of the first byte of the loaded "ROM" to the next
// routine.
void __attribute__(( naked, section( ".text.init" ), noinline )) _start()
{
  register uint32_t start asm( "r0" );

  asm ( "adr %[loc], _start" : [loc] "=r" (start) ); // Guaranteed PC relative

  locate_rom_and_enter_kernel( start );
}

void __attribute__(( naked )) locate_rom_and_enter_kernel( uint32_t start )
{
  volatile startup *startup = (void*) (((uint8_t*) &boot_data) - ((uint8_t*) _start) + start);

  uint32_t core_number = get_core_number();

  uint32_t volatile *states = (uint32_t*) (top_of_ram / 2); // Assumes top_of_ram > 2 * size_of_rom
  uint32_t max_cores = 0;

  if (core_number == 0) {
    asm volatile( "mov sp, %[stack]" : : [stack] "r" (states) );

    max_cores = number_of_cores();
    for (uint32_t i = 0; i < max_cores; i++) {
      states[i] = 0;
    }

    // Free other cores to indicate they're at CORES_AT_BOOT_START
    startup->states_initialised = true;

    wait_for_cores_to_reach( states, max_cores, CORES_AT_BOOT_START );

    startup->relocation_offset = relocate_as_necessary( start, startup );

    // Other cores are blocked, waiting for the old location of states[0] to change)
    // Release them before starting to work with the potentially new location
    release_from_checkpoint( states, CORES_AT_BOOT_START );

    // Now, we all rush to enter the potentially relocated code
  }
  else {
    while (!startup->states_initialised) {}
    at_checkpoint( states, core_number, CORES_AT_BOOT_START );
  }

  if (startup->relocation_offset != 0) {
    uint32_t relocated_startup = startup->relocation_offset + (uint32_t) startup;

    asm goto (
        "\n  adr lr, %l[relocated]"
        "\n  add lr, lr, %[offset]"
        "\n  mov pc, lr"

        : 
        : [offset] "r" (startup->relocation_offset)
        :
        : relocated );
relocated:
    startup = (volatile void*) relocated_startup;
  }

  uint32_t core_workspace_space = (sizeof( core_workspace ) + 0xfff) & ~0xfff;

  if (core_number == 0) {
    // OK, now running in this routine at the potentially new location

    uint32_t space_needed = (core_workspace_space * max_cores);

    while (space_needed >= startup->ram_blocks[0].size) { asm( "wfi" ); } // This is never going to happen

    startup->core_workspaces = pre_mmu_allocate_physical_memory( space_needed, 4096, startup );

    startup->shared_memory = pre_mmu_allocate_physical_memory( sizeof( shared_workspace ), 4096, startup );

    {
      uint32_t *p = (void*) startup->shared_memory;
      for (int i = 0; i < sizeof( shared_workspace ) / 4; i++) {
        p[i] = 0;
      }
    }

uint32_t volatile *gpio = (uint32_t*) 0x3f200000;
gpio[2] = (gpio[2] & ~(3 << 6)) | (1 << 6); // Output, pin 22
gpio[0x1c/4] = (1 << 22); // Set
uint32_t volatile *mailbox = (uint32_t*) 0x3f00b880;
uint32_t width = 1920;
uint32_t height = 1080;
uint32_t vwidth = 1920;
uint32_t vheight = 1080;
const uint32_t __attribute__(( aligned( 16 ) )) tags[36] =
{ sizeof( tags ), 0, 0x00028001, 8, 0, 1, 3, 0x00028001, 8, 0, 2, 3, 
          // Tags: Tag, buffer size, request code, buffer
          0x00040001, // Allocate buffer
          8, 0, 2 << 20, 0, // Size, Code, In: Alignment, Out: Base, Size
          0x00048003, // Set physical (display) width/height
          8, 0, width, height,
          0x00048004, // Set virtual (buffer) width/height
          8, 0, vwidth, vheight,
          0x00048005, // Set depth
          4, 0, 32,
          0x00048006, // Set pixel order
          4, 0, 0,    // 0 = BGR, 1 = RGB
0 };
mailbox[8] = 8 | (uint32_t)tags;
while ((mailbox[6] & (1 << 30)) != 0) {}


uint32_t *s = tags[15] & ~0xc0000000;
for (int i = 0; i < 1920*1000; i++) { s[i] = 0xffff8888; }

/*
uint32_t volatile *uart = (uint32_t*) 0x3f201000;
uart[0x30/4] = 0; // Disable
uart[0x24/4] = 6; // IBRD
uart[0x28/4] = 62; // FBRD
uart[0x30/4] = 0b1100000001; // Enable: TX, RX, no flow control
uart[0] = 'S';

for (int i = 0; i < 10000000; i++) { asm ( "" ); }
gpio[0x28/4] = (1 << 22); // Clr

bool on = false;
for (;;) {
while (uart[0x18/4] & (1 << 4)) { }
uart[0] = uart[0] + 1;
if (on) 
gpio[0x28/4] = (1 << 22); // Clr
else
gpio[0x1c/4] = (1 << 22); // Set
on = !on;
}
*/

    wait_for_cores_to_reach( states, max_cores, CORES_RUNNING_AT_NEW_LOCATION );

    // Now all cores are at the new location, so the RAM outside the "ROM" area can be used
    release_from_checkpoint( states, CORES_RUNNING_AT_NEW_LOCATION );
  }
  else {
    at_checkpoint( states, core_number, CORES_RUNNING_AT_NEW_LOCATION );
  }

  {
    // Clear out workspaces (in parallel)
    core_workspace *ws = (void*) (startup->core_workspaces + core_number * core_workspace_space);
    uint32_t *p = (uint32_t *) ws;
    for (int i = 0; i < core_workspace_space/sizeof( uint32_t ); i++) {
      p[i] = 0;
    }
    ws->core_number = core_number;

    asm ( "  mov sp, %[stack]" : : [stack] "r" (sizeof( ws->kernel.svc_stack ) + (uint32_t) &ws->kernel.svc_stack) );
    pre_mmu_with_stacks( ws, max_cores, startup );
  }

  __builtin_unreachable();
}

static void *memcpy(void *dest, const void *src, size_t n)
{
  uint32_t *d = dest;
  const uint32_t *s = src;
  
  // No check for alignment or non-word size
  for (n = n / sizeof( uint32_t ); n > 0; n--) {
    *d++ = *s++;
  }

  return dest; // Standard behaviour, ignored.
}


static uint32_t __attribute__(( noinline )) relocate_as_necessary( uint32_t start, volatile startup *startup )
{
  // No MMU, no cache, small stack available.

  startup->final_location = start;

  if (!naturally_aligned( start )) {
    // Needs relocating to somewhere, put it to top or bottom of RAM,
    // whichever doesn't overlap with the current location

    if (start < size_of_rom) // Can't go to bottom of memory (source and destination overlap)
      startup->final_location = top_of_ram - size_of_rom; // Top, instead
    else
      startup->final_location = 0;
  }

  // Physical location of (copy of) the ROM is either at 0, its original
  // location, or top of ram. Whichever, it is "naturally" aligned (so the MMU
  // can map it easily in large chunks).

  int free_block = 0;
  // If the final location is at the top or bottom of memory, there will be
  // one initial block of free memory, if it's still in the middle, there will
  // be two.

  if (startup->final_location > 0) {
    startup->ram_blocks[free_block].base = 0;
    startup->ram_blocks[free_block].size = startup->final_location;
    free_block++;
  }

  if (startup->final_location + size_of_rom < top_of_ram) {
    startup->ram_blocks[free_block].base = startup->final_location + size_of_rom;
    startup->ram_blocks[free_block].size = top_of_ram - startup->ram_blocks[free_block].base;
    free_block++;
  }

  // May add further blocks of RAM, here, but it's better to do it once the kernel is running.

  // Now entries in startup structure (stored in the "ROM" image) have been finalised, we can copy
  // the whole lot to the new location ready to be jumped to.

  if (startup->final_location != start) {
    memcpy( (void*) startup->final_location, (const void*) start, size_of_rom );
  }

  return startup->final_location - start;
}

// Currently only copes with two alignments/sizes, probably good enough...
static uint32_t allocate_physical_memory( uint32_t size, uint32_t alignment, ram_block *block )
{
  uint32_t result = 1;
  if (block->size >= size && 0 == (block->base & (alignment - 1))) {
    result = block->base;
    block->size -= size;
    block->base += size;
  }
  return result;
}

uint32_t pre_mmu_allocate_physical_memory( uint32_t size, uint32_t alignment, volatile startup *startup )
{
  // Always allocate a least one full page.
  if (0 != (size & 0xfff)) size = (size + 0xfff) & ~0xfff;

  if (startup->less_aligned.size == 0 && 0 != (startup->ram_blocks[0].base & (alignment - 1))) {
    uint32_t misalignment = alignment - (startup->ram_blocks[0].base & (alignment - 1));
    startup->less_aligned.base = startup->ram_blocks[0].base;
    startup->less_aligned.size = misalignment;
    startup->ram_blocks[0].size -= misalignment;
    startup->ram_blocks[0].base += misalignment;
  }

  uint32_t result = allocate_physical_memory( size, alignment, (ram_block*) &startup->less_aligned );
  int block = 0;

  while (0 != (result & 1) && 0 != startup->ram_blocks[block].size) {
    result = allocate_physical_memory( size, alignment, (ram_block*) &startup->ram_blocks[block++] );
  }

  return result;
}

void BOOT_finished_allocating( uint32_t core, volatile startup *startup )
{
  startup->core_entered_mmu = core;
}

void __attribute__(( noreturn, noinline )) pre_mmu_with_stacks( core_workspace *ws, uint32_t max_cores, volatile startup *startup )
{
  // We're running in RAM, at a naturally aligned location, with no MMU (but possibly cached instructions)
  // The MMU is not running yet, which can cause problems with synchronisation primitives not working.

  // Instead, core 0 will release the cores one at a time, so they can safely allocate memory without
  // concurrency problems, establish an MMU, and use proper synchronisation primitives.

  // The pointers passed to this routine are to absolute physical memory.

  if (ws->core_number == 0) {
    for (int i = 1; i < max_cores; i++) {
      startup->core_to_enter_mmu = i;
      while (i != startup->core_entered_mmu) {}
    }
    startup->core_to_enter_mmu = 0;
  }
  else {
    while (startup->core_to_enter_mmu != ws->core_number) {}
  }

  // Allocate memory pre-MMU, call BOOT_finished_allocating, maps kernel
  // workspace and translation tables into virtual memory, and finally
  // jump to Kernel_start in virtual memory.
  MMU_enter( ws, startup );
}

