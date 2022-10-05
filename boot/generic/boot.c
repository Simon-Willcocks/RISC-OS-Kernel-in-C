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

char const build_time[] = "C kernel built: " __DATE__ " " __TIME__ ;

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
extern const uint32_t minimum_ram;
static const uint32_t top_of_ram = (uint32_t) &minimum_ram;
extern int rom_size;
static const uint32_t size_of_rom = (uint32_t) &rom_size; // 5 << 20;

static uint32_t relocate_as_necessary( uint32_t start, volatile startup *startup );
uint32_t pre_mmu_allocate_physical_memory( uint32_t size, uint32_t alignment, volatile startup *startup );
void __attribute__(( noreturn, noinline )) pre_mmu_with_stacks( core_workspace *ws, uint32_t max_cores, volatile startup *startup );

void __attribute__(( noreturn )) locate_rom_and_enter_kernel( uint32_t start, uint32_t core_number, uint32_t volatile *states );

// The whole point of this routine is to be linked at the start of the execuable, and
// to pass the actual location of the first byte of the loaded "ROM" to the next
// routine.
void __attribute__(( naked, section( ".text.init" ), noinline, noreturn )) _start()
{
  register uint32_t start asm( "r0" );

  asm ( "adr %[loc], _start" : [loc] "=r" (start) ); // Guaranteed PC relative

  uint32_t core_number = get_core_number();

  // Assumes top_of_ram > 2 * size_of_rom and that the ROM
  // is loaded near the top or bottom of RAM.
  uint32_t volatile *states = (uint32_t*) (top_of_ram / 2);
  uint32_t const tiny_stack_size = 4096;

  // Allocate a tiny stack per core in RAM that is currently unused.
  asm volatile( "mov sp, %[stack]" : : [stack] "r" (states - core_number * tiny_stack_size) );

  locate_rom_and_enter_kernel( start, core_number, states );

  __builtin_unreachable();
}

void __attribute__(( noreturn )) locate_rom_and_enter_kernel( uint32_t start, uint32_t core_number, uint32_t volatile *states )
{
  volatile startup *startup = (void*) (((uint8_t*) &boot_data) - ((uint8_t*) _start) + start);

  uint32_t max_cores = 0;

  if (core_number == 0) {

#if 0
uint32_t *gpio = (void*) 0x3f200000;
// Set gpios 22 and 27 (physical pins 15 and 13) to output
gpio[2] = (gpio[2] & ~(7 << (2 * 3))) | (1 << (2 * 3));
gpio[2] = (gpio[2] & ~(7 << (7 * 3))) | (1 << (7 * 3));
for (;;){
asm ( "dsb sy" );
for (int i = 0; i < 1000; i++) asm ( "nop" );
gpio[0x1c/4] = (1 << 22);
gpio[0x28/4] = (1 << 27);
asm ( "dsb sy" );
for (int i = 0; i < 2000; i++) asm ( "nop" );
gpio[0x28/4] = (1 << 22);
gpio[0x1c/4] = (1 << 27);
}
#endif

    // Identify the kind of processor we're working with.
    // The overall system (onna chip) will be established later.
    max_cores = pre_mmu_identify_processor();
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

    wait_for_cores_to_reach( states, max_cores, CORES_RUNNING_AT_NEW_LOCATION );

    // Now all cores are at the new location, so the RAM outside the "ROM" area can be used
    release_from_checkpoint( states, CORES_RUNNING_AT_NEW_LOCATION );
  }
  else {
    at_checkpoint( states, core_number, CORES_RUNNING_AT_NEW_LOCATION );
  }

  core_workspace *ws = (void*) (startup->core_workspaces + core_number * core_workspace_space);

  memset( ws, 0, sizeof( core_workspace ) );
  ws->core_number = core_number;

  asm ( "  mov sp, %[stack]" : : [stack] "r" (sizeof( ws->kernel.svc_stack ) + (uint32_t) &ws->kernel.svc_stack) );

  pre_mmu_with_stacks( ws, max_cores, startup );

  __builtin_unreachable();
}

static void copy(void *dest, const void *src, size_t n)
{
  uint32_t *d = dest;
  const uint32_t *s = src;
  
  // No check for alignment or non-word size
  for (n = n / sizeof( uint32_t ); n > 0; n--) {
    *d++ = *s++;
  }
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
    copy( (void*) startup->final_location, (const void*) start, size_of_rom );
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

// Duplicated and modified from memory/simple/memory_manager.c
// Later implementations are likely to be more complicated, but this is good enough for booting.
static bool aligned( uint32_t b, uint32_t alignment )
{
  return 0 == (b & (alignment - 1));
}

static uint32_t misalignment( uint32_t b, uint32_t alignment )
{
  return alignment - (b & (alignment - 1));
}

static uint32_t allocate_pages( uint32_t size, uint32_t alignment, ram_block *blocks )
{
  uint32_t result = -1;

  ram_block *p = blocks;

  while (p->size != 0
      && (!aligned( p->base, alignment )
       || p->size < size)) {
    p++;
  }

  if (p->size == 0) {
    // Find a big enough block to split, and take the aligned part off into another free block
    // Note: p points to a free entry

    ram_block *big = blocks;
    while (big->size != 0
        && big->size < size + misalignment( big->base, alignment )) {
      big++;
    }

    if (big->size != 0) {
      uint32_t mis = misalignment( big->base, alignment );
      p->size = big->size - mis;
      p->base = big->base + mis;
      big->size = mis;
    }
  }

  if (p->size != 0) {
    result = p->base;
    p->base += size;
    p->size -= size;

    if (p->size == 0) {
      do {
        *p = *(p+1);
        p++;
      } while (p->size != 0);
    }
  }

  return result;
}

uint32_t pre_mmu_allocate_physical_memory( uint32_t size, uint32_t alignment, volatile startup *startup )
{
  // Always allocate a least one full page.
  if (0 != (size & 0xfff)) size = (size + 0xfff) & ~0xfff;

  return allocate_pages( size, alignment, startup->ram_blocks );
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

  // Before doing any MMU stuff, establish that all cores are part of an SMP system
  set_smp_mode();

  if (ws->core_number == 0) {
    shared_workspace *shared_memory = (void*) startup->shared_memory;

    // Block other cores from continuing until core 0 has enabled the MMU
    shared_memory->kernel.boot_lock = 1; // lock is claimed by core 0 FIXME: knowledge of implementation of claim_lock

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

