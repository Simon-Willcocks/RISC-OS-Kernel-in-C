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

// TaskSlots in RISC OS are always USR mode RWX. This is not ideal, but the way forward is to
// migrate critical services to Secure mode Aarch64.
typedef struct TaskSlot TaskSlot;

typedef struct physical_memory_block { // All 4k pages
  uint32_t virtual_base;
  uint32_t physical_base;
  uint32_t size:20;
  uint32_t res:12;
} physical_memory_block;

uint32_t pre_mmu_allocate_physical_memory( uint32_t size, uint32_t alignment, volatile startup *startup );

uint32_t Kernel_allocate_physical_memory( uint32_t size, uint32_t alignment );
void __attribute__(( noreturn )) Kernel_start();

static const uint32_t natural_alignment = (1 << 20); // alignment needed to avoid small pages

static inline bool naturally_aligned( uint32_t location )
{
  // 1MB sections, with this MMU
  return ((natural_alignment - 1) & location) == 0;
}

// An instance of this struct will be in the core workspace, called mmu:
struct MMU_workspace {
  struct Level_two_translation_table *zero_page_l2tt;
  struct Level_two_translation_table *kernel_l2tt;
};

struct MMU_shared_workspace {
  uint32_t lock;
  struct Level_one_translation_table *global_l1tt; // Physical address, mapped to Global_L1TT
  struct Level_two_translation_table *physical_l2tts;
  struct Level_two_translation_table *global_l2tt;

  // Virtual address:
  struct Level_two_translation_table *kernel_l2tt;
};

// This routine is a service to the MMU code from the Kernel. It returns
// information about the physical block of memory that should appear at
// the given virtual address.
physical_memory_block Kernel_physical_address( uint32_t va );

TaskSlot *MMU_new_slot();
void TaskSlot_add( TaskSlot *slot, physical_memory_block memory );
uint32_t TaskSlot_asid( TaskSlot *slot );

void MMU_switch_to( TaskSlot *slot );

// Kernel memory mapping routines
void MMU_map_at( void *va, uint32_t pa, uint32_t size );
void MMU_map_shared_at( void *va, uint32_t pa, uint32_t size );
void MMU_map_device_at( void *va, uint32_t pa, uint32_t size ); // Devices always shared

// Map the block twice into virtual memory (where? who decides?)
// The reason is that that allows the readers and writers to see
// contiguous memory, even for data that overruns the end of the
// memory and starts again at the beginning.
// Does it have to be the full double, or just the configured
// maximum block size?
// Note: this memory can be in top bit set address range, since
// only new code will use it.
//void MMU_map_pipe( uint32_t phys, uint32_t size, uint32_t over );

void BOOT_finished_allocating( uint32_t core, volatile startup *startup );
void Initialise_privileged_mode_stacks(); // Must be called before any exceptions

// MMU_enter allocates raw memory (not multi-processor safe), calls BOOT_finished_allocating when it's done, builds
// an environment where the ROM, etc. are mapped into virtual memory, and calls Kernel_start, when it has.
void __attribute__(( noreturn, noinline )) MMU_enter( core_workspace *ws, startup *startup );
