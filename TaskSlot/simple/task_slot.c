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

/* This file encapsulates how the TaskSlot structure is maintained.
 * All modifications to the set of slots or the content of a slot shall be protected
 * by claiming shared.mmu.lock.
 */

#include "inkernel.h"

// Initial version, using arrays rather than linked lists or similar.

struct TaskSlot {
  bool allocated;
  physical_memory_block blocks[10];
};

extern TaskSlot task_slots[];

physical_memory_block Kernel_physical_address( TaskSlot *slot, uint32_t va )
{
  for (int i = 0; i < number_of( slot->blocks ); i++) {
    if (slot->blocks[i].virtual_base <= va && slot->blocks[i].virtual_base + slot->blocks[i].size > va) {
      return slot->blocks[i];
    }
  }
  physical_memory_block fail = { 0, 0, 0 };
  return fail;
}

TaskSlot *MMU_new_slot()
{
  TaskSlot *result = 0;

  claim_lock( &shared.mmu.lock );

  bool first_core = 0 == shared.mmu.slots_memory;

  if (first_core) // Allocate physical memory, to be shared
    shared.mmu.slots_memory = Kernel_allocate_pages( 4096, 4096 );

  // First call to this routine for this core? (Assumes MMU_switch_to will be
  // called before the second call to MMU_new_slot.)
  if (workspace.mmu.current == 0) {
    MMU_map_shared_at( &task_slots, shared.mmu.slots_memory, 4096 );

    if (first_core)
      bzero( &task_slots, 4096 ); // FIXME make expandable
  }

  for (int i = 0; i < 4096/sizeof( TaskSlot ) && result == 0; i++) {
    if (!task_slots[i].allocated) {
      result = &task_slots[i];
      result->allocated = true;
    }
  }

  release_lock( &shared.mmu.lock );

  if (result == 0) for (;;) {} // FIXME: expand

  return result;
}

void TaskSlot_add( TaskSlot *slot, physical_memory_block memory )
{
  claim_lock( &shared.mmu.lock );
  for (int i = 0; i < number_of( slot->blocks ); i++) {
    if (slot->blocks[i].size == 0) {
      slot->blocks[i] = memory;
      break;
    }
  }
  release_lock( &shared.mmu.lock );
}

uint32_t TaskSlot_asid( TaskSlot *slot )
{
  return (slot - task_slots) + 1;
}


