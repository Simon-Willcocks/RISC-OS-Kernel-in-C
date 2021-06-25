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


// There are essentially three areas in the memory map: low, medium and high.
// The low memory is where applications are mapped in (and the Wimp switches them in and out)
// The medium memory is where the RMA and DAs are stored; they're shared between applications (and cores)
// The high memory is where the OS sits, and device memory is mapped in, it is shared and practically static.

// The OS does not have to care about how the memory is managed, just that it is mapped in or
// out as required, and that the caches can be cleaned.

// To support lazy mapping, the kernel will provide a service to the MMU code, to map a virtual
// address on request, or the whole map.

// Where items are located in virtual memory is controlled by the linker script, but pages that are
// writable by the kernel will often be located in the same section of memory as the vector table.

typedef struct physical_memory_block { // All 4k pages
  uint32_t virtual_base;
  uint32_t physical_base;
  uint32_t size;
} physical_memory_block;

uint32_t pre_mmu_allocate_physical_memory( uint32_t size, uint32_t alignment, volatile startup *startup );

physical_memory_block Kernel_physical_address( uint32_t map, uint32_t va );
uint32_t Kernel_allocate_physical_memory( uint32_t size, uint32_t alignment );
void __attribute__(( noreturn )) Kernel_start();

static inline bool naturally_aligned( uint32_t location )
{
  // 1MB sections, with this MMU
  return (((1 << 20)-1) & location) == 0;
}

// An instance of this struct will be in the core workspace, called mmu:
struct MMU_workspace {
  uint32_t *l1tt_pa;
  uint32_t *l2tt_pa; // 4 tables, of 256 entries each
};

struct MMU_shared_workspace { // Placeholder
};

void MMU_new_low_map( void *map );
void MMU_switch_to_map( void *map );

void BOOT_finished_allocating( uint32_t core, volatile startup *startup );

// MMU_enter allocates raw memory (not multi-processor safe), calls BOOT_finished_allocating when it's done, builds
// an environment where the ROM, etc. are mapped into virtual memory, and calls Kernel_start, when it has.
void __attribute__(( noreturn, noinline )) MMU_enter( core_workspace *ws, volatile startup *startup );
