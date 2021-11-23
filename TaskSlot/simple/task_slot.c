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
// A TaskSlot is essentially a user process, with blocks of RAM located at 0x8000
// This can be extended to implement threads. TODO
// Since the filing system in RO is aggressively attuned to a single processor
// model, running a single program at a time, other than Wimp tasks, each TaskSlot will
// need to store its idea of the filing system context (CSD, etc.) TODO rewrite that!
// Also, it should probably create a Code Sys$ReturnCode that can update the
// parent TaskSlot.

typedef struct handler handler;

struct handler {
  uint32_t code;
  uint32_t private_word;
  uint32_t buffer;
};

struct TaskSlot {
  bool allocated;
  physical_memory_block blocks[10];
  handler handlers[17];
  Task task;
};

extern TaskSlot task_slots[];

/* Handlers. Per slot, or per thread, I wonder? I'll go for per slot, to start with (matches with the idea of
   cleaning up the handlers on exit).
0 	Memory limit 	Memory limit 	Unused 	Unused
1 	Undefined instruction 	Handler code 	Unused 	Unused
2 	Prefetch abort 	Handler code 	Unused 	Unused
3 	Data abort 	Handler code 	Unused 	Unused
4 	Address exception 	Handler code 	Unused 	Unused
5 	Other exceptions (reserved) 	Unused 	Unused 	Unused
6 	Error 	Handler code 	Handler R0 	Error buffer
7 	CallBack 	Handler code 	Handler R12 	Register dump buffer
8 	BreakPoint? 	Handler code 	Handler R12 	Register dump buffer
9 	Escape 	Handler code 	Handler R12 	Unused
10 	Event? 	Handler code 	Handler R12 	Unused
11 	Exit 	Handler code 	Handler R12 	Unused
12 	Unused SWI? 	Handler code 	Handler R12 	Unused
13 	Exception registers 	Register dump buffer 	Unused 	Unused
14 	Application space 	Memory limit 	Unused 	Unused
15 	Currently active object 	CAO pointer 	Unused 	Unused
16 	UpCall 	Handler code 	Handler R12 	Unused
*/

void __attribute__(( noinline )) do_ChangeEnvironment( uint32_t *regs )
{
  if (regs[0] > 16) {
    asm( "bkpt 1" );
  }
  handler *h = &workspace.task_slot.running->slot->handlers[regs[0]];
  handler old = *h;
  if (regs[1] != 0) {
    h->code = regs[1];
  }
  if (regs[2] != 0) {
    h->private_word = regs[2];
  }
  if (regs[3] != 0) {
    h->buffer = regs[3];
  }

  regs[1] = old.code;
  regs[2] = old.private_word;
  regs[3] = old.buffer;
}

void __attribute__(( naked )) default_os_changeenvironment()
{
  register uint32_t *regs;
  asm ( "push { r0-r3 }\n  mov %[regs], sp" : [regs] "=r" (regs) );

  do_ChangeEnvironment( regs );

  asm ( "pop { r0-r3, pc }" );
}

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


