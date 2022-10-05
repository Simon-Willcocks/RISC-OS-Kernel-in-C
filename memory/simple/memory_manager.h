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

typedef struct DynamicArea DynamicArea;

struct Memory_manager_workspace {
  DynamicArea *dynamic_areas;
};

typedef struct {
  uint32_t base_page;
  uint32_t size; // pages
} free_block;

// For debugging a deadlock; make both locks the same word, then detect when they're being re-claimed.
#define os_heap_lock lock

struct Memory_manager_shared_workspace {
  uint32_t lock;
//  uint32_t os_heap_lock; // Should only be needed for shared heaps, and possibly one lock per heap (maybe invert the magic word, but be aware of interrupt handlers using OS_Heap - how is that dealt with today?).
  // Might be best in a shares.swis struct instead of here...

  uint32_t dynamic_areas_setup_lock; // This has to be separate from dynamic_areas_lock, because OS_Heap uses OS_DynamicArea
  uint32_t dynamic_areas_lock;
  free_block free_blocks[64]; // This is the real free memory, not what we tell the applications!
  DynamicArea *dynamic_areas;
  uint32_t rma_memory;  // Required before you can access the RMA dynamic areas
  uint32_t last_da_address;
  uint32_t user_da_number;

  uint32_t os_memory_active_state; // FIXME Just a toggle, at the moment

  uint32_t device_page_lock;
  struct {
    uint32_t pages:12;
    uint32_t page_number:20;
  } device_pages[63];
};

void Initialise_system_DAs();

void Kernel_add_free_RAM( uint32_t base_page, uint32_t size_in_pages );
uint32_t Kernel_allocate_pages( uint32_t size, uint32_t alignment );

void __attribute__(( naked, noreturn )) Kernel_default_prefetch();
void __attribute__(( naked, noreturn )) Kernel_default_data_abort();
